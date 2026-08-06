#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <vector>

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

static const uint32_t kPostFxVertSpv[] =
#include "rw_postfx_vert.h"
;
static const uint32_t kPostFxFragSpv[] =
#include "rw_postfx_frag.h"
;
#endif

namespace rw {
namespace vulkan {

#ifdef RW_VULKAN

Globals gvk;
RenderState gstate;

struct RetiredBuffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
};

struct RetiredImage
{
	VkImageView view;
	VkImage image;
	VkDeviceMemory memory;
};

// These vectors deliberately live outside Globals. Globals is a C-style
// backend record that is cleared with memset when the OpenXR device closes.
static std::vector<RetiredBuffer> retiredBuffers[NUM_FRAME_CONTEXTS];
static std::vector<RetiredImage> retiredImages[NUM_FRAME_CONTEXTS];
// Streaming uploads are recorded while the main frame command buffer is open,
// then submitted immediately before it in the same batch. The frame fence
// therefore owns both their temporary resources and their command buffers.
static std::vector<VkCommandBuffer> frameUploadCommands[NUM_FRAME_CONTEXTS];

static uint32
retirementFrame(void)
{
	if(gvk.inFrame)
		return gvk.activeFrame;
	if(gvk.hasSubmittedFrame)
		return gvk.lastSubmittedFrame;
	return gvk.activeFrame;
}

void
retireBuffer(VkBuffer buffer, VkDeviceMemory memory)
{
	if(buffer == VK_NULL_HANDLE && memory == VK_NULL_HANDLE)
		return;
	if(!gvk.initialised || (!gvk.inFrame && !gvk.hasSubmittedFrame)){
		if(buffer) vkDestroyBuffer(gvk.device, buffer, nil);
		if(memory) vkFreeMemory(gvk.device, memory, nil);
		return;
	}
	RetiredBuffer retired = { buffer, memory };
	retiredBuffers[retirementFrame()].push_back(retired);
}

void
retireImage(VkImageView view, VkImage image, VkDeviceMemory memory)
{
	if(view == VK_NULL_HANDLE && image == VK_NULL_HANDLE &&
	   memory == VK_NULL_HANDLE)
		return;
	if(!gvk.initialised || (!gvk.inFrame && !gvk.hasSubmittedFrame)){
		if(view) vkDestroyImageView(gvk.device, view, nil);
		if(image) vkDestroyImage(gvk.device, image, nil);
		if(memory) vkFreeMemory(gvk.device, memory, nil);
		return;
	}
	RetiredImage retired = { view, image, memory };
	retiredImages[retirementFrame()].push_back(retired);
}

static void
flushRetired(uint32 frameIndex)
{
	if(!frameUploadCommands[frameIndex].empty()){
		vkFreeCommandBuffers(gvk.device, gvk.commandPool,
		                     (uint32)frameUploadCommands[frameIndex].size(),
		                     frameUploadCommands[frameIndex].data());
		frameUploadCommands[frameIndex].clear();
	}

	for(size_t i = 0; i < retiredBuffers[frameIndex].size(); i++){
		const RetiredBuffer &retired = retiredBuffers[frameIndex][i];
		if(retired.buffer) vkDestroyBuffer(gvk.device, retired.buffer, nil);
		if(retired.memory) vkFreeMemory(gvk.device, retired.memory, nil);
	}
	retiredBuffers[frameIndex].clear();

	for(size_t i = 0; i < retiredImages[frameIndex].size(); i++){
		const RetiredImage &retired = retiredImages[frameIndex][i];
		if(retired.view) vkDestroyImageView(gvk.device, retired.view, nil);
		if(retired.image) vkDestroyImage(gvk.device, retired.image, nil);
		if(retired.memory) vkFreeMemory(gvk.device, retired.memory, nil);
	}
	retiredImages[frameIndex].clear();
}

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
	if(vkEndCommandBuffer(commandBuffer) != VK_SUCCESS){
		vkFreeCommandBuffers(gvk.device, gvk.commandPool, 1, &commandBuffer);
		return;
	}

	if(gvk.inFrame){
		// Do not submit (or wait) once for every streamed texture/mesh. endFrame
		// prepends all of these command buffers to the main frame in one batch;
		// its fence then provides an exact lifetime boundary for staging data.
		frameUploadCommands[gvk.activeFrame].push_back(commandBuffer);
		return;
	}

