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

#include <vector>

static const uint32_t kWorldVertSpv[] =
#include "rw_world_vert.h"
;
static const uint32_t kWorldFragSpv[] =
#include "rw_world_frag.h"
;
static const uint32_t kIm2DVertSpv[] =
#include "rw_im2d_vert.h"
;
static const uint32_t kIm2DFragSpv[] =
#include "rw_im2d_frag.h"
;
static const uint32_t kIm3DVertSpv[] =
#include "rw_im3d_vert.h"
;
static const uint32_t kSkinVertSpv[] =
#include "rw_skin_vert.h"
;

namespace rw {
namespace vulkan {

namespace {

struct PipelineEntry
{
	uint64 key;
	VkPipeline pipeline;
};

// A pipeline asked for during a frame, to be built before the next render pass
// opens. The render state is captured because it is what the build reads.
struct PendingPipeline
{
	uint64 key;
	uint32 shader;
	VkPrimitiveTopology topology;
	RenderState state;
};

struct StateFrame
{
	VkDescriptorSet sceneDescriptor;
	VkBuffer sceneBuffer;
	VkDeviceMemory sceneMemory;
	void *sceneMapped;

	VkDescriptorSet boneDescriptor;
	VkBuffer dynamicBuffer;
	VkDeviceMemory dynamicMemory;
	uint8 *dynamicMapped;
	VkDeviceSize dynamicCapacity;
	VkDeviceSize dynamicOffset;
	bool32 dynamicOverflowReported;
};

struct State
{
	VkDescriptorSetLayout sceneLayout;
	VkDescriptorSetLayout textureLayout;
	VkDescriptorSetLayout boneLayout;
	VkPipelineLayout pipelineLayout;
	VkDescriptorPool descriptorPool;

	VkDeviceSize boneAlignment;

	SceneData scene;
	StateFrame frames[NUM_FRAME_CONTEXTS];
	uint32 activeFrame;

	VkShaderModule modules[SHADER_COUNT][2];	// [variant][0=vert,1=frag]

	std::vector<PipelineEntry> pipelines;
	std::vector<PendingPipeline> pending;
	uint64 lastPipelineKey;
	VkPipeline lastPipeline;
	bool32 lastPipelineValid;

	// Sampler cache, indexed by the packed sampler key.
	VkSampler samplers[64];

	// 1x1 white texture bound for untextured materials, so no shader variant
	// or descriptor gymnastics are needed for the untextured case.
	VkImage whiteImage;
	VkDeviceMemory whiteMemory;
	VkImageView whiteView;
	VkDescriptorSet whiteDescriptor;

