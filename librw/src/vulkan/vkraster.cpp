#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
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

int32 nativeRasterOffset;

#ifdef RW_VULKAN

#define GETVULKANRASTER(raster) \
	PLUGINOFFSET(VulkanRaster, raster, nativeRasterOffset)

// ---------------------------------------------------------------------------
// Format mapping
//
// Everything the game streams is either 32-bit BGRA or a DXT block format.
// Adreno has no BC support at all, so the DXT path cannot simply be handed to
// Vulkan the way it is on D3D12; see allocateCompressed.
// ---------------------------------------------------------------------------

static VkFormat
vulkanFormatFromRasterFormat(int32 format, int32 *bytesPerPixelOut)
{
	int32 bytesPerPixel = 0;
	VkFormat vkFormat = VK_FORMAT_UNDEFINED;

	switch(format & 0xF00){
	case Raster::C8888:
		vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
		bytesPerPixel = 4;
		break;
	case Raster::C888:
		// No widely supported 24-bit sampled format on tilers; widen to 32-bit
		// and leave alpha at one. rasterLock hands out a 4-byte stride to match.
		vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
		bytesPerPixel = 4;
		break;
	case Raster::C1555:
		vkFormat = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
		bytesPerPixel = 2;
		break;
	case Raster::C565:
		vkFormat = VK_FORMAT_R5G6B5_UNORM_PACK16;
		bytesPerPixel = 2;
		break;
	case Raster::C4444:
		vkFormat = VK_FORMAT_R4G4B4A4_UNORM_PACK16;
		bytesPerPixel = 2;
		break;
	case Raster::LUM8:
		vkFormat = VK_FORMAT_R8_UNORM;
		bytesPerPixel = 1;
		break;
	case Raster::C555:
		vkFormat = VK_FORMAT_R5G5B5A1_UNORM_PACK16;
		bytesPerPixel = 2;
		break;
	case Raster::D16:
		vkFormat = VK_FORMAT_D16_UNORM;
		bytesPerPixel = 2;
		break;
	case Raster::D24:
	case Raster::D32:
		vkFormat = VK_FORMAT_D24_UNORM_S8_UINT;
		bytesPerPixel = 4;
		break;
	default:
		break;
	}

	if(bytesPerPixelOut != nil)
		*bytesPerPixelOut = bytesPerPixel;
	return vkFormat;
}

static uint32
levelDimension(uint32 base, int32 level)
{
	uint32 value = base >> level;
	return value != 0 ? value : 1;
}

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

static void*
createNativeRaster(void *object, int32 offset, int32)
{
	VulkanRaster *native = PLUGINOFFSET(VulkanRaster, object, offset);
	memset(native, 0, sizeof(VulkanRaster));
	native->format = VK_FORMAT_UNDEFINED;
	native->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	native->lockedLevel = -1;
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	VulkanRaster *native = PLUGINOFFSET(VulkanRaster, object, offset);
	if(gvk.device != VK_NULL_HANDLE){
		if(native->stagingMapped != nil)
			vkUnmapMemory(gvk.device, native->stagingMemory);
		if(native->stagingBuffer)
			vkDestroyBuffer(gvk.device, native->stagingBuffer, nil);
		if(native->stagingMemory)
			vkFreeMemory(gvk.device, native->stagingMemory, nil);
		if(native->view)
			vkDestroyImageView(gvk.device, native->view, nil);
		if(native->image)
			vkDestroyImage(gvk.device, native->image, nil);
		if(native->memory)
			vkFreeMemory(gvk.device, native->memory, nil);
	}
	memset(native, 0, sizeof(VulkanRaster));
	native->lockedLevel = -1;
	return object;
}

static void*
copyNativeRaster(void *dst, void *, int32 offset, int32)
{
	VulkanRaster *native = PLUGINOFFSET(VulkanRaster, dst, offset);
	memset(native, 0, sizeof(VulkanRaster));
	native->format = VK_FORMAT_UNDEFINED;
	native->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	native->lockedLevel = -1;
	return dst;
}

