#pragma once

// Bridge between the Android/OpenXR application layer and the game.
//
// On Windows the skeleton owns main() and spins its own loop. Here the loop
// belongs to OpenXR: xrWaitFrame paces the application, and the game has to be
// stepped from inside a frame that has already begun. So the skeleton is
// reduced to init / step / shutdown and the app layer drives it.

#include <vulkan/vulkan.h>

namespace androidgame {

// The Vulkan objects the OpenXR session created. librw's Vulkan backend adopts
// these rather than opening a device of its own, so they have to reach
// rsRWINITIALIZE intact: RwEngineOpen forwards displayID straight into
// rw::EngineOpenParams on this platform.
struct VulkanContext
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

// Runs rsINITIALIZE and rsRWINITIALIZE against the device the OpenXR session
// already created.
bool Initialise(const VulkanContext &context);

// Controller state pushed in from the OpenXR layer once per frame. CapturePad
// turns it into the game's pad state, which is the same seam the desktop build
// uses for its tracked controllers.
struct PadInput
{
	float leftStickX, leftStickY;
	float rightStickX, rightStickY;
	float leftTrigger, rightTrigger;
	float leftGrip, rightGrip;
	bool a, b, x, y;
	bool menu;
	bool leftStickClick, rightStickClick;
};

void SetPadInput(const PadInput &input);

// Predicted display time of the frame about to be stepped, in nanoseconds.
// Drives the game clock: it is vsync-quantised where the wall clock jitters
// with scheduling, and that jitter reads as world motion stuttering in the
// headset. Pass 0 to fall back to the wall clock.
void SetFrameTimeNs(long long ns);

// Horizontal field of view of one eye, degrees. The game's sprite and
// screen mathematics run against it during rendering, as on the desktop.
void SetEyeFovDeg(float fov);

// The game camera's view window (tangent of the half angles). The Im2D plane
// has to subtend exactly this, or screen coordinates map to the wrong
// direction and world sprites drift as the player moves.
void GetIm2DViewWindow(float *x, float *y);

// Desktop debug overlay port (vrdebug.cpp): chord handling + FPS smoothing,
// then the RGBA pixel block for the compositor quad layer (nil = hidden).
void VrDebugUpdate(const PadInput &input);
const unsigned char *VrDebugPixels(int *width, int *height);

// One iteration of the game's gGameState machine, including rsIDLE. Must be
// called between vulkan::beginFrame and vulkan::endFrame.
void Step(void);

// True once the game asked to exit.
bool WantsToQuit(void);

void Shutdown(void);

} // namespace androidgame
