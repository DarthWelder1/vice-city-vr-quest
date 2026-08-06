#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwvk.h"
#include "rwvkimpl.h"

#define PLUGIN_ID ID_DRIVER

#ifdef RW_VULKAN
#include <android/log.h>
#define VKLOG(...) __android_log_print(ANDROID_LOG_INFO, "librw-vk", __VA_ARGS__)
#define VKERR(...) __android_log_print(ANDROID_LOG_ERROR, "librw-vk", __VA_ARGS__)
#endif

namespace rw {
namespace vulkan {

#ifdef RW_VULKAN

Globals gvk;
RenderState gstate;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

bool32
findMemoryType(uint32 typeBits, VkMemoryPropertyFlags properties, uint32 *indexOut)
{
	for(uint32 i = 0; i < gvk.memoryProperties.memoryTypeCount; i++){
		if((typeBits & (1u << i)) == 0)
			continue;
		if((gvk.memoryProperties.memoryTypes[i].propertyFlags & properties) == properties){
			*indexOut = i;
			return 1;
		}
	}
	VKERR("no memory type for bits 0x%x properties 0x%x", typeBits, properties);
	return 0;
}

bool32
createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
             VkMemoryPropertyFlags properties, VkBuffer *bufferOut,
             VkDeviceMemory *memoryOut)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if(vkCreateBuffer(gvk.device, &bufferInfo, nil, bufferOut) != VK_SUCCESS)
		return 0;

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(gvk.device, *bufferOut, &requirements);

	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits, properties, &typeIndex)){
		vkDestroyBuffer(gvk.device, *bufferOut, nil);
		*bufferOut = VK_NULL_HANDLE;
		return 0;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, memoryOut) != VK_SUCCESS){
		vkDestroyBuffer(gvk.device, *bufferOut, nil);
		*bufferOut = VK_NULL_HANDLE;
		return 0;
	}
	vkBindBufferMemory(gvk.device, *bufferOut, *memoryOut, 0);
	return 1;
}

VkCommandBuffer
beginOneShot(void)
{
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = gvk.commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if(vkAllocateCommandBuffers(gvk.device, &allocInfo, &commandBuffer) != VK_SUCCESS)
		return VK_NULL_HANDLE;

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);
	return commandBuffer;
}

void
endOneShot(VkCommandBuffer commandBuffer)
{
	if(commandBuffer == VK_NULL_HANDLE)
		return;
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &commandBuffer;
	vkQueueSubmit(gvk.queue, 1, &submit, VK_NULL_HANDLE);
	// Texture uploads are rare relative to draws and the streaming code already
	// spreads them across frames, so a queue wait here is acceptable and keeps
	// staging lifetime trivial.
	vkQueueWaitIdle(gvk.queue);
	vkFreeCommandBuffers(gvk.device, gvk.commandPool, 1, &commandBuffer);
}

void
transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image,
                      VkImageAspectFlags aspect, uint32 levelCount,
                      VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.levelCount = levelCount;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

	if(from == VK_IMAGE_LAYOUT_UNDEFINED &&
	   to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL){
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	}else if(from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
	         to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}else if(from == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
	         to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL){
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}

	vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
	                     0, nil, 0, nil, 1, &barrier);
}

bool32
deviceSupportsBC(void)
{
	return gvk.supportsBC;
}

// ---------------------------------------------------------------------------
// Attachments
// ---------------------------------------------------------------------------

static bool32
createDepthBuffer(void)
{
	static const VkFormat candidates[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D16_UNORM,
	};
	gvk.depthFormat = VK_FORMAT_UNDEFINED;
	for(uint32 i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++){
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(gvk.physicalDevice, candidates[i],
		                                    &properties);
		if(properties.optimalTilingFeatures &
		   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){
			gvk.depthFormat = candidates[i];
			break;
		}
	}
	if(gvk.depthFormat == VK_FORMAT_UNDEFINED){
		VKERR("no depth format supported");
		return 0;
	}

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = gvk.depthFormat;
	imageInfo.extent.width = gvk.width;
	imageInfo.extent.height = gvk.height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil, &gvk.depthImage) != VK_SUCCESS)
		return 0;

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, gvk.depthImage, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return 0;

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, &gvk.depthMemory) != VK_SUCCESS)
		return 0;
	vkBindImageMemory(gvk.device, gvk.depthImage, gvk.depthMemory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = gvk.depthImage;
	viewInfo.viewType = gvk.viewCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
	                                      : VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = gvk.depthFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	return vkCreateImageView(gvk.device, &viewInfo, nil, &gvk.depthView) == VK_SUCCESS;
}

