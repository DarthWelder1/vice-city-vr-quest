#pragma once

#ifdef RW_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace rw {

#ifdef RW_VULKAN
// On Android the OpenXR runtime creates the Vulkan instance and device itself
// (xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR) so it can inject the
// extensions it needs to share swapchain images. The backend therefore adopts
// an existing device instead of opening one, which is the opposite of how the
// D3D12 backend is entered on Windows.
struct EngineOpenParams
{
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	uint32 queueFamilyIndex;
	VkQueue queue;

	// Colour target description owned by the OpenXR layer. viewCount is 2 for
	// the multiview stereo path and 1 for any mono pass.
	uint32 width;
	uint32 height;
	uint32 viewCount;
	VkFormat colourFormat;
};
#endif

namespace vulkan {

void registerPlatformPlugins(void);
void registerNativeRaster(void);
extern int32 nativeRasterOffset;

Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster *raster, int32 level, int32 lockMode);
void rasterUnlock(Raster *raster, int32 level);
int32 rasterNumLevels(Raster *raster);
bool32 imageFindRasterFormat(Image *image, int32 type,
                             int32 *width, int32 *height,
                             int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);
void setRasterHasAlpha(Raster *raster, bool32 hasAlpha);
bool32 rasterHasAlpha(Raster *raster);

// Compressed uploads. Adreno exposes ASTC and ETC2 natively but not BC/DXT,
// which is what every Vice City TXD actually contains, so the DXT path has to
// transcode on the way in. See vkraster.cpp.
bool32 allocateCompressed(Raster *raster, int32 dxt, int32 numLevels,
                          bool32 hasAlpha);
bool32 deviceSupportsBC(void);

void *destroyNativeData(void *object, int32 offset, int32 size);

// ---------------------------------------------------------------------------
// Object pipeline
// ---------------------------------------------------------------------------

struct InstanceData
{
	uint32 numIndex;
	uint32 minVert;
	int32 numVertices;
	Material *material;
	bool32 vertexAlpha;
	uint32 offset;		// byte offset into the index buffer
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32 serialNumber;
	uint32 numMeshes;
	uint32 totalNumIndex;
	uint32 totalNumVertex;
	uint32 primType;	// VkPrimitiveTopology

	uint16 *indexBuffer;	// CPU copies, kept so a mesh change can reinstance
	uint8 *vertexBuffer;

#ifdef RW_VULKAN
	VkBuffer vbo;
	VkDeviceMemory vboMemory;
	VkBuffer ibo;
	VkDeviceMemory iboMemory;
#else
	void *vbo, *vboMemory, *ibo, *iboMemory;
#endif

	InstanceData *inst;
};

class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*uninstanceCB)(Geometry *geo, InstanceDataHeader *header);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
};

void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header);
void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header);
void freeInstanceData(Geometry *geometry);
void *destroyNativeGeometryData(void *object, int32 offset, int32 size);

ObjPipeline *makeDefaultPipeline(void);

// Skinned geometry. Declared here, next to the default pipeline, so skin.cpp
// can reach it the same way it reaches every other platform's.
void initSkin(void);
ObjPipeline *makeSkinPipeline(void);

// Environment-mapped materials (vehicle bodies). Same arrangement for
// matfx.cpp.
void initMatFX(void);
ObjPipeline *makeMatFXPipeline(void);

// Immediate-mode vertex layouts. Kept backend-owned, matching how the D3D12
// backend defines its own rather than borrowing the D3D9 ABI.
struct Im3DVertex
{
	V3d position;
	uint32 color;
	float32 u, v;

	void setX(float32 value) { position.x = value; }
	void setY(float32 value) { position.y = value; }
	void setZ(float32 value) { position.z = value; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		color = ((uint32)a << 24) | ((uint32)b << 16) |
		        ((uint32)g << 8) | (uint32)r;
	}
	void setU(float32 value) { u = value; }
	void setV(float32 value) { v = value; }

