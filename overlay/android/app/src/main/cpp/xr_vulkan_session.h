#pragma once

struct android_app;

#include <vulkan/vulkan.h>

// OpenXR session bound to a Vulkan device, targeting a single two-layer
// swapchain rendered with VK_KHR_multiview.
//
// This is the platform floor the ported VR layer sits on. The desktop build
// binds OpenXR to D3D12 and resolves a double-wide target; here the equivalent
// role is played by an array target plus multiview, which is the arrangement
// Quest's runtime and tiler are built around.
namespace xrvk {

// Creates the loader, instance, system, Vulkan device and session. Safe to
// call once the activity exists; does not require a window surface.
bool create(android_app *app);
void destroy(void);

// Drains the OpenXR event queue and tracks session state. Returns false when
// the runtime has asked the application to exit.
bool pollEvents(void);

// True once the session reached a state in which frames must be submitted.
bool shouldRender(void);

// One xrWaitFrame / xrBeginFrame / xrEndFrame cycle.
void renderFrame(void);

// The Vulkan objects the runtime created, which librw's backend adopts.
struct GraphicsContext
{
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	VkQueue queue;
	unsigned int queueFamilyIndex;
	unsigned int width;
	unsigned int height;
	unsigned int viewCount;
	VkFormat colourFormat;
};

// Valid only after create() succeeded.
bool getContext(GraphicsContext *out);

// Snapshot of the tracked controllers, refreshed once per frame while the
// session holds focus. Axes are -1..1, triggers and grips 0..1.
struct TrackedPose
{
	float position[3];
	float orientation[4];
	bool valid;
};

struct ControllerInput
{
	float leftStickX, leftStickY;
	float rightStickX, rightStickY;
	float leftTrigger, rightTrigger;
	float leftGrip, rightGrip;
	bool a, b, x, y;
	bool menu;
	bool leftStickClick, rightStickClick;
	TrackedPose gripPose[2];
	TrackedPose aimPose[2];
};

void getInput(ControllerInput *out);

// Draws the contents of one frame, called between swapchain image acquire and
// release with the image already waited on. viewProj holds the per-eye
// view-projection matrices, column-major, indexed by view.
//
// Keeping this a callback is what stops the OpenXR layer from having to know
// about librw or the game at all; when none is installed the bring-up scene is
// drawn instead.
// im2dWorld maps screen pixel coordinates onto a quad placed in front of the
// head, so 2D content can be projected per eye instead of written straight to
// clip space.
// headPos is the mid-eye position in play space this frame; headYaw is the
// head's rotation about the vertical (radians, 0 = play-space forward -Z,
// positive turning left). The renderer anchors the game world to the head,
// so turning in place pivots around the player's own head rather than around
// wherever the Guardian origin happens to be in the room.
// headQuat is the full head orientation (x,y,z,w) and eyeFovDeg the
// horizontal field of view of an eye in degrees; both feed the game's own
// camera and sprite mathematics the way the desktop VR layer does.
// im2dDistance is where that plane sits, needed to turn a vertex's own depth
// back into a world position.
typedef void (*FrameRenderer)(VkImage image, VkImageView view,
                              const float viewProj[2][16],
                              const float im2dWorld[16], float im2dDistance,
                              const float headPos[3], float headYaw,
                              const float headQuat[4], float eyeFovDeg);
void setFrameRenderer(FrameRenderer renderer);

// Selects how the frame just rendered is submitted. Immersive gameplay uses
// the stereo projection layer; frontend and cinematic frames use array layer
// zero as one shared image on a world-locked cinema quad.
void setTheaterMode(bool enabled);

// Predicted display time of the frame currently being rendered, in
// nanoseconds. Zero before the first frame. Vsync-quantised, which makes it
// the right clock to drive game time from: wall-clock deltas jitter with
// scheduling while frames present on exact display intervals, and the
// difference reads as the world stuttering.
long long getPredictedDisplayTimeNs(void);

// RGBA pixels for the head-locked UI overlay quad, or nullptr to hide it.
// Compact diagnostics/settings use 512x128; full menus use 1024x768.
void setDebugOverlay(const unsigned char *rgba, int width, int height);

// Meta runtime app timings. When XR_META_performance_metrics is exposed these
// are the runtime's own CPU/GPU frame times, sampled after xrEndFrame. The
// Vulkan backend also keeps an independent timestamp pair for validation and
// as a fallback on runtimes without the extension.
struct PerformanceMetrics
{
	float appCpuFrameMs;
	float appGpuFrameMs;
	float displayRefreshRateHz;
	bool appCpuFrameValid;
	bool appGpuFrameValid;
};
void setPerformanceMetricsEnabled(bool enabled);
bool getPerformanceMetrics(PerformanceMetrics *out);

} // namespace xrvk