	bool32 initialised;
};

State gs;

const VkDeviceSize DYNAMIC_CAPACITY = 8 * 1024 * 1024;
const uint32 MAX_TEXTURE_DESCRIPTORS = 4096;

VkBlendFactor
blendFactor(uint32 rwBlend)
{
	switch(rwBlend){
	case BLENDZERO:          return VK_BLEND_FACTOR_ZERO;
	case BLENDONE:           return VK_BLEND_FACTOR_ONE;
	case BLENDSRCCOLOR:      return VK_BLEND_FACTOR_SRC_COLOR;
	case BLENDINVSRCCOLOR:   return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case BLENDSRCALPHA:      return VK_BLEND_FACTOR_SRC_ALPHA;
	case BLENDINVSRCALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case BLENDDESTALPHA:     return VK_BLEND_FACTOR_DST_ALPHA;
	case BLENDINVDESTALPHA:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case BLENDDESTCOLOR:     return VK_BLEND_FACTOR_DST_COLOR;
	case BLENDINVDESTCOLOR:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case BLENDSRCALPHASAT:   return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	default:                 return VK_BLEND_FACTOR_ONE;
	}
}

// The same factor for the alpha channel. A colour-derived factor has an alpha
// counterpart -- SRC_COLOR becomes SRC_ALPHA -- and using the colour form on
// the alpha channel produces a different result. Mirrors the pairs the D3D12
// backend sets in getPipelineBlend.
VkBlendFactor
alphaBlendFactor(uint32 rwBlend)
{
	switch(rwBlend){
	case BLENDSRCCOLOR:      return VK_BLEND_FACTOR_SRC_ALPHA;
	case BLENDINVSRCCOLOR:   return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case BLENDDESTCOLOR:     return VK_BLEND_FACTOR_DST_ALPHA;
	case BLENDINVDESTCOLOR:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	default:                 return blendFactor(rwBlend);
	}
}

VkCullModeFlags
cullMode(uint32 rwCull)
{
	switch(rwCull){
	case CULLNONE:  return VK_CULL_MODE_NONE;
	case CULLBACK:  return VK_CULL_MODE_BACK_BIT;
	case CULLFRONT: return VK_CULL_MODE_FRONT_BIT;
	default:        return VK_CULL_MODE_NONE;
	}
}

VkFilter
minMagFilter(uint32 rwFilter)
{
	switch(rwFilter){
	case Texture::NEAREST:
	case Texture::MIPNEAREST:
		return VK_FILTER_NEAREST;
	default:
		return VK_FILTER_LINEAR;
	}
}

VkSamplerMipmapMode
mipmapMode(uint32 rwFilter)
{
	switch(rwFilter){
	case Texture::MIPNEAREST:
	case Texture::LINEARMIPNEAREST:
		return VK_SAMPLER_MIPMAP_MODE_NEAREST;
	default:
		return VK_SAMPLER_MIPMAP_MODE_LINEAR;
	}
}

VkSamplerAddressMode
addressMode(uint32 rwAddress)
{
	switch(rwAddress){
	case Texture::MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case Texture::CLAMP:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case Texture::BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	default:              return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
}

// filter (3 bits) | addressU (2) | addressV (2)
uint32
makeSamplerKey(uint32 filter, uint32 addressU, uint32 addressV)
{
	const uint32 f = (filter - 1) & 0x7;
	const uint32 u = (addressU - 1) & 0x3;
	const uint32 v = (addressV - 1) & 0x3;
	return (f << 4) | (u << 2) | v;
}

VkSampler
getSampler(uint32 key, uint32 filter, uint32 addressU, uint32 addressV)
{
	if(key >= 64)
		key = 0;
	if(gs.samplers[key] != VK_NULL_HANDLE)
		return gs.samplers[key];

	VkSamplerCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter = minMagFilter(filter);
	info.minFilter = minMagFilter(filter);
	info.mipmapMode = mipmapMode(filter);
	info.addressModeU = addressMode(addressU);
	info.addressModeV = addressMode(addressV);
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.maxLod = VK_LOD_CLAMP_NONE;
	if(gvk.deviceFeatures.samplerAnisotropy){
		info.anisotropyEnable = VK_TRUE;
		// Vice City's texture set is low resolution; past 4x the sampling cost
		// on a tiler buys nothing visible.
		info.maxAnisotropy = 4.0f;
		if(info.maxAnisotropy > gvk.deviceProperties.limits.maxSamplerAnisotropy)
			info.maxAnisotropy = gvk.deviceProperties.limits.maxSamplerAnisotropy;
	}
	if(vkCreateSampler(gvk.device, &info, nil, &gs.samplers[key]) != VK_SUCCESS){
		VKERR("vkCreateSampler failed for key %u", key);
		return VK_NULL_HANDLE;
	}
	return gs.samplers[key];
}

uint64
makePipelineKey(uint32 shader, VkPrimitiveTopology topology)
{
	const bool32 blend = gstate.vertexAlphaEnabled;
	const bool32 alphaTest = gstate.alphaTestFunction != ALPHAALWAYS;
	const bool32 strictIm2D =
		shader == SHADER_IM2D && getImmediate2DStrictDepth();
	uint64 key = 0;
	key |= (uint64)(shader & 0x7);
	key |= (uint64)(topology & 0xF) << 3;
	key |= (uint64)(blend ? 1 : 0) << 7;
	key |= (uint64)(gstate.srcBlend & 0xF) << 8;
	key |= (uint64)(gstate.dstBlend & 0xF) << 12;
	key |= (uint64)(gstate.zTestEnabled ? 1 : 0) << 16;
	key |= (uint64)(gstate.zWriteEnabled ? 1 : 0) << 17;
	key |= (uint64)(gstate.cullMode & 0x3) << 18;
	key |= (uint64)(alphaTest ? 1 : 0) << 20;
	key |= (uint64)(strictIm2D ? 1 : 0) << 21;
	return key;
}

VkShaderModule
createModule(const uint32_t *code, size_t byteSize)
{
	VkShaderModuleCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = byteSize;
	info.pCode = code;
	VkShaderModule module = VK_NULL_HANDLE;
	if(vkCreateShaderModule(gvk.device, &info, nil, &module) != VK_SUCCESS)
		VKERR("vkCreateShaderModule failed");
	return module;
}

bool32
createWhiteTexture(void)
{
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.extent.width = 1;
	imageInfo.extent.height = 1;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil, &gs.whiteImage) != VK_SUCCESS)
		return 0;

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, gs.whiteImage, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return 0;
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, &gs.whiteMemory) != VK_SUCCESS)
		return 0;
	vkBindImageMemory(gvk.device, gs.whiteImage, gs.whiteMemory, 0);

	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	if(!createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 &staging, &stagingMemory))
		return 0;
	void *mapped = nil;
	vkMapMemory(gvk.device, stagingMemory, 0, 4, 0, &mapped);
	const uint8 white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	memcpy(mapped, white, 4);
	vkUnmapMemory(gvk.device, stagingMemory);

	VkCommandBuffer commandBuffer = beginOneShot();
	transitionImageLayout(commandBuffer, gs.whiteImage, VK_IMAGE_ASPECT_COLOR_BIT, 1,
	                      VK_IMAGE_LAYOUT_UNDEFINED,
	                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = 1;
	region.imageExtent.height = 1;
	region.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(commandBuffer, staging, gs.whiteImage,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	transitionImageLayout(commandBuffer, gs.whiteImage, VK_IMAGE_ASPECT_COLOR_BIT, 1,
	                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	endOneShot(commandBuffer);

	vkDestroyBuffer(gvk.device, staging, nil);
	vkFreeMemory(gvk.device, stagingMemory, nil);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = gs.whiteImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	if(vkCreateImageView(gvk.device, &viewInfo, nil, &gs.whiteView) != VK_SUCCESS)
		return 0;

	VkDescriptorSetAllocateInfo setInfo = {};
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setInfo.descriptorPool = gs.descriptorPool;
	setInfo.descriptorSetCount = 1;
	setInfo.pSetLayouts = &gs.textureLayout;
	if(vkAllocateDescriptorSets(gvk.device, &setInfo, &gs.whiteDescriptor) != VK_SUCCESS)
		return 0;

	VkDescriptorImageInfo imageDescriptor = {};
	imageDescriptor.sampler = getSampler(makeSamplerKey(Texture::LINEAR,
	                                                    Texture::WRAP, Texture::WRAP),
	                                     Texture::LINEAR, Texture::WRAP, Texture::WRAP);
	imageDescriptor.imageView = gs.whiteView;
	imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet write = {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = gs.whiteDescriptor;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageDescriptor;
	vkUpdateDescriptorSets(gvk.device, 1, &write, 0, nil);
	return 1;
}

} // namespace

// ---------------------------------------------------------------------------

bool32
stateInit(void)
{
	if(gs.initialised)
		return 1;
	memset(&gs, 0, sizeof(gs));

	VkDescriptorSetLayoutBinding sceneBinding = {};
	sceneBinding.binding = 0;
	sceneBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sceneBinding.descriptorCount = 1;
	sceneBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo sceneLayoutInfo = {};
	sceneLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	sceneLayoutInfo.bindingCount = 1;
	sceneLayoutInfo.pBindings = &sceneBinding;
	if(vkCreateDescriptorSetLayout(gvk.device, &sceneLayoutInfo, nil,
	                               &gs.sceneLayout) != VK_SUCCESS)
		return 0;

	VkDescriptorSetLayoutBinding textureBinding = {};
	textureBinding.binding = 0;
	textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	textureBinding.descriptorCount = 1;
	textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo textureLayoutInfo = {};
	textureLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	textureLayoutInfo.bindingCount = 1;
	textureLayoutInfo.pBindings = &textureBinding;
	if(vkCreateDescriptorSetLayout(gvk.device, &textureLayoutInfo, nil,
	                               &gs.textureLayout) != VK_SUCCESS)
		return 0;

	// Bone matrices for the skin pipeline. Dynamic so a draw selects its block
	// with an offset instead of needing a descriptor set of its own.
	VkDescriptorSetLayoutBinding boneBinding = {};
	boneBinding.binding = 0;
	boneBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	boneBinding.descriptorCount = 1;
	boneBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutCreateInfo boneLayoutInfo = {};
	boneLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	boneLayoutInfo.bindingCount = 1;
	boneLayoutInfo.pBindings = &boneBinding;
	if(vkCreateDescriptorSetLayout(gvk.device, &boneLayoutInfo, nil,
	                               &gs.boneLayout) != VK_SUCCESS)
		return 0;

	// Every pipeline declares all three sets so they can share one layout;
	// only the skin pipeline actually binds set 2.
	const VkDescriptorSetLayout layouts[3] = {
		gs.sceneLayout, gs.textureLayout, gs.boneLayout
	};
	VkPushConstantRange pushRange = {};
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.size = sizeof(PushConstants);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 3;
	pipelineLayoutInfo.pSetLayouts = layouts;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if(vkCreatePipelineLayout(gvk.device, &pipelineLayoutInfo, nil,
	                          &gs.pipelineLayout) != VK_SUCCESS)
		return 0;

	VkDescriptorPoolSize poolSizes[3] = {};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[0].descriptorCount = 4;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[1].descriptorCount =
		MAX_TEXTURE_DESCRIPTORS*NUM_FRAME_CONTEXTS + 1;
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	poolSizes[2].descriptorCount = NUM_FRAME_CONTEXTS;

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets =
		MAX_TEXTURE_DESCRIPTORS*NUM_FRAME_CONTEXTS + 16;
	poolInfo.poolSizeCount = 3;
	poolInfo.pPoolSizes = poolSizes;
	if(vkCreateDescriptorPool(gvk.device, &poolInfo, nil, &gs.descriptorPool) != VK_SUCCESS)
		return 0;

	for(uint32 frame = 0; frame < NUM_FRAME_CONTEXTS; frame++){
		StateFrame &sf = gs.frames[frame];
		if(!createBuffer(sizeof(SceneData),
		                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &sf.sceneBuffer, &sf.sceneMemory))
			return 0;
		if(vkMapMemory(gvk.device, sf.sceneMemory, 0,
		               sizeof(SceneData), 0,
		               &sf.sceneMapped) != VK_SUCCESS)
			return 0;

		VkDescriptorSetAllocateInfo sceneSetInfo = {};
		sceneSetInfo.sType =
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		sceneSetInfo.descriptorPool = gs.descriptorPool;
		sceneSetInfo.descriptorSetCount = 1;
		sceneSetInfo.pSetLayouts = &gs.sceneLayout;
		if(vkAllocateDescriptorSets(gvk.device, &sceneSetInfo,
		                            &sf.sceneDescriptor) != VK_SUCCESS)
			return 0;

		VkDescriptorBufferInfo sceneBufferInfo =
			{ sf.sceneBuffer, 0, sizeof(SceneData) };
		VkWriteDescriptorSet sceneWrite = {};
		sceneWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		sceneWrite.dstSet = sf.sceneDescriptor;
		sceneWrite.dstBinding = 0;
		sceneWrite.descriptorCount = 1;
		sceneWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		sceneWrite.pBufferInfo = &sceneBufferInfo;
		vkUpdateDescriptorSets(gvk.device, 1, &sceneWrite, 0, nil);
	}

	gs.modules[SHADER_WORLD][0] = createModule(kWorldVertSpv, sizeof(kWorldVertSpv));
	gs.modules[SHADER_WORLD][1] = createModule(kWorldFragSpv, sizeof(kWorldFragSpv));
	gs.modules[SHADER_IM2D][0] = createModule(kIm2DVertSpv, sizeof(kIm2DVertSpv));
	gs.modules[SHADER_IM2D][1] = createModule(kIm2DFragSpv, sizeof(kIm2DFragSpv));
	gs.modules[SHADER_IM3D][0] = createModule(kIm3DVertSpv, sizeof(kIm3DVertSpv));
	// im3d reuses the world fragment shader: same texturing, colour and fog.
	gs.modules[SHADER_IM3D][1] = gs.modules[SHADER_WORLD][1];
	gs.modules[SHADER_SKIN][0] = createModule(kSkinVertSpv, sizeof(kSkinVertSpv));
	// Skinning only changes where the vertex ends up, so the world fragment
	// shader applies unchanged here too.
	gs.modules[SHADER_SKIN][1] = gs.modules[SHADER_WORLD][1];

	// Vertex/index/immediate data and bone blocks are private to each frame
	// context, so the CPU never rewrites bytes an earlier submit still reads.
	for(uint32 frame = 0; frame < NUM_FRAME_CONTEXTS; frame++){
		StateFrame &sf = gs.frames[frame];
		if(!createBuffer(DYNAMIC_CAPACITY,
		                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
		                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &sf.dynamicBuffer, &sf.dynamicMemory))
			return 0;
		if(vkMapMemory(gvk.device, sf.dynamicMemory, 0,
		               DYNAMIC_CAPACITY, 0,
		               (void**)&sf.dynamicMapped) != VK_SUCCESS)
			return 0;
		sf.dynamicCapacity = DYNAMIC_CAPACITY;

		VkDescriptorSetAllocateInfo boneSetInfo = {};
		boneSetInfo.sType =
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		boneSetInfo.descriptorPool = gs.descriptorPool;
		boneSetInfo.descriptorSetCount = 1;
		boneSetInfo.pSetLayouts = &gs.boneLayout;
		if(vkAllocateDescriptorSets(gvk.device, &boneSetInfo,
		                            &sf.boneDescriptor) != VK_SUCCESS)
			return 0;

		VkDescriptorBufferInfo boneBufferInfo = {
			sf.dynamicBuffer, 0,
			RW_MAX_BONES*16*sizeof(float32)
		};
		VkWriteDescriptorSet boneWrite = {};
		boneWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		boneWrite.dstSet = sf.boneDescriptor;
		boneWrite.dstBinding = 0;
		boneWrite.descriptorCount = 1;
		boneWrite.descriptorType =
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		boneWrite.pBufferInfo = &boneBufferInfo;
		vkUpdateDescriptorSets(gvk.device, 1, &boneWrite, 0, nil);
	}

	gs.boneAlignment = gvk.deviceProperties.limits.minUniformBufferOffsetAlignment;
	if(gs.boneAlignment == 0)
		gs.boneAlignment = 256;

	if(!createWhiteTexture())
		return 0;

	gs.initialised = 1;

	// Build the pipelines here rather than on first use.
	//
	// Two reasons. A compile is slow, and doing it mid-frame costs a dropped
	// frame in the headset. More usefully right now: a compile that hangs is
	// then reproducible at startup over adb, instead of only once someone is
	// wearing the headset and the game has reached a menu.
	{
		RenderState saved = gstate;
		gstate.vertexAlphaEnabled = 0;
		gstate.srcBlend = BLENDSRCALPHA;
		gstate.dstBlend = BLENDINVSRCALPHA;
		gstate.zTestEnabled = 1;
		gstate.zWriteEnabled = 1;
		gstate.cullMode = CULLNONE;
		gstate.alphaTestFunction = ALPHAALWAYS;
		for(uint32 shader = 0; shader < SHADER_COUNT; shader++){
			VKLOG("warming shader %u", shader);
			getPipeline(shader, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
		}

		// Every combination the fixed-function state can produce. Guessing one
		// at a time was not converging, and the whole matrix costs well under
		// a second: 16 state combinations across the shader variants, at
		// roughly 15 ms each, once, at startup.
		for(int blend = 0; blend < 2; blend++)
		for(int depth = 0; depth < 2; depth++)
		for(int alphaTest = 0; alphaTest < 2; alphaTest++)
		for(int cull = 0; cull < 2; cull++){
			gstate.vertexAlphaEnabled = blend;
			gstate.zTestEnabled = depth;
			gstate.zWriteEnabled = depth;
			gstate.cullMode = cull ? CULLBACK : CULLNONE;
			gstate.alphaTestFunction = alphaTest ? ALPHAGREATEREQUAL : ALPHAALWAYS;
			for(uint32 shader = 0; shader < SHADER_COUNT; shader++){
				VKLOG("warm blend=%d depth=%d alpha=%d cull=%d shader=%u",
				      blend, depth, alphaTest, cull, shader);
				getPipeline(shader, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			}
		}

		// Two extra Im2D pipelines reproduce the radar's depth mask. Warm them
		// before the first frame so opening the HUD never causes a dropped draw.
		setImmediate2DStrictDepth(1);
		gstate.vertexAlphaEnabled = 1;
		gstate.srcBlend = BLENDZERO;
		gstate.dstBlend = BLENDONE;
		gstate.zTestEnabled = 0;
		gstate.zWriteEnabled = 1;
		gstate.cullMode = CULLNONE;
		gstate.alphaTestFunction = ALPHAALWAYS;
		getPipeline(SHADER_IM2D, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

		gstate.vertexAlphaEnabled = 0;
		gstate.srcBlend = BLENDSRCALPHA;
		gstate.dstBlend = BLENDINVSRCALPHA;
		gstate.zTestEnabled = 1;
		gstate.zWriteEnabled = 0;
		getPipeline(SHADER_IM2D, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
		setImmediate2DStrictDepth(0);

		gstate = saved;
		VKLOG("pipeline warm-up finished");
	}

	// Defaults that keep geometry visible before the game supplies anything.
	// A zeroed block means ambient 0 and no lights, which renders the world
	// black, and an identity-less im2dTransform collapses the HUD to a point.
	gs.scene.ambient[0] = gs.scene.ambient[1] = gs.scene.ambient[2] = 1.0f;
	gs.scene.ambient[3] = 1.0f;
	gs.scene.lightCount[0] = 0.0f;
	gs.scene.fogParams[3] = 0.0f;	// disabled
	gs.scene.im2dTransform[0] = gs.scene.im2dTransform[5] =
	gs.scene.im2dTransform[10] = gs.scene.im2dTransform[15] = 1.0f;

	VKLOG("pipeline state initialised");
	return 1;
}

void
stateShutdown(void)
{
	if(!gs.initialised)
		return;
	vkDeviceWaitIdle(gvk.device);

	for(size_t i = 0; i < gs.pipelines.size(); i++)
		vkDestroyPipeline(gvk.device, gs.pipelines[i].pipeline, nil);
	gs.pipelines.clear();

	// SHADER_IM3D's fragment module aliases SHADER_WORLD's, so it must not be
	// destroyed twice.
	gs.modules[SHADER_IM3D][1] = VK_NULL_HANDLE;
	// Several variants share a fragment module -- im3d and skin both reuse the
	// world one -- so destroy each distinct handle exactly once. Walking the
	// array blindly hands the same handle to the driver more than once.
	for(int v = 0; v < SHADER_COUNT; v++)
		for(int s = 0; s < 2; s++){
			VkShaderModule module = gs.modules[v][s];
			if(module == VK_NULL_HANDLE)
				continue;
			vkDestroyShaderModule(gvk.device, module, nil);
			for(int ov = v; ov < SHADER_COUNT; ov++)
				for(int os = 0; os < 2; os++)
					if(gs.modules[ov][os] == module)
						gs.modules[ov][os] = VK_NULL_HANDLE;
		}

	for(int i = 0; i < 64; i++)
		if(gs.samplers[i])
			vkDestroySampler(gvk.device, gs.samplers[i], nil);

	if(gs.whiteView) vkDestroyImageView(gvk.device, gs.whiteView, nil);
	if(gs.whiteImage) vkDestroyImage(gvk.device, gs.whiteImage, nil);
	if(gs.whiteMemory) vkFreeMemory(gvk.device, gs.whiteMemory, nil);

	for(uint32 frame = 0; frame < NUM_FRAME_CONTEXTS; frame++){
		StateFrame &sf = gs.frames[frame];
		if(sf.dynamicMapped)
			vkUnmapMemory(gvk.device, sf.dynamicMemory);
		if(sf.dynamicBuffer)
			vkDestroyBuffer(gvk.device, sf.dynamicBuffer, nil);
		if(sf.dynamicMemory)
			vkFreeMemory(gvk.device, sf.dynamicMemory, nil);

		if(sf.sceneMapped)
			vkUnmapMemory(gvk.device, sf.sceneMemory);
		if(sf.sceneBuffer)
			vkDestroyBuffer(gvk.device, sf.sceneBuffer, nil);
		if(sf.sceneMemory)
			vkFreeMemory(gvk.device, sf.sceneMemory, nil);
	}

	if(gs.descriptorPool) vkDestroyDescriptorPool(gvk.device, gs.descriptorPool, nil);
	if(gs.pipelineLayout) vkDestroyPipelineLayout(gvk.device, gs.pipelineLayout, nil);
	if(gs.boneLayout) vkDestroyDescriptorSetLayout(gvk.device, gs.boneLayout, nil);
	if(gs.textureLayout) vkDestroyDescriptorSetLayout(gvk.device, gs.textureLayout, nil);
	if(gs.sceneLayout) vkDestroyDescriptorSetLayout(gvk.device, gs.sceneLayout, nil);

	memset(&gs, 0, sizeof(gs));
}

VkPipelineLayout
getPipelineLayout(void)
{
	return gs.pipelineLayout;
}

VkDescriptorSet
getSceneDescriptor(void)
{
	return gs.frames[gs.activeFrame].sceneDescriptor;
}

VkDescriptorSet
getBoneDescriptor(void)
{
	return gs.frames[gs.activeFrame].boneDescriptor;
}

VkDeviceSize
getBoneBlockAlignment(void)
{
	return gs.boneAlignment;
}

SceneData *
getSceneData(void)
{
	return &gs.scene;
}

void
setStateFrame(uint32 frame)
{
	gs.activeFrame = frame % NUM_FRAME_CONTEXTS;
}

void
uploadSceneData(void)
{
	StateFrame &sf = gs.frames[gs.activeFrame];
	if(sf.sceneMapped != nil)
		memcpy(sf.sceneMapped, &gs.scene, sizeof(SceneData));
}

VkPipeline
getPipeline(uint32 shader, VkPrimitiveTopology topology)
{
	if(!gs.initialised || shader >= SHADER_COUNT)
		return VK_NULL_HANDLE;

	const uint64 key = makePipelineKey(shader, topology);
	if(gs.lastPipelineValid && gs.lastPipelineKey == key)
		return gs.lastPipeline;

	for(size_t i = 0; i < gs.pipelines.size(); i++)
		if(gs.pipelines[i].key == key){
			gs.lastPipelineKey = key;
			gs.lastPipeline = gs.pipelines[i].pipeline;
			gs.lastPipelineValid = 1;
			return gs.pipelines[i].pipeline;
		}

	// Never compile inside a render pass.
	//
	// vkCreateGraphicsPipelines does not return on this Adreno driver when it
	// is called while a command buffer is recording with an active render
	// pass. The same pipeline, same state, same shaders compiles in about 15 ms
	// at startup -- all 48 fixed-function combinations do -- so it is the
	// context that matters, not the pipeline.
	//
	// A miss during a frame therefore records the request and skips the draw.
	// beginFrame compiles it before opening the next render pass, so the state
	// costs one dropped draw the first time it is ever seen and is cached from
	// then on.
	if(gvk.inFrame){
		for(size_t i = 0; i < gs.pending.size(); i++)
			if(gs.pending[i].key == key)
				return VK_NULL_HANDLE;
		PendingPipeline pending;
		pending.key = key;
		pending.shader = shader;
		pending.topology = topology;
		pending.state = gstate;
		gs.pending.push_back(pending);
		return VK_NULL_HANDLE;
	}

	VK_CHECKPOINT("vk/createPipeline");
	VKLOG("compiling pipeline %zu (shader %u, topology %u, key %llu)",
	      gs.pipelines.size() + 1, shader, (unsigned)topology,
	      (unsigned long long)key);

	const bool32 alphaTest = gstate.alphaTestFunction != ALPHAALWAYS;
	const int32 alphaTestConstant = alphaTest ? 1 : 0;
	VkSpecializationMapEntry specEntry = { 0, 0, sizeof(int32) };
	VkSpecializationInfo specInfo = {};
	specInfo.mapEntryCount = 1;
	specInfo.pMapEntries = &specEntry;
	specInfo.dataSize = sizeof(int32);
	specInfo.pData = &alphaTestConstant;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = gs.modules[shader][0];
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = gs.modules[shader][1];
	stages[1].pName = "main";
	stages[1].pSpecializationInfo = &specInfo;

	// Vertex layouts, one per shader variant.
	VkVertexInputBindingDescription binding = {};
	binding.binding = 0;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attributes[6] = {};
	uint32 attributeCount = 0;

	if(shader == SHADER_WORLD){
		binding.stride = sizeof(WorldVertex);
		attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,
		                  (uint32)offsetof(WorldVertex, position) };
		attributes[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,
		                  (uint32)offsetof(WorldVertex, normal) };
		attributes[2] = { 2, 0, VK_FORMAT_R8G8B8A8_UNORM,
		                  (uint32)offsetof(WorldVertex, colour) };
		attributes[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT,
		                  (uint32)offsetof(WorldVertex, texCoord) };
		attributeCount = 4;
	}else if(shader == SHADER_SKIN){
		binding.stride = sizeof(SkinVertex);
		attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,
		                  (uint32)offsetof(SkinVertex, position) };
		attributes[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,
		                  (uint32)offsetof(SkinVertex, normal) };
		attributes[2] = { 2, 0, VK_FORMAT_R8G8B8A8_UNORM,
		                  (uint32)offsetof(SkinVertex, colour) };
		attributes[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT,
		                  (uint32)offsetof(SkinVertex, texCoord) };
		attributes[4] = { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
		                  (uint32)offsetof(SkinVertex, weights) };
		// UINT, not UNORM: the shader indexes the bone array with these.
		attributes[5] = { 5, 0, VK_FORMAT_R8G8B8A8_UINT,
		                  (uint32)offsetof(SkinVertex, boneIndices) };
		attributeCount = 6;
	}else if(shader == SHADER_IM2D){
		binding.stride = sizeof(Im2DVertex);
		attributes[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
		                  (uint32)offsetof(Im2DVertex, x) };
		attributes[1] = { 1, 0, VK_FORMAT_R8G8B8A8_UNORM,
		                  (uint32)offsetof(Im2DVertex, color) };
		attributes[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,
		                  (uint32)offsetof(Im2DVertex, u) };
		attributeCount = 3;
	}else{
		binding.stride = sizeof(Im3DVertex);
		attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,
		                  (uint32)offsetof(Im3DVertex, position) };
		attributes[1] = { 1, 0, VK_FORMAT_R8G8B8A8_UNORM,
		                  (uint32)offsetof(Im3DVertex, color) };
		attributes[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,
		                  (uint32)offsetof(Im3DVertex, u) };
		attributeCount = 3;
	}

	VkPipelineVertexInputStateCreateInfo vertexInput = {};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = attributeCount;
	vertexInput.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = topology;

	VkPipelineViewportStateCreateInfo viewportState = {};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasteriser = {};
	rasteriser.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
	rasteriser.cullMode = shader == SHADER_IM2D ? VK_CULL_MODE_NONE
	                                            : cullMode(gstate.cullMode);
	// RenderWare winds counter-clockwise. The original CLOCKWISE choice here
	// was tuned by eye and looked plausible only because closed buildings
	// render near-identically inside out; what it actually did was cull every
	// road when seen from above and every wall facing the viewer, leaving
	// interiors visible. Counter-clockwise is the correct winding for this
	// projection.
	rasteriser.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasteriser.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = {};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil = {};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = gstate.zTestEnabled ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = gstate.zWriteEnabled ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

	if(shader == SHADER_IM2D){
		if(getImmediate2DStrictDepth()){
			// The radar first writes its invisible outside-circle mask, then
			// draws equal-depth tiles with strict LESS so only the unmasked
			// circle survives.
			if(gstate.zWriteEnabled){
				// Vulkan suppresses depth writes whenever depth testing is
				// disabled. RenderWare requests test-off/write-on for the
				// invisible mask, so express that as an ALWAYS test.
				depthStencil.depthTestEnable = VK_TRUE;
				depthStencil.depthWriteEnable = VK_TRUE;
				depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
			}else if(gstate.zTestEnabled){
				depthStencil.depthTestEnable = VK_TRUE;
				depthStencil.depthWriteEnable = VK_FALSE;
				depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
			}
		}else{
			// Regular HUD/fonts never write into the world depth buffer.
			depthStencil.depthWriteEnable = VK_FALSE;
		}
	}

	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.blendEnable = gstate.vertexAlphaEnabled ? VK_TRUE : VK_FALSE;
	blendAttachment.srcColorBlendFactor = blendFactor(gstate.srcBlend);
	blendAttachment.dstColorBlendFactor = blendFactor(gstate.dstBlend);
	blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	// The alpha channel takes its own factors, as the D3D12 backend sets them:
	// a colour factor applied to alpha is a different quantity. Shadows blend
	// ZERO/INV_SRC_COLOR, whose alpha counterpart is ZERO/INV_SRC_ALPHA --
	// feeding the colour factor there flattened palm shadows into hard black
	// silhouettes.
	blendAttachment.srcAlphaBlendFactor = alphaBlendFactor(gstate.srcBlend);
	blendAttachment.dstAlphaBlendFactor = alphaBlendFactor(gstate.dstBlend);
	blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo colourBlend = {};
	colourBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colourBlend.attachmentCount = 1;
	colourBlend.pAttachments = &blendAttachment;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT,
	                                         VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
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
	info.layout = gs.pipelineLayout;
	info.renderPass = gvk.renderPass;
	info.subpass = 0;

	PipelineEntry entry;
	entry.key = key;
	entry.pipeline = VK_NULL_HANDLE;
	if(vkCreateGraphicsPipelines(gvk.device, VK_NULL_HANDLE, 1, &info, nil,
	                             &entry.pipeline) != VK_SUCCESS){
		VKERR("vkCreateGraphicsPipelines failed for key %llu",
		      (unsigned long long)key);
		return VK_NULL_HANDLE;
	}
	VK_CHECKPOINT("vk/pipelineReady");
	gs.pipelines.push_back(entry);
	gs.lastPipelineKey = key;
	gs.lastPipeline = entry.pipeline;
	gs.lastPipelineValid = 1;
	return entry.pipeline;
}

void
compilePendingPipelines(void)
{
	if(gs.pending.empty() || gvk.inFrame)
		return;

	const RenderState saved = gstate;
	std::vector<PendingPipeline> work;
	work.swap(gs.pending);
	for(size_t i = 0; i < work.size(); i++){
		gstate = work[i].state;
		getPipeline(work[i].shader, work[i].topology);
	}
	gstate = saved;
}

VkDescriptorSet
getTextureDescriptor(Raster *raster)
{
	if(raster == nil)
		return gs.whiteDescriptor;

	VulkanRaster *native = PLUGINOFFSET(VulkanRaster, raster, nativeRasterOffset);
	if(native->view == VK_NULL_HANDLE){
		// Substituting white is only harmless for untextured geometry. Under
		// a multiplicative blend -- shadows use dst*(1-src) -- white turns the
		// whole quad black, so a missing texture shows up as a hard silhouette
		// rather than as something obviously untextured. Say so once.
		static int32 reported = 0;
		if(reported++ < 8)
			VKERR("texture raster %dx%d has no image; drawing white",
			      raster->width, raster->height);
		return gs.whiteDescriptor;
	}

	const uint32 key = makeSamplerKey(gstate.textureFilter,
	                                  gstate.textureAddressU,
	                                  gstate.textureAddressV);
	const uint32 frame = gs.activeFrame;
	if(native->descriptorSet[frame] != VK_NULL_HANDLE &&
	   native->samplerKey[frame] == key)
		return native->descriptorSet[frame];

	if(native->descriptorSet[frame] == VK_NULL_HANDLE){
		VkDescriptorSetAllocateInfo setInfo = {};
		setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		setInfo.descriptorPool = gs.descriptorPool;
		setInfo.descriptorSetCount = 1;
		setInfo.pSetLayouts = &gs.textureLayout;
		if(vkAllocateDescriptorSets(gvk.device, &setInfo,
		                            &native->descriptorSet[frame]) != VK_SUCCESS){
			// The pool is sized for the whole streamed texture set; running out
			// means the budget was wrong, so say so instead of drawing white.
			VKERR("descriptor pool exhausted (%u sets)", MAX_TEXTURE_DESCRIPTORS);
			native->descriptorSet[frame] = VK_NULL_HANDLE;
			return gs.whiteDescriptor;
		}
	}

	VkDescriptorImageInfo imageDescriptor = {};
	imageDescriptor.sampler = getSampler(key, gstate.textureFilter,
	                                     gstate.textureAddressU,
	                                     gstate.textureAddressV);
	imageDescriptor.imageView = native->view;
	imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet write = {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = native->descriptorSet[frame];
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageDescriptor;
	vkUpdateDescriptorSets(gvk.device, 1, &write, 0, nil);

	native->samplerKey[frame] = key;
	return native->descriptorSet[frame];
}

bool32
allocateDynamic(VkDeviceSize size, VkDeviceSize alignment, VkBuffer *bufferOut,
                VkDeviceSize *offsetOut, void **mappedOut)
{
	if(!gs.initialised || size == 0)
		return 0;
	if(alignment == 0)
		alignment = 4;

	StateFrame &sf = gs.frames[gs.activeFrame];
	VkDeviceSize offset = (sf.dynamicOffset + alignment - 1) & ~(alignment - 1);
	if(offset + size > sf.dynamicCapacity){
		if(!sf.dynamicOverflowReported){
			VKERR("dynamic buffer exhausted at %llu bytes; immediate-mode "
			      "geometry will be dropped this frame",
			      (unsigned long long)sf.dynamicCapacity);
			sf.dynamicOverflowReported = 1;
		}
		return 0;
	}

	*bufferOut = sf.dynamicBuffer;
	*offsetOut = offset;
	*mappedOut = sf.dynamicMapped + offset;
	sf.dynamicOffset = offset + size;
	return 1;
}

void
resetDynamic(void)
{
	StateFrame &sf = gs.frames[gs.activeFrame];
	sf.dynamicOffset = 0;
	sf.dynamicOverflowReported = 0;
}

}
}

#endif
