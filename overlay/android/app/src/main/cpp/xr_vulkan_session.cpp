#include "xr_vulkan_session.h"
#include "platform_android.h"
// For the game camera's view window, which the Im2D plane must match.
#include "android.h"

#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

static const uint32_t kBringupVertSpv[] =
#include "bringup_vert.h"
;
static const uint32_t kBringupFragSpv[] =
#include "bringup_frag.h"
;

#define XR_CHECK(expr, what) \
	do { \
		XrResult xrCheckResult = (expr); \
		if(XR_FAILED(xrCheckResult)){ \
			LOGE("%s failed: %d", what, (int)xrCheckResult); \
			return false; \
		} \
	} while(0)

#define VK_CHECK(expr, what) \
	do { \
		VkResult vkCheckResult = (expr); \
		if(vkCheckResult != VK_SUCCESS){ \
			LOGE("%s failed: %d", what, (int)vkCheckResult); \
			return false; \
		} \
	} while(0)

namespace xrvk {
namespace {

// ---------------------------------------------------------------------------
// Minimal column-major 4x4 maths. Column-major is what GLSL consumes directly,
// so the uniform block needs no transpose on upload.
// ---------------------------------------------------------------------------

struct Mat4 { float m[16]; };

Mat4
identity(void)
{
	Mat4 r = {};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

Mat4
multiply(const Mat4 &a, const Mat4 &b)
{
	Mat4 r;
	for(int col = 0; col < 4; col++)
		for(int row = 0; row < 4; row++){
			float sum = 0.0f;
			for(int k = 0; k < 4; k++)
				sum += a.m[k*4 + row] * b.m[col*4 + k];
			r.m[col*4 + row] = sum;
		}
	return r;
}

// Asymmetric off-axis projection for a Vulkan clip volume: depth 0..1 and a
// downward Y axis. The four OpenXR angles are signed half-angles from the
// view direction, so tangents cannot be assumed symmetric on Quest optics.
Mat4
projectionFromFov(const XrFovf &fov, float nearZ, float farZ)
{
	const float tanLeft = tanf(fov.angleLeft);
	const float tanRight = tanf(fov.angleRight);
	const float tanUp = tanf(fov.angleUp);
	const float tanDown = tanf(fov.angleDown);

	const float tanWidth = tanRight - tanLeft;
	const float tanHeight = tanDown - tanUp;	// flipped for Vulkan

	Mat4 r = {};
	r.m[0]  = 2.0f / tanWidth;
	r.m[5]  = 2.0f / tanHeight;
	r.m[8]  = (tanRight + tanLeft) / tanWidth;
	r.m[9]  = (tanUp + tanDown) / tanHeight;
	r.m[10] = -farZ / (farZ - nearZ);
	r.m[11] = -1.0f;
	r.m[14] = -(farZ * nearZ) / (farZ - nearZ);
	return r;
}

// Rigid inverse of the view pose: transpose the rotation, then rotate the
// negated translation by it.
Mat4
viewFromPose(const XrPosef &pose)
{
	const XrQuaternionf &q = pose.orientation;
	const float x = q.x, y = q.y, z = q.z, w = q.w;

	const float r00 = 1.0f - 2.0f*(y*y + z*z);
	const float r01 = 2.0f*(x*y - z*w);
	const float r02 = 2.0f*(x*z + y*w);
	const float r10 = 2.0f*(x*y + z*w);
	const float r11 = 1.0f - 2.0f*(x*x + z*z);
	const float r12 = 2.0f*(y*z - x*w);
	const float r20 = 2.0f*(x*z - y*w);
	const float r21 = 2.0f*(y*z + x*w);
	const float r22 = 1.0f - 2.0f*(x*x + y*y);

	const float px = pose.position.x, py = pose.position.y, pz = pose.position.z;

	Mat4 r = {};
	// Transposed rotation, written column-major.
	r.m[0] = r00; r.m[1] = r01; r.m[2] = r02;
	r.m[4] = r10; r.m[5] = r11; r.m[6] = r12;
	r.m[8] = r20; r.m[9] = r21; r.m[10] = r22;
	r.m[12] = -(r00*px + r10*py + r20*pz);
	r.m[13] = -(r01*px + r11*py + r21*pz);
	r.m[14] = -(r02*px + r12*py + r22*pz);
	r.m[15] = 1.0f;
	return r;
}

// Rigid head transform from a pose: rotation, then translation.
Mat4
worldFromPose(const XrPosef &pose)
{
	const XrQuaternionf &q = pose.orientation;
	const float x = q.x, y = q.y, z = q.z, w = q.w;

	Mat4 r = {};
	r.m[0] = 1.0f - 2.0f*(y*y + z*z);
	r.m[1] = 2.0f*(x*y + z*w);
	r.m[2] = 2.0f*(x*z - y*w);
	r.m[4] = 2.0f*(x*y - z*w);
	r.m[5] = 1.0f - 2.0f*(x*x + z*z);
	r.m[6] = 2.0f*(y*z + x*w);
	r.m[8] = 2.0f*(x*z + y*w);
	r.m[9] = 2.0f*(y*z - x*w);
	r.m[10] = 1.0f - 2.0f*(x*x + y*y);
	r.m[12] = pose.position.x;
	r.m[13] = pose.position.y;
	r.m[14] = pose.position.z;
	r.m[15] = 1.0f;
	return r;
}

// Maps screen pixel coordinates onto a quad of the given size, centred a fixed
// distance ahead of the head. Screen Y grows downward, hence the negative Y
// scale; the plane sits at -Z because that is forward in OpenXR view space.
Mat4
im2dPlane(const XrPosef &headPose, float screenWidth, float screenHeight,
          float distance, float planeWidth, float planeHeight)
{
	Mat4 plane = {};
	plane.m[0] = screenWidth > 0.0f ? planeWidth / screenWidth : 0.0f;
	plane.m[5] = screenHeight > 0.0f ? -planeHeight / screenHeight : 0.0f;
	plane.m[10] = 1.0f;
	plane.m[12] = -planeWidth * 0.5f;
	plane.m[13] = planeHeight * 0.5f;
	plane.m[14] = -distance;
	plane.m[15] = 1.0f;

	return multiply(worldFromPose(headPose), plane);
}

Mat4
translationScale(float tx, float ty, float tz, float sx, float sy, float sz)
{
	Mat4 r = identity();
	r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
	r.m[12] = tx; r.m[13] = ty; r.m[14] = tz;
	return r;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Vertex { float position[3]; float normal[3]; };

const Vertex kCubeVertices[] = {
	// +X
	{{ 0.5f,-0.5f,-0.5f},{ 1, 0, 0}}, {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0}},
	{{ 0.5f, 0.5f, 0.5f},{ 1, 0, 0}}, {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0}},
	// -X
	{{-0.5f,-0.5f, 0.5f},{-1, 0, 0}}, {{-0.5f, 0.5f, 0.5f},{-1, 0, 0}},
	{{-0.5f, 0.5f,-0.5f},{-1, 0, 0}}, {{-0.5f,-0.5f,-0.5f},{-1, 0, 0}},
	// +Y
	{{-0.5f, 0.5f,-0.5f},{ 0, 1, 0}}, {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0}},
	{{ 0.5f, 0.5f, 0.5f},{ 0, 1, 0}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0}},
	// -Y
	{{-0.5f,-0.5f, 0.5f},{ 0,-1, 0}}, {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0}},
	{{ 0.5f,-0.5f,-0.5f},{ 0,-1, 0}}, {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0}},
	// +Z
	{{-0.5f,-0.5f, 0.5f},{ 0, 0, 1}}, {{ 0.5f,-0.5f, 0.5f},{ 0, 0, 1}},
	{{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1}}, {{-0.5f, 0.5f, 0.5f},{ 0, 0, 1}},
	// -Z
	{{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1}}, {{-0.5f,-0.5f,-0.5f},{ 0, 0,-1}},
	{{-0.5f, 0.5f,-0.5f},{ 0, 0,-1}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 0,-1}},
};