static bool32
createRenderPass(void)
{
	VkAttachmentDescription attachments[2] = {};
	attachments[0].format = gvk.colourFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].format = gvk.depthFormat;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	// Nothing reads depth after the pass. Discarding it spares the tiler a
	// writeback of a full two-layer depth surface every single frame, which is
	// one of the larger easy wins on a mobile GPU.
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colourRef = {};
	colourRef.attachment = 0;
	colourRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference depthRef = {};
	depthRef.attachment = 1;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colourRef;
	subpass.pDepthStencilAttachment = &depthRef;

	const uint32 viewMask = gvk.viewCount > 1 ? 0x3u : 0x1u;
	const uint32 correlationMask = viewMask;
	VkRenderPassMultiviewCreateInfo multiview = {};
	multiview.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
	multiview.subpassCount = 1;
	multiview.pViewMasks = &viewMask;
	multiview.correlationMaskCount = 1;
	multiview.pCorrelationMasks = &correlationMask;

	VkRenderPassCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.pNext = gvk.viewCount > 1 ? &multiview : nil;
	info.attachmentCount = 2;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	return vkCreateRenderPass(gvk.device, &info, nil, &gvk.renderPass) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

bool32
beginFrame(VkImage colourImage, VkImageView colourView)
{
	if(!gvk.initialised || gvk.inFrame)
		return 0;
	(void)colourImage;

	// Anything a previous frame had to defer is built here, before a render
	// pass is open. Compiling inside one does not return on this driver.
	compilePendingPipelines();

	// The colour view changes every frame because it comes from whichever
	// OpenXR swapchain image was acquired, so the framebuffer is transient.
	if(gvk.framebuffer != VK_NULL_HANDLE){
		vkDestroyFramebuffer(gvk.device, gvk.framebuffer, nil);
		gvk.framebuffer = VK_NULL_HANDLE;
	}
	const VkImageView attachments[2] = { colourView, gvk.depthView };
	VkFramebufferCreateInfo framebufferInfo = {};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass = gvk.renderPass;
	framebufferInfo.attachmentCount = 2;
	framebufferInfo.pAttachments = attachments;
	framebufferInfo.width = gvk.width;
	framebufferInfo.height = gvk.height;
	// Multiview derives the layer count from the view mask, so the framebuffer
	// is declared single-layer even though the attachments have two.
	framebufferInfo.layers = 1;
	if(vkCreateFramebuffer(gvk.device, &framebufferInfo, nil,
	                       &gvk.framebuffer) != VK_SUCCESS)
		return 0;

	vkWaitForFences(gvk.device, 1, &gvk.frameFence, VK_TRUE, UINT64_MAX);
	vkResetFences(gvk.device, 1, &gvk.frameFence);
	vkResetCommandBuffer(gvk.frameCommands, 0);

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(gvk.frameCommands, &beginInfo);

	// Safe to rewind the immediate-mode allocator here: the fence wait above
	// already guarantees the previous frame finished reading it.
	resetDynamic();
	uploadSceneData();

	// Open the multiview pass for the whole frame. Every draw the game issues
	// has to land inside a render pass; without this they are recorded outside
	// one and the driver faults on the first vkCmdDraw.
	// The clear colour doubles as the sky. Vice City paints its sky as
	// full-screen 2D gradient quads, which on the headset land on the floating
	// panel as a grey glass pane mid-view; the Quest build skips those quads
	// and clears the whole view to the timecycle colour instead.
	VkClearValue clears[2];
	memset(clears, 0, sizeof(clears));
	clears[0].color.float32[0] = gvk.clearColour[0];
	clears[0].color.float32[1] = gvk.clearColour[1];
	clears[0].color.float32[2] = gvk.clearColour[2];
	clears[0].color.float32[3] = 1.0f;
	clears[1].depthStencil.depth = 1.0f;

	VkRenderPassBeginInfo passInfo = {};
	passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	passInfo.renderPass = gvk.renderPass;
	passInfo.framebuffer = gvk.framebuffer;
	passInfo.renderArea.extent.width = gvk.width;
	passInfo.renderArea.extent.height = gvk.height;
	passInfo.clearValueCount = 2;
	passInfo.pClearValues = clears;
	vkCmdBeginRenderPass(gvk.frameCommands, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport = {};
	viewport.width = (float32)gvk.width;
	viewport.height = (float32)gvk.height;
	viewport.maxDepth = 1.0f;
	VkRect2D scissor = {};
	scissor.extent.width = gvk.width;
	scissor.extent.height = gvk.height;
	vkCmdSetViewport(gvk.frameCommands, 0, 1, &viewport);
	vkCmdSetScissor(gvk.frameCommands, 0, 1, &scissor);

	gvk.inFrame = 1;
	return 1;
}

void
endFrame(void)
{
	if(!gvk.inFrame)
		return;
	reportImStats();
	vkCmdEndRenderPass(gvk.frameCommands);
	vkEndCommandBuffer(gvk.frameCommands);

	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &gvk.frameCommands;
	vkQueueSubmit(gvk.queue, 1, &submit, gvk.frameFence);
	gvk.inFrame = 0;
}

void
multiplyMatrix(float32 out[16], const float32 a[16], const float32 b[16])
{
	for(int32 col = 0; col < 4; col++)
		for(int32 row = 0; row < 4; row++)
			out[col*4 + row] =
				a[0*4 + row]*b[col*4 + 0] +
				a[1*4 + row]*b[col*4 + 1] +
				a[2*4 + row]*b[col*4 + 2] +
				a[3*4 + row]*b[col*4 + 3];
}

void
setStereoViewProjection(const float32 left[16], const float32 right[16])
{
	memcpy(gvk.stereoViewProjection[0], left, sizeof(float32)*16);
	memcpy(gvk.stereoViewProjection[1], right, sizeof(float32)*16);

	SceneData *scene = getSceneData();
	memcpy(scene->viewProj[0], left, sizeof(float32)*16);
	memcpy(scene->viewProj[1], right, sizeof(float32)*16);
}

// Plants play space on the game camera, so world geometry reaches clip space
// through the same viewpoint the flat game would use.
//
// Two conventions meet here. OpenXR reports the head in play space: metres,
// Y up, looking down -Z, right handed. RenderWare has the world Z up with a
// mirrored X -- the same flip the D3D backends bake into their view matrix
// (see beginUpdate in d3d12device.cpp) and the same one the desktop VR layer
// applies in ToGameVector. So the play space basis, written in world space, is
//
//     x -> -right    y -> up    z -> -at    origin -> pos
//
// and world geometry is transformed by the inverse of that before the per-eye
// matrices are applied. The head pose then rides on top of the game camera
// instead of replacing it.
//
// Skipping this step draws the world in RenderWare's frame through an OpenXR
// camera, which is a 90 degree rotation about X: the ground stands up vertical.
//
// The result goes into a global rather than the scene uniform block on purpose.
// Vice City runs more than one camera per frame, and every draw recorded into a
// frame reads that block at whatever it holds when the frame is submitted, so a
// second camera would silently retransform geometry already recorded under the
// first. Folding it into each draw's model matrix captures it at record time.
static void
updateWorldToPlay(Camera *cam)
{
	// First person: the anchor is the player's head in the world, not the
	// follow camera. The basis is a fixed yaw latched at activation, so the
	// game world stays still while the player physically turns; the head
	// pose in the per-eye view matrices does all the looking.
	if(gvk.firstPersonActive){
		const float32 a = gvk.fpAnchorYaw;
		// Play axes expressed in world space: forward -Z looks along the
		// anchor yaw, +Y is world up, +X completes the right-handed set.
		const float32 px[3] = {  sinf(a), -cosf(a), 0.0f };
		const float32 py[3] = {  0.0f,     0.0f,    1.0f };
		const float32 pz[3] = { -cosf(a), -sinf(a), 0.0f };
		float32 *m = gvk.worldToPlay;
		m[0]  = px[0]; m[4] = px[1]; m[8]  = px[2];
		m[1]  = py[0]; m[5] = py[1]; m[9]  = py[2];
		m[2]  = pz[0]; m[6] = pz[1]; m[10] = pz[2];
		m[3]  = 0.0f;  m[7] = 0.0f;  m[11] = 0.0f;
		const float32 *w = gvk.fpHeadWorld;
		m[12] = -(px[0]*w[0] + px[1]*w[1] + px[2]*w[2]) + gvk.headPosition[0];
		m[13] = -(py[0]*w[0] + py[1]*w[1] + py[2]*w[2]) + gvk.headPosition[1];
		m[14] = -(pz[0]*w[0] + pz[1]*w[1] + pz[2]*w[2]) + gvk.headPosition[2];
		m[15] = 1.0f;
		return;
	}

	// The frontend runs before a camera frame exists. Identity leaves world
	// space equal to play space there, which keeps anything drawn in 3D around
	// the head rather than flinging it away by an uninitialised transform.
	if(cam == nil || cam->getFrame() == nil){
		memset(gvk.worldToPlay, 0, sizeof(gvk.worldToPlay));
		gvk.worldToPlay[0] = gvk.worldToPlay[5] =
		gvk.worldToPlay[10] = gvk.worldToPlay[15] = 1.0f;
		return;
	}

	Matrix *ltm = cam->getFrame()->getLTM();

	static int32 cameraLogCounter = 0;
	if((cameraLogCounter++ % 120) == 0)
		printf("[rwvk] cam pos %.1f %.1f %.1f | right %.2f %.2f %.2f | "
		       "up %.2f %.2f %.2f | at %.2f %.2f %.2f\n",
		       ltm->pos.x, ltm->pos.y, ltm->pos.z,
		       ltm->right.x, ltm->right.y, ltm->right.z,
		       ltm->up.x, ltm->up.y, ltm->up.z,
		       ltm->at.x, ltm->at.y, ltm->at.z);

	const V3d bx = neg(ltm->right);
	const V3d by = ltm->up;
	const V3d bz = neg(ltm->at);
	const V3d origin = ltm->pos;

	// Orthonormal basis, so the inverse is the transpose with the translation
	// rotated back through it.
	//
	// The camera point lands on the player's HEAD, not on the play space
	// origin. The origin is wherever the Guardian was set up: anchoring there
	// puts the viewpoint at floor height and makes every turn of the head
	// sweep an arc whose radius is the player's distance from the middle of
	// their room. Pinning the head instead means turning in place pivots
	// around the player, at the cost of positional head movement -- an
	// acceptable trade until the full VR layer is ported.
	float32 *m = gvk.worldToPlay;
	m[0]  = bx.x; m[4] = bx.y; m[8]  = bx.z;
	m[1]  = by.x; m[5] = by.y; m[9]  = by.z;
	m[2]  = bz.x; m[6] = bz.y; m[10] = bz.z;
	m[3]  = 0.0f; m[7] = 0.0f; m[11] = 0.0f;
	m[12] = -dot(bx, origin) + gvk.headPosition[0];
	m[13] = -dot(by, origin) + gvk.headPosition[1];
	m[14] = -dot(bz, origin) + gvk.headPosition[2];
	m[15] = 1.0f;
}

void
setIm2DTransform(const float32 transform[16], float32 planeDistance,
                 const float32 eye[3])
{
	SceneData *scene = getSceneData();
	memcpy(scene->im2dTransform, transform, sizeof(float32)*16);
	scene->im2dParams[0] = planeDistance;
	scene->im2dParams[1] = eye[0];
	scene->im2dParams[2] = eye[1];
	scene->im2dParams[3] = eye[2];
}

void
setHeadPose(const float32 position[3], float32 yaw, const float32 quat[4])
{
	memcpy(gvk.headPosition, position, sizeof(gvk.headPosition));
	gvk.headYaw = yaw;
	memcpy(gvk.headQuat, quat, sizeof(gvk.headQuat));
}

bool32
getFirstPersonViewFrame(float32 rwRight[3], float32 rwUp[3],
                        float32 rwAt[3], float32 position[3])
{
	if(!gvk.firstPersonActive)
		return 0;

	// Head axes in play space from the quaternion.
	const float32 x = gvk.headQuat[0], y = gvk.headQuat[1];
	const float32 z = gvk.headQuat[2], w = gvk.headQuat[3];
	const float32 hr[3] = { 1-2*(y*y+z*z), 2*(x*y+z*w),   2*(x*z-y*w) };
	const float32 hu[3] = { 2*(x*y-z*w),   1-2*(x*x+z*z), 2*(y*z+x*w) };
	const float32 hf[3] = { -2*(x*z+y*w),  -2*(y*z-x*w),  -(1-2*(x*x+y*y)) };

	// Play axes in world space, the same basis updateWorldToPlay builds.
	const float32 a = gvk.fpAnchorYaw;
	const float32 px[3] = {  sinf(a), -cosf(a), 0.0f };
	const float32 py[3] = {  0.0f,     0.0f,    1.0f };
	const float32 pz[3] = { -cosf(a), -sinf(a), 0.0f };

	// world = px*play.x + py*play.y + pz*play.z for each head axis.
	for(int i = 0; i < 3; i++){
		// RenderWare camera convention: the right column points LEFT.
		rwRight[i] = -(px[i]*hr[0] + py[i]*hr[1] + pz[i]*hr[2]);
		rwUp[i]    =   px[i]*hu[0] + py[i]*hu[1] + pz[i]*hu[2];
		rwAt[i]    =   px[i]*hf[0] + py[i]*hf[1] + pz[i]*hf[2];
	}
	memcpy(position, gvk.fpHeadWorld, 3*sizeof(float32));
	return 1;
}

void
setFirstPersonAnchor(const float32 headWorld[3], float32 headingYaw,
                     bool32 followHeading, bool32 active)
{
	if(active && (!gvk.firstPersonActive ||
	              gvk.fpFollowHeading != followHeading)){
		// (Re)latch so the player's current physical facing looks along the
		// heading: view yaw = anchorYaw + headYaw. Re-latching on a mode
		// change makes entering a vehicle start looking down its nose.
		gvk.fpLatchedHeadYaw = gvk.headYaw;
		gvk.fpAnchorYaw = headingYaw - gvk.headYaw;
	}
	gvk.firstPersonActive = active;
	gvk.fpFollowHeading = followHeading;
	if(active){
		memcpy(gvk.fpHeadWorld, headWorld, sizeof(gvk.fpHeadWorld));
		// Cockpit: the anchor turns with whatever is being ridden, keeping
		// the physical-facing offset taken at entry.
		if(followHeading)
			gvk.fpAnchorYaw = headingYaw - gvk.fpLatchedHeadYaw;
	}
}

bool32
getFirstPersonViewYaw(float32 *yawOut)
{
	if(!gvk.firstPersonActive)
		return 0;
	*yawOut = gvk.fpAnchorYaw + gvk.headYaw;
	return 1;
}

void
setClearColour(uint8 red, uint8 green, uint8 blue)
{
	gvk.clearColour[0] = red / 255.0f;
	gvk.clearColour[1] = green / 255.0f;
	gvk.clearColour[2] = blue / 255.0f;
}

bool32
isInitialized(void) { return gvk.initialised; }

const char *
getAdapterName(void)
{
	return gvk.initialised ? gvk.deviceProperties.deviceName : "";
}

bool32
waitForGpu(void)
{
	if(!gvk.initialised)
		return 0;
	return vkDeviceWaitIdle(gvk.device) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Device interface
// ---------------------------------------------------------------------------

static void
beginUpdate(Camera *cam)
{
	// The per-eye projection comes from OpenXR, but where the viewer stands in
	// Vice City comes from the game camera, exactly as it does on the desktop
	// build -- there the VR layer writes the eye pose into the RwCamera's frame
	// and librw derives the view matrix from it. Multiview needs both eyes at
	// once, so the composition happens here instead.
	updateWorldToPlay(cam);
}

static void
endUpdate(Camera *cam)
{
	(void)cam;
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	(void)cam;
	(void)col;
	(void)mode;
	// The render pass clears both attachments on load, so an explicit clear is
	// only needed for mid-pass clears, which Vice City does not issue.
}

static void
showRaster(Raster *raster, uint32 flags)
{
	// Presentation belongs to OpenXR: the composited image is handed back
	// through xrReleaseSwapchainImage, not presented by the renderer.
	(void)raster;
	(void)flags;
}

static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	(void)raster;
	(void)x;
	(void)y;
	return 0;
}

static void
setRenderState(int32 state, void *value)
{
	// TEXTURERASTER carries a pointer, not a packed value, so it is handled
	// before the truncation below.
	if(state == TEXTURERASTER){
		gstate.texture = (Raster*)value;
		return;
	}

	const uint32 v = (uint32)(uintptr)value;
	switch(state){
	case VERTEXALPHA:      gstate.vertexAlphaEnabled = v; break;
	case SRCBLEND:         gstate.srcBlend = v; break;
	case DESTBLEND:        gstate.dstBlend = v; break;
	case ZTESTENABLE:      gstate.zTestEnabled = v; break;
	case ZWRITEENABLE:     gstate.zWriteEnabled = v; break;
	case CULLMODE:         gstate.cullMode = v; break;
	case ALPHATESTFUNC:    gstate.alphaTestFunction = v; break;
	case ALPHATESTREF:     gstate.alphaTestRef = v; break;
	case FOGENABLE:        gstate.fogEnabled = v; break;
	case FOGCOLOR: {
		RGBA c;
		memcpy(&c, &value, sizeof(c));
		gstate.fogColor = c;
		break;
	}
	case TEXTUREADDRESS:   gstate.textureAddressU = gstate.textureAddressV = v; break;
	case TEXTUREADDRESSU:  gstate.textureAddressU = v; break;
	case TEXTUREADDRESSV:  gstate.textureAddressV = v; break;
	case TEXTUREFILTER:    gstate.textureFilter = v; break;
	default: break;
	}
}

static void *
getRenderState(int32 state)
{
	if(state == TEXTURERASTER)
		return gstate.texture;

	uint32 v = 0;
	switch(state){
	case VERTEXALPHA:      v = gstate.vertexAlphaEnabled; break;
	case SRCBLEND:         v = gstate.srcBlend; break;
	case DESTBLEND:        v = gstate.dstBlend; break;
	case ZTESTENABLE:      v = gstate.zTestEnabled; break;
	case ZWRITEENABLE:     v = gstate.zWriteEnabled; break;
	case CULLMODE:         v = gstate.cullMode; break;
	case ALPHATESTFUNC:    v = gstate.alphaTestFunction; break;
	case ALPHATESTREF:     v = gstate.alphaTestRef; break;
	case FOGENABLE:        v = gstate.fogEnabled; break;
	case FOGCOLOR:
		memcpy(&v, &gstate.fogColor, sizeof(v));
		break;
	case TEXTUREADDRESS:
		v = gstate.textureAddressU == gstate.textureAddressV ?
		    gstate.textureAddressU : 0;
		break;
	case TEXTUREADDRESSU:  v = gstate.textureAddressU; break;
	case TEXTUREADDRESSV:  v = gstate.textureAddressV; break;
	case TEXTUREFILTER:    v = gstate.textureFilter; break;
	default: break;
	}
	return (void*)(uintptr)v;
}

static void
resetRenderState(void)
{
	memset(&gstate, 0, sizeof(gstate));
	gstate.zTestEnabled = 1;
	gstate.zWriteEnabled = 1;
	gstate.srcBlend = BLENDSRCALPHA;
	gstate.dstBlend = BLENDINVSRCALPHA;
	gstate.cullMode = CULLBACK;
	gstate.alphaTestFunction = ALPHAGREATEREQUAL;
	gstate.alphaTestRef = 128;
	gstate.textureFilter = Texture::LINEAR;
	gstate.textureAddressU = Texture::WRAP;
	gstate.textureAddressV = Texture::WRAP;
}

static int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	switch(req){
	case DEVICEOPEN: {
		EngineOpenParams *params = (EngineOpenParams*)arg;
		if(params == nil || params->device == VK_NULL_HANDLE){
			VKERR("DEVICEOPEN without an OpenXR-created Vulkan device");
			return 0;
		}
		gvk.instance = params->instance;
		gvk.physicalDevice = params->physicalDevice;
		gvk.device = params->device;
		gvk.queue = params->queue;
		gvk.queueFamilyIndex = params->queueFamilyIndex;
		gvk.width = params->width;
		gvk.height = params->height;
		gvk.viewCount = params->viewCount ? params->viewCount : 1;
		gvk.colourFormat = params->colourFormat;

		vkGetPhysicalDeviceProperties(gvk.physicalDevice, &gvk.deviceProperties);
		vkGetPhysicalDeviceMemoryProperties(gvk.physicalDevice, &gvk.memoryProperties);
		vkGetPhysicalDeviceFeatures(gvk.physicalDevice, &gvk.deviceFeatures);
		gvk.supportsBC = gvk.deviceFeatures.textureCompressionBC ? 1 : 0;

		VKLOG("adopting device %s, %ux%u x%u views, BC=%d",
		      gvk.deviceProperties.deviceName, gvk.width, gvk.height,
		      gvk.viewCount, (int)gvk.supportsBC);

		VkCommandPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = gvk.queueFamilyIndex;
		if(vkCreateCommandPool(gvk.device, &poolInfo, nil, &gvk.commandPool) != VK_SUCCESS)
			return 0;

		VkCommandBufferAllocateInfo commandInfo = {};
		commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandInfo.commandPool = gvk.commandPool;
		commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandInfo.commandBufferCount = 1;
		if(vkAllocateCommandBuffers(gvk.device, &commandInfo, &gvk.frameCommands) != VK_SUCCESS)
			return 0;

		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if(vkCreateFence(gvk.device, &fenceInfo, nil, &gvk.frameFence) != VK_SUCCESS)
			return 0;

		if(!createDepthBuffer())
			return 0;
		if(!createRenderPass())
			return 0;

		resetRenderState();
		// The pipeline cache builds against gvk.renderPass, so it can only be
		// created once the render pass exists.
		if(!stateInit())
			return 0;
		gvk.initialised = 1;
		return 1;
	}

	case DEVICECLOSE:
		if(gvk.device != VK_NULL_HANDLE){
			vkDeviceWaitIdle(gvk.device);
			stateShutdown();
			if(gvk.framebuffer) vkDestroyFramebuffer(gvk.device, gvk.framebuffer, nil);
			if(gvk.renderPass) vkDestroyRenderPass(gvk.device, gvk.renderPass, nil);
			if(gvk.depthView) vkDestroyImageView(gvk.device, gvk.depthView, nil);
			if(gvk.depthImage) vkDestroyImage(gvk.device, gvk.depthImage, nil);
			if(gvk.depthMemory) vkFreeMemory(gvk.device, gvk.depthMemory, nil);
			if(gvk.frameFence) vkDestroyFence(gvk.device, gvk.frameFence, nil);
			if(gvk.commandPool) vkDestroyCommandPool(gvk.device, gvk.commandPool, nil);
		}
		// The device itself belongs to OpenXR and is destroyed with the
		// session, never here.
		memset(&gvk, 0, sizeof(gvk));
		return 1;

	case DEVICEINIT:
	case DEVICETERM:
	case DEVICEFINALIZE:
		return 1;

	case DEVICEGETNUMSUBSYSTEMS:
	case DEVICEGETNUMVIDEOMODES:
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
	case DEVICEGETCURRENTVIDEOMODE:
		return 0;
	case DEVICESETSUBSYSTEM:
	case DEVICESETVIDEOMODE:
		return 1;
	case DEVICEGETSUBSSYSTEMINFO: {
		SubSystemInfo *info = (SubSystemInfo*)arg;
		if(info != nil && n == 0){
			strncpy(info->name, gvk.deviceProperties.deviceName,
			        sizeof(info->name)-1);
			info->name[sizeof(info->name)-1] = '\0';
			return 1;
		}
		return 0;
	}
	case DEVICEGETVIDEOMODEINFO: {
		VideoMode *mode = (VideoMode*)arg;
		// The headset owns the resolution; report the OpenXR target so any
		// caller inspecting the mode sees the real per-eye size.
		if(mode != nil && n == 0){
			mode->width = (int32)gvk.width;
			mode->height = (int32)gvk.height;
			mode->depth = 32;
			mode->flags = VIDEOMODEEXCLUSIVE;
			return 1;
		}
		return 0;
	}
	default:
		return 0;
	}
}