	// Startup uploads have no later frame fence to inherit. Keep the
	// synchronous fallback for that uncommon path.
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &commandBuffer;
	const VkResult submitted =
		vkQueueSubmit(gvk.queue, 1, &submit, VK_NULL_HANDLE);
	if(submitted != VK_SUCCESS){
		VKERR("one-shot submit failed: %d", (int)submitted);
		vkFreeCommandBuffers(gvk.device, gvk.commandPool, 1, &commandBuffer);
		return;
	}
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
createDepthBuffer(FrameContext &frame)
{
	static const VkFormat candidates[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D16_UNORM,
	};
	if(gvk.depthFormat == VK_FORMAT_UNDEFINED){
		for(uint32 i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++){
			VkFormatProperties properties;
			vkGetPhysicalDeviceFormatProperties(
				gvk.physicalDevice, candidates[i], &properties);
			if(properties.optimalTilingFeatures &
			   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){
				gvk.depthFormat = candidates[i];
				break;
			}
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
	if(vkCreateImage(gvk.device, &imageInfo, nil, &frame.depthImage) != VK_SUCCESS)
		return 0;

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, frame.depthImage, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return 0;

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, &frame.depthMemory) != VK_SUCCESS)
		return 0;
	vkBindImageMemory(gvk.device, frame.depthImage, frame.depthMemory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.depthImage;
	viewInfo.viewType = gvk.viewCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
	                                      : VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = gvk.depthFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	return vkCreateImageView(gvk.device, &viewInfo, nil,
	                         &frame.depthView) == VK_SUCCESS;
}

static bool32
createSceneColour(FrameContext &frame)
{
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = gvk.colourFormat;
	imageInfo.extent.width = gvk.width;
	imageInfo.extent.height = gvk.height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                  VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil,
	                 &frame.sceneColourImage) != VK_SUCCESS){
		VKERR("failed to create post-FX scene colour image");
		return 0;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, frame.sceneColourImage,
	                             &requirements);

	// FXAA samples neighbouring texels in a separate pass, so this image must
	// be backed by ordinary device-local memory rather than transient/lazy
	// tile storage.
	uint32 typeIndex = UINT32_MAX;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex)){
		vkDestroyImage(gvk.device, frame.sceneColourImage, nil);
		frame.sceneColourImage = VK_NULL_HANDLE;
		return 0;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil,
	                    &frame.sceneColourMemory) != VK_SUCCESS){
		vkDestroyImage(gvk.device, frame.sceneColourImage, nil);
		frame.sceneColourImage = VK_NULL_HANDLE;
		return 0;
	}
	if(vkBindImageMemory(gvk.device, frame.sceneColourImage,
	                     frame.sceneColourMemory, 0) != VK_SUCCESS)
		return 0;

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.sceneColourImage;
	// The post shader always uses sampler2DArray so gl_ViewIndex can select the
	// correct eye. A one-layer array is valid for the mono fallback too.
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = gvk.colourFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	if(vkCreateImageView(gvk.device, &viewInfo, nil,
	                     &frame.sceneColourView) != VK_SUCCESS){
		VKERR("failed to create post-FX scene colour view");
		return 0;
	}
	return 1;
}

static bool32
createRenderPass(void)
{
	VkAttachmentDescription attachments[2] = {};
	// World/HUD target. It is stored because the following post pass samples
	// neighbouring texels for FXAA.
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

	VkAttachmentReference sceneRef = {};
	sceneRef.attachment = 0;
	sceneRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference depthRef = {};
	depthRef.attachment = 1;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &sceneRef;
	subpass.pDepthStencilAttachment = &depthRef;

	const uint32 viewMask = gvk.viewCount > 1 ? 0x3u : 0x1u;
	const uint32 correlationMask = viewMask;
	const uint32 viewMasks[1] = { viewMask };
	VkRenderPassMultiviewCreateInfo multiview = {};
	multiview.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
	multiview.subpassCount = 1;
	multiview.pViewMasks = viewMasks;
	multiview.correlationMaskCount = 1;
	multiview.pCorrelationMasks = &correlationMask;

	VkRenderPassCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.pNext = gvk.viewCount > 1 ? &multiview : nil;
	info.attachmentCount = 2;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	if(vkCreateRenderPass(gvk.device, &info, nil, &gvk.renderPass) != VK_SUCCESS)
		return 0;

	// The resolve pass owns only the acquired OpenXR colour view. The private
	// scene image is sampled through a descriptor after an explicit barrier.
	VkAttachmentDescription destination = {};
	destination.format = gvk.colourFormat;
	destination.samples = VK_SAMPLE_COUNT_1_BIT;
	destination.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	destination.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	destination.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	destination.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	destination.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	destination.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference destinationRef = {};
	destinationRef.attachment = 0;
	destinationRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkSubpassDescription postSubpass = {};
	postSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	postSubpass.colorAttachmentCount = 1;
	postSubpass.pColorAttachments = &destinationRef;
	VkRenderPassMultiviewCreateInfo postMultiview = multiview;
	VkRenderPassCreateInfo postInfo = {};
	postInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	postInfo.pNext = gvk.viewCount > 1 ? &postMultiview : nil;
	postInfo.attachmentCount = 1;
	postInfo.pAttachments = &destination;
	postInfo.subpassCount = 1;
	postInfo.pSubpasses = &postSubpass;
	if(vkCreateRenderPass(gvk.device, &postInfo, nil,
	                      &gvk.postFxRenderPass) != VK_SUCCESS){
		vkDestroyRenderPass(gvk.device, gvk.renderPass, nil);
		gvk.renderPass = VK_NULL_HANDLE;
		return 0;
	}
	return 1;
}