	float32 getX(void) { return position.x; }
	float32 getY(void) { return position.y; }
	float32 getZ(void) { return position.z; }
	RGBA getColor(void) {
		return makeRGBA(color & 0xFF, (color >> 8) & 0xFF,
		                (color >> 16) & 0xFF, (color >> 24) & 0xFF);
	}
	float32 getU(void) { return u; }
	float32 getV(void) { return v; }
};

struct Im2DVertex
{
	float32 x, y, z, w;
	uint32 color;
	float32 u, v;

	void setScreenX(float32 value) { x = value; }
	void setScreenY(float32 value) { y = value; }
	void setScreenZ(float32 value) { z = value; }
	void setCameraZ(float32 value) { w = value; }
	void setRecipCameraZ(float32 value) { w = 1.0f/value; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		color = ((uint32)a << 24) | ((uint32)b << 16) |
		        ((uint32)g << 8) | (uint32)r;
	}
	void setU(float32 value, float32) { u = value; }
	void setV(float32 value, float32) { v = value; }

	float32 getScreenX(void) { return x; }
	float32 getScreenY(void) { return y; }
	float32 getScreenZ(void) { return z; }
	float32 getCameraZ(void) { return w; }
	float32 getRecipCameraZ(void) { return 1.0f/w; }
	RGBA getColor(void) {
		return makeRGBA(color & 0xFF, (color >> 8) & 0xFF,
		                (color >> 16) & 0xFF, (color >> 24) & 0xFF);
	}
	float32 getU(void) { return u; }
	float32 getV(void) { return v; }
};

// ---------------------------------------------------------------------------
// Frame interface driven by the OpenXR layer.
//
// The Vulkan backend does not own a swapchain: OpenXR hands it a colour image
// per frame. beginFrame binds that image and opens the multiview render pass;
// endFrame closes it and submits. Everything the game draws in between goes to
// both eyes in a single pass.
// ---------------------------------------------------------------------------
bool32 beginFrame(VkImage colourImage, VkImageView colourView);
void endFrame(void);

// Native profiler support.  GPU time is measured by two Vulkan timestamp
// queries around the complete multiview render pass and resolve.  The value
// returned is from the latest completed submission, never a CPU-frame-time
// estimate. Unsupported devices return false from getGpuFrameTimeMs.
void setGpuFrameTimingEnabled(bool32 enabled);
bool32 getGpuFrameTimeMs(float32 *milliseconds);

// Per-eye view-projection matrices for the multiview uniform block.
void setStereoViewProjection(const float32 left[16], const float32 right[16]);

// Places the Im2D screen plane in the world. Maps screen pixel coordinates to
// a quad in front of the head, so both eyes project it and it converges.
// planeDistance is where that plane sits and eye the viewer position, both in
// play space: they let a screen vertex carrying its own camera depth be put
// back at its true world position instead of flattened onto the plane.
void setIm2DTransform(const float32 transform[16], float32 planeDistance,
                      const float32 eye[3]);

// Scales immediate-mode UI vertices around the render-target centre. The game
// enables this only while drawing the immersive gameplay interface, keeping
// world-space sprites and theater/menu frames on their exact projection.
void setIm2DSafeAreaScale(float32 scale);
void setIm2DSafeAreaTransform(float32 scaleX, float32 scaleY,
                              float32 offsetX, float32 offsetY);

// Radar tiles use the original RenderWare depth-mask trick: four invisible
// corner fans write depth, then the map is accepted only inside the circle.
// The role is supplied per immediate-mode draw by the game renderer.
void setImmediate2DStrictDepth(bool32 enabled);
bool32 getImmediate2DStrictDepth(void);

// Mid-eye pose in play space this frame: position plus yaw about the vertical
// (0 = play forward -Z, positive turning left). The world transform pins the
// anchor to it, so turning in place pivots around the player's head instead
// of around the Guardian origin.
void setHeadPose(const float32 position[3], float32 yaw,
                 const float32 quat[4]);