void *
destroyNativeData(void *object, int32 offset, int32 size)
{
	return destroyNativeRaster(object, offset, size);
}

void
registerNativeRaster(void)
{
	nativeRasterOffset = Raster::registerPlugin(
		sizeof(VulkanRaster), ID_RASTERVULKAN, createNativeRaster,
		destroyNativeRaster, copyNativeRaster);
}

// ---------------------------------------------------------------------------
// Raster interface
// ---------------------------------------------------------------------------

Raster *
rasterCreate(Raster *raster)
{
	VulkanRaster *native = GETVULKANRASTER(raster);

	if(raster->type != Raster::TEXTURE && raster->type != Raster::CAMERATEXTURE &&
	   raster->type != Raster::ZBUFFER && raster->type != Raster::CAMERA){
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}
	if(raster->flags & Raster::DONTALLOCATE)
		return raster;
	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}

	// Raster::DEFAULT means "backend's choice". The game creates cameras and
	// z-buffers this way, so refusing it here rejects the main render targets.
	int32 requestedFormat = raster->format;
	if((requestedFormat & 0xF00) == 0){
		requestedFormat |= (raster->type & 0xF) == Raster::ZBUFFER ?
			Raster::D24 : Raster::C8888;
		raster->format = requestedFormat;
	}

	int32 bytesPerPixel = 0;
	native->format = vulkanFormatFromRasterFormat(requestedFormat, &bytesPerPixel);
	if(native->format == VK_FORMAT_UNDEFINED){
		VKERR("unsupported raster format 0x%x", raster->format);
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}
	raster->depth = bytesPerPixel * 8;
	raster->stride = raster->width * bytesPerPixel;

	native->numLevels = 1;
	if(raster->format & (Raster::MIPMAP | Raster::AUTOMIPMAP)){
		uint32 size = raster->width > raster->height ?
		              (uint32)raster->width : (uint32)raster->height;
		while(size > 1){
			size >>= 1;
			native->numLevels++;
		}
	}

	const bool32 isDepth = (raster->type & 0xF) == Raster::ZBUFFER;

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = native->format;
	imageInfo.extent.width = (uint32)raster->width;
	imageInfo.extent.height = (uint32)raster->height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = (uint32)native->numLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = isDepth ?
		(VkImageUsageFlags)VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
		(VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		                    VK_IMAGE_USAGE_SAMPLED_BIT);
	if((raster->type & 0xF) == Raster::CAMERATEXTURE ||
	   (raster->type & 0xF) == Raster::CAMERA)
		imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	if(vkCreateImage(gvk.device, &imageInfo, nil, &native->image) != VK_SUCCESS){
		VKERR("vkCreateImage failed for %dx%d", raster->width, raster->height);
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, native->image, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex)){
		vkDestroyImage(gvk.device, native->image, nil);
		native->image = VK_NULL_HANDLE;
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, &native->memory) != VK_SUCCESS){
		vkDestroyImage(gvk.device, native->image, nil);
		native->image = VK_NULL_HANDLE;
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}
	vkBindImageMemory(gvk.device, native->image, native->memory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = native->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = native->format;
	viewInfo.subresourceRange.aspectMask = isDepth ?
		VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = (uint32)native->numLevels;
	viewInfo.subresourceRange.layerCount = 1;
	if(vkCreateImageView(gvk.device, &viewInfo, nil, &native->view) != VK_SUCCESS){
		VKERR("vkCreateImageView failed");
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}

	native->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	return raster;
}

