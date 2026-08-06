#pragma once

#ifdef RW_VULKAN

#include <vulkan/vulkan.h>

namespace rw {
namespace vulkan {

// Native raster attached to every rw::Raster on this platform.
struct VulkanRaster
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFormat format;
	VkImageLayout layout;

	// Staging allocation kept alive between rasterLock and rasterUnlock. The
	// game locks a mip level, writes into it and unlocks, so the upload cannot
	// be issued until unlock time.
	VkBuffer stagingBuffer;
	VkDeviceMemory stagingMemory;
	uint8 *stagingMapped;
	int32 lockedLevel;
	uint32 lockedFlags;

	int32 numLevels;
	bool32 hasAlpha;
	bool32 isCompressed;
	// Set when the source was a DXT/BC texture that this device cannot sample
	// natively and had to be decompressed on upload.
	bool32 wasTranscoded;

	// Cached descriptor set 1 for this texture, plus the sampler key it was
	// built with so a filter or addressing change rebuilds it.
	VkDescriptorSet descriptorSet;
	uint32 samplerKey;
};

// Mirrors the state the fixed-function RenderWare pipeline expects. Vulkan has
// no render-state machine, so the values are accumulated here and folded into
// a pipeline key when a draw is recorded.
struct RenderState
{
	uint32 vertexAlphaEnabled;
	uint32 srcBlend;
	uint32 dstBlend;
	uint32 zTestEnabled;
	uint32 zWriteEnabled;
	uint32 cullMode;
	uint32 alphaTestFunction;
	uint32 alphaTestRef;
	uint32 fogEnabled;
	RGBA fogColor;
	// Set through SetRenderStatePtr(TEXTURERASTER). Immediate mode reads it
	// back to pick the descriptor set, so it has to be stored, not dropped.
	Raster *texture;
	uint32 textureFilter;
	uint32 textureAddressU;
	uint32 textureAddressV;
};

struct Globals
{
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	VkQueue queue;
	uint32 queueFamilyIndex;

	VkPhysicalDeviceProperties deviceProperties;
	VkPhysicalDeviceMemoryProperties memoryProperties;
	VkPhysicalDeviceFeatures deviceFeatures;

	VkCommandPool commandPool;
	// Recording target for the frame currently between beginFrame and endFrame.
	VkCommandBuffer frameCommands;
	VkFence frameFence;

	VkRenderPass renderPass;
	VkFramebuffer framebuffer;
	VkImage depthImage;
	VkDeviceMemory depthMemory;
	VkImageView depthView;
	VkFormat depthFormat;

	uint32 width;
	uint32 height;
	uint32 viewCount;
	VkFormat colourFormat;

	bool32 initialised;
	bool32 inFrame;
	bool32 supportsBC;

	float32 stereoViewProjection[2][16];
	// Game world space to OpenXR play space, rebuilt from the camera in
	// beginUpdate and folded into each draw's model matrix. Captured when the
	// draw is recorded, so a frame that switches cameras stays correct.
	float32 worldToPlay[16];
	// Mid-eye pose in play space, updated once per frame by the app layer.
	float32 headPosition[3];
	float32 headYaw;
	float32 headQuat[4];
	// Render pass clear colour; carries the timecycle sky. See beginFrame.
	float32 clearColour[3];