// The current first person VIEW expressed in game world axes: where the
// player is actually looking, head orientation composed onto the anchor.
// Fails (returns 0) when first person is inactive. rwRight follows the
// RenderWare camera convention (it points to the viewer's LEFT), matching
// what BeginEye writes into the camera frame on the desktop.
bool32 getFirstPersonViewFrame(float32 rwRight[3], float32 rwUp[3],
                               float32 rwAt[3], float32 position[3]);

// Converts an OpenXR pose expressed in the current play space into Vice
// City's world. Axes are the controller's local +X, +Y and forward -Z. This
// uses the exact same first-person anchor as world rendering, keeping native
// Quest hands rigidly registered with both eyes and the game world.
bool32 playPoseToFirstPersonWorld(const float32 playPosition[3],
                                  const float32 playQuaternion[4],
                                  float32 worldRight[3],
                                  float32 worldUp[3],
                                  float32 worldForward[3],
                                  float32 worldPosition[3]);

// First person: anchor the view on the player's head in the game world
// instead of on the follow camera. headingYaw is the character's (or the
// vehicle's) facing in world (radians, atan2(fwd.y, fwd.x)). On activation
// the player's current physical facing is latched to it, so they start
// looking the way the character faces. With followHeading set the anchor
// tracks headingYaw every frame -- cockpit behaviour for vehicles -- and a
// change of followHeading re-latches, so entering a car starts looking down
// its nose. Passing active=false falls back to the camera anchor.
void setFirstPersonAnchor(const float32 headWorld[3], float32 headingYaw,
                          bool32 followHeading, bool32 active);

// Vehicle variant of setFirstPersonAnchor. right/up/forward are the
// vehicle's orthonormal world axes. It preserves pitch and roll while still
// applying the physical-yaw latch used by cockpit mode.
void setFirstPersonAnchorBasis(const float32 headWorld[3],
                               const float32 right[3],
                               const float32 up[3],
                               const float32 forward[3],
                               float32 headingYaw, bool32 active);

// View direction in game-world yaw while first person is active. Returns 0
// when inactive. Lets the pad layer make stick movement view-relative.
bool32 getFirstPersonViewYaw(float32 *yawOut);

// HMD yaw relative to the facing direction latched when first-person mode was
// entered or recentered. This is the stable basis used by head-relative
// locomotion; vehicle heading and world heading are deliberately excluded.
bool32 getFirstPersonLocalHeadYaw(float32 *yawOut);

// Sky colour for the frame; used as the render pass clear colour so the sky
// covers the full view rather than the 2D panel.
void setClearColour(uint8 red, uint8 green, uint8 blue);

// Final-image colour treatment. Mode 0 is a plain resolve and mode 1 is Vice
// City's POSTFX_NORMAL filter. The filter runs once for both multiview layers
// after all world and 2D rendering has completed.
void setPostFx(uint32 mode, uint32 red, uint32 green, uint32 blue,
               float32 intensity);
void setFxaaEnabled(bool32 enabled);

// Texture straight from DXT blocks (BC formats), no CPU decode. Returns nil
// when the device lacks BC support or the type is not DXT1/3/5; the caller
// falls back to software decoding.
Raster *rasterFromDXT(int32 width, int32 height, int32 dxt, bool32 hasAlpha,
                      const uint8 *blocks, uint32 size);

// Immediate mode, implemented in vkim.cpp and wired into the device table.
void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1,
                        int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType primType, void *vertices,
                                int32 numVertices, void *indices, int32 numIndices);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType primType);
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices,
                                int32 numIndices);
void im3DEnd(void);

bool32 isInitialized(void);
const char *getAdapterName(void);
bool32 waitForGpu(void);

extern Device renderdevice;

}
}