// Creates a texture directly from DXT blocks, no CPU decode. Adreno 740
// reports full BC support, so stream data reaches the GPU the same way the
// desktop D3D12 build uploads it. Decoding on the CPU instead cost
// milliseconds per streamed texture, which surfaced as frame drops -- the
// world lurching -- whenever driving streamed new map sectors in.
Raster *
rasterFromDXT(int32 width, int32 height, int32 dxt, bool32 hasAlpha,
              const uint8 *blocks, uint32 size)
{
	if(!gvk.supportsBC || blocks == nil || size == 0)
		return nil;

	VkFormat format;
	switch(dxt){
	// Always the alpha-carrying BC1, as the D3D12 backend picks
	// DXGI_FORMAT_BC1_UNORM for DXT1 regardless of the TXD's alpha flag.
	// DXT1 encodes punch-through transparency in the block itself; reading it
	// as BC1_RGB forces alpha to 1, the alpha test then discards nothing, and
	// masked foliage renders as solid black quads -- the palm silhouettes.
	case 1: format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
	case 2:
	case 3: format = VK_FORMAT_BC2_UNORM_BLOCK; break;
	case 4:
	case 5: format = VK_FORMAT_BC3_UNORM_BLOCK; break;
	default: return nil;
	}

	Raster *raster = Raster::create(width, height, 32,
		Raster::TEXTURE | Raster::C8888 | Raster::DONTALLOCATE);
	if(raster == nil)
		return nil;
	VulkanRaster *native = GETVULKANRASTER(raster);
	native->format = format;
	native->numLevels = 1;
	native->hasAlpha = hasAlpha;

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = format;
	imageInfo.extent.width = (uint32)width;
	imageInfo.extent.height = (uint32)height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                  VK_IMAGE_USAGE_SAMPLED_BIT;
	if(vkCreateImage(gvk.device, &imageInfo, nil, &native->image) != VK_SUCCESS){
		raster->destroy();
		return nil;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, native->image, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex)){
		raster->destroy();
		return nil;
	}
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, &native->memory) != VK_SUCCESS){
		raster->destroy();
		return nil;
	}
	vkBindImageMemory(gvk.device, native->image, native->memory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = native->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	if(vkCreateImageView(gvk.device, &viewInfo, nil, &native->view) != VK_SUCCESS){
		raster->destroy();
		return nil;
	}

	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	if(!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 &staging, &stagingMemory)){
		raster->destroy();
		return nil;
	}
	void *mapped = nil;
	vkMapMemory(gvk.device, stagingMemory, 0, size, 0, &mapped);
	memcpy(mapped, blocks, size);
	vkUnmapMemory(gvk.device, stagingMemory);

	VkCommandBuffer commandBuffer = beginOneShot();
	if(commandBuffer != VK_NULL_HANDLE){
		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT, 1,
		                      VK_IMAGE_LAYOUT_UNDEFINED,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = (uint32)width;
		region.imageExtent.height = (uint32)height;
		region.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(commandBuffer, staging, native->image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT, 1,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		endOneShot(commandBuffer);
		native->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	vkDestroyBuffer(gvk.device, staging, nil);
	vkFreeMemory(gvk.device, stagingMemory, nil);

	return raster;
}

uint8 *
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	VulkanRaster *native = GETVULKANRASTER(raster);
	if(native->image == VK_NULL_HANDLE || level >= native->numLevels)
		return nil;
	// A stale lock means an earlier unlock never ran. Refusing here would
	// cascade into a texture that is never uploaded, so drop the old staging
	// allocation and carry on.
	if(native->lockedLevel >= 0){
		static bool32 warned = 0;
		if(!warned){
			VKERR("raster was still locked at level %d; releasing stale staging",
			      native->lockedLevel);
			warned = 1;
		}
		if(native->stagingMapped != nil)
			vkUnmapMemory(gvk.device, native->stagingMemory);
		if(native->stagingBuffer)
			vkDestroyBuffer(gvk.device, native->stagingBuffer, nil);
		if(native->stagingMemory)
			vkFreeMemory(gvk.device, native->stagingMemory, nil);
		native->stagingBuffer = VK_NULL_HANDLE;
		native->stagingMemory = VK_NULL_HANDLE;
		native->stagingMapped = nil;
		native->lockedLevel = -1;
	}

	const uint32 width = levelDimension((uint32)raster->width, level);
	const uint32 height = levelDimension((uint32)raster->height, level);
	const uint32 bytesPerPixel = (uint32)(raster->depth / 8);
	const VkDeviceSize size = (VkDeviceSize)width * height * bytesPerPixel;

	if(!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 &native->stagingBuffer, &native->stagingMemory))
		return nil;
	if(vkMapMemory(gvk.device, native->stagingMemory, 0, size, 0,
	               (void**)&native->stagingMapped) != VK_SUCCESS){
		vkDestroyBuffer(gvk.device, native->stagingBuffer, nil);
		vkFreeMemory(gvk.device, native->stagingMemory, nil);
		native->stagingBuffer = VK_NULL_HANDLE;
		native->stagingMemory = VK_NULL_HANDLE;
		return nil;
	}
	// LOCKREAD would need a device-to-host copy first. Nothing in the game
	// reads a streamed texture back, so it is refused loudly rather than
	// silently returning uninitialised staging memory.
	if(lockMode & Raster::LOCKREAD)
		memset(native->stagingMapped, 0, (size_t)size);

	native->lockedLevel = level;
	native->lockedFlags = (uint32)lockMode;
	raster->stride = (int32)(width * bytesPerPixel);
	raster->width = (int32)width;
	raster->height = (int32)height;
	return native->stagingMapped;
}