	// First person anchor: the player's head in the game world. anchorYaw is
	// the world yaw that play-space forward maps to. On foot it is latched at
	// activation (room-scale: the world holds still); in a vehicle it tracks
	// the vehicle heading (cockpit: the world turns with the car), with the
	// head yaw latched at entry riding on top.
	bool32 firstPersonActive;
	bool32 fpFollowHeading;
	float32 fpHeadWorld[3];
	float32 fpAnchorYaw;
	float32 fpLatchedHeadYaw;
};

extern Globals gvk;
extern RenderState gstate;

// ---------------------------------------------------------------------------
// Shader variants and pipeline state
// ---------------------------------------------------------------------------

// One fixed interleaved layout for all world geometry, instead of deriving a
// vertex format per Geometry the way the GL backend does. Geometries without
// normals or prelighting get defaults filled in at instance time.
//
// This trades a little memory for one vertex-input description and therefore a
// far smaller pipeline cache. On a tiler, pipeline count and the associated
// shader patching cost matter more than 36 bytes per vertex.
struct WorldVertex
{
	float32 position[3];
	float32 normal[3];
	uint8 colour[4];
	float32 texCoord[2];
};

// Skinned geometry: the world vertex plus the four bone weights and indices
// RenderWare stores per vertex. Peds and anything else driven by an HAnim
// hierarchy comes through here.
struct SkinVertex
{
	float32 position[3];
	float32 normal[3];
	uint8 colour[4];
	float32 texCoord[2];
	float32 weights[4];
	uint8 boneIndices[4];
};

// RenderWare's own ceiling, and what the bone uniform block is sized for.
#define RW_MAX_BONES 64

enum ShaderVariant
{
	SHADER_WORLD,
	SHADER_IM2D,
	SHADER_IM3D,
	SHADER_SKIN,
	SHADER_COUNT
};

// Mirrors SceneData in rw_common.glsl. Written once per frame.
struct SceneData
{
	// Play space (OpenXR, metres, Y up) to clip. World geometry gets there
	// through the game camera transform folded into PushConstants::model.
	float32 viewProj[2][16];
	float32 fogColour[4];
	float32 fogParams[4];
	float32 ambient[4];
	float32 lightDirection[8][4];
	float32 lightColour[8][4];
	float32 lightCount[4];
	float32 im2dTransform[16];
	// x = Im2D plane distance, yzw = eye position in play space.
	float32 im2dParams[4];
};

// Mirrors PushConstants in rw_common.glsl. Exactly 128 bytes -- the guaranteed
// minimum -- so no device needs a fallback path and no byte is wasted.
//
// Lighting rides in the push constants rather than the scene block because it
// is resolved per atomic: world geometry carries its light baked into prelight
// (Geometry::LIGHT unset, ambient contribution zero), while peds and vehicles
// are lit dynamically. One block per frame cannot express that split.
struct PushConstants
{
	float32 model[16];
	float32 materialColour[4];
	float32 surfaceProps[4];
	// rgb = summed ambient from the world, already zeroed for unlit geometry.
	float32 ambientLight[4];
	// xyz = first directional's direction, rotated into play space to match
	// the normals; w = the light colour packed RGBA8 (unpackUnorm4x8).
	float32 lightDirColour[4];
};

// Stall checkpoints, provided by the application layer. Same binary, so this
// is just a forward declaration rather than a dependency.
} // namespace vulkan
} // namespace rw
namespace platform { void setCheckpoint(const char *label); }
namespace rw {
namespace vulkan {
#define VK_CHECKPOINT(label) platform::setCheckpoint(label)

// out = a*b, both column major. out may not alias a or b.
void multiplyMatrix(float32 out[16], const float32 a[16], const float32 b[16]);

// initSkin and makeSkinPipeline are declared in rwvk.h, next to the default
// pipeline, because skin.cpp needs them and only sees the public header.

// Geometry buffers that live for the lifetime of a model, staged into
// device-local memory.
void destroyStaticBuffer(VkBuffer &buffer, VkDeviceMemory &memory);
bool32 uploadStaticBuffer(const void *data, VkDeviceSize size,
                          VkBufferUsageFlags usage, VkBuffer *bufferOut,
                          VkDeviceMemory *memoryOut);

// The material walk shared by the object pipelines. boneOffset is nil for
// unskinned geometry; when set, descriptor set 2 is bound at that offset.
//
// Two passes, opaque then blended, mirroring the D3D12 backend's
// MeshSelection. A model's meshes arrive in file order, and drawing a blended
// mesh before the opaque mesh behind it leaves nothing to blend against: the
// triangle shows the background through the model.
void drawAtomicMeshes(Atomic *atomic, InstanceDataHeader *header, uint32 shader,
                      const uint32 *boneOffset);

bool32 stateInit(void);
void stateShutdown(void);

// Builds (or returns a cached) pipeline for the given shader variant and
// topology, folding in the current render-state cache.
VkPipeline getPipeline(uint32 shader, VkPrimitiveTopology topology);

// Compiles anything getPipeline had to defer. Must be called outside a render
// pass; beginFrame does it before opening one.
void compilePendingPipelines(void);

// One line per frame while the counts change; silent once they settle.
void reportImStats(void);
VkPipelineLayout getPipelineLayout(void);

// Descriptor set 1 for a raster, or the 1x1 white fallback when raster is nil.
VkDescriptorSet getTextureDescriptor(Raster *raster);
VkDescriptorSet getSceneDescriptor(void);

// Descriptor set 2: the bone matrix block, a dynamic uniform buffer aliased
// over the per-frame allocator. One set exists for the whole run; each draw
// picks its own matrices with a dynamic offset. Only the skin pipeline binds
// it, but the layout is part of every pipeline so the two share a layout.
VkDescriptorSet getBoneDescriptor(void);
// Alignment a bone block must start at to be usable as a dynamic offset.
VkDeviceSize getBoneBlockAlignment(void);

SceneData *getSceneData(void);
void uploadSceneData(void);

// Bump allocator over a per-frame host-visible buffer. Reset in beginFrame,
// which is safe because beginFrame has already waited on the previous frame's
// fence. Used for all immediate-mode geometry.
bool32 allocateDynamic(VkDeviceSize size, VkDeviceSize alignment,
                       VkBuffer *bufferOut, VkDeviceSize *offsetOut,
                       void **mappedOut);
void resetDynamic(void);

// Shared helpers used by both the device and the raster implementation.
bool32 findMemoryType(uint32 typeBits, VkMemoryPropertyFlags properties,
                      uint32 *indexOut);
bool32 createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties, VkBuffer *bufferOut,
                    VkDeviceMemory *memoryOut);
// Records and submits a one-shot command buffer, then waits for it. Used for
// texture uploads outside the frame.
VkCommandBuffer beginOneShot(void);
void endOneShot(VkCommandBuffer commandBuffer);
void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image,
                           VkImageAspectFlags aspect, uint32 levelCount,
                           VkImageLayout from, VkImageLayout to);

}
}

#endif