const uint16_t kCubeIndices[] = {
	 0, 1, 2,  0, 2, 3,      4, 5, 6,  4, 6, 7,
	 8, 9,10,  8,10,11,     12,13,14, 12,14,15,
	16,17,18, 16,18,19,     20,21,22, 20,22,23,
};

struct PushConstants { Mat4 model; float tint[4]; };

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct SwapchainImage
{
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkCommandBuffer commandBuffer;
	VkFence fence;
	VkBuffer uniformBuffer;
	VkDeviceMemory uniformMemory;
	void *uniformMapped;
	VkDescriptorSet descriptorSet;
};

struct State
{
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace space = XR_NULL_HANDLE;
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
	bool running = false;
	bool exitRequested = false;

	XrViewConfigurationView viewConfigs[2];
	// STAGE is preferred because a standing game wants a floor-relative origin,
	// but it depends on a configured play area and stops reporting a valid
	// position without one. LOCAL always tracks, so it is kept as a fallback.
	XrSpace localSpace = XR_NULL_HANDLE;
	XrSpace viewSpace = XR_NULL_HANDLE;

	// Head-locked debug overlay quad, fed by the game's port of the desktop
	// debug panel.
	XrSwapchain debugSwapchain = XR_NULL_HANDLE;
	std::vector<VkImage> debugImages;
	int debugWidth = 0, debugHeight = 0;
	VkBuffer debugStaging = VK_NULL_HANDLE;
	VkDeviceMemory debugStagingMemory = VK_NULL_HANDLE;
	void *debugStagingMapped = nullptr;
	VkCommandBuffer debugCommand = VK_NULL_HANDLE;
	VkFence debugFence = VK_NULL_HANDLE;
	bool debugVisible = false;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	int64_t swapchainFormat = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;

	VkInstance vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VkQueue queue = VK_NULL_HANDLE;

	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

	VkImage depthImage = VK_NULL_HANDLE;
	VkDeviceMemory depthMemory = VK_NULL_HANDLE;
	VkImageView depthView = VK_NULL_HANDLE;
	VkFormat depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

	VkBuffer vertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
	VkBuffer indexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory indexMemory = VK_NULL_HANDLE;

	std::vector<SwapchainImage> images;

	XrTime predictedDisplayTime = 0;
	long long lastPredictedDisplayTimeNs = 0;
	bool hasRefreshRateExt = false;
	float elapsed = 0.0f;

	FrameRenderer frameRenderer = nullptr;

	// Input
	XrActionSet actionSet = XR_NULL_HANDLE;
	XrPath handPath[2] = { XR_NULL_PATH, XR_NULL_PATH };
	XrAction thumbstickAction = XR_NULL_HANDLE;
	XrAction triggerAction = XR_NULL_HANDLE;
	XrAction squeezeAction = XR_NULL_HANDLE;
	XrAction thumbClickAction = XR_NULL_HANDLE;
	XrAction aAction = XR_NULL_HANDLE;
	XrAction bAction = XR_NULL_HANDLE;
	XrAction xAction = XR_NULL_HANDLE;
	XrAction yAction = XR_NULL_HANDLE;
	XrAction menuAction = XR_NULL_HANDLE;
	ControllerInput input = {};
};

State g;

// ---------------------------------------------------------------------------
// Vulkan helpers
// ---------------------------------------------------------------------------

bool
findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t *out)
{
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(g.physicalDevice, &memProps);
	for(uint32_t i = 0; i < memProps.memoryTypeCount; i++){
		if((typeBits & (1u << i)) == 0)
			continue;
		if((memProps.memoryTypes[i].propertyFlags & properties) == properties){
			*out = i;
			return true;
		}
	}
	LOGE("no Vulkan memory type for bits %u properties %u", typeBits, properties);
	return false;
}

bool
createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
             VkMemoryPropertyFlags properties, VkBuffer *bufferOut,
             VkDeviceMemory *memoryOut)
{
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK(vkCreateBuffer(g.device, &bufferInfo, nullptr, bufferOut), "vkCreateBuffer");

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(g.device, *bufferOut, &requirements);

	uint32_t typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits, properties, &typeIndex))
		return false;

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	VK_CHECK(vkAllocateMemory(g.device, &allocInfo, nullptr, memoryOut), "vkAllocateMemory");
	VK_CHECK(vkBindBufferMemory(g.device, *bufferOut, *memoryOut, 0), "vkBindBufferMemory");
	return true;
}

bool
uploadThroughHostBuffer(const void *data, VkDeviceSize size,
                        VkBufferUsageFlags usage, VkBuffer *bufferOut,
                        VkDeviceMemory *memoryOut)
{
	// The bring-up geometry is a few kilobytes and is written once. A
	// host-visible buffer avoids a staging copy and a transfer submission
	// before the device is even known to work.
	if(!createBuffer(size, usage,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 bufferOut, memoryOut))
		return false;
	void *mapped = nullptr;
	VK_CHECK(vkMapMemory(g.device, *memoryOut, 0, size, 0, &mapped), "vkMapMemory");
	memcpy(mapped, data, (size_t)size);
	vkUnmapMemory(g.device, *memoryOut);
	return true;
}

VkShaderModule
createShaderModule(const uint32_t *code, size_t byteSize)
{
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = byteSize;
	info.pCode = code;
	VkShaderModule module = VK_NULL_HANDLE;
	if(vkCreateShaderModule(g.device, &info, nullptr, &module) != VK_SUCCESS){
		LOGE("vkCreateShaderModule failed");
		return VK_NULL_HANDLE;
	}
	return module;
}

// ---------------------------------------------------------------------------
// OpenXR setup
// ---------------------------------------------------------------------------

bool
initialiseLoader(android_app *app)
{
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	XrResult result = xrGetInstanceProcAddr(
		XR_NULL_HANDLE, "xrInitializeLoaderKHR",
		(PFN_xrVoidFunction *)&xrInitializeLoaderKHR);
	if(XR_FAILED(result) || xrInitializeLoaderKHR == nullptr){
		LOGE("xrInitializeLoaderKHR unavailable: %d", (int)result);
		return false;
	}

	XrLoaderInitInfoAndroidKHR loaderInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInfo.applicationVM = app->activity->vm;
	loaderInfo.applicationContext = app->activity->clazz;
	XR_CHECK(xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo),
	         "xrInitializeLoaderKHR");
	return true;
}

