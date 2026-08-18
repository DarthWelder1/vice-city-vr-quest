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
	float renderScaleEffectivePercent;
	unsigned int viewCount;
	VkFormat colourFormat;
};

// Runs rsINITIALIZE and rsRWINITIALIZE against the device the OpenXR session
// already created.
bool Initialise(const VulkanContext &context,
	bool *renderTargetStartupFailure = nullptr);

// Completes the one-time game/frontend setup before OpenXR starts asking the
// game for frames.  InitialiseOnceAfterRW can take hundreds of milliseconds;
// doing it from Step would leave an already-begun XR frame open for that whole
// time and make the compositor reproject a half-transitioned startup image.
bool PrepareFrontendBeforeFrames(void);

// Controller state pushed in from the OpenXR layer once per frame. CapturePad
// turns it into the game's pad state, which is the same seam the desktop build
// uses for its tracked controllers.
struct PadInput
{
	struct Pose
	{
		float position[3];
		float orientation[4];
		bool valid;
	};

	float leftStickX, leftStickY;
	float rightStickX, rightStickY;
	float leftTrigger, rightTrigger;
	float leftGrip, rightGrip;
	bool a, b, x, y;
	bool menu;
	bool leftStickClick, rightStickClick;
	Pose gripPose[2];
	Pose aimPose[2];
};

void SetPadInput(const PadInput &input);
const PadInput &GetPadInput(void);
// Weapon-fire vibration on one controller; forwarded to the OpenXR session.
void TriggerWeaponHaptic(int hand, float strength);
// Preferred display refresh rate in Hz; forwarded to the OpenXR session.
void SetPreferredRefreshRate(int hz);

// Renders the native Quest tracked hands after the world/effects pass. The
// function is a no-op in cinema mode and until both OpenXR and the player
// camera have produced a valid pose for the current frame.
void RenderTrackedHands(void);

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
bool VrMenuConsumesInput(void);
bool VrViceCityColorEnabled(void);
bool VrFxaaEnabled(void);
int VrSpatialAaMode(void);
// Wrist panels: the minimap and the money/health/wanted readout, each on its
// own arm. Index with the WRIST_PANEL_* values from librw's rwvk.h.
bool VrWristPanelEnabled(int panel);
bool VrWristPanelUnderside(int panel);
// Which wrist a panel is worn on, and where exactly it sits there. Nothing
// about a grip pose locates the wrist behind it, so this is calibrated in the
// HUD menu. The values belong to the side of the wrist in use, not to a hand:
// only the left hand is ever calibrated and the right one mirrors it.
int VrWristPanelHand(int panel);
void VrGetWristPanelCalibration(int panel, float *alongCm, float *acrossCm,
                                float *liftCm, float *pitchDeg, float *yawDeg,
                                float *rollDeg, float *scale);
// The weapon icon and ammo counter in the corner of the interface, and the
// clock above them. Both are switches of their own on the headset.
bool VrHudWeaponPanelEnabled(void);
bool VrHudClockEnabled(void);
bool VrGameplayHudEnabled(void);
// Puts the interface plane on a wrist and hands back that panel's texture to
// bind, or null when there is nothing to show yet; End restores the
// head-locked plane. Each call also asks the backend to render the panel at
// the top of the next frame -- that request is one-shot on purpose.
void *BeginVrWristPanel(int panel, float centreX, float centreY, float width,
                        float height);
void EndVrWristPanel(void);
void VrGetGameplayHudSettings(int *widthPercent, int *scalePercent,
                              int *offsetXCm, int *offsetYCm);
bool VrUsesHeadRelativeMovement(void);
// HEAD DIRECTED only: Tommy is reoriented onto the movement direction.
bool VrUsesHeadDirectedMovement(void);
bool VrHeadBobbingEnabled(void);
bool VrUsesExperimentalHeadTurning(void);
float VrHeadTurnScale(void);
bool VrUsesSnapTurn(void);
float VrSmoothTurnScale(void);
int VrSnapTurnAngleDegrees(void);
float VrScopeZoomFactor(void);

// Frontend, loading and cinematic frames are flat content. They are rendered
// once and submitted to both eyes on a world-locked cinema quad instead of
// being interpreted as an immersive stereo world.
bool VrShouldUseTheaterMode(void);

// One iteration of the game's gGameState machine, including rsIDLE. Must be
// called between vulkan::beginFrame and vulkan::endFrame.
void Step(void);

// True once the game asked to exit.
bool WantsToQuit(void);

void Shutdown(void);

} // namespace androidgame