void
rasterUnlock(Raster *raster, int32 level)
{
	VulkanRaster *native = GETVULKANRASTER(raster);
	if(native->lockedLevel < 0 || native->stagingBuffer == VK_NULL_HANDLE)
		return;

	const uint32 width = levelDimension((uint32)raster->width, 0);
	const uint32 height = levelDimension((uint32)raster->height, 0);

	VkCommandBuffer commandBuffer = beginOneShot();
	if(commandBuffer != VK_NULL_HANDLE){
		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT,
		                      (uint32)native->numLevels,
		                      native->layout == VK_IMAGE_LAYOUT_UNDEFINED ?
		                          VK_IMAGE_LAYOUT_UNDEFINED :
		                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = (uint32)level;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = width;
		region.imageExtent.height = height;
		region.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(commandBuffer, native->stagingBuffer, native->image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT,
		                      (uint32)native->numLevels,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		endOneShot(commandBuffer);
		native->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	vkUnmapMemory(gvk.device, native->stagingMemory);
	vkDestroyBuffer(gvk.device, native->stagingBuffer, nil);
	vkFreeMemory(gvk.device, native->stagingMemory, nil);
	native->stagingBuffer = VK_NULL_HANDLE;
	native->stagingMemory = VK_NULL_HANDLE;
	native->stagingMapped = nil;
	native->lockedLevel = -1;
}

int32
rasterNumLevels(Raster *raster)
{
	VulkanRaster *native = GETVULKANRASTER(raster);
	return native->numLevels > 0 ? native->numLevels : 1;
}

bool32
imageFindRasterFormat(Image *image, int32 type, int32 *width, int32 *height,
                      int32 *depth, int32 *format)
{
	if((type & 0xF) != Raster::TEXTURE)
		return 0;

	int32 formatOut = 0;
	switch(image->depth){
	case 32: formatOut = image->hasAlpha() ? Raster::C8888 : Raster::C888; break;
	case 24: formatOut = Raster::C888; break;
	case 16: formatOut = Raster::C1555; break;
	// Palettised sources are expanded before upload: no mobile GPU samples a
	// paletted format, and the game's 8-bit TXDs are small enough that the
	// widened copy costs less than any indirection would.
	case 8:
	case 4:
		formatOut = Raster::C8888;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	*width = image->width;
	*height = image->height;
	*depth = formatOut == Raster::C888 ? 32 : (formatOut == Raster::C8888 ? 32 : 16);
	*format = formatOut | type;
	return 1;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
	if((raster->type & 0xF) != Raster::TEXTURE)
		return 0;

	uint8 *dst = rasterLock(raster, 0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(dst == nil)
		return 0;

	// Palettised sources are expanded inline rather than through a temporary
	// Image. Building one and pointing its palette at the caller's meant
	// unpalettize() freed a palette this function does not own, and the caller
	// freed it again afterwards -- a double free of the 1024-byte palette that
	// corrupted the heap and took down unrelated threads much later.
	const int32 bytesPerPixel = raster->depth / 8;
	const uint8 *palette = image->palette;

	for(int32 y = 0; y < raster->height; y++){
		uint8 *dstRow = dst + (size_t)y * raster->stride;
		const uint8 *srcRow = image->pixels + (size_t)y * image->stride;

		switch(image->depth){
		case 32:
			memcpy(dstRow, srcRow, (size_t)raster->width * 4);
			break;
		case 24:
			for(int32 x = 0; x < raster->width; x++){
				dstRow[x*4 + 0] = srcRow[x*3 + 0];
				dstRow[x*4 + 1] = srcRow[x*3 + 1];
				dstRow[x*4 + 2] = srcRow[x*3 + 2];
				dstRow[x*4 + 3] = 0xFF;
			}
			break;
		case 8:
			if(palette == nil)
				break;
			for(int32 x = 0; x < raster->width; x++){
				const uint8 *entry = &palette[srcRow[x] * 4];
				dstRow[x*4 + 0] = entry[0];
				dstRow[x*4 + 1] = entry[1];
				dstRow[x*4 + 2] = entry[2];
				dstRow[x*4 + 3] = entry[3];
			}
			break;
		case 4:
			if(palette == nil)
				break;
			// Two pixels per byte, left in the high nibble.
			for(int32 x = 0; x < raster->width; x++){
				const uint8 packed = srcRow[x >> 1];
				const uint8 index = (x & 1) ? (packed & 0xF) : (packed >> 4);
				const uint8 *entry = &palette[index * 4];
				dstRow[x*4 + 0] = entry[0];
				dstRow[x*4 + 1] = entry[1];
				dstRow[x*4 + 2] = entry[2];
				dstRow[x*4 + 3] = entry[3];
			}
			break;
		default:
			memcpy(dstRow, srcRow, (size_t)raster->width * bytesPerPixel);
			break;
		}
	}

	rasterUnlock(raster, 0);
	return 1;
}

Image *
rasterToImage(Raster *raster)
{
	// Reading a device-local image back needs a transfer-src copy and a host
	// buffer round trip. Nothing on this platform asks for it yet; returning
	// nil is better than an empty image that silently corrupts a screenshot.
	(void)raster;
	VKERR("rasterToImage is not implemented on the Vulkan backend");
	return nil;
}

void
setRasterHasAlpha(Raster *raster, bool32 hasAlpha)
{
	GETVULKANRASTER(raster)->hasAlpha = hasAlpha;
}

bool32
allocateCompressed(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha)
{
	// Adreno exposes ETC2 and ASTC but not BC/DXT, and every Vice City TXD is
	// DXT1/3/5. The blocks therefore cannot be uploaded as-is the way the
	// D3D12 backend does; they have to be decoded, or transcoded offline into
	// ASTC during data staging. Reporting failure here lets the caller fall
	// back to the decompressed path rather than creating an unsampleable image.
	(void)raster;
	(void)numLevels;
	(void)hasAlpha;
	if(!gvk.supportsBC){
		static bool32 warned = 0;
		if(!warned){
			VKLOG("device has no BC support; DXT%d rasters will be decompressed",
			      dxt);
			warned = 1;
		}
		return 0;
	}
	return 0;
}

#else

void registerNativeRaster(void) {}
void *destroyNativeData(void *object, int32, int32) { return object; }

#endif

}
}