// ---------------------------------------------------------------------------
// Plugin registration
// ---------------------------------------------------------------------------

static void*
driverOpen(void *object, int32, int32)
{
	Driver *driver = engine->driver[PLATFORM_VULKAN];
	driver->defaultPipeline = makeDefaultPipeline();
	driver->rasterNativeOffset = nativeRasterOffset;
	driver->rasterCreate = rasterCreate;
	driver->rasterLock = rasterLock;
	driver->rasterUnlock = rasterUnlock;
	driver->rasterLockPalette = null::rasterLockPalette;
	driver->rasterUnlockPalette = null::rasterUnlockPalette;
	driver->rasterNumLevels = rasterNumLevels;
	driver->imageFindRasterFormat = imageFindRasterFormat;
	driver->rasterFromImage = rasterFromImage;
	driver->rasterToImage = rasterToImage;
	return object;
}

static void*
driverClose(void *object, int32, int32)
{
	return object;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_VULKAN, 0, PLATFORM_VULKAN,
	                       driverOpen, driverClose);
	registerNativeRaster();
}

#else

// Built without RW_VULKAN: keep the symbols so the platform can be selected at
// configure time without conditionalising every caller.
void registerPlatformPlugins(void) {}

#endif

Device renderdevice = {
	0.0f, 1.0f,
#ifdef RW_VULKAN
	vulkan::beginUpdate,
	vulkan::endUpdate,
	vulkan::clearCamera,
	vulkan::showRaster,
	vulkan::rasterRenderFast,
	vulkan::setRenderState,
	vulkan::getRenderState,
	vulkan::im2DRenderLine,
	vulkan::im2DRenderTriangle,
	vulkan::im2DRenderPrimitive,
	vulkan::im2DRenderIndexedPrimitive,
	vulkan::im3DTransform,
	vulkan::im3DRenderPrimitive,
	vulkan::im3DRenderIndexedPrimitive,
	vulkan::im3DEnd,
	vulkan::deviceSystem,
#else
	null::beginUpdate,
	null::endUpdate,
	null::clearCamera,
	null::showRaster,
	null::rasterRenderFast,
	null::setRenderState,
	null::getRenderState,
	null::im2DRenderLine,
	null::im2DRenderTriangle,
	null::im2DRenderPrimitive,
	null::im2DRenderIndexedPrimitive,
	null::im3DTransform,
	null::im3DRenderPrimitive,
	null::im3DRenderIndexedPrimitive,
	null::im3DEnd,
	null::deviceSystem,
#endif
};

}
}