static VkShaderModule
createPostFxModule(const uint32_t *code, size_t byteSize)
{
	VkShaderModuleCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = byteSize;
	info.pCode = code;

	VkShaderModule module = VK_NULL_HANDLE;
	if(vkCreateShaderModule(gvk.device, &info, nil, &module) != VK_SUCCESS)
		VKERR("failed to create post-FX shader module");
	return module;
}

static bool32
createSceneFramebuffer(FrameContext &frame)
{
	const VkImageView attachments[2] = {
		frame.sceneColourView, frame.depthView
	};
	VkFramebufferCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	info.renderPass = gvk.renderPass;
	info.attachmentCount = 2;
	info.pAttachments = attachments;
	info.width = gvk.width;
	info.height = gvk.height;
	// Multiview derives the layers from the render-pass view mask.
	info.layers = 1;
	return vkCreateFramebuffer(gvk.device, &info, nil,
	                           &frame.sceneFramebuffer) == VK_SUCCESS;
}

static bool32
createPostFxResources(void)
{
	VkSamplerCreateInfo samplerInfo = {};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;
	if(vkCreateSampler(gvk.device, &samplerInfo, nil,
	                   &gvk.postFxSampler) != VK_SUCCESS){
		VKERR("failed to create post-FX sampler");
		return 0;
	}

	VkDescriptorSetLayoutBinding inputBinding = {};
	inputBinding.binding = 0;
	inputBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	inputBinding.descriptorCount = 1;
	inputBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo = {};
	descriptorLayoutInfo.sType =
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptorLayoutInfo.bindingCount = 1;
	descriptorLayoutInfo.pBindings = &inputBinding;
	if(vkCreateDescriptorSetLayout(gvk.device, &descriptorLayoutInfo, nil,
	                               &gvk.postFxDescriptorLayout) != VK_SUCCESS){
		VKERR("failed to create post-FX descriptor layout");
		return 0;
	}

	VkDescriptorPoolSize poolSize = {};
	poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSize.descriptorCount = NUM_FRAME_CONTEXTS;
	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = NUM_FRAME_CONTEXTS;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	if(vkCreateDescriptorPool(gvk.device, &poolInfo, nil,
	                          &gvk.postFxDescriptorPool) != VK_SUCCESS){
		VKERR("failed to create post-FX descriptor pool");
		return 0;
	}

	VkDescriptorSetAllocateInfo descriptorInfo = {};
	descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorInfo.descriptorPool = gvk.postFxDescriptorPool;
	descriptorInfo.descriptorSetCount = NUM_FRAME_CONTEXTS;
	VkDescriptorSetLayout descriptorLayouts[NUM_FRAME_CONTEXTS];
	VkDescriptorSet descriptorSets[NUM_FRAME_CONTEXTS];
	for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
		descriptorLayouts[i] = gvk.postFxDescriptorLayout;
	descriptorInfo.pSetLayouts = descriptorLayouts;
	if(vkAllocateDescriptorSets(gvk.device, &descriptorInfo,
	                            descriptorSets) != VK_SUCCESS){
		VKERR("failed to allocate post-FX descriptor set");
		return 0;
	}

	VkDescriptorImageInfo imageDescriptors[NUM_FRAME_CONTEXTS] = {};
	VkWriteDescriptorSet writes[NUM_FRAME_CONTEXTS] = {};
	for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
		gvk.frames[i].postFxDescriptor = descriptorSets[i];
		imageDescriptors[i].sampler = gvk.postFxSampler;
		imageDescriptors[i].imageView = gvk.frames[i].sceneColourView;
		imageDescriptors[i].imageLayout =
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = descriptorSets[i];
		writes[i].dstBinding = 0;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType =
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &imageDescriptors[i];
	}
	vkUpdateDescriptorSets(gvk.device, NUM_FRAME_CONTEXTS, writes, 0, nil);

	VkPushConstantRange pushRange = {};
	pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(PostFxPushConstants);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &gvk.postFxDescriptorLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if(vkCreatePipelineLayout(gvk.device, &pipelineLayoutInfo, nil,
	                          &gvk.postFxPipelineLayout) != VK_SUCCESS){
		VKERR("failed to create post-FX pipeline layout");
		return 0;
	}

	VkShaderModule vertexModule =
		createPostFxModule(kPostFxVertSpv, sizeof(kPostFxVertSpv));
	VkShaderModule fragmentModule =
		createPostFxModule(kPostFxFragSpv, sizeof(kPostFxFragSpv));
	if(vertexModule == VK_NULL_HANDLE || fragmentModule == VK_NULL_HANDLE){
		if(vertexModule) vkDestroyShaderModule(gvk.device, vertexModule, nil);
		if(fragmentModule) vkDestroyShaderModule(gvk.device, fragmentModule, nil);
		return 0;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertexModule;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragmentModule;
	stages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vertexInput = {};
	vertexInput.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
	inputAssembly.sType =
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState = {};
	viewportState.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasteriser = {};
	rasteriser.sType =
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
	rasteriser.cullMode = VK_CULL_MODE_NONE;
	rasteriser.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasteriser.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = {};
	multisample.sType =
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil = {};
	depthStencil.sType =
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.blendEnable = VK_FALSE;
	blendAttachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo colourBlend = {};
	colourBlend.sType =
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colourBlend.attachmentCount = 1;
	colourBlend.pAttachments = &blendAttachment;

	const VkDynamicState dynamicStates[2] = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicState = {};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = stages;
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasteriser;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colourBlend;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = gvk.postFxPipelineLayout;
	pipelineInfo.renderPass = gvk.postFxRenderPass;
	pipelineInfo.subpass = 0;

	const VkResult pipelineResult =
		vkCreateGraphicsPipelines(gvk.device, VK_NULL_HANDLE, 1,
		                          &pipelineInfo, nil, &gvk.postFxPipeline);
	vkDestroyShaderModule(gvk.device, vertexModule, nil);
	vkDestroyShaderModule(gvk.device, fragmentModule, nil);
	if(pipelineResult != VK_SUCCESS){
		VKERR("failed to create post-FX graphics pipeline");
		return 0;
	}

	VKLOG("sampled multiview FXAA/post-FX resolve enabled");
	return 1;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

static void
readCompletedFrameTimestamps(FrameContext &frame)
{
	if(frame.timestampQueryPool == VK_NULL_HANDLE || !frame.timestampPending)
		return;

	uint64 timestamps[2] = { 0, 0 };
	const VkResult result = vkGetQueryPoolResults(
		gvk.device, frame.timestampQueryPool, 0, 2,
		sizeof(timestamps), timestamps, sizeof(uint64),
		VK_QUERY_RESULT_64_BIT);
	frame.timestampPending = 0;
	if(result != VK_SUCCESS)
		return;

	const uint32 validBits = gvk.frameTimestampValidBits;
	uint64 delta = timestamps[1]-timestamps[0];
	if(validBits > 0 && validBits < 64){
		const uint64 mask = (1ULL << validBits)-1ULL;
		delta = (timestamps[1]-timestamps[0]) & mask;
	}
	gvk.frameGpuMilliseconds =
		(float32)((double)delta*
		          (double)gvk.deviceProperties.limits.timestampPeriod/
		          1000000.0);
	gvk.frameTimestampAvailable = 1;
}

bool32
beginFrame(VkImage colourImage, VkImageView colourView)
{
	if(!gvk.initialised || gvk.inFrame)
		return 0;
	(void)colourImage;

	// Anything a previous frame had to defer is built here, before a render
	// pass is open. Compiling inside one does not return on this driver.
	compilePendingPipelines();

	const uint32 frameIndex = gvk.nextFrame;
	FrameContext &frame = gvk.frames[frameIndex];
	if(frame.submissionPending){
		// Wait only when this particular slot comes around again. The other
		// slot remains on the GPU while the CPU records the new frame.
		if(vkWaitForFences(gvk.device, 1, &frame.fence, VK_TRUE,
		                   UINT64_MAX) != VK_SUCCESS)
			return 0;
		frame.submissionPending = 0;
		// Both timestamp readback and deferred resource destruction are now
		// non-blocking and safe for this retired slot.
		readCompletedFrameTimestamps(frame);
		flushRetired(frameIndex);
	}
	gvk.activeFrame = frameIndex;
	gvk.frameCommands = frame.commandBuffer;
	setStateFrame(frameIndex);

	// OpenXR rotates through a fixed swapchain image set. Cache one post-pass
	// framebuffer for each (frame slot, colour view) pair. The private
	// scene/depth framebuffer belongs permanently to the frame slot.
	frame.framebuffer = VK_NULL_HANDLE;
	for(uint32 i = 0; i < frame.numFramebuffers; i++)
		if(frame.framebufferColourViews[i] == colourView){
			frame.framebuffer = frame.framebuffers[i];
			break;
		}
	if(frame.framebuffer == VK_NULL_HANDLE){
		if(frame.numFramebuffers >= MAX_FRAMEBUFFERS_PER_CONTEXT){
			VKERR("OpenXR framebuffer cache exhausted");
			return 0;
		}
		const VkImageView attachments[1] = { colourView };
		VkFramebufferCreateInfo framebufferInfo = {};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = gvk.postFxRenderPass;
		framebufferInfo.attachmentCount = 1;
		framebufferInfo.pAttachments = attachments;
		framebufferInfo.width = gvk.width;
		framebufferInfo.height = gvk.height;
		// Multiview derives the layer count from the view mask, so the
		// framebuffer itself is declared single-layer.
		framebufferInfo.layers = 1;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		if(vkCreateFramebuffer(gvk.device, &framebufferInfo, nil,
		                       &framebuffer) != VK_SUCCESS)
			return 0;
		const uint32 cacheSlot = frame.numFramebuffers++;
		frame.framebufferColourViews[cacheSlot] = colourView;
		frame.framebuffers[cacheSlot] = framebuffer;
		frame.framebuffer = framebuffer;
		VKLOG("cached OpenXR framebuffer frame=%u image=%u",
		      frameIndex, cacheSlot);
	}

	vkResetCommandBuffer(gvk.frameCommands, 0);

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(gvk.frameCommands, &beginInfo);
	if(gvk.frameTimestampEnabled &&
	   frame.timestampQueryPool != VK_NULL_HANDLE){
		vkCmdResetQueryPool(gvk.frameCommands,
		                    frame.timestampQueryPool, 0, 2);
		vkCmdWriteTimestamp(gvk.frameCommands,
		                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                    frame.timestampQueryPool, 0);
	}

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
	passInfo.framebuffer = frame.sceneFramebuffer;
	passInfo.renderArea.extent.width = gvk.width;
	passInfo.renderArea.extent.height = gvk.height;
	passInfo.clearValueCount = 2;
	passInfo.pClearValues = clears;
	vkCmdBeginRenderPass(gvk.frameCommands, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

	// Set 0 is the same scene buffer for every world and immediate-mode draw
	// in this frame. All librw scene pipelines share getPipelineLayout(), so
	// bind it once instead of repeating the command for every atomic/primitive.
	VkDescriptorSet sceneSet = getSceneDescriptor();
	vkCmdBindDescriptorSets(gvk.frameCommands, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        getPipelineLayout(), 0, 1, &sceneSet, 0, nil);

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
	FrameContext &frame = gvk.frames[gvk.activeFrame];
	reportImStats();

	// Make the stored world image visible to the sampled post pass. This is a
	// full-image dependency rather than BY_REGION because FXAA reads adjacent
	// pixels.
	vkCmdEndRenderPass(gvk.frameCommands);
	VkImageMemoryBarrier sceneBarrier = {};
	sceneBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	sceneBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	sceneBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	sceneBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sceneBarrier.image = frame.sceneColourImage;
	sceneBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	sceneBarrier.subresourceRange.levelCount = 1;
	sceneBarrier.subresourceRange.layerCount = gvk.viewCount;
	vkCmdPipelineBarrier(gvk.frameCommands,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     0, 0, nil, 0, nil, 1, &sceneBarrier);

	VkRenderPassBeginInfo postPass = {};
	postPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	postPass.renderPass = gvk.postFxRenderPass;
	postPass.framebuffer = frame.framebuffer;
	postPass.renderArea.extent.width = gvk.width;
	postPass.renderArea.extent.height = gvk.height;
	vkCmdBeginRenderPass(gvk.frameCommands, &postPass,
	                     VK_SUBPASS_CONTENTS_INLINE);

	// Resolve the sampled scene into the OpenXR swapchain. This always runs:
	// mode 0 is a byte-for-byte colour pass-through, while mode 1 applies the
	// Vice City colour filter. FXAA is selected with a push-constant flag, so
	// toggling it never rebuilds or switches a graphics pipeline.
	VkViewport viewport = {};
	viewport.width = (float32)gvk.width;
	viewport.height = (float32)gvk.height;
	viewport.maxDepth = 1.0f;
	VkRect2D scissor = {};
	scissor.extent.width = gvk.width;
	scissor.extent.height = gvk.height;
	vkCmdSetViewport(gvk.frameCommands, 0, 1, &viewport);
	vkCmdSetScissor(gvk.frameCommands, 0, 1, &scissor);
	vkCmdBindPipeline(gvk.frameCommands, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                  gvk.postFxPipeline);
	vkCmdBindDescriptorSets(gvk.frameCommands,
	                        VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        gvk.postFxPipelineLayout, 0, 1,
	                        &frame.postFxDescriptor, 0, nil);
	vkCmdPushConstants(gvk.frameCommands, gvk.postFxPipelineLayout,
	                   VK_SHADER_STAGE_FRAGMENT_BIT, 0,
	                   sizeof(gvk.postFxConstants), &gvk.postFxConstants);
	vkCmdDraw(gvk.frameCommands, 3, 1, 0, 0);
	vkCmdEndRenderPass(gvk.frameCommands);
	if(gvk.frameTimestampEnabled &&
	   frame.timestampQueryPool != VK_NULL_HANDLE)
		vkCmdWriteTimestamp(gvk.frameCommands,
		                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		                    frame.timestampQueryPool, 1);
	vkEndCommandBuffer(gvk.frameCommands);

	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	// Copies must be available before this frame consumes newly streamed
	// textures and geometry. Put every upload before the render command buffer
	// in one queue batch; this also reduces vkQueueSubmit traffic to once/frame.
	std::vector<VkCommandBuffer> &uploads =
		frameUploadCommands[gvk.activeFrame];
	uploads.push_back(gvk.frameCommands);
	submit.commandBufferCount = (uint32)uploads.size();
	submit.pCommandBuffers = uploads.data();
	VkResult submitted = vkResetFences(gvk.device, 1, &frame.fence);
	if(submitted == VK_SUCCESS)
		submitted = vkQueueSubmit(gvk.queue, 1, &submit, frame.fence);
	// Keep only the upload buffers in the retirement list. The persistent main
	// frame command buffer belongs to FrameContext and is reset on slot reuse.
	uploads.pop_back();
	if(submitted == VK_SUCCESS){
		frame.submissionPending = 1;
		frame.timestampPending =
			gvk.frameTimestampEnabled &&
			frame.timestampQueryPool != VK_NULL_HANDLE;
		gvk.lastSubmittedFrame = gvk.activeFrame;
		gvk.hasSubmittedFrame = 1;
		gvk.nextFrame = (gvk.activeFrame + 1) % NUM_FRAME_CONTEXTS;
	}else{
		VKERR("frame submit failed: %d", (int)submitted);
		// The fence was reset but no work owns it. Draining the queue makes
		// every previously submitted slot and every retirement queue safe.
		vkQueueWaitIdle(gvk.queue);
		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
			gvk.frames[i].submissionPending = 0;
			gvk.frames[i].timestampPending = 0;
			flushRetired(i);
		}
		gvk.hasSubmittedFrame = 0;
	}
	gvk.inFrame = 0;
}

void
setGpuFrameTimingEnabled(bool32 enabled)
{
	const bool32 value = enabled ? 1 : 0;
	if(gvk.frameTimestampEnabled == value)
		return;
	gvk.frameTimestampEnabled = value;
	gvk.frameTimestampAvailable = 0;
}

bool32
getGpuFrameTimeMs(float32 *milliseconds)
{
	if(milliseconds != nil)
		*milliseconds = gvk.frameGpuMilliseconds;
	return gvk.frames[0].timestampQueryPool != VK_NULL_HANDLE &&
	       gvk.frameTimestampAvailable;
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
getFirstPersonPlayBasis(float32 px[3], float32 py[3], float32 pz[3])
{
	if(gvk.fpUseFullBasis){
		memcpy(px, gvk.fpPlayX, sizeof(gvk.fpPlayX));
		memcpy(py, gvk.fpPlayY, sizeof(gvk.fpPlayY));
		memcpy(pz, gvk.fpPlayZ, sizeof(gvk.fpPlayZ));
		return;
	}
	const float32 a = gvk.fpAnchorYaw;
	px[0] =  sinf(a); px[1] = -cosf(a); px[2] = 0.0f;
	py[0] =  0.0f;    py[1] =  0.0f;    py[2] = 1.0f;
	pz[0] = -cosf(a); pz[1] = -sinf(a); pz[2] = 0.0f;
}

static void
updateWorldToPlay(Camera *cam)
{
	// First person: the anchor is the player's head in the world, not the
	// follow camera. The basis is a fixed yaw latched at activation, so the
	// game world stays still while the player physically turns; the head
	// pose in the per-eye view matrices does all the looking.
	if(gvk.firstPersonActive){
		// Play axes expressed in world space. Normally these are reconstructed
		// from yaw and world-up; cockpit mode can instead provide the full
		// vehicle basis so pitch/roll remain coherent in both eyes.
		float32 px[3], py[3], pz[3];
		getFirstPersonPlayBasis(px, py, pz);
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
	float32 px[3], py[3], pz[3];
	getFirstPersonPlayBasis(px, py, pz);

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

bool32
playPoseToFirstPersonWorld(const float32 playPosition[3],
                           const float32 playQuaternion[4],
                           float32 worldRight[3], float32 worldUp[3],
                           float32 worldForward[3], float32 worldPosition[3])
{
	if(!gvk.firstPersonActive)
		return 0;

	const float32 x = playQuaternion[0], y = playQuaternion[1];
	const float32 z = playQuaternion[2], w = playQuaternion[3];
	const float32 pr[3] = {
		1-2*(y*y+z*z), 2*(x*y+z*w), 2*(x*z-y*w) };
	const float32 pu[3] = {
		2*(x*y-z*w), 1-2*(x*x+z*z), 2*(y*z+x*w) };
	const float32 pf[3] = {
		-2*(x*z+y*w), -2*(y*z-x*w), -(1-2*(x*x+y*y)) };

	float32 px[3], py[3], pz[3];
	getFirstPersonPlayBasis(px, py, pz);
	const float32 relative[3] = {
		playPosition[0] - gvk.headPosition[0],
		playPosition[1] - gvk.headPosition[1],
		playPosition[2] - gvk.headPosition[2] };

	for(int i = 0; i < 3; i++){
		worldRight[i] =
			px[i]*pr[0] + py[i]*pr[1] + pz[i]*pr[2];
		worldUp[i] =
			px[i]*pu[0] + py[i]*pu[1] + pz[i]*pu[2];
		worldForward[i] =
			px[i]*pf[0] + py[i]*pf[1] + pz[i]*pf[2];
		worldPosition[i] = gvk.fpHeadWorld[i] +
			px[i]*relative[0] + py[i]*relative[1] + pz[i]*relative[2];
	}
	return 1;
}

void
setFirstPersonAnchor(const float32 headWorld[3], float32 headingYaw,
                     bool32 followHeading, bool32 active)
{
	if(active && (!gvk.firstPersonActive ||
	              gvk.fpFollowHeading != followHeading ||
	              gvk.fpUseFullBasis)){
		// (Re)latch so the player's current physical facing looks along the
		// heading: view yaw = anchorYaw + headYaw. Re-latching on a mode
		// change makes entering a vehicle start looking down its nose.
		gvk.fpLatchedHeadYaw = gvk.headYaw;
		gvk.fpAnchorYaw = headingYaw - gvk.headYaw;
	}
	gvk.firstPersonActive = active;
	gvk.fpFollowHeading = followHeading;
	gvk.fpUseFullBasis = 0;
	if(active){
		memcpy(gvk.fpHeadWorld, headWorld, sizeof(gvk.fpHeadWorld));
		// Cockpit: the anchor turns with whatever is being ridden, keeping
		// the physical-facing offset taken at entry.
		if(followHeading)
			gvk.fpAnchorYaw = headingYaw - gvk.fpLatchedHeadYaw;
	}
}

void
setFirstPersonAnchorBasis(const float32 headWorld[3],
                          const float32 right[3],
                          const float32 up[3],
                          const float32 forward[3],
                          float32 headingYaw, bool32 active)
{
	if(active && (!gvk.firstPersonActive ||
	              !gvk.fpFollowHeading ||
	              !gvk.fpUseFullBasis)){
		gvk.fpLatchedHeadYaw = gvk.headYaw;
	}
	gvk.firstPersonActive = active;
	gvk.fpFollowHeading = active;
	gvk.fpUseFullBasis = active;
	if(!active)
		return;

	memcpy(gvk.fpHeadWorld, headWorld, sizeof(gvk.fpHeadWorld));
	gvk.fpAnchorYaw = headingYaw-gvk.fpLatchedHeadYaw;

	// Rotate the vehicle frame by the inverse of the physical yaw that was
	// present when cockpit mode activated. This is the full-basis equivalent
	// of fpAnchorYaw = headingYaw - fpLatchedHeadYaw.
	const float32 c = cosf(gvk.fpLatchedHeadYaw);
	const float32 s = sinf(gvk.fpLatchedHeadYaw);
	for(int32 i = 0; i < 3; i++){
		gvk.fpPlayX[i] = right[i]*c-forward[i]*s;
		gvk.fpPlayY[i] = up[i];
		gvk.fpPlayZ[i] = -(forward[i]*c+right[i]*s);
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

bool32
getFirstPersonLocalHeadYaw(float32 *yawOut)
{
	if(!gvk.firstPersonActive || yawOut == nil)
		return 0;
	float32 yaw = gvk.headYaw-gvk.fpLatchedHeadYaw;
	const float32 pi = 3.14159265358979323846f;
	const float32 twoPi = 2.0f*pi;
	while(yaw >= pi) yaw -= twoPi;
	while(yaw < -pi) yaw += twoPi;
	*yawOut = yaw;
	return 1;
}

void
setClearColour(uint8 red, uint8 green, uint8 blue)
{
	gvk.clearColour[0] = red / 255.0f;
	gvk.clearColour[1] = green / 255.0f;
	gvk.clearColour[2] = blue / 255.0f;
}

void
setPostFx(uint32 mode, uint32 red, uint32 green, uint32 blue,
          float32 intensity)
{
	if(red > 255) red = 255;
	if(green > 255) green = 255;
	if(blue > 255) blue = 255;
	if(intensity < 0.0f) intensity = 0.0f;

	const float32 scale = intensity / 255.0f;
	gvk.postFxConstants.blurColour[0] = red * scale;
	gvk.postFxConstants.blurColour[1] = green * scale;
	gvk.postFxConstants.blurColour[2] = blue * scale;
	// POSTFX_NORMAL uses the same fixed blend alpha on the PC renderer.
	gvk.postFxConstants.blurColour[3] = 30.0f / 255.0f;
	// 0 = pass-through, 1 = Vice City colour treatment, 2 = an opaque
	// transition mask used while the cutscene world/camera is being rebuilt.
	gvk.postFxConstants.mode[0] = mode <= 2 ? mode : 0u;
	gvk.postFxConstants.mode[2] = gvk.width;
	gvk.postFxConstants.mode[3] = gvk.height;
}

void
setFxaaEnabled(bool32 enabled)
{
	gvk.postFxConstants.mode[1] = enabled ? 1u : 0u;
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
		commandInfo.commandBufferCount = NUM_FRAME_CONTEXTS;
		VkCommandBuffer commandBuffers[NUM_FRAME_CONTEXTS] = {};
		if(vkAllocateCommandBuffers(gvk.device, &commandInfo,
		                            commandBuffers) != VK_SUCCESS)
			return 0;
		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
			gvk.frames[i].commandBuffer = commandBuffers[i];

		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
			if(vkCreateFence(gvk.device, &fenceInfo, nil,
			                 &gvk.frames[i].fence) != VK_SUCCESS)
				return 0;

		// Timestamp queries are optional.  Check the actual queue family rather
		// than assuming that every Vulkan graphics queue exposes timestamp bits.
		uint32 queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(
			gvk.physicalDevice, &queueFamilyCount, nil);
		if(gvk.queueFamilyIndex < queueFamilyCount){
			VkQueueFamilyProperties *queueFamilies =
				new VkQueueFamilyProperties[queueFamilyCount];
			vkGetPhysicalDeviceQueueFamilyProperties(
				gvk.physicalDevice, &queueFamilyCount, queueFamilies);
			gvk.frameTimestampValidBits =
				queueFamilies[gvk.queueFamilyIndex].timestampValidBits;
			delete[] queueFamilies;
		}
		if(gvk.deviceProperties.limits.timestampComputeAndGraphics &&
		   gvk.frameTimestampValidBits != 0){
			VkQueryPoolCreateInfo queryInfo = {};
			queryInfo.sType =
				VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
			queryInfo.queryCount = 2;
			bool32 allQueryPoolsCreated = 1;
			for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
				if(vkCreateQueryPool(gvk.device, &queryInfo, nil,
				                     &gvk.frames[i].timestampQueryPool) !=
				   VK_SUCCESS){
					allQueryPoolsCreated = 0;
					break;
				}
			if(allQueryPoolsCreated)
				VKLOG("GPU frame timestamps enabled (%u valid bits, "
				      "%.3f ns/tick)",
				      gvk.frameTimestampValidBits,
				      gvk.deviceProperties.limits.timestampPeriod);
			else{
				for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
					if(gvk.frames[i].timestampQueryPool)
						vkDestroyQueryPool(
							gvk.device,
							gvk.frames[i].timestampQueryPool, nil);
					gvk.frames[i].timestampQueryPool = VK_NULL_HANDLE;
				}
				VKLOG("GPU frame timestamps unavailable: query pool "
				      "creation failed");
			}
		}else
			VKLOG("GPU frame timestamps unavailable on queue family %u",
			      gvk.queueFamilyIndex);

		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
			if(!createDepthBuffer(gvk.frames[i]))
				return 0;
			if(!createSceneColour(gvk.frames[i]))
				return 0;
		}
		if(!createRenderPass())
			return 0;
		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
			if(!createSceneFramebuffer(gvk.frames[i]))
				return 0;
		if(!createPostFxResources())
			return 0;

		resetRenderState();
		// The pipeline cache builds against gvk.renderPass, so it can only be
		// created once the render pass exists.
		if(!stateInit())
			return 0;
		gvk.activeFrame = 0;
		gvk.nextFrame = 0;
		gvk.lastSubmittedFrame = 0;
		gvk.hasSubmittedFrame = 0;
		gvk.frameCommands = gvk.frames[0].commandBuffer;
		setStateFrame(0);
		gvk.initialised = 1;
		VKLOG("%u Vulkan frame contexts enabled", NUM_FRAME_CONTEXTS);
		return 1;
	}

	case DEVICECLOSE:
		if(gvk.device != VK_NULL_HANDLE){
			vkDeviceWaitIdle(gvk.device);
			for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
				flushRetired(i);
			stateShutdown();
			for(uint32 frameIndex = 0;
			    frameIndex < NUM_FRAME_CONTEXTS; frameIndex++){
				FrameContext &frame = gvk.frames[frameIndex];
				if(frame.sceneFramebuffer)
					vkDestroyFramebuffer(
						gvk.device, frame.sceneFramebuffer, nil);
				for(uint32 i = 0; i < frame.numFramebuffers; i++)
					if(frame.framebuffers[i])
						vkDestroyFramebuffer(
							gvk.device, frame.framebuffers[i], nil);
			}
			if(gvk.postFxPipeline) vkDestroyPipeline(gvk.device, gvk.postFxPipeline, nil);
			if(gvk.postFxPipelineLayout) vkDestroyPipelineLayout(gvk.device, gvk.postFxPipelineLayout, nil);
			if(gvk.postFxDescriptorPool) vkDestroyDescriptorPool(gvk.device, gvk.postFxDescriptorPool, nil);
			if(gvk.postFxDescriptorLayout) vkDestroyDescriptorSetLayout(gvk.device, gvk.postFxDescriptorLayout, nil);
			if(gvk.postFxSampler) vkDestroySampler(gvk.device, gvk.postFxSampler, nil);
			if(gvk.postFxRenderPass) vkDestroyRenderPass(gvk.device, gvk.postFxRenderPass, nil);
			if(gvk.renderPass) vkDestroyRenderPass(gvk.device, gvk.renderPass, nil);
			for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
				FrameContext &frame = gvk.frames[i];
				if(frame.sceneColourView)
					vkDestroyImageView(gvk.device,
					                   frame.sceneColourView, nil);
				if(frame.sceneColourImage)
					vkDestroyImage(gvk.device,
					               frame.sceneColourImage, nil);
				if(frame.sceneColourMemory)
					vkFreeMemory(gvk.device,
					             frame.sceneColourMemory, nil);
				if(frame.depthView)
					vkDestroyImageView(gvk.device, frame.depthView, nil);
				if(frame.depthImage)
					vkDestroyImage(gvk.device, frame.depthImage, nil);
				if(frame.depthMemory)
					vkFreeMemory(gvk.device, frame.depthMemory, nil);
				if(frame.timestampQueryPool)
					vkDestroyQueryPool(gvk.device,
					                   frame.timestampQueryPool, nil);
				if(frame.fence)
					vkDestroyFence(gvk.device, frame.fence, nil);
			}
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