bool
createInstance(android_app *app)
{
	uint32_t extensionCount = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
	std::vector<XrExtensionProperties> available(
		extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
	xrEnumerateInstanceExtensionProperties(nullptr, extensionCount,
	                                       &extensionCount, available.data());

	auto has = [&](const char *name){
		for(const XrExtensionProperties &e : available)
			if(strcmp(e.extensionName, name) == 0)
				return true;
		return false;
	};

	std::vector<const char *> enabled;
	if(!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)){
		LOGE("runtime does not expose %s", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
		return false;
	}
	enabled.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if(has(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
		enabled.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
	// Pin the display to 72 Hz. Left alone, the runtime promotes fast apps to
	// 90, where this one hovers at 80-88 and misses a vsync every couple of
	// seconds; each miss doubles that frame's world motion, felt as the world
	// lurching. A held 72 beats a missed 90.
	g.hasRefreshRateExt = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if(g.hasRefreshRateExt)
		enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);

	XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = app->activity->vm;
	androidInfo.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
	info.next = &androidInfo;
	info.enabledExtensionCount = (uint32_t)enabled.size();
	info.enabledExtensionNames = enabled.data();
	strcpy(info.applicationInfo.applicationName, "Vice City VR");
	info.applicationInfo.applicationVersion = 1;
	strcpy(info.applicationInfo.engineName, "reVC");
	info.applicationInfo.engineVersion = 1;
	info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

	XR_CHECK(xrCreateInstance(&info, &g.instance), "xrCreateInstance");

	XrInstanceProperties properties = { XR_TYPE_INSTANCE_PROPERTIES };
	if(XR_SUCCEEDED(xrGetInstanceProperties(g.instance, &properties)))
		LOGI("OpenXR runtime: %s", properties.runtimeName);
	return true;
}

bool
createVulkan(void)
{
	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(g.instance, &systemInfo, &g.systemId), "xrGetSystem");

	PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
	PFN_xrCreateVulkanInstanceKHR createVulkanInstance = nullptr;
	PFN_xrGetVulkanGraphicsDevice2KHR getGraphicsDevice = nullptr;
	PFN_xrCreateVulkanDeviceKHR createVulkanDevice = nullptr;
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsRequirements2KHR",
	                      (PFN_xrVoidFunction *)&getRequirements);
	xrGetInstanceProcAddr(g.instance, "xrCreateVulkanInstanceKHR",
	                      (PFN_xrVoidFunction *)&createVulkanInstance);
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsDevice2KHR",
	                      (PFN_xrVoidFunction *)&getGraphicsDevice);
	xrGetInstanceProcAddr(g.instance, "xrCreateVulkanDeviceKHR",
	                      (PFN_xrVoidFunction *)&createVulkanDevice);
	if(getRequirements == nullptr || createVulkanInstance == nullptr ||
	   getGraphicsDevice == nullptr || createVulkanDevice == nullptr){
		LOGE("XR_KHR_vulkan_enable2 entry points missing");
		return false;
	}

	XrGraphicsRequirementsVulkan2KHR requirements = {
		XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
	XR_CHECK(getRequirements(g.instance, g.systemId, &requirements),
	         "xrGetVulkanGraphicsRequirements2KHR");

	// The runtime creates the VkInstance on our behalf so it can inject the
	// extensions it needs for swapchain sharing.
	VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	appInfo.pApplicationName = "Vice City VR";
	appInfo.applicationVersion = 1;
	appInfo.pEngineName = "reVC";
	appInfo.engineVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo vkInstanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	vkInstanceInfo.pApplicationInfo = &appInfo;

	XrVulkanInstanceCreateInfoKHR xrVkInstanceInfo = {
		XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
	xrVkInstanceInfo.systemId = g.systemId;
	xrVkInstanceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xrVkInstanceInfo.vulkanCreateInfo = &vkInstanceInfo;

	VkResult vkResult = VK_SUCCESS;
	XR_CHECK(createVulkanInstance(g.instance, &xrVkInstanceInfo, &g.vkInstance, &vkResult),
	         "xrCreateVulkanInstanceKHR");
	VK_CHECK(vkResult, "vkCreateInstance (via OpenXR)");

	XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo = {
		XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
	deviceGetInfo.systemId = g.systemId;
	deviceGetInfo.vulkanInstance = g.vkInstance;
	XR_CHECK(getGraphicsDevice(g.instance, &deviceGetInfo, &g.physicalDevice),
	         "xrGetVulkanGraphicsDevice2KHR");

	VkPhysicalDeviceProperties physicalProperties;
	vkGetPhysicalDeviceProperties(g.physicalDevice, &physicalProperties);
	LOGI("Vulkan device: %s (API %u.%u.%u)", physicalProperties.deviceName,
	     VK_VERSION_MAJOR(physicalProperties.apiVersion),
	     VK_VERSION_MINOR(physicalProperties.apiVersion),
	     VK_VERSION_PATCH(physicalProperties.apiVersion));

	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g.physicalDevice, &familyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(familyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(g.physicalDevice, &familyCount, families.data());
	bool foundFamily = false;
	for(uint32_t i = 0; i < familyCount; i++)
		if(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){
			g.queueFamily = i;
			foundFamily = true;
			break;
		}
	if(!foundFamily){
		LOGE("no graphics queue family");
		return false;
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = g.queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;

	// Multiview is the load-bearing feature of this backend, so it is requested
	// explicitly rather than relying on it being enabled by default.
	VkPhysicalDeviceMultiviewFeatures multiviewFeatures = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES };
	multiviewFeatures.multiview = VK_TRUE;

	VkPhysicalDeviceFeatures deviceFeatures = {};
	deviceFeatures.samplerAnisotropy = VK_TRUE;
	deviceFeatures.textureCompressionASTC_LDR = VK_TRUE;

	VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	deviceInfo.pNext = &multiviewFeatures;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.pEnabledFeatures = &deviceFeatures;

	XrVulkanDeviceCreateInfoKHR xrDeviceInfo = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
	xrDeviceInfo.systemId = g.systemId;
	xrDeviceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xrDeviceInfo.vulkanPhysicalDevice = g.physicalDevice;
	xrDeviceInfo.vulkanCreateInfo = &deviceInfo;

	XR_CHECK(createVulkanDevice(g.instance, &xrDeviceInfo, &g.device, &vkResult),
	         "xrCreateVulkanDeviceKHR");
	VK_CHECK(vkResult, "vkCreateDevice (via OpenXR)");

	vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);
	return true;
}

bool
createSession(void)
{
	XrGraphicsBindingVulkan2KHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
	binding.instance = g.vkInstance;
	binding.physicalDevice = g.physicalDevice;
	binding.device = g.device;
	binding.queueFamilyIndex = g.queueFamily;
	binding.queueIndex = 0;

	XrSessionCreateInfo info = { XR_TYPE_SESSION_CREATE_INFO };
	info.next = &binding;
	info.systemId = g.systemId;
	XR_CHECK(xrCreateSession(g.instance, &info, &g.session), "xrCreateSession");

	// STAGE gives a floor-relative origin, which is what a standing game wants.
	// Not every runtime configuration offers it, so LOCAL remains the fallback.
	uint32_t spaceCount = 0;
	xrEnumerateReferenceSpaces(g.session, 0, &spaceCount, nullptr);
	std::vector<XrReferenceSpaceType> spaces(spaceCount);
	xrEnumerateReferenceSpaces(g.session, spaceCount, &spaceCount, spaces.data());
	XrReferenceSpaceType chosen = XR_REFERENCE_SPACE_TYPE_LOCAL;
	for(XrReferenceSpaceType type : spaces)
		if(type == XR_REFERENCE_SPACE_TYPE_STAGE){
			chosen = XR_REFERENCE_SPACE_TYPE_STAGE;
			break;
		}

	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.referenceSpaceType = chosen;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	XR_CHECK(xrCreateReferenceSpace(g.session, &spaceInfo, &g.space),
	         "xrCreateReferenceSpace");

	// Always create LOCAL as well. If STAGE turns out not to be tracking --
	// which happens without a configured play area -- locating views in it
	// yields no valid position, and rendering would be skipped entirely.
	XrReferenceSpaceCreateInfo localInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	localInfo.poseInReferenceSpace.orientation.w = 1.0f;
	if(XR_FAILED(xrCreateReferenceSpace(g.session, &localInfo, &g.localSpace)))
		g.localSpace = XR_NULL_HANDLE;

	// VIEW space carries the head-locked quad layers (the desktop's debug
	// overlay and VR menu attach to gViewSpace the same way).
	XrReferenceSpaceCreateInfo viewInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	viewInfo.poseInReferenceSpace.orientation.w = 1.0f;
	if(XR_FAILED(xrCreateReferenceSpace(g.session, &viewInfo, &g.viewSpace)))
		g.viewSpace = XR_NULL_HANDLE;

	LOGI("reference space: %s (local fallback %s)",
	     chosen == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL",
	     g.localSpace != XR_NULL_HANDLE ? "available" : "unavailable");
	return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

XrAction
makeAction(const char *name, const char *localised, XrActionType type,
           bool perHand)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	strcpy(info.actionName, name);
	strcpy(info.localizedActionName, localised);
	if(perHand){
		info.countSubactionPaths = 2;
		info.subactionPaths = g.handPath;
	}
	XrAction action = XR_NULL_HANDLE;
	if(XR_FAILED(xrCreateAction(g.actionSet, &info, &action)))
		LOGE("xrCreateAction failed for %s", name);
	return action;
}

bool
createActions(void)
{
	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(setInfo.actionSetName, "gameplay");
	strcpy(setInfo.localizedActionSetName, "Gameplay");
	XR_CHECK(xrCreateActionSet(g.instance, &setInfo, &g.actionSet),
	         "xrCreateActionSet");

	xrStringToPath(g.instance, "/user/hand/left", &g.handPath[0]);
	xrStringToPath(g.instance, "/user/hand/right", &g.handPath[1]);

	g.thumbstickAction = makeAction("thumbstick", "Thumbstick",
	                                XR_ACTION_TYPE_VECTOR2F_INPUT, true);
	g.triggerAction = makeAction("trigger", "Trigger",
	                             XR_ACTION_TYPE_FLOAT_INPUT, true);
	g.squeezeAction = makeAction("squeeze", "Grip",
	                             XR_ACTION_TYPE_FLOAT_INPUT, true);
	g.thumbClickAction = makeAction("thumbclick", "Thumbstick Click",
	                                XR_ACTION_TYPE_BOOLEAN_INPUT, true);
	g.aAction = makeAction("abutton", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.bAction = makeAction("bbutton", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.xAction = makeAction("xbutton", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.yAction = makeAction("ybutton", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.menuAction = makeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);

	struct Binding { XrAction action; const char *path; };
	const Binding bindings[] = {
		{ g.thumbstickAction, "/user/hand/left/input/thumbstick" },
		{ g.thumbstickAction, "/user/hand/right/input/thumbstick" },
		{ g.triggerAction,    "/user/hand/left/input/trigger/value" },
		{ g.triggerAction,    "/user/hand/right/input/trigger/value" },
		{ g.squeezeAction,    "/user/hand/left/input/squeeze/value" },
		{ g.squeezeAction,    "/user/hand/right/input/squeeze/value" },
		{ g.thumbClickAction, "/user/hand/left/input/thumbstick/click" },
		{ g.thumbClickAction, "/user/hand/right/input/thumbstick/click" },
		{ g.aAction,          "/user/hand/right/input/a/click" },
		{ g.bAction,          "/user/hand/right/input/b/click" },
		{ g.xAction,          "/user/hand/left/input/x/click" },
		{ g.yAction,          "/user/hand/left/input/y/click" },
		// Only the left controller exposes a menu button; the right one's
		// system button belongs to the runtime.
		{ g.menuAction,       "/user/hand/left/input/menu/click" },
	};

	std::vector<XrActionSuggestedBinding> suggested;
	for(const Binding &binding : bindings){
		XrPath path = XR_NULL_PATH;
		if(XR_FAILED(xrStringToPath(g.instance, binding.path, &path)))
			continue;
		suggested.push_back({ binding.action, path });
	}

	XrPath profile = XR_NULL_PATH;
	xrStringToPath(g.instance, "/interaction_profiles/oculus/touch_controller",
	               &profile);
	XrInteractionProfileSuggestedBinding suggestion = {
		XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggestion.interactionProfile = profile;
	suggestion.countSuggestedBindings = (uint32_t)suggested.size();
	suggestion.suggestedBindings = suggested.data();
	XR_CHECK(xrSuggestInteractionProfileBindings(g.instance, &suggestion),
	         "xrSuggestInteractionProfileBindings");

	XrSessionActionSetsAttachInfo attachInfo = {
		XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &g.actionSet;
	XR_CHECK(xrAttachSessionActionSets(g.session, &attachInfo),
	         "xrAttachSessionActionSets");

	LOGI("input: %zu bindings attached", suggested.size());
	return true;
}

float
readFloat(XrAction action, int hand)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	info.subactionPath = g.handPath[hand];
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
	if(XR_FAILED(xrGetActionStateFloat(g.session, &info, &state)) || !state.isActive)
		return 0.0f;
	return state.currentState;
}

bool
readBool(XrAction action, int hand)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	info.subactionPath = hand >= 0 ? g.handPath[hand] : XR_NULL_PATH;
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
	if(XR_FAILED(xrGetActionStateBoolean(g.session, &info, &state)) || !state.isActive)
		return false;
	return state.currentState == XR_TRUE;
}

void
readStick(int hand, float *outX, float *outY)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = g.thumbstickAction;
	info.subactionPath = g.handPath[hand];
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
	if(XR_FAILED(xrGetActionStateVector2f(g.session, &info, &state)) || !state.isActive){
		*outX = *outY = 0.0f;
		return;
	}
	*outX = state.currentState.x;
	*outY = state.currentState.y;
}

void
syncInput(void)
{
	if(g.actionSet == XR_NULL_HANDLE || g.sessionState != XR_SESSION_STATE_FOCUSED)
		return;

	XrActiveActionSet active = { g.actionSet, XR_NULL_PATH };
	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &active;
	if(XR_FAILED(xrSyncActions(g.session, &syncInfo)))
		return;

	readStick(0, &g.input.leftStickX, &g.input.leftStickY);
	readStick(1, &g.input.rightStickX, &g.input.rightStickY);
	g.input.leftTrigger = readFloat(g.triggerAction, 0);
	g.input.rightTrigger = readFloat(g.triggerAction, 1);
	g.input.leftGrip = readFloat(g.squeezeAction, 0);
	g.input.rightGrip = readFloat(g.squeezeAction, 1);
	g.input.leftStickClick = readBool(g.thumbClickAction, 0);
	g.input.rightStickClick = readBool(g.thumbClickAction, 1);
	g.input.a = readBool(g.aAction, -1);
	g.input.b = readBool(g.bAction, -1);
	g.input.x = readBool(g.xAction, -1);
	g.input.y = readBool(g.yAction, -1);
	g.input.menu = readBool(g.menuAction, -1);
}

bool
createSwapchain(void)
{
	uint32_t viewCount = 0;
	XR_CHECK(xrEnumerateViewConfigurationViews(
	             g.instance, g.systemId,
	             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
	         "xrEnumerateViewConfigurationViews");
	if(viewCount != 2){
		LOGE("expected a stereo view configuration, got %u views", viewCount);
		return false;
	}
	for(uint32_t i = 0; i < 2; i++)
		g.viewConfigs[i] = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
	XR_CHECK(xrEnumerateViewConfigurationViews(
	             g.instance, g.systemId,
	             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, g.viewConfigs),
	         "xrEnumerateViewConfigurationViews");

	g.renderWidth = g.viewConfigs[0].recommendedImageRectWidth;
	g.renderHeight = g.viewConfigs[0].recommendedImageRectHeight;
	LOGI("per-eye render target: %ux%u", g.renderWidth, g.renderHeight);

	uint32_t formatCount = 0;
	xrEnumerateSwapchainFormats(g.session, 0, &formatCount, nullptr);
	std::vector<int64_t> formats(formatCount);
	xrEnumerateSwapchainFormats(g.session, formatCount, &formatCount, formats.data());

	// UNORM before SRGB on purpose. The game writes colours that are already
	// sRGB-encoded, so an sRGB attachment would encode them a second time and
	// wash the whole image out. With UNORM the bytes reach the compositor
	// exactly as the game produced them.
	const int64_t preferred[] = {
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_SRGB,
		VK_FORMAT_B8G8R8A8_SRGB,
	};
	g.swapchainFormat = 0;
	for(int64_t want : preferred){
		for(int64_t have : formats)
			if(have == want){
				g.swapchainFormat = want;
				break;
			}
		if(g.swapchainFormat != 0)
			break;
	}
	if(g.swapchainFormat == 0){
		LOGE("no supported swapchain format");
		return false;
	}

	// One swapchain with two array layers, not two swapchains. Multiview reads
	// gl_ViewIndex to pick the layer, so both eyes come out of a single pass.
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = g.swapchainFormat;
	info.sampleCount = 1;
	info.width = g.renderWidth;
	info.height = g.renderHeight;
	info.faceCount = 1;
	info.arraySize = 2;
	info.mipCount = 1;
	XR_CHECK(xrCreateSwapchain(g.session, &info, &g.swapchain), "xrCreateSwapchain");

	uint32_t imageCount = 0;
	xrEnumerateSwapchainImages(g.swapchain, 0, &imageCount, nullptr);
	std::vector<XrSwapchainImageVulkan2KHR> xrImages(
		imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
	XR_CHECK(xrEnumerateSwapchainImages(
	             g.swapchain, imageCount, &imageCount,
	             (XrSwapchainImageBaseHeader *)xrImages.data()),
	         "xrEnumerateSwapchainImages");

	g.images.resize(imageCount);
	for(uint32_t i = 0; i < imageCount; i++)
		g.images[i].image = xrImages[i].image;
	LOGI("swapchain: %u images, format %lld", imageCount, (long long)g.swapchainFormat);

	// Debug overlay quad swapchain, same dimensions as the desktop panel.
	{
		g.debugWidth = 512;
		g.debugHeight = 128;
		XrSwapchainCreateInfo dinfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		dinfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
		                   XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		dinfo.format = g.swapchainFormat;
		dinfo.sampleCount = 1;
		dinfo.width = (uint32_t)g.debugWidth;
		dinfo.height = (uint32_t)g.debugHeight;
		dinfo.faceCount = 1;
		dinfo.arraySize = 1;
		dinfo.mipCount = 1;
		if(XR_SUCCEEDED(xrCreateSwapchain(g.session, &dinfo, &g.debugSwapchain))){
			uint32_t count = 0;
			xrEnumerateSwapchainImages(g.debugSwapchain, 0, &count, nullptr);
			std::vector<XrSwapchainImageVulkan2KHR> imgs(
				count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
			if(XR_SUCCEEDED(xrEnumerateSwapchainImages(
			       g.debugSwapchain, count, &count,
			       (XrSwapchainImageBaseHeader *)imgs.data()))){
				g.debugImages.resize(count);
				for(uint32_t i = 0; i < count; i++)
					g.debugImages[i] = imgs[i].image;
			}
		}else{
			LOGE("debug overlay swapchain unavailable");
			g.debugSwapchain = XR_NULL_HANDLE;
		}
	}
	return true;
}

bool
createDepthBuffer(void)
{
	const VkFormat candidates[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D16_UNORM,
	};
	bool chosen = false;
	for(VkFormat format : candidates){
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(g.physicalDevice, format, &properties);
		if(properties.optimalTilingFeatures &
		   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){
			g.depthFormat = format;
			chosen = true;
			break;
		}
	}
	if(!chosen){
		LOGE("no usable depth format");
		return false;
	}

	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = g.depthFormat;
	imageInfo.extent = { g.renderWidth, g.renderHeight, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 2;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK(vkCreateImage(g.device, &imageInfo, nullptr, &g.depthImage), "vkCreateImage(depth)");

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(g.device, g.depthImage, &requirements);
	uint32_t typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return false;

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	VK_CHECK(vkAllocateMemory(g.device, &allocInfo, nullptr, &g.depthMemory),
	         "vkAllocateMemory(depth)");
	VK_CHECK(vkBindImageMemory(g.device, g.depthImage, g.depthMemory, 0),
	         "vkBindImageMemory(depth)");

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = g.depthImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = g.depthFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 2;
	VK_CHECK(vkCreateImageView(g.device, &viewInfo, nullptr, &g.depthView),
	         "vkCreateImageView(depth)");
	return true;
}

bool
createRenderPass(void)
{
	VkAttachmentDescription attachments[2] = {};
	attachments[0].format = (VkFormat)g.swapchainFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].format = g.depthFormat;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	// Depth is never read after the pass, and discarding it saves the tiler a
	// full writeback of a two-layer depth surface every frame.
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colourRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colourRef;
	subpass.pDepthStencilAttachment = &depthRef;

	VkSubpassDependency dependency = {};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	// Both views in one subpass. The correlation mask tells the driver the two
	// views are spatially coherent, which is what lets the tiler share work.
	const uint32_t viewMask = 0x3;
	const uint32_t correlationMask = 0x3;
	VkRenderPassMultiviewCreateInfo multiview = {
		VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO };
	multiview.subpassCount = 1;
	multiview.pViewMasks = &viewMask;
	multiview.correlationMaskCount = 1;
	multiview.pCorrelationMasks = &correlationMask;

	VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	info.pNext = &multiview;
	info.attachmentCount = 2;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 1;
	info.pDependencies = &dependency;
	VK_CHECK(vkCreateRenderPass(g.device, &info, nullptr, &g.renderPass), "vkCreateRenderPass");
	return true;
}

bool
createPipeline(void)
{
	VkDescriptorSetLayoutBinding uboBinding = {};
	uboBinding.binding = 0;
	uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	uboBinding.descriptorCount = 1;
	uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = &uboBinding;
	VK_CHECK(vkCreateDescriptorSetLayout(g.device, &layoutInfo, nullptr,
	                                     &g.descriptorSetLayout),
	         "vkCreateDescriptorSetLayout");

	VkPushConstantRange pushRange = {};
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(PushConstants);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &g.descriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	VK_CHECK(vkCreatePipelineLayout(g.device, &pipelineLayoutInfo, nullptr,
	                                &g.pipelineLayout),
	         "vkCreatePipelineLayout");

	VkShaderModule vertexModule = createShaderModule(kBringupVertSpv, sizeof(kBringupVertSpv));
	VkShaderModule fragmentModule = createShaderModule(kBringupFragSpv, sizeof(kBringupFragSpv));
	if(vertexModule == VK_NULL_HANDLE || fragmentModule == VK_NULL_HANDLE)
		return false;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertexModule;
	stages[0].pName = "main";
	stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragmentModule;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding = { 0, sizeof(Vertex),
	                                            VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attributes[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, position) },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, normal) },
	};

	VkPipelineVertexInputStateCreateInfo vertexInput = {
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = 2;
	vertexInput.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState = {
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasteriser = {
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
	rasteriser.cullMode = VK_CULL_MODE_BACK_BIT;
	// Vulkan's flipped Y makes the winding of the source geometry come out
	// clockwise on screen.
	rasteriser.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rasteriser.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = {
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil = {
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo colourBlend = {
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colourBlend.attachmentCount = 1;
	colourBlend.pAttachments = &blendAttachment;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT,
	                                         VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {
		VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState = &viewportState;
	info.pRasterizationState = &rasteriser;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depthStencil;
	info.pColorBlendState = &colourBlend;
	info.pDynamicState = &dynamicState;
	info.layout = g.pipelineLayout;
	info.renderPass = g.renderPass;
	info.subpass = 0;

	VkResult result = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &info,
	                                            nullptr, &g.pipeline);
	vkDestroyShaderModule(g.device, vertexModule, nullptr);
	vkDestroyShaderModule(g.device, fragmentModule, nullptr);
	VK_CHECK(result, "vkCreateGraphicsPipelines");
	return true;
}

bool
createPerImageResources(void)
{
	const uint32_t imageCount = (uint32_t)g.images.size();

	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = g.queueFamily;
	VK_CHECK(vkCreateCommandPool(g.device, &poolInfo, nullptr, &g.commandPool),
	         "vkCreateCommandPool");

	VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount };
	VkDescriptorPoolCreateInfo descriptorPoolInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	descriptorPoolInfo.maxSets = imageCount;
	descriptorPoolInfo.poolSizeCount = 1;
	descriptorPoolInfo.pPoolSizes = &poolSize;
	VK_CHECK(vkCreateDescriptorPool(g.device, &descriptorPoolInfo, nullptr,
	                                &g.descriptorPool),
	         "vkCreateDescriptorPool");

	for(uint32_t i = 0; i < imageCount; i++){
		SwapchainImage &image = g.images[i];

		VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image = image.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		viewInfo.format = (VkFormat)g.swapchainFormat;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 2;
		VK_CHECK(vkCreateImageView(g.device, &viewInfo, nullptr, &image.view),
		         "vkCreateImageView(colour)");

		const VkImageView attachments[2] = { image.view, g.depthView };
		VkFramebufferCreateInfo framebufferInfo = {
			VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebufferInfo.renderPass = g.renderPass;
		framebufferInfo.attachmentCount = 2;
		framebufferInfo.pAttachments = attachments;
		framebufferInfo.width = g.renderWidth;
		framebufferInfo.height = g.renderHeight;
		// Multiview drives the layer count through the view mask, so the
		// framebuffer itself is declared single-layer.
		framebufferInfo.layers = 1;
		VK_CHECK(vkCreateFramebuffer(g.device, &framebufferInfo, nullptr,
		                             &image.framebuffer),
		         "vkCreateFramebuffer");

		VkCommandBufferAllocateInfo commandInfo = {
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		commandInfo.commandPool = g.commandPool;
		commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandInfo.commandBufferCount = 1;
		VK_CHECK(vkAllocateCommandBuffers(g.device, &commandInfo, &image.commandBuffer),
		         "vkAllocateCommandBuffers");

		VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VK_CHECK(vkCreateFence(g.device, &fenceInfo, nullptr, &image.fence),
		         "vkCreateFence");

		if(!createBuffer(sizeof(Mat4) * 2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &image.uniformBuffer, &image.uniformMemory))
			return false;
		VK_CHECK(vkMapMemory(g.device, image.uniformMemory, 0, sizeof(Mat4) * 2, 0,
		                     &image.uniformMapped),
		         "vkMapMemory(uniform)");

		VkDescriptorSetAllocateInfo setInfo = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		setInfo.descriptorPool = g.descriptorPool;
		setInfo.descriptorSetCount = 1;
		setInfo.pSetLayouts = &g.descriptorSetLayout;
		VK_CHECK(vkAllocateDescriptorSets(g.device, &setInfo, &image.descriptorSet),
		         "vkAllocateDescriptorSets");

		VkDescriptorBufferInfo bufferInfo = { image.uniformBuffer, 0, sizeof(Mat4) * 2 };
		VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = image.descriptorSet;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &bufferInfo;
		vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void
drawScene(VkCommandBuffer commandBuffer)
{
	struct Instance { float x, y, z, sx, sy, sz, r, gr, b; };
	// A floor slab, a marker directly ahead, and a ring the head can be turned
	// through. Enough to read tracking, stereo separation and depth by eye.
	static const Instance instances[] = {
		{  0.0f, -0.05f,  0.0f, 8.0f, 0.1f, 8.0f, 0.16f, 0.17f, 0.22f },
		{  0.0f,  1.2f,  -2.0f, 0.3f, 0.3f, 0.3f, 0.95f, 0.35f, 0.55f },
		{  2.0f,  0.5f,   0.0f, 0.4f, 1.0f, 0.4f, 0.25f, 0.70f, 0.95f },
		{ -2.0f,  0.5f,   0.0f, 0.4f, 1.0f, 0.4f, 0.25f, 0.70f, 0.95f },
		{  0.0f,  0.5f,   2.0f, 0.4f, 1.0f, 0.4f, 0.95f, 0.75f, 0.25f },
		{  1.4f,  0.5f,  -1.4f, 0.4f, 1.0f, 0.4f, 0.40f, 0.90f, 0.45f },
		{ -1.4f,  0.5f,  -1.4f, 0.4f, 1.0f, 0.4f, 0.40f, 0.90f, 0.45f },
		{  1.4f,  0.5f,   1.4f, 0.4f, 1.0f, 0.4f, 0.85f, 0.85f, 0.35f },
		{ -1.4f,  0.5f,   1.4f, 0.4f, 1.0f, 0.4f, 0.85f, 0.85f, 0.35f },
	};

	for(const Instance &instance : instances){
		PushConstants push;
		push.model = translationScale(instance.x, instance.y, instance.z,
		                              instance.sx, instance.sy, instance.sz);
		push.tint[0] = instance.r;
		push.tint[1] = instance.gr;
		push.tint[2] = instance.b;
		push.tint[3] = 1.0f;
		vkCmdPushConstants(commandBuffer, g.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
		                   0, sizeof(push), &push);
		vkCmdDrawIndexed(commandBuffer,
		                 (uint32_t)(sizeof(kCubeIndices)/sizeof(kCubeIndices[0])),
		                 1, 0, 0, 0);
	}
}

// Returns false when nothing was rendered. The caller must not submit a
// projection layer in that case: claiming a layer whose swapchain image was
// never acquired makes the compositor drop the frame, and Horizon OS replaces
// the app with its "still running" panel.
bool
renderLayer(const XrView views[2])
{
	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	const XrResult acquired = xrAcquireSwapchainImage(g.swapchain, &acquireInfo,
	                                                  &imageIndex);
	if(XR_FAILED(acquired)){
		LOGE("xrAcquireSwapchainImage failed: %d", (int)acquired);
		return false;
	}

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	if(XR_FAILED(xrWaitSwapchainImage(g.swapchain, &waitInfo))){
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(g.swapchain, &releaseInfo);
		LOGE("xrWaitSwapchainImage failed");
		return false;
	}

	SwapchainImage &image = g.images[imageIndex];

	Mat4 viewProj[2];
	for(int eye = 0; eye < 2; eye++){
		const Mat4 projection = projectionFromFov(views[eye].fov, 0.05f, 1000.0f);
		const Mat4 viewMatrix = viewFromPose(views[eye].pose);
		viewProj[eye] = multiply(projection, viewMatrix);
	}

	// With a renderer installed the game owns the frame: it records and submits
	// through librw's Vulkan backend, so none of the bring-up scene's command
	// buffer, fence or descriptor state is touched.
	if(g.frameRenderer != nullptr){
		float matrices[2][16];
		memcpy(matrices[0], viewProj[0].m, sizeof(matrices[0]));
		memcpy(matrices[1], viewProj[1].m, sizeof(matrices[1]));

		// Head pose: the midpoint between the eyes, with the left eye's
		// orientation. Anchoring 2D content to one eye instead would make the
		// panel swing as the head rolls.
		XrPosef head = views[0].pose;
		head.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
		head.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
		head.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;

		// The plane has to subtend exactly what the game projected with, or a
		// screen coordinate maps to the wrong direction and world sprites
		// drift as the player moves. viewWindow holds the tangents of the
		// half angles, so the full width at distance d is 2*d*viewWindow.x.
		float viewWindowX = 0.6f, viewWindowY = 0.6f;
		androidgame::GetIm2DViewWindow(&viewWindowX, &viewWindowY);
		const float im2dDistance = 2.0f;
		const Mat4 im2dWorld = im2dPlane(head, (float)g.renderWidth,
		                                 (float)g.renderHeight, im2dDistance,
		                                 2.0f*im2dDistance*viewWindowX,
		                                 2.0f*im2dDistance*viewWindowY);
		const float headPos[3] = {
			head.position.x, head.position.y, head.position.z
		};
		// Head yaw: where the head's forward (-Z rotated by the orientation)
		// lands in the horizontal plane. atan2(-fx, -fz) is 0 facing -Z and
		// grows turning left, matching a right-handed rotation about +Y.
		const XrQuaternionf &hq = head.orientation;
		const float fx = -(2.0f*(hq.x*hq.z + hq.y*hq.w));
		const float fz = -(1.0f - 2.0f*(hq.x*hq.x + hq.y*hq.y));
		const float headYaw = atan2f(-fx, -fz);
		const float headQuat[4] = { hq.x, hq.y, hq.z, hq.w };
		// The eye fov drives the game's sprite mathematics, mirroring the
		// desktop BeginEye handing the game an eye fov: coronas and other
		// screen-space effects size themselves from it.
		const float eyeFovDeg =
			(views[0].fov.angleRight - views[0].fov.angleLeft) * 57.29578f;
		g.frameRenderer(image.image, image.view, matrices, im2dWorld.m,
		                im2dDistance, headPos, headYaw, headQuat, eyeFovDeg);

		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(g.swapchain, &releaseInfo);
		return true;
	}

	vkWaitForFences(g.device, 1, &image.fence, VK_TRUE, UINT64_MAX);
	vkResetFences(g.device, 1, &image.fence);
	memcpy(image.uniformMapped, viewProj, sizeof(viewProj));

	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(image.commandBuffer, 0);
	vkBeginCommandBuffer(image.commandBuffer, &beginInfo);

	VkClearValue clears[2] = {};
	clears[0].color = { { 0.05f, 0.06f, 0.10f, 1.0f } };
	clears[1].depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo passInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	passInfo.renderPass = g.renderPass;
	passInfo.framebuffer = image.framebuffer;
	passInfo.renderArea.extent = { g.renderWidth, g.renderHeight };
	passInfo.clearValueCount = 2;
	passInfo.pClearValues = clears;
	vkCmdBeginRenderPass(image.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport = { 0.0f, 0.0f, (float)g.renderWidth, (float)g.renderHeight,
	                        0.0f, 1.0f };
	VkRect2D scissor = { { 0, 0 }, { g.renderWidth, g.renderHeight } };
	vkCmdSetViewport(image.commandBuffer, 0, 1, &viewport);
	vkCmdSetScissor(image.commandBuffer, 0, 1, &scissor);

	vkCmdBindPipeline(image.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
	vkCmdBindDescriptorSets(image.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        g.pipelineLayout, 0, 1, &image.descriptorSet, 0, nullptr);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(image.commandBuffer, 0, 1, &g.vertexBuffer, &offset);
	vkCmdBindIndexBuffer(image.commandBuffer, g.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

	drawScene(image.commandBuffer);

	vkCmdEndRenderPass(image.commandBuffer);
	vkEndCommandBuffer(image.commandBuffer);

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &image.commandBuffer;
	vkQueueSubmit(g.queue, 1, &submit, image.fence);

	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(g.swapchain, &releaseInfo);
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool
create(android_app *app)
{
	if(!initialiseLoader(app)) return false;
	if(!createInstance(app)) return false;
	if(!createVulkan()) return false;
	if(!createSession()) return false;
	// Action sets must be attached before the first xrSyncActions and can
	// never be attached twice, so this belongs here rather than in the frame
	// loop.
	if(!createActions()) return false;
	if(!createSwapchain()) return false;
	if(!createDepthBuffer()) return false;
	if(!createRenderPass()) return false;
	if(!createPipeline()) return false;
	if(!createPerImageResources()) return false;

	if(!uploadThroughHostBuffer(kCubeVertices, sizeof(kCubeVertices),
	                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                            &g.vertexBuffer, &g.vertexMemory))
		return false;
	if(!uploadThroughHostBuffer(kCubeIndices, sizeof(kCubeIndices),
	                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                            &g.indexBuffer, &g.indexMemory))
		return false;

	LOGI("OpenXR/Vulkan session ready");
	return true;
}

bool
getContext(GraphicsContext *out)
{
	if(out == nullptr || g.device == VK_NULL_HANDLE)
		return false;
	out->instance = g.vkInstance;
	out->physicalDevice = g.physicalDevice;
	out->device = g.device;
	out->queue = g.queue;
	out->queueFamilyIndex = g.queueFamily;
	out->width = g.renderWidth;
	out->height = g.renderHeight;
	out->viewCount = 2;
	out->colourFormat = (VkFormat)g.swapchainFormat;
	return true;
}

long long
getPredictedDisplayTimeNs(void)
{
	return g.lastPredictedDisplayTimeNs;
}

void
setDebugOverlay(const unsigned char *rgba)
{
	g.debugVisible = rgba != nullptr;
	if(rgba == nullptr || g.debugSwapchain == XR_NULL_HANDLE ||
	   g.commandPool == VK_NULL_HANDLE)
		return;

	const VkDeviceSize size =
		(VkDeviceSize)g.debugWidth * g.debugHeight * 4;
	if(g.debugStaging == VK_NULL_HANDLE){
		if(!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &g.debugStaging, &g.debugStagingMemory))
			return;
		if(vkMapMemory(g.device, g.debugStagingMemory, 0, size, 0,
		               &g.debugStagingMapped) != VK_SUCCESS)
			return;

		VkCommandBufferAllocateInfo commandInfo = {};
		commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandInfo.commandPool = g.commandPool;
		commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandInfo.commandBufferCount = 1;
		vkAllocateCommandBuffers(g.device, &commandInfo, &g.debugCommand);
		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		vkCreateFence(g.device, &fenceInfo, nullptr, &g.debugFence);
	}
	if(g.debugStagingMapped == nullptr || g.debugCommand == VK_NULL_HANDLE)
		return;
	memcpy(g.debugStagingMapped, rgba, (size_t)size);

	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if(XR_FAILED(xrAcquireSwapchainImage(g.debugSwapchain, &acquireInfo, &imageIndex)))
		return;
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	if(XR_FAILED(xrWaitSwapchainImage(g.debugSwapchain, &waitInfo))){
		xrReleaseSwapchainImage(g.debugSwapchain, &releaseInfo);
		return;
	}

	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(g.debugCommand, 0);
	vkBeginCommandBuffer(g.debugCommand, &begin);

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = g.debugImages[imageIndex];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(g.debugCommand, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
	                     0, nullptr, 1, &barrier);

	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = (uint32_t)g.debugWidth;
	region.imageExtent.height = (uint32_t)g.debugHeight;
	region.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(g.debugCommand, g.debugStaging,
	                       g.debugImages[imageIndex],
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(g.debugCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
	                     0, nullptr, 1, &barrier);

	vkEndCommandBuffer(g.debugCommand);
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &g.debugCommand;
	vkResetFences(g.device, 1, &g.debugFence);
	vkQueueSubmit(g.queue, 1, &submit, g.debugFence);
	vkWaitForFences(g.device, 1, &g.debugFence, VK_TRUE, UINT64_MAX);

	xrReleaseSwapchainImage(g.debugSwapchain, &releaseInfo);
}

void
setFrameRenderer(FrameRenderer renderer)
{
	g.frameRenderer = renderer;
}

void
getInput(ControllerInput *out)
{
	if(out != nullptr)
		*out = g.input;
}

bool
pollEvents(void)
{
	XrEventDataBuffer event;
	for(;;){
		event = { XR_TYPE_EVENT_DATA_BUFFER };
		if(xrPollEvent(g.instance, &event) != XR_SUCCESS)
			break;

		switch(event.type){
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			const XrEventDataSessionStateChanged &changed =
				*(const XrEventDataSessionStateChanged *)&event;
			g.sessionState = changed.state;
			LOGI("session state -> %d", (int)g.sessionState);

			if(g.sessionState == XR_SESSION_STATE_READY){
				XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType =
					XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if(XR_SUCCEEDED(xrBeginSession(g.session, &beginInfo))){
					g.running = true;
					// A Quest app runs at 72 Hz unless it asks for more --
					// the system setting alone does not raise it. Take the
					// highest rate the runtime offers, which is what the
					// headset's own setting caps.
					if(g.hasRefreshRateExt){
						PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
						PFN_xrRequestDisplayRefreshRateFB request = nullptr;
						xrGetInstanceProcAddr(g.instance,
							"xrEnumerateDisplayRefreshRatesFB",
							(PFN_xrVoidFunction *)&enumerate);
						xrGetInstanceProcAddr(g.instance,
							"xrRequestDisplayRefreshRateFB",
							(PFN_xrVoidFunction *)&request);
						if(enumerate != nullptr && request != nullptr){
							uint32_t count = 0;
							enumerate(g.session, 0, &count, nullptr);
							std::vector<float> rates(count);
							if(count > 0 &&
							   XR_SUCCEEDED(enumerate(g.session, count, &count,
							                          rates.data()))){
								float best = 0.0f;
								for(float rate : rates)
									if(rate > best)
										best = rate;
								if(best > 0.0f){
									const XrResult result =
										request(g.session, best);
									LOGI("display refresh: requested %.0f Hz "
									     "of %u offered, result %d",
									     best, count, (int)result);
								}
							}
						}
					}
				}
			}else if(g.sessionState == XR_SESSION_STATE_STOPPING){
				g.running = false;
				xrEndSession(g.session);
			}else if(g.sessionState == XR_SESSION_STATE_EXITING ||
			         g.sessionState == XR_SESSION_STATE_LOSS_PENDING){
				g.running = false;
				g.exitRequested = true;
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			g.exitRequested = true;
			break;
		default:
			break;
		}
	}
	return !g.exitRequested;
}

bool
shouldRender(void)
{
	return g.running;
}

void
renderFrame(void)
{
	if(!g.running)
		return;

	XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frameState = { XR_TYPE_FRAME_STATE };
	if(XR_FAILED(xrWaitFrame(g.session, &waitInfo, &frameState)))
		return;
	g.predictedDisplayTime = frameState.predictedDisplayTime;
	g.lastPredictedDisplayTimeNs = (long long)frameState.predictedDisplayTime;

	XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
	if(XR_FAILED(xrBeginFrame(g.session, &beginInfo)))
		return;

	// Sampled once per frame, after the wait, so the state the game reads is
	// the freshest available for this display time.
	syncInput();

	XrCompositionLayerProjectionView projectionViews[2] = {};
	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad debugLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader *layers[2] = { nullptr, nullptr };
	uint32_t layerCount = 0;

	if(frameState.shouldRender){
		XrViewState viewState = { XR_TYPE_VIEW_STATE };
		XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = frameState.predictedDisplayTime;
		locateInfo.space = g.space;

		XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
		uint32_t viewCount = 0;
		XrResult located = xrLocateViews(g.session, &locateInfo, &viewState,
		                                 2, &viewCount, views);

		// Fall back to LOCAL when the chosen space is not reporting an
		// orientation. STAGE stops doing so without a configured play area,
		// and refusing to render then is what left the compositor with nothing
		// to show but its placeholder panel.
		XrSpace submitSpace = g.space;
		if((XR_FAILED(located) ||
		    (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) &&
		   g.localSpace != XR_NULL_HANDLE){
			locateInfo.space = g.localSpace;
			located = xrLocateViews(g.session, &locateInfo, &viewState,
			                        2, &viewCount, views);
			submitSpace = g.localSpace;
		}

		// Only orientation is required. A missing position means the runtime
		// could not place the head in the world, which is worth rendering
		// through with a stale position rather than dropping the frame.
		static XrViewStateFlags reportedFlags = ~(XrViewStateFlags)0;
		if(viewState.viewStateFlags != reportedFlags){
			reportedFlags = viewState.viewStateFlags;
			LOGI("view state flags 0x%llx", (unsigned long long)reportedFlags);
		}

		if(XR_SUCCEEDED(located) &&
		   (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
		   renderLayer(views)){

			for(int eye = 0; eye < 2; eye++){
				projectionViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
				projectionViews[eye].pose = views[eye].pose;
				projectionViews[eye].fov = views[eye].fov;
				projectionViews[eye].subImage.swapchain = g.swapchain;
				projectionViews[eye].subImage.imageRect.extent = {
					(int32_t)g.renderWidth, (int32_t)g.renderHeight };
				// Both eyes share one swapchain; the array index is what
				// separates them, matching the multiview layer.
				projectionViews[eye].subImage.imageArrayIndex = (uint32_t)eye;
			}

			layer.space = submitSpace;
			layer.viewCount = 2;
			layer.views = projectionViews;
			layers[0] = (const XrCompositionLayerBaseHeader *)&layer;
			layerCount = 1;

			// Head-locked debug overlay: pose and size are the desktop's.
			if(g.debugVisible && g.debugSwapchain != XR_NULL_HANDLE &&
			   g.viewSpace != XR_NULL_HANDLE){
				debugLayer.layerFlags =
					XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				debugLayer.space = g.viewSpace;
				debugLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				debugLayer.subImage.swapchain = g.debugSwapchain;
				debugLayer.subImage.imageRect.extent = {
					(int32_t)g.debugWidth, (int32_t)g.debugHeight };
				debugLayer.pose.orientation.w = 1.0f;
				debugLayer.pose.position.y = -0.24f;
				debugLayer.pose.position.z = -1.5f;
				debugLayer.size.width = 1.2f;
				debugLayer.size.height =
					1.2f*(float)g.debugHeight/(float)g.debugWidth;
				layers[layerCount++] =
					(const XrCompositionLayerBaseHeader *)&debugLayer;
			}
		}
	}

	// An empty frame is what makes Horizon OS drop the app back to its "still
	// running" panel, so say the first few times it happens and why, instead of
	// letting it pass silently.
	if(layerCount == 0){
		static int reportedEmpty = 0;
		if(reportedEmpty < 5){
			reportedEmpty++;
			LOGE("empty frame submitted (shouldRender=%d): the compositor will "
			     "show the placeholder panel", (int)frameState.shouldRender);
		}
	}

	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = layerCount;
	endInfo.layers = layers;
	const XrResult ended = xrEndFrame(g.session, &endInfo);
	if(XR_FAILED(ended)){
		static int reportedEnd = 0;
		if(reportedEnd < 5){
			reportedEnd++;
			LOGE("xrEndFrame failed: %d", (int)ended);
		}
	}
}

void
destroy(void)
{
	if(g.device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(g.device);

	for(SwapchainImage &image : g.images){
		if(image.fence) vkDestroyFence(g.device, image.fence, nullptr);
		if(image.framebuffer) vkDestroyFramebuffer(g.device, image.framebuffer, nullptr);
		if(image.view) vkDestroyImageView(g.device, image.view, nullptr);
		if(image.uniformMemory){
			vkUnmapMemory(g.device, image.uniformMemory);
			vkFreeMemory(g.device, image.uniformMemory, nullptr);
		}
		if(image.uniformBuffer) vkDestroyBuffer(g.device, image.uniformBuffer, nullptr);
	}
	g.images.clear();

	if(g.vertexBuffer) vkDestroyBuffer(g.device, g.vertexBuffer, nullptr);
	if(g.vertexMemory) vkFreeMemory(g.device, g.vertexMemory, nullptr);
	if(g.indexBuffer) vkDestroyBuffer(g.device, g.indexBuffer, nullptr);
	if(g.indexMemory) vkFreeMemory(g.device, g.indexMemory, nullptr);
	if(g.depthView) vkDestroyImageView(g.device, g.depthView, nullptr);
	if(g.depthImage) vkDestroyImage(g.device, g.depthImage, nullptr);
	if(g.depthMemory) vkFreeMemory(g.device, g.depthMemory, nullptr);
	if(g.pipeline) vkDestroyPipeline(g.device, g.pipeline, nullptr);
	if(g.pipelineLayout) vkDestroyPipelineLayout(g.device, g.pipelineLayout, nullptr);
	if(g.descriptorPool) vkDestroyDescriptorPool(g.device, g.descriptorPool, nullptr);
	if(g.descriptorSetLayout)
		vkDestroyDescriptorSetLayout(g.device, g.descriptorSetLayout, nullptr);
	if(g.renderPass) vkDestroyRenderPass(g.device, g.renderPass, nullptr);
	if(g.commandPool) vkDestroyCommandPool(g.device, g.commandPool, nullptr);

	if(g.debugFence) vkDestroyFence(g.device, g.debugFence, nullptr);
	if(g.debugStagingMapped) vkUnmapMemory(g.device, g.debugStagingMemory);
	if(g.debugStaging) vkDestroyBuffer(g.device, g.debugStaging, nullptr);
	if(g.debugStagingMemory) vkFreeMemory(g.device, g.debugStagingMemory, nullptr);

	if(g.debugSwapchain) xrDestroySwapchain(g.debugSwapchain);
	if(g.swapchain) xrDestroySwapchain(g.swapchain);
	if(g.viewSpace) xrDestroySpace(g.viewSpace);
	if(g.localSpace) xrDestroySpace(g.localSpace);
	if(g.space) xrDestroySpace(g.space);
	if(g.session) xrDestroySession(g.session);
	if(g.device) vkDestroyDevice(g.device, nullptr);
	if(g.vkInstance) vkDestroyInstance(g.vkInstance, nullptr);
	if(g.instance) xrDestroyInstance(g.instance);

	g = State();
}

} // namespace xrvk
