#include "common.h"

#ifdef GTA_VR_OPENXR

#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#ifdef RW_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#include <d3d12.h>
#else
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifdef RW_D3D12
#include "../../vendor/librw/src/d3d12/rwd3d12impl.h"
#endif

#include "OculusVR.h"
#include "DLAA.h"
#include "Camera.h"
#include "ControllerConfig.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "Game.h"
#include "Hud.h"
#include "Matrix.h"
#include "Pad.h"
#include "PlayerPed.h"
#include "ProjectileInfo.h"
#include "Timer.h"
#include "Vehicle.h"
#include "Automobile.h"
#include "Bike.h"
#include "VehicleModelInfo.h"
#include "World.h"
#include "postfx.h"
#include "WeaponInfo.h"
#include "WeaponType.h"
#include "crossplatform.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <float.h>
#include <time.h>
#include <vector>

extern void RenderVrGameplayHud(void);
extern bool CaptureMovieFrameRGBA(uint8 *destination, int destinationWidth, int destinationHeight);
extern int GetVrCheatCount(void);
extern const char *GetVrCheatName(int index);
extern bool ActivateVrCheat(int index);
extern const char *GetVrVehicleModelName(int model);
extern const char *GetVrCurrentWeaponName(void);
extern int GetVrCurrentWeaponType(void);
extern int GetVrWeaponTypeForSlot(int slot);
extern const char *GetVrWeaponName(int weaponType);
extern bool IsVrWeaponSlotOwned(int slot);
class CVehicle;
extern CVehicle *FindPlayerVehicle(void);

namespace OculusVR
{
bool IsTrackedDetonatorActiveInternal(int hand);
bool IsTrackedDetonatorHandReservedInternal(int hand);
uint32 GetTrackedDetonatorHandMaskInternal();
void ResetTrackedDetonatorInteraction(bool clearCharges);
bool BuildTrackedWeaponAimInternal(int hand, int weaponType, CVector *source,
	CVector *direction, bool applyOneHandSway);
void UpdateTrackedScopeState();
void ResetTrackedScopeState();
void ClearDroppedWeapon(int hand);
bool IsHandBusyWithReload(int hand);

namespace
{
bool IsWeaponSupportHandInternal(int hand);
void ClearWeaponSupportForHand(int hand);

enum eVrHolsterPoint
{
	HOLSTER_WAIST_LEFT = 0,
	HOLSTER_WAIST_RIGHT,
	HOLSTER_CHEST_LEFT,
	HOLSTER_CHEST_RIGHT,
	HOLSTER_CHEST_CENTER,
	HOLSTER_BACK_LEFT,
	HOLSTER_BACK_RIGHT,
	HOLSTER_POINT_COUNT
};

enum {
	EYE_COUNT = 2,
	VR_DEBUG_WIDTH = 512,
	VR_DEBUG_HEIGHT = 128,
	VR_MENU_WIDTH = 1024,
	VR_MENU_HEIGHT = 768,
	VR_STARTUP_WIDTH = 1280,
	VR_STARTUP_HEIGHT = 720,
	VR_HUD_WIDTH = 1920,
	VR_HUD_HEIGHT = 1080,
	HOLSTER_MENU_ITEM_COUNT = HOLSTER_POINT_COUNT+1
};
const float gRenderScaleOptions[] = {
	1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.25f, 2.5f,
	2.75f, 3.0f, 3.25f, 3.5f, 3.75f, 4.0f
};
enum { VR_RENDER_SCALE_DEFAULT = 3 };
enum { DLAA_ACTIVATION_WARMUP_FRAMES = 30 };

enum eVrDrivingType
{
	VR_DRIVING_DEFAULT = 0,
	VR_DRIVING_IMMERSIVE,
	VR_DRIVING_MOTION,
	VR_DRIVING_TYPE_COUNT
};

enum eVrMainMenuItem
{
	VR_MAIN_RENDER_SCALE = 0,
	VR_MAIN_VRS,
	VR_MAIN_DLAA,
	VR_MAIN_JITTER,
	VR_MAIN_FXAA,
	VR_MAIN_COLOR,
	VR_MAIN_HUD,
	VR_MAIN_HANDS,
	VR_MAIN_LASER,
	VR_MAIN_HOLSTER_HIGHLIGHTS,
	VR_MAIN_MANUAL_RELOAD,
	VR_MAIN_SCOPE_AIM,
	VR_MAIN_VEHICLE_SETTINGS,
	VR_MAIN_DEBUG,
	VR_MAIN_GRIP_LOCK,
	VR_MAIN_CALIBRATION,
	VR_MAIN_HOLSTERS,
	VR_MENU_ITEM_COUNT
};

enum eVrVehicleMenuItem
{
	VR_VEHICLE_DRIVING_TYPE = 0,
	VR_VEHICLE_DRIVING_Y,
	VR_VEHICLE_MOTION_HAND,
	VR_VEHICLE_HANDLE_HIGHLIGHTS,
	VR_VEHICLE_CALIBRATION,
	VR_VEHICLE_BACK,
	VR_VEHICLE_MENU_ITEM_COUNT
};

enum eVrCalibrationMenuItem
{
	VR_CAL_AIM_OFFSET_X = 0,
	VR_CAL_AIM_OFFSET_Y,
	VR_CAL_AIM_OFFSET_Z,
	VR_CAL_AIM_ROT_X,
	VR_CAL_AIM_ROT_Y,
	VR_CAL_AIM_ROT_Z,
	VR_CAL_WEAPON_OFFSET_X,
	VR_CAL_WEAPON_OFFSET_Y,
	VR_CAL_WEAPON_OFFSET_Z,
	VR_CAL_WEAPON_ROT_X,
	VR_CAL_WEAPON_ROT_Y,
	VR_CAL_WEAPON_ROT_Z,
	VR_CAL_SUPPORT_OFFSET_X,
	VR_CAL_SUPPORT_OFFSET_Y,
	VR_CAL_SUPPORT_OFFSET_Z,
	VR_CAL_BACK,
	VR_CALIBRATION_MENU_ITEM_COUNT
};

enum eVrBikeCalibrationMenuItem
{
	VR_BIKE_CAL_HAND = 0,
	VR_BIKE_CAL_OFFSET_X,
	VR_BIKE_CAL_OFFSET_Y,
	VR_BIKE_CAL_OFFSET_Z,
	VR_BIKE_CAL_ROT_X,
	VR_BIKE_CAL_ROT_Y,
	VR_BIKE_CAL_ROT_Z,
	VR_BIKE_CAL_WHEELIE_HEIGHT,
	VR_BIKE_CAL_STAND_HEIGHT,
	VR_BIKE_CAL_BACK,
	VR_BIKE_CALIBRATION_MENU_ITEM_COUNT
};

struct Swapchain
{
	XrSwapchain handle;
#ifdef RW_D3D12
	std::vector<XrSwapchainImageD3D12KHR> images;
#else
	std::vector<XrSwapchainImageOpenGLKHR> images;
#endif
	int width;
	int height;
	uint32_t acquiredIndex;
	bool acquired;

	Swapchain() : handle(XR_NULL_HANDLE), width(0), height(0), acquiredIndex(0), acquired(false) {}
};

struct EyeBuffer
{
	Swapchain swapchain;
	RwRaster *color;
	RwRaster *depth;
	int renderWidth;
	int renderHeight;

	EyeBuffer() : color(nil), depth(nil), renderWidth(0), renderHeight(0) {}
};

struct Actions
{
	XrActionSet set;
	XrPath hands[EYE_COUNT];
	XrAction stick;
	XrAction squeeze;
	XrAction trigger;
	XrAction gripPose;
	XrAction aimPose;
	XrSpace gripSpace[EYE_COUNT];
	XrSpace aimSpace[EYE_COUNT];
	XrAction a;
	XrAction b;
	XrAction x;
	XrAction y;
	XrAction stickClick;
	XrAction menu;

	Actions() : set(XR_NULL_HANDLE), stick(XR_NULL_HANDLE), squeeze(XR_NULL_HANDLE),
		trigger(XR_NULL_HANDLE), gripPose(XR_NULL_HANDLE), aimPose(XR_NULL_HANDLE), a(XR_NULL_HANDLE), b(XR_NULL_HANDLE), x(XR_NULL_HANDLE),
		y(XR_NULL_HANDLE), stickClick(XR_NULL_HANDLE), menu(XR_NULL_HANDLE)
	{
		hands[0] = hands[1] = XR_NULL_PATH;
		gripSpace[0] = gripSpace[1] = XR_NULL_HANDLE;
		aimSpace[0] = aimSpace[1] = XR_NULL_HANDLE;
	}
};

XrInstance gInstance = XR_NULL_HANDLE;
XrSystemId gSystemId = XR_NULL_SYSTEM_ID;
XrSession gSession = XR_NULL_HANDLE;
XrSpace gLocalSpace = XR_NULL_HANDLE;
XrSpace gViewSpace = XR_NULL_HANDLE;
XrSpace gGameplaySpace = XR_NULL_HANDLE;
XrSessionState gSessionState = XR_SESSION_STATE_UNKNOWN;
bool gSessionRunning;
bool gExitRequested;
bool gFrameBegun;
bool gFramePrepared;
bool gWasSubmitting;
bool gTouchWasConnected;
bool gTrackedWeaponTriggerPressed[EYE_COUNT];
bool gTrackedWeaponTriggerJustPressed[EYE_COUNT];
bool gTrackedWeaponTriggerJustReleased[EYE_COUNT];
// The remote-grenade controller is an auxiliary tracked prop rather than a
// Vice City inventory weapon.  Keeping it outside gHeldWeaponSlot preserves the
// projectile in one hand, permits several charges to be thrown in succession,
// and avoids replacing the camera which shares WEAPONSLOT_OTHER.
int gTrackedDetonatorHand = -1;
bool gTrackedDetonatorWasActive[EYE_COUNT];
bool gTrackedDetonatorWaitForTriggerRelease[EYE_COUNT];
bool gTrackedDetonatorTriggerJustPressed[EYE_COUNT];
bool gTrackedThrowablePreviewActive[EYE_COUNT];
struct ManualReloadState
{
	bool active;
	bool requested;
	bool movedAwayFromSpot;
	int weaponHand;
	int magazineHand;
	int slot;
	int weaponType;
	ULONGLONG grabbedAt;

	ManualReloadState() : active(false), requested(false),
		movedAwayFromSpot(false), weaponHand(-1), magazineHand(-1),
		slot(-1), weaponType(-1), grabbedAt(0) {}
};
ManualReloadState gManualReload[EYE_COUNT];
bool gManualReloadGripDown[EYE_COUNT];
uint32 gWeaponHolsterMask;
// Six configurable equipment points plus one dedicated throwable point replace
// the old implicit one-position-per-slot layout. Configurable values are Vice
// City inventory slots, or -1 for an empty point.
int gHolsterPointWeaponSlot[HOLSTER_POINT_COUNT] = {
	WEAPONSLOT_SUBMACHINEGUN, WEAPONSLOT_HANDGUN,
	WEAPONSLOT_SHOTGUN, WEAPONSLOT_MELEE,
	WEAPONSLOT_PROJECTILE, WEAPONSLOT_HEAVY, WEAPONSLOT_RIFLE
};
const char *gHolsterPointNames[HOLSTER_POINT_COUNT] = {
	"WAIST LEFT", "WAIST RIGHT", "CHEST LEFT", "CHEST RIGHT",
	"CHEST CENTER - THROWABLE", "BACK LEFT", "BACK RIGHT"
};
const char *gHolsterPointSettingNames[HOLSTER_POINT_COUNT] = {
	"HolsterWaistLeftSlot", "HolsterWaistRightSlot",
	"HolsterChestLeftSlot", "HolsterChestRightSlot",
	nil, "HolsterBackLeftSlot", "HolsterBackRightSlot"
};
const int gHolsterSlotChoices[] = {
	-1, WEAPONSLOT_MELEE, WEAPONSLOT_HANDGUN, WEAPONSLOT_SHOTGUN,
	WEAPONSLOT_SUBMACHINEGUN, WEAPONSLOT_RIFLE, WEAPONSLOT_HEAVY,
	WEAPONSLOT_SNIPER, WEAPONSLOT_OTHER
};
int gWeaponHolsterSelection[EYE_COUNT] = { -1, -1 };
bool gWeaponHolsterGripDown[EYE_COUNT];
int gHeldWeaponSlot[EYE_COUNT] = { -1, -1 };
// A supporting hand is deliberately not given a second copy of the inventory
// slot.  The primary hand remains the sole owner of the weapon/trigger while
// this relation only supplies the two-handed pose.
int gWeaponSupportHand[EYE_COUNT] = { -1, -1 };
CMatrix gTrackedWeaponRenderMatrix[EYE_COUNT];
int gTrackedWeaponRenderMatrixSlot[EYE_COUNT] = { -1, -1 };
CMatrix gTrackedWeaponContactMatrix[EYE_COUNT];
int gTrackedWeaponContactMatrixSlot[EYE_COUNT] = { -1, -1 };
int gTrackedWeaponContactMatrixType[EYE_COUNT] = { -1, -1 };
uint32 gTrackedWeaponContactMatrixFrame[EYE_COUNT] = { 0, 0 };
int gDroppedWeaponSlot[EYE_COUNT] = { -1, -1 };
CMatrix gDroppedWeaponMatrix[EYE_COUNT];
CVector gDroppedWeaponStartPosition[EYE_COUNT];
CVector gDroppedWeaponGravityUp[EYE_COUNT];
CVector gDroppedWeaponLinearVelocity[EYE_COUNT];
CVector gDroppedWeaponAngularVelocity[EYE_COUNT];
ULONGLONG gDroppedWeaponStartTime[EYE_COUNT];
int gActiveTrackedFireHand = -1;
int gActiveTrackedFireWeaponType = -1;
bool gActiveTrackedFireAimValid;
CVector gActiveTrackedFireSource;
CVector gActiveTrackedFireDirection;
bool gTrackedAimCacheValid[EYE_COUNT];
uint32 gTrackedAimCacheFrame[EYE_COUNT] = { 0xFFFFFFFFU, 0xFFFFFFFFU };
int gTrackedAimCacheWeaponType[EYE_COUNT] = { -1, -1 };
CVector gTrackedAimCacheSource[EYE_COUNT];
CVector gTrackedAimCacheDirection[EYE_COUNT];
int gTrackedScopeHand = -1;
int gTrackedScopeWeaponType = -1;
int gTrackedScopeCandidateHand = -1;
int gTrackedScopeCandidateWeaponType = -1;
ULONGLONG gTrackedScopeCandidateSince;
ULONGLONG gTrackedScopeInvalidSince;
bool gTrackedScopeReticleTargetValid;
CVector gTrackedScopeReticleTarget;
struct PhysicalMeleeStrike
{
	bool pending;
	int slot;
	int weaponType;
	CVector sweepStart;
	CVector sweepEnd;
	CVector rootStart;
	CVector rootEnd;
	float speed;
	uint32 frame;

	PhysicalMeleeStrike() : pending(false), slot(-1), weaponType(-1),
		speed(0.0f), frame(0) {}
};
PhysicalMeleeStrike gPhysicalMeleeStrike[EYE_COUNT];
struct PhysicalMeleeMotion
{
	bool valid;
	bool armed;
	int slot;
	int weaponType;
	CVector previousWorldPoint;
	CVector previousWorldRoot;
	CVector previousTrackingPoint;
	CVector previousTrackingRoot;
	uint32 lastStrikeTime;
	uint32 calmSinceTime;
	uint32 previousFrame;
	bool usedContactMatrix;
	int supportHand;
	bool strikeInProgress;
	float strikePeakSpeed;
	uint32 strikeContinueUntil;

	PhysicalMeleeMotion() : valid(false), armed(false), slot(-1),
		weaponType(-1), previousWorldPoint(0.0f, 0.0f, 0.0f),
		previousWorldRoot(0.0f, 0.0f, 0.0f),
		previousTrackingPoint(0.0f, 0.0f, 0.0f),
		previousTrackingRoot(0.0f, 0.0f, 0.0f),
		lastStrikeTime(0), calmSinceTime(0), previousFrame(0),
		usedContactMatrix(false), supportHand(-1), strikeInProgress(false),
		strikePeakSpeed(0.0f), strikeContinueUntil(0) {}
};
PhysicalMeleeMotion gPhysicalMeleeMotion[EYE_COUNT];
bool gPhysicalMeleeFreshGrab[EYE_COUNT] = { false, false };
bool gPhysicalGameplayWasAvailable;
bool gTouchPerfShortcutDown;
bool gTouchDebugShortcutDown;
bool gTouchWeatherShortcutDown;
bool gTouchRecenterShortcutDown;
bool gTouchSpsShortcutDown;
bool gTouchVrsShortcutDown;
bool gTouchVrMenuShortcutDown;
bool gRecenterRequested;
bool gFirstPersonEnabled = true;
bool gTrackingCenterValid;
bool gDebugVisible;
bool gDlaaEnabled = true;
bool gDlaaStereoActivationReady;
bool gDlaaStereoActivationFailed;
uint32 gDlaaStereoWarmupFrames = DLAA_ACTIVATION_WARMUP_FRAMES;
bool gAntiAliasingEnabled = true;
bool gLightingEnabled = true;
bool gGameplayHudVisible = false;
bool gVrHandsEnabled = true;
bool gWeaponLaserEnabled = false;
bool gWeaponHolsterHighlightsEnabled = false;
bool gManualReloadEnabled = false;
bool gPhysicalScopeAimEnabled = true;
bool gWeaponGripLockEnabled;
int gDrivingType;
int gMotionSteeringHand = 1;
bool gBikeHandleHighlightsEnabled = true;
bool gFullStereoSinglePass = true;
bool gVrMenuVisible;
bool gVrVehicleMenuVisible;
bool gVrHolsterMenuVisible;
bool gVrCalibrationMenuVisible;
bool gVrBikeCalibrationMenuVisible;
bool gCheatMenuVisible;
bool gVrMenuVerticalDown;
bool gVrMenuHorizontalDown;
bool gVrMenuSelectDown;
bool gVrMenuBackDown;
bool gVrMenuDecreaseDown;
bool gVrMenuIncreaseDown;
ULONGLONG gVrMenuDecreaseRepeatAt;
ULONGLONG gVrMenuIncreaseRepeatAt;
bool gVrSettingsLoaded;
bool gRenderScaleChangePending;
int gVrMenuSelection;
int gVrVehicleMenuSelection;
int gVrHolsterMenuSelection;
int gVrCalibrationMenuSelection;
int gVrBikeCalibrationMenuSelection;
int gCheatMenuSelection;
// Calibration integers are stored in half-units: 1 = 0.5 cm or 0.5 degree.
// This keeps INI values exact while allowing sub-centimetre/sub-degree tuning.
enum { WEAPON_CALIBRATION_VALUE_SCALE = 2 };
int gWeaponOffsetXCm = 0;
int gWeaponOffsetYCm = 2;
int gWeaponOffsetZCm = -10;
int gWeaponAimOffsetXCm = 0;
int gWeaponAimOffsetYCm = 8;
int gWeaponAimOffsetZCm = 0;
int gWeaponAimRotationXDeg = 0;
int gWeaponAimRotationYDeg = 0;
int gWeaponAimRotationZDeg = 0;
int gWeaponRotationXDeg = 0;
int gWeaponRotationYDeg = 18;
int gWeaponRotationZDeg = 14;
int gDrivingYOffsetCm = 15;
enum { VR_BIKE_MODEL_COUNT = 6 };
const int gVrBikeModels[VR_BIKE_MODEL_COUNT] = {
	MI_ANGEL, MI_FREEWAY, MI_PCJ600,
	MI_FAGGIO, MI_PIZZABOY, MI_SANCHEZ
};
const char *gVrBikeNames[VR_BIKE_MODEL_COUNT] = {
	"ANGEL", "FREEWAY", "PCJ 600", "FAGGIO", "PIZZA BOY", "SANCHEZ"
};
struct BikeHandleCalibration
{
	int offsetX, offsetY, offsetZ;
	int rotationX, rotationY, rotationZ;
	bool valid;
	BikeHandleCalibration() : offsetX(0), offsetY(0), offsetZ(0),
		rotationX(0), rotationY(0), rotationZ(0), valid(false) {}
};
BikeHandleCalibration gBikeHandleCalibration[VR_BIKE_MODEL_COUNT][EYE_COUNT];
enum { VR_CAR_MODEL_COUNT = MI_LAST_VEHICLE-MI_FIRST_VEHICLE+1 };
BikeHandleCalibration gCarWheelCalibration[VR_CAR_MODEL_COUNT][EYE_COUNT];
struct BikeLeanCalibration
{
	int wheelieHeightCm;
	int standHeightCm;
	bool valid;
	BikeLeanCalibration() : wheelieHeightCm(20), standHeightCm(50),
		valid(false) {}
};
BikeLeanCalibration gBikeLeanCalibration[VR_BIKE_MODEL_COUNT];
int gBikeCalibrationEditHand;
bool gBikeHandleGrabbed[EYE_COUNT];
bool gBikeHandleGripDown[EYE_COUNT];
float gImmersiveBikeSteering;
float gImmersiveBikeThrottle;
float gImmersiveBikeLean;
bool gBikeThrottleReferenceValid;
CVector gBikeThrottleReference;
bool gBikeLeanReferenceValid[EYE_COUNT];
float gBikeLeanReferenceUp[EYE_COUNT];
int gBikeLeanGestureState;
float gBikeHandleDistance[EYE_COUNT] = { 1000.0f, 1000.0f };
bool gCarWheelGrabbed[EYE_COUNT];
bool gCarWheelGripDown[EYE_COUNT];
float gCarWheelDistance[EYE_COUNT] = { 1000.0f, 1000.0f };
float gCarWheelGrabReferenceAngle[EYE_COUNT];
float gCarWheelContinuousAngle[EYE_COUNT];
bool gCarWheelAngleValid[EYE_COUNT];
float gCarWheelTwoHandReferenceAngle;
float gCarWheelTwoHandContinuousAngle;
bool gCarWheelTwoHandAngleValid;
float gImmersiveCarSteering;
float gImmersiveCarPhysicalAngle;
bool gImmersiveCarHornPressed;
bool gCarHornContact[EYE_COUNT];
bool gCarHornArmed[EYE_COUNT];
float gCarHornPreviousDistance[EYE_COUNT] = { 1000.0f, 1000.0f };
bool gVrRadioButtonDown;
bool gVrRadioChangeJustPressed;
CVehicle *gMotionSteeringVehicle;
float gMotionVehicleSteering;
float gMotionVehiclePhysicalAngle;
bool gMotionSteeringReferenceValid;
float gMotionSteeringReferenceHeading;
bool gNewGameCinemaHold;
ULONGLONG gNewGameCinemaHoldStartedAt;
int gActiveWeaponCalibration = -1;
int gActiveWeaponCalibrationHand = -1;
int gCalibrationEditHand = 1;
struct WeaponCalibration
{
	int offsetX, offsetY, offsetZ;
	int aimOffsetX, aimOffsetY, aimOffsetZ;
	int aimRotationX, aimRotationY, aimRotationZ;
	int rotationX, rotationY, rotationZ;
	bool valid;
	WeaponCalibration() : offsetX(0), offsetY(2), offsetZ(-10),
		aimOffsetX(0), aimOffsetY(8), aimOffsetZ(0),
		aimRotationX(0), aimRotationY(0), aimRotationZ(0),
		rotationX(0), rotationY(18), rotationZ(14), valid(false) {}
};
WeaponCalibration gWeaponCalibration[EYE_COUNT][WEAPONTYPE_TOTALWEAPONS];
int gSupportGripOffsetXCm = 0;
int gSupportGripOffsetYCm = 60;
int gSupportGripOffsetZCm = -10;
int gActiveSupportGripCalibration = -1;
int gActiveSupportGripCalibrationHand = -1;
struct SupportGripCalibration
{
	int offsetX, offsetY, offsetZ;
	bool valid;
	SupportGripCalibration() : offsetX(0), offsetY(60), offsetZ(-10),
		valid(false) {}
};
SupportGripCalibration gSupportGripCalibration[EYE_COUNT][WEAPONTYPE_TOTALWEAPONS];
XrPosef gTrackedHandPose[EYE_COUNT];
bool gTrackedHandPoseValid[EYE_COUNT];
XrPosef gTrackedHandAimPose[EYE_COUNT];
bool gTrackedHandAimPoseValid[EYE_COUNT];
XrVector3f gTrackedHandLinearVelocity[EYE_COUNT];
XrVector3f gTrackedHandAngularVelocity[EYE_COUNT];
float gTrackedHandGrip[EYE_COUNT];
float gTrackedHandTrigger[EYE_COUNT];
int gRenderScaleIndex = VR_RENDER_SCALE_DEFAULT;
// Calibration modes keep the rendered Halton projection identical while
// varying only the pixel-space value reported to Streamline.  OFF is the sole
// mode which disables projection jitter itself.
int gTemporalJitterMode = 1;
float gRenderScale = gRenderScaleOptions[VR_RENDER_SCALE_DEFAULT];
uint32 gTemporalFrameIndex;
float gTemporalJitterX;
float gTemporalJitterY;
float gTemporalJitterClipX;
float gTemporalJitterClipY;
#ifdef RW_D3D12
uint32 gFixedFoveatedProfile = rw::d3d12::FIXED_FOVEATED_QUALITY;
#endif
int gRetryFrames;
int64_t gColorFormat;
XrFrameState gFrameState = { XR_TYPE_FRAME_STATE };
XrView gLocatedViews[EYE_COUNT] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
XrPosef gRenderPose[EYE_COUNT];
XrFovf gRenderFov[EYE_COUNT];
float gSourceTanX;
float gSourceTanY;
XrVector3f gTrackingCenterOrigin;
XrViewConfigurationView gViewConfig[EYE_COUNT] = {
	{ XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW }
};
EyeBuffer gEye[EYE_COUNT];
#ifdef RW_D3D12
// Both eyes share one double-wide render target.  The per-eye rasters below
// are lightweight sub-rasters that select their half of this allocation.
// This keeps the existing sequential eye path bit-for-bit useful while also
// providing the target layout required by single-pass stereo.
RwRaster *gStereoColor;
RwRaster *gStereoDepth;
#endif
Swapchain gHudSwapchain;
Swapchain gCinemaSwapchain;
bool gCinemaFrameValid;
bool gCinemaAnchorValid;
XrPosef gCinemaAnchorPose;
Swapchain gDebugSwapchain;
Swapchain gVrMenuSwapchain;
RwRaster *gHudColor;
RwRaster *gHudDepth;
#ifndef RW_D3D12
GLuint gCopyFramebuffer;
GLuint gFxaaProgram;
GLuint gFxaaVertexArray;
GLint gFxaaTextureUniform = -1;
GLint gFxaaInverseSizeUniform = -1;
GLint gFxaaUvScaleUniform = -1;
GLint gFxaaUvOffsetUniform = -1;
GLint gFxaaEnabledUniform = -1;
GLint gColorModeUniform = -1;
GLint gBlurColorUniform = -1;
GLint gContrastMultUniform = -1;
GLint gContrastAddUniform = -1;
#endif
Actions gActions;

uint8 gDebugPixels[VR_DEBUG_WIDTH*VR_DEBUG_HEIGHT*4];
uint8 gVrMenuPixels[VR_MENU_WIDTH*VR_MENU_HEIGHT*4];
uint8 gStartupPixels[VR_STARTUP_WIDTH*VR_STARTUP_HEIGHT*4];
struct VrWeaponIconCache
{
	int width;
	int height;
	std::vector<uint8> rgba;
	VrWeaponIconCache() : width(0), height(0) {}
};
VrWeaponIconCache gVrWeaponIconCache[WEAPONTYPE_TOTALWEAPONS];
HDC gStartupCaptureDc;
HBITMAP gStartupCaptureBitmap;
HGDIOBJ gStartupCaptureOldBitmap;
void *gStartupCaptureBits;
double gDebugPreviousFrameMs;
float gDebugSmoothedFrameMs;
int gDebugFps;
bool gVrLogStarted;
int gVrLoggedFrames;
int gVrLoggedRenderableFrames;

void VrLog(const char *format, ...)
{
#ifdef RW_D3D12
	FILE *file = fopen("openxr_d3d12.log", gVrLogStarted ? "a" : "w");
	if(!file)
		return;
	gVrLogStarted = true;
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
	fclose(file);
#else
	(void)format;
#endif
}

struct DebugGlyph { char character; uint8 rows[7]; };
const DebugGlyph gDebugGlyphs[] = {
	{' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, {'%',{0x11,0x12,0x02,0x04,0x08,0x09,0x11}},
	{'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, {'<',{0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
	{'>',{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
	{':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}}, {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
	{'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}, {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
	{'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}}, {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
	{'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}}, {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
	{'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}, {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
	{'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}}, {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
	{'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
	{'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
	{'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'I',{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}}, {'J',{0x01,0x01,0x01,0x01,0x11,0x11,0x0E}},
	{'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
	{'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x19,0x15,0x13,0x13,0x11}},
	{'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
	{'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
	{'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}, {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
	{'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
	{'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
	{'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}}
};

void BackupVrSettings(const char *reason);

bool IsAssignableHolsterSlot(int slot)
{
	return slot == WEAPONSLOT_MELEE || slot == WEAPONSLOT_HANDGUN ||
		slot == WEAPONSLOT_SHOTGUN ||
		slot == WEAPONSLOT_SUBMACHINEGUN || slot == WEAPONSLOT_RIFLE ||
		slot == WEAPONSLOT_HEAVY || slot == WEAPONSLOT_SNIPER ||
		slot == WEAPONSLOT_OTHER;
}

void LoadVrSettings()
{
	if(gVrSettingsLoaded)
		return;
	BackupVrSettings("startup");
	const char *path = ".\\vr_settings.ini";
	gRenderScaleIndex = GetPrivateProfileIntA("VR", "RenderScale", VR_RENDER_SCALE_DEFAULT, path);
	gRenderScaleIndex = Min(Max(gRenderScaleIndex, 0), (int)ARRAY_SIZE(gRenderScaleOptions)-1);
	gRenderScale = gRenderScaleOptions[gRenderScaleIndex];
	gDlaaEnabled = GetPrivateProfileIntA("VR", "DLAA", 1, path) != 0;
	gTemporalJitterMode = GetPrivateProfileIntA("VR", "TemporalJitter", 1, path);
	gTemporalJitterMode = Min(Max(gTemporalJitterMode, 0), 5);
	gAntiAliasingEnabled = GetPrivateProfileIntA("VR", "AntiAliasing", 1, path) != 0;
	gLightingEnabled = GetPrivateProfileIntA("VR", "ViceCityColor", 1, path) != 0;
	gGameplayHudVisible = GetPrivateProfileIntA("VR", "GameplayHud", 0, path) != 0;
	gVrHandsEnabled = GetPrivateProfileIntA("VR", "VrHands", 1, path) != 0;
	gWeaponLaserEnabled = GetPrivateProfileIntA("VR", "WeaponLaser", 0, path) != 0;
	gWeaponHolsterHighlightsEnabled =
		GetPrivateProfileIntA("VR", "HolsterHighlights", 0, path) != 0;
	gManualReloadEnabled =
		GetPrivateProfileIntA("VR", "ManualReloading", 0, path) != 0;
	gPhysicalScopeAimEnabled =
		GetPrivateProfileIntA("VR", "PhysicalScopeAim", 1, path) != 0;
	gWeaponGripLockEnabled =
		GetPrivateProfileIntA("VR", "WeaponGripLock", 0, path) != 0;
	char drivingTypeText[16] = {};
	GetPrivateProfileStringA("VR", "DrivingType", "", drivingTypeText,
		sizeof(drivingTypeText), path);
	if(drivingTypeText[0] != '\0')
		gDrivingType = atoi(drivingTypeText);
	else
		gDrivingType =
			GetPrivateProfileIntA("VR", "ImmersiveDriving", 0, path) != 0 ?
				VR_DRIVING_IMMERSIVE : VR_DRIVING_DEFAULT;
	gDrivingType = Min(Max(gDrivingType, (int)VR_DRIVING_DEFAULT),
		(int)VR_DRIVING_TYPE_COUNT-1);
	gMotionSteeringHand =
		GetPrivateProfileIntA("VR", "MotionSteeringHand", 1, path);
	gMotionSteeringHand = Min(Max(gMotionSteeringHand, 0), EYE_COUNT-1);
	gBikeHandleHighlightsEnabled =
		GetPrivateProfileIntA("VR", "BikeHandleHighlights", 1, path) != 0;
	const int defaultHolsterSlots[HOLSTER_POINT_COUNT] = {
		WEAPONSLOT_SUBMACHINEGUN, WEAPONSLOT_HANDGUN,
		WEAPONSLOT_SHOTGUN, WEAPONSLOT_MELEE,
		WEAPONSLOT_PROJECTILE, WEAPONSLOT_HEAVY, WEAPONSLOT_RIFLE
	};
	bool usedHolsterSlots[TOTAL_WEAPON_SLOTS] = {};
	gHolsterPointWeaponSlot[HOLSTER_CHEST_CENTER] = WEAPONSLOT_PROJECTILE;
	usedHolsterSlots[WEAPONSLOT_PROJECTILE] = true;
	for(int point = 0; point < HOLSTER_POINT_COUNT; point++){
		if(point == HOLSTER_CHEST_CENTER)
			continue;
		int slot = (int)(int32)GetPrivateProfileIntA("VR",
			gHolsterPointSettingNames[point], defaultHolsterSlots[point], path);
		if(slot != -1 && (!IsAssignableHolsterSlot(slot) ||
		   usedHolsterSlots[slot]))
			slot = -1;
		gHolsterPointWeaponSlot[point] = slot;
		if(slot >= 0)
			usedHolsterSlots[slot] = true;
	}
	// Existing installations already persisted CHEST RIGHT as EMPTY before
	// physical melee weapons existed.  Migrate only the first genuinely empty
	// point and never displace a loadout chosen by the player.
	if(!usedHolsterSlots[WEAPONSLOT_MELEE]){
		for(int point = 0; point < HOLSTER_POINT_COUNT; point++){
			if(point == HOLSTER_CHEST_CENTER)
				continue;
			if(gHolsterPointWeaponSlot[point] != -1)
				continue;
			gHolsterPointWeaponSlot[point] = WEAPONSLOT_MELEE;
			usedHolsterSlots[WEAPONSLOT_MELEE] = true;
			char slotValue[16];
			sprintf(slotValue, "%d", WEAPONSLOT_MELEE);
			WritePrivateProfileStringA("VR", gHolsterPointSettingNames[point],
				slotValue, path);
			break;
		}
	}
	gWeaponOffsetXCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponOffsetXCm", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponOffsetYCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponOffsetYCm", 1, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponOffsetZCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponOffsetZCm", -5, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimOffsetXCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimOffsetXCm", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimOffsetYCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimOffsetYCm", 4, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimOffsetZCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimOffsetZCm", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimRotationXDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimRotationXDeg", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimRotationYDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimRotationYDeg", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimRotationZDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimRotationZDeg", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	// Versioned local-axis keys deliberately ignore the short-lived Euler
	// calibration values from earlier builds, whose Y/Z axes collapsed at the
	// legacy weapon frame's 90-degree base rotation.
	gWeaponRotationXDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponLocalRotationXDeg", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponRotationYDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponLocalRotationYDeg", 9, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponRotationZDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponLocalRotationZDeg", 7, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponOffsetXCm = Min(Max(gWeaponOffsetXCm, -100), 100);
	gWeaponOffsetYCm = Min(Max(gWeaponOffsetYCm, -100), 100);
	gWeaponOffsetZCm = Min(Max(gWeaponOffsetZCm, -100), 100);
	gWeaponAimOffsetXCm = Min(Max(gWeaponAimOffsetXCm, -100), 100);
	gWeaponAimOffsetYCm = Min(Max(gWeaponAimOffsetYCm, -100), 100);
	gWeaponAimOffsetZCm = Min(Max(gWeaponAimOffsetZCm, -100), 100);
	gWeaponAimRotationXDeg = Min(Max(gWeaponAimRotationXDeg, -360), 360);
	gWeaponAimRotationYDeg = Min(Max(gWeaponAimRotationYDeg, -360), 360);
	gWeaponAimRotationZDeg = Min(Max(gWeaponAimRotationZDeg, -360), 360);
	gWeaponRotationXDeg = Min(Max(gWeaponRotationXDeg, -360), 360);
	gWeaponRotationYDeg = Min(Max(gWeaponRotationYDeg, -360), 360);
	gWeaponRotationZDeg = Min(Max(gWeaponRotationZDeg, -360), 360);
	gDrivingYOffsetCm = GetPrivateProfileIntA("VR", "DrivingYOffsetCm", 15, path);
	gDrivingYOffsetCm = Min(Max(gDrivingYOffsetCm, -100), 150);
#ifdef RW_D3D12
	gFixedFoveatedProfile = GetPrivateProfileIntA("VR", "VRS", rw::d3d12::FIXED_FOVEATED_QUALITY, path);
	gFixedFoveatedProfile = Min(Max(gFixedFoveatedProfile, 0U),
		(uint32)rw::d3d12::FIXED_FOVEATED_PROFILE_COUNT-1);
#endif
	gVrSettingsLoaded = true;
}

void SaveVrSetting(const char *name, int value)
{
	char textValue[32];
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA("VR", name, textValue, ".\\vr_settings.ini");
}

int GetVrBikeModelIndex(int model)
{
	for(int index = 0; index < VR_BIKE_MODEL_COUNT; index++)
		if(gVrBikeModels[index] == model)
			return index;
	return -1;
}

CBike *GetActivePlayerBike()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle && vehicle->IsBike() ? (CBike*)vehicle : nil;
}

CAutomobile *GetActivePlayerCar()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle && vehicle->IsCar() && !vehicle->IsRealHeli() &&
		!vehicle->IsRealPlane() ? (CAutomobile*)vehicle : nil;
}

bool IsVrDrivingEnvironmentActive()
{
	// Loading, frontend and cinema transitions temporarily expose partially
	// rebuilt player/vehicle state.  Immersive driving must be completely inert
	// there: querying that state from the OpenXR input/render path can race the
	// scripted transition into the opening hotel drive.
	if(gDrivingType == VR_DRIVING_DEFAULT ||
	   gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bGameNotLoaded ||
	   FrontEndMenuManager.m_bWantToRestart ||
	   FrontEndMenuManager.m_bWantToLoad ||
	   CGame::playingIntro ||
	   CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn)
		return false;
	CPlayerPed *player = FindPlayerPed();
	CVehicle *vehicle = FindPlayerVehicle();
	return player && vehicle && vehicle->pDriver == player;
}

bool IsImmersiveDrivingEnvironmentActive()
{
	return gDrivingType == VR_DRIVING_IMMERSIVE &&
		IsVrDrivingEnvironmentActive();
}

bool IsMotionDrivingEnvironmentActive()
{
	return gDrivingType == VR_DRIVING_MOTION &&
		IsVrDrivingEnvironmentActive();
}

bool IsImmersiveBikeDrivingActiveInternal(CVehicle *expected = nil)
{
	if(!IsImmersiveDrivingEnvironmentActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	return bike &&
		(!expected || expected == bike) &&
		GetVrBikeModelIndex(bike->GetModelIndex()) >= 0;
}

bool IsImmersiveCarDrivingActiveInternal(CVehicle *expected = nil)
{
	if(!IsImmersiveDrivingEnvironmentActive())
		return false;
	CAutomobile *car = GetActivePlayerCar();
	return car && (!expected || expected == car);
}

bool IsImmersiveDrivingActiveInternal(CVehicle *expected = nil)
{
	return IsImmersiveBikeDrivingActiveInternal(expected) ||
		IsImmersiveCarDrivingActiveInternal(expected);
}

bool IsVrBikeDrivingActiveInternal(CVehicle *expected = nil)
{
	if(!IsVrDrivingEnvironmentActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	return bike && (!expected || expected == bike) &&
		GetVrBikeModelIndex(bike->GetModelIndex()) >= 0;
}

bool IsVrCarDrivingActiveInternal(CVehicle *expected = nil)
{
	if(!IsVrDrivingEnvironmentActive())
		return false;
	CAutomobile *car = GetActivePlayerCar();
	return car && (!expected || expected == car);
}

bool IsVrDrivingActiveInternal(CVehicle *expected = nil)
{
	return IsVrBikeDrivingActiveInternal(expected) ||
		IsVrCarDrivingActiveInternal(expected);
}

const char *GetActiveVrBikeName()
{
	CBike *bike = GetActivePlayerBike();
	const int index = bike ? GetVrBikeModelIndex(bike->GetModelIndex()) : -1;
	return index >= 0 ? gVrBikeNames[index] : "NO BIKE";
}

const char *GetActiveVrVehicleName()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle ? GetVrVehicleModelName(vehicle->GetModelIndex()) :
		"NO VEHICLE";
}

int GetDefaultBikeHandleOffsetX(int bikeIndex, int hand)
{
	static const int halfWidthCm[VR_BIKE_MODEL_COUNT] = {
		36, 36, 32, 26, 26, 32
	};
	if(bikeIndex < 0 || bikeIndex >= VR_BIKE_MODEL_COUNT ||
	   hand < 0 || hand >= EYE_COUNT)
		return 0;
	return halfWidthCm[bikeIndex]*WEAPON_CALIBRATION_VALUE_SCALE*
		(hand == 0 ? -1 : 1);
}

BikeHandleCalibration *GetBikeHandleCalibration(int model, int hand)
{
	const int bikeIndex = GetVrBikeModelIndex(model);
	if(bikeIndex < 0 || hand < 0 || hand >= EYE_COUNT)
		return nil;
	BikeHandleCalibration &calibration =
		gBikeHandleCalibration[bikeIndex][hand];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, "BikeHandle_%d_%s", model,
		hand == 0 ? "Left" : "Right");
	const char *path = ".\\vr_settings.ini";
	calibration.offsetX = (int)(int32)GetPrivateProfileIntA(section,
		"OffsetX", GetDefaultBikeHandleOffsetX(bikeIndex, hand), path);
	calibration.offsetY = (int)(int32)GetPrivateProfileIntA(section,
		"OffsetY", 0, path);
	calibration.offsetZ = (int)(int32)GetPrivateProfileIntA(section,
		"OffsetZ", 0, path);
	calibration.rotationX = (int)(int32)GetPrivateProfileIntA(section,
		"RotationX", 0, path);
	calibration.rotationY = (int)(int32)GetPrivateProfileIntA(section,
		"RotationY", 0, path);
	calibration.rotationZ = (int)(int32)GetPrivateProfileIntA(section,
		"RotationZ", 0, path);
	calibration.offsetX = Min(Max(calibration.offsetX, -300), 300);
	calibration.offsetY = Min(Max(calibration.offsetY, -300), 300);
	calibration.offsetZ = Min(Max(calibration.offsetZ, -300), 300);
	calibration.rotationX = Min(Max(calibration.rotationX, -720), 720);
	calibration.rotationY = Min(Max(calibration.rotationY, -720), 720);
	calibration.rotationZ = Min(Max(calibration.rotationZ, -720), 720);
	calibration.valid = true;
	return &calibration;
}

void SaveBikeHandleCalibrationValue(int model, int hand,
	const char *name, int value)
{
	if(GetVrBikeModelIndex(model) < 0 || hand < 0 || hand >= EYE_COUNT ||
	   !name)
		return;
	char section[64], textValue[32];
	sprintf(section, "BikeHandle_%d_%s", model,
		hand == 0 ? "Left" : "Right");
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA(section, name, textValue,
		".\\vr_settings.ini");
}

int GetVrCarModelIndex(int model)
{
	if(model < MI_FIRST_VEHICLE || model > MI_LAST_VEHICLE)
		return -1;
	CBaseModelInfo *base = CModelInfo::GetModelInfo(model);
	if(!base || ((CVehicleModelInfo*)base)->m_vehicleType != VEHICLE_TYPE_CAR)
		return -1;
	return model-MI_FIRST_VEHICLE;
}

BikeHandleCalibration *GetCarWheelCalibration(int model, int hand)
{
	const int carIndex = GetVrCarModelIndex(model);
	if(carIndex < 0 || hand < 0 || hand >= EYE_COUNT)
		return nil;
	BikeHandleCalibration &calibration =
		gCarWheelCalibration[carIndex][hand];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, "CarWheelV2_%d_%s", model,
		hand == 0 ? "Left" : "Right");
	const char *path = ".\\vr_settings.ini";
	const int defaultX = 18*WEAPON_CALIBRATION_VALUE_SCALE*
		(hand == 0 ? -1 : 1);
	calibration.offsetX = (int)(int32)GetPrivateProfileIntA(section,
		"OffsetX", defaultX, path);
	calibration.offsetY = (int)(int32)GetPrivateProfileIntA(section,
		"OffsetY", 0, path);
	calibration.offsetZ = (int)(int32)GetPrivateProfileIntA(section,
		"OffsetZ", 0, path);
	calibration.rotationX = (int)(int32)GetPrivateProfileIntA(section,
		"RotationX", 0, path);
	calibration.rotationY = (int)(int32)GetPrivateProfileIntA(section,
		"RotationY", 0, path);
	calibration.rotationZ = (int)(int32)GetPrivateProfileIntA(section,
		"RotationZ", 0, path);
	calibration.offsetX = Min(Max(calibration.offsetX, -300), 300);
	calibration.offsetY = Min(Max(calibration.offsetY, -300), 300);
	calibration.offsetZ = Min(Max(calibration.offsetZ, -300), 300);
	calibration.rotationX =
		Min(Max(calibration.rotationX, -720), 720);
	calibration.rotationY =
		Min(Max(calibration.rotationY, -720), 720);
	calibration.rotationZ =
		Min(Max(calibration.rotationZ, -720), 720);
	calibration.valid = true;
	return &calibration;
}

void SaveCarWheelCalibrationValue(int model, int hand,
	const char *name, int value)
{
	if(GetVrCarModelIndex(model) < 0 || hand < 0 || hand >= EYE_COUNT ||
	   !name)
		return;
	char section[64], textValue[32];
	sprintf(section, "CarWheelV2_%d_%s", model,
		hand == 0 ? "Left" : "Right");
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA(section, name, textValue,
		".\\vr_settings.ini");
}

BikeLeanCalibration *GetBikeLeanCalibration(int model)
{
	const int bikeIndex = GetVrBikeModelIndex(model);
	if(bikeIndex < 0)
		return nil;
	BikeLeanCalibration &calibration = gBikeLeanCalibration[bikeIndex];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, "BikeControl_%d", model);
	const char *path = ".\\vr_settings.ini";
	calibration.wheelieHeightCm = Min(Max(
		(int)(int32)GetPrivateProfileIntA(section,
			"WheelieHeightCm", 20, path), 5), 100);
	calibration.standHeightCm = Min(Max(
		(int)(int32)GetPrivateProfileIntA(section,
			"StandHeightCm", 50, path), 5), 100);
	calibration.valid = true;
	return &calibration;
}

void SaveBikeLeanCalibrationValue(int model, const char *name, int value)
{
	if(GetVrBikeModelIndex(model) < 0 || !name)
		return;
	char section[64], textValue[32];
	sprintf(section, "BikeControl_%d", model);
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA(section, name, textValue,
		".\\vr_settings.ini");
}

void ResetImmersiveBikeInteraction()
{
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gBikeHandleGrabbed[hand] = false;
		gBikeHandleGripDown[hand] = false;
		gBikeHandleDistance[hand] = 1000.0f;
		gBikeLeanReferenceValid[hand] = false;
	}
	gImmersiveBikeSteering = 0.0f;
	gImmersiveBikeThrottle = 0.0f;
	gImmersiveBikeLean = 0.0f;
	gBikeLeanGestureState = 0;
	gBikeThrottleReferenceValid = false;
}

void ResetImmersiveCarInteraction()
{
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gCarWheelGrabbed[hand] = false;
		gCarWheelGripDown[hand] = false;
		gCarWheelDistance[hand] = 1000.0f;
		gCarWheelGrabReferenceAngle[hand] = 0.0f;
		gCarWheelContinuousAngle[hand] = 0.0f;
		gCarWheelAngleValid[hand] = false;
		gCarHornContact[hand] = false;
		gCarHornArmed[hand] = false;
		gCarHornPreviousDistance[hand] = 1000.0f;
	}
	gImmersiveCarSteering = 0.0f;
	gImmersiveCarPhysicalAngle = 0.0f;
	gCarWheelTwoHandReferenceAngle = 0.0f;
	gCarWheelTwoHandContinuousAngle = 0.0f;
	gCarWheelTwoHandAngleValid = false;
	gImmersiveCarHornPressed = false;
}

void ResetMotionSteeringInteraction()
{
	gMotionSteeringVehicle = nil;
	gMotionVehicleSteering = 0.0f;
	gMotionVehiclePhysicalAngle = 0.0f;
	gMotionSteeringReferenceValid = false;
	gMotionSteeringReferenceHeading = 0.0f;
}

void ResetImmersiveDrivingInteraction()
{
	ResetImmersiveBikeInteraction();
	ResetImmersiveCarInteraction();
	ResetMotionSteeringInteraction();
	gVrRadioButtonDown = false;
	gVrRadioChangeJustPressed = false;
}

int GetVehicleCalibrationMenuItemCount()
{
	return IsImmersiveCarDrivingActiveInternal() ? 8 :
		VR_BIKE_CALIBRATION_MENU_ITEM_COUNT;
}

int GetVehicleCalibrationMenuItemForRow(int row)
{
	if(IsImmersiveCarDrivingActiveInternal() && row == 7)
		return VR_BIKE_CAL_BACK;
	return row;
}

int FindHolsterPointForSlot(int slot)
{
	for(int point = 0; point < HOLSTER_POINT_COUNT; point++)
		if(gHolsterPointWeaponSlot[point] == slot)
			return point;
	return -1;
}

const char *GetHolsterSlotCategoryName(int slot)
{
	switch(slot){
	case WEAPONSLOT_MELEE: return "MELEE";
	case WEAPONSLOT_PROJECTILE: return "THROWABLE";
	case WEAPONSLOT_HANDGUN: return "HANDGUN";
	case WEAPONSLOT_SHOTGUN: return "SHOTGUN";
	case WEAPONSLOT_SUBMACHINEGUN: return "SMG";
	case WEAPONSLOT_RIFLE: return "RIFLE";
	case WEAPONSLOT_HEAVY: return "HEAVY";
	case WEAPONSLOT_SNIPER: return "SCOPED";
	case WEAPONSLOT_OTHER: return "CAMERA";
	default: return "EMPTY";
	}
}

void FormatHolsterSlotDisplayName(int slot, char *buffer)
{
	if(!buffer)
		return;
	if(slot < 0){
		strcpy(buffer, "EMPTY");
		return;
	}
	// A holster point owns an inventory category, while the icon identifies
	// the concrete weapon currently stored in it.
	if(IsVrWeaponSlotOwned(slot))
		sprintf(buffer, "%s", GetHolsterSlotCategoryName(slot));
	else
		sprintf(buffer, "%s SLOT EMPTY", GetHolsterSlotCategoryName(slot));
}

void CycleHolsterPointSlot(int point, int direction)
{
	if(point < 0 || point >= HOLSTER_POINT_COUNT || direction == 0)
		return;
	// Grenades, tear gas and Molotovs always share Vice City's projectile slot;
	// keeping its centre-chest point fixed makes it impossible to lose behind a
	// configurable duplicate assignment.
	if(point == HOLSTER_CHEST_CENTER)
		return;
	int choice = 0;
	for(int i = 0; i < (int)ARRAY_SIZE(gHolsterSlotChoices); i++)
		if(gHolsterSlotChoices[i] == gHolsterPointWeaponSlot[point]){
			choice = i;
			break;
		}
	for(int step = 0; step < (int)ARRAY_SIZE(gHolsterSlotChoices); step++){
		choice = (choice+(direction > 0 ? 1 : -1)+
			ARRAY_SIZE(gHolsterSlotChoices)) % ARRAY_SIZE(gHolsterSlotChoices);
		const int candidate = gHolsterSlotChoices[choice];
		const int occupiedPoint = FindHolsterPointForSlot(candidate);
		if(candidate >= 0 && occupiedPoint >= 0 && occupiedPoint != point)
			continue;
		gHolsterPointWeaponSlot[point] = candidate;
		SaveVrSetting(gHolsterPointSettingNames[point], candidate);
		debug("[OpenXR] Holster %s: %s\n", gHolsterPointNames[point],
			GetHolsterSlotCategoryName(candidate));
		return;
	}
}

void BackupVrSettings(const char *reason)
{
	static uint32 backupSequence = 0;
	CreateDirectoryA(".\\vr_settings_backups", nil);
	SYSTEMTIME time;
	GetLocalTime(&time);
	char backupPath[MAX_PATH];
	sprintf(backupPath,
		".\\vr_settings_backups\\vr_settings_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%u_%s.ini",
		time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
		time.wSecond, time.wMilliseconds, (unsigned long)GetCurrentProcessId(),
		backupSequence++, reason ? reason : "snapshot");
	// CREATE_NEW semantics: an existing snapshot is never overwritten.
	CopyFileA(".\\vr_settings.ini", backupPath, TRUE);
}

bool GetWeaponCalibrationSection(int weaponType, char *section)
{
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return false;
	sprintf(section, "VRWeapon_%02d_%s", weaponType, GetVrWeaponName(weaponType));
	return true;
}

int GetCalibrationWeaponType()
{
	const int hand = gCalibrationEditHand == 0 ? 0 : 1;
	// The virtual remote uses the stock detonator model but never occupies an
	// inventory slot.  Give it its own per-hand WEAPONTYPE_DETONATOR profile so
	// adjusting the controller cannot disturb the already calibrated C4 charge.
	if(IsTrackedDetonatorHandReservedInternal(hand))
		return WEAPONTYPE_DETONATOR;
	int weaponType = gHeldWeaponSlot[hand] >= 0 ?
		GetVrWeaponTypeForSlot(gHeldWeaponSlot[hand]) : -1;
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		weaponType = GetVrCurrentWeaponType();
	return weaponType;
}

bool GetCurrentWeaponCalibrationSection(char *section)
{
	return GetWeaponCalibrationSection(GetCalibrationWeaponType(), section);
}

const char *GetWeaponCalibrationKey(int hand, const char *name, char *key)
{
	// Existing unprefixed values are the user's carefully calibrated RIGHT-hand
	// profiles.  Never rename or rewrite them during migration.  LEFT lives next
	// to them under an explicit prefix and is seeded from the normalized right
	// profile on first use.
	if(hand == 0){
		sprintf(key, "Left%s", name);
		return key;
	}
	return name;
}

void WriteWeaponCalibrationValue(const char *section, int hand,
	const char *name, int value)
{
	char textValue[32];
	char key[64];
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA(section, GetWeaponCalibrationKey(hand, name, key),
		textValue, ".\\vr_settings.ini");
}

int ReadWeaponCalibrationValue(const char *section, int hand,
	const char *name, int fallback)
{
	// GetPrivateProfileInt returns UINT even for negative text. Cast the result
	// back to signed before clamping; mixing UINT with a negative clamp bound is
	// what previously converted every loaded value to its positive maximum.
	char key[64];
	return (int)(int32)GetPrivateProfileIntA(section,
		GetWeaponCalibrationKey(hand, name, key), fallback, ".\\vr_settings.ini");
}

bool IsWeaponCalibrationConfigured(const char *section, int hand)
{
	char key[64];
	return GetPrivateProfileIntA(section,
		GetWeaponCalibrationKey(hand, "Configured", key), 0,
		".\\vr_settings.ini") != 0;
}

void SaveCompleteWeaponCalibration(const char *section, int hand)
{
	WriteWeaponCalibrationValue(section, hand, "Configured", 1);
	WriteWeaponCalibrationValue(section, hand, "ValueScale", WEAPON_CALIBRATION_VALUE_SCALE);
	WriteWeaponCalibrationValue(section, hand, "OffsetX", gWeaponOffsetXCm);
	WriteWeaponCalibrationValue(section, hand, "OffsetY", gWeaponOffsetYCm);
	WriteWeaponCalibrationValue(section, hand, "OffsetZ", gWeaponOffsetZCm);
	WriteWeaponCalibrationValue(section, hand, "AimOffsetX", gWeaponAimOffsetXCm);
	WriteWeaponCalibrationValue(section, hand, "AimOffsetY", gWeaponAimOffsetYCm);
	WriteWeaponCalibrationValue(section, hand, "AimOffsetZ", gWeaponAimOffsetZCm);
	WriteWeaponCalibrationValue(section, hand, "AimRotationX", gWeaponAimRotationXDeg);
	WriteWeaponCalibrationValue(section, hand, "AimRotationY", gWeaponAimRotationYDeg);
	WriteWeaponCalibrationValue(section, hand, "AimRotationZ", gWeaponAimRotationZDeg);
	WriteWeaponCalibrationValue(section, hand, "RotationX", gWeaponRotationXDeg);
	WriteWeaponCalibrationValue(section, hand, "RotationY", gWeaponRotationYDeg);
	WriteWeaponCalibrationValue(section, hand, "RotationZ", gWeaponRotationZDeg);
}

void SyncCurrentWeaponCalibration()
{
	const int weaponType = GetCalibrationWeaponType();
	const int hand = gCalibrationEditHand == 0 ? 0 : 1;
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS ||
	   (weaponType == gActiveWeaponCalibration &&
	    hand == gActiveWeaponCalibrationHand))
		return;
	if(gActiveWeaponCalibration >= 0){
		char reason[48];
		sprintf(reason, "weapon_%02d_%c_done", gActiveWeaponCalibration,
			gActiveWeaponCalibrationHand == 0 ? 'L' : 'R');
		BackupVrSettings(reason);
	}
	char section[96];
	if(!GetCurrentWeaponCalibrationSection(section))
		return;
	gActiveWeaponCalibration = weaponType;
	gActiveWeaponCalibrationHand = hand;
	int readHand = hand;
	bool seedLeftFromRight = false;
	if(!IsWeaponCalibrationConfigured(section, hand)){
		if(hand == 0 && IsWeaponCalibrationConfigured(section, 1)){
			// Normalize the established right-hand values before writing the left
			// seed.  Copying raw legacy values would double old ValueScale=1 files.
			readHand = 1;
			seedLeftFromRight = true;
			BackupVrSettings("left_hand_calibration_seed");
		}else{
		// A newly encountered weapon inherits the values currently visible in
		// the menu. This gives it a usable starting point and immediately creates
		// a persistent profile which subsequent edits update automatically.
			SaveCompleteWeaponCalibration(section, hand);
			gWeaponCalibration[hand][weaponType].valid = false;
			return;
		}
	}
	const int storedScale = ReadWeaponCalibrationValue(section, readHand,
		"ValueScale", 1);
	const int fallbackDivisor = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	const int rawOffsetX = ReadWeaponCalibrationValue(section, readHand, "OffsetX", gWeaponOffsetXCm/fallbackDivisor);
	const int rawOffsetY = ReadWeaponCalibrationValue(section, readHand, "OffsetY", gWeaponOffsetYCm/fallbackDivisor);
	const int rawOffsetZ = ReadWeaponCalibrationValue(section, readHand, "OffsetZ", gWeaponOffsetZCm/fallbackDivisor);
	const int rawAimOffsetX = ReadWeaponCalibrationValue(section, readHand, "AimOffsetX", 0);
	const int rawAimOffsetY = ReadWeaponCalibrationValue(section, readHand, "AimOffsetY", gWeaponAimOffsetYCm/fallbackDivisor);
	const int rawAimOffsetZ = ReadWeaponCalibrationValue(section, readHand, "AimOffsetZ", 0);
	const int rawAimRotationX = ReadWeaponCalibrationValue(section, readHand, "AimRotationX", 0);
	const int rawAimRotationY = ReadWeaponCalibrationValue(section, readHand, "AimRotationY", 0);
	const int rawAimRotationZ = ReadWeaponCalibrationValue(section, readHand, "AimRotationZ", 0);
	const int rawRotationX = ReadWeaponCalibrationValue(section, readHand, "RotationX", gWeaponRotationXDeg/fallbackDivisor);
	const int rawRotationY = ReadWeaponCalibrationValue(section, readHand, "RotationY", gWeaponRotationYDeg/fallbackDivisor);
	const int rawRotationZ = ReadWeaponCalibrationValue(section, readHand, "RotationZ", gWeaponRotationZDeg/fallbackDivisor);
	const bool knownBrokenClampProfile = rawOffsetX == 50 && rawOffsetY == 50 &&
		rawOffsetZ == 50 && rawAimOffsetY == 50 && rawRotationX == 180 &&
		rawRotationY == 180 && rawRotationZ == 180;
	if(knownBrokenClampProfile){
		// Preserve the complete damaged file before replacing only this known
		// corruption signature with the last safe in-memory calibration.
		BackupVrSettings("broken_clamp_profile");
		SaveCompleteWeaponCalibration(section, hand);
		debug("[OpenXR] Recovered corrupt weapon calibration %s from last safe values\n", section);
		return;
	}
	const int conversion = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponOffsetXCm = Min(Max(rawOffsetX*conversion, -100), 100);
	gWeaponOffsetYCm = Min(Max(rawOffsetY*conversion, -100), 100);
	gWeaponOffsetZCm = Min(Max(rawOffsetZ*conversion, -100), 100);
	gWeaponAimOffsetXCm = Min(Max(rawAimOffsetX*conversion, -100), 100);
	gWeaponAimOffsetYCm = Min(Max(rawAimOffsetY*conversion, -100), 100);
	gWeaponAimOffsetZCm = Min(Max(rawAimOffsetZ*conversion, -100), 100);
	gWeaponAimRotationXDeg = Min(Max(rawAimRotationX*conversion, -360), 360);
	gWeaponAimRotationYDeg = Min(Max(rawAimRotationY*conversion, -360), 360);
	gWeaponAimRotationZDeg = Min(Max(rawAimRotationZ*conversion, -360), 360);
	gWeaponRotationXDeg = Min(Max(rawRotationX*conversion, -360), 360);
	gWeaponRotationYDeg = Min(Max(rawRotationY*conversion, -360), 360);
	gWeaponRotationZDeg = Min(Max(rawRotationZ*conversion, -360), 360);
	gWeaponCalibration[hand][weaponType].valid = false;
	if(seedLeftFromRight || storedScale != WEAPON_CALIBRATION_VALUE_SCALE)
		SaveCompleteWeaponCalibration(section, hand);
	if(seedLeftFromRight)
		debug("[OpenXR] Seeded LEFT calibration for %s from RIGHT profile\n",
			GetVrWeaponName(weaponType));
}

void CaptureCurrentWeaponCalibration(WeaponCalibration &calibration)
{
	calibration.offsetX = gWeaponOffsetXCm;
	calibration.offsetY = gWeaponOffsetYCm;
	calibration.offsetZ = gWeaponOffsetZCm;
	calibration.aimOffsetX = gWeaponAimOffsetXCm;
	calibration.aimOffsetY = gWeaponAimOffsetYCm;
	calibration.aimOffsetZ = gWeaponAimOffsetZCm;
	calibration.aimRotationX = gWeaponAimRotationXDeg;
	calibration.aimRotationY = gWeaponAimRotationYDeg;
	calibration.aimRotationZ = gWeaponAimRotationZDeg;
	calibration.rotationX = gWeaponRotationXDeg;
	calibration.rotationY = gWeaponRotationYDeg;
	calibration.rotationZ = gWeaponRotationZDeg;
	calibration.valid = true;
}

const WeaponCalibration *GetWeaponCalibration(int hand, int weaponType)
{
	if(hand < 0 || hand >= EYE_COUNT ||
	   weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return nil;
	if(hand == gCalibrationEditHand && weaponType == GetCalibrationWeaponType()){
		SyncCurrentWeaponCalibration();
		CaptureCurrentWeaponCalibration(gWeaponCalibration[hand][weaponType]);
		return &gWeaponCalibration[hand][weaponType];
	}
	WeaponCalibration &calibration = gWeaponCalibration[hand][weaponType];
	if(calibration.valid)
		return &calibration;
	char section[96];
	if(!GetWeaponCalibrationSection(weaponType, section))
		return nil;
	int readHand = hand;
	if(!IsWeaponCalibrationConfigured(section, hand)){
		if(hand == 0 && IsWeaponCalibrationConfigured(section, 1))
			readHand = 1;
		else{
		CaptureCurrentWeaponCalibration(calibration);
		return &calibration;
		}
	}
	const int storedScale = ReadWeaponCalibrationValue(section, readHand,
		"ValueScale", 1);
	const int conversion = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	calibration.offsetX = Min(Max(ReadWeaponCalibrationValue(section, readHand, "OffsetX", 0)*conversion, -100), 100);
	calibration.offsetY = Min(Max(ReadWeaponCalibrationValue(section, readHand, "OffsetY", 1)*conversion, -100), 100);
	calibration.offsetZ = Min(Max(ReadWeaponCalibrationValue(section, readHand, "OffsetZ", -5)*conversion, -100), 100);
	calibration.aimOffsetX = Min(Max(ReadWeaponCalibrationValue(section, readHand, "AimOffsetX", 0)*conversion, -100), 100);
	calibration.aimOffsetY = Min(Max(ReadWeaponCalibrationValue(section, readHand, "AimOffsetY", 4)*conversion, -100), 100);
	calibration.aimOffsetZ = Min(Max(ReadWeaponCalibrationValue(section, readHand, "AimOffsetZ", 0)*conversion, -100), 100);
	calibration.aimRotationX = Min(Max(ReadWeaponCalibrationValue(section, readHand, "AimRotationX", 0)*conversion, -360), 360);
	calibration.aimRotationY = Min(Max(ReadWeaponCalibrationValue(section, readHand, "AimRotationY", 0)*conversion, -360), 360);
	calibration.aimRotationZ = Min(Max(ReadWeaponCalibrationValue(section, readHand, "AimRotationZ", 0)*conversion, -360), 360);
	calibration.rotationX = Min(Max(ReadWeaponCalibrationValue(section, readHand, "RotationX", 0)*conversion, -360), 360);
	calibration.rotationY = Min(Max(ReadWeaponCalibrationValue(section, readHand, "RotationY", 9)*conversion, -360), 360);
	calibration.rotationZ = Min(Max(ReadWeaponCalibrationValue(section, readHand, "RotationZ", 7)*conversion, -360), 360);
	calibration.valid = true;
	return &calibration;
}

void SaveCurrentWeaponCalibrationValue(const char *name, int value)
{
	SyncCurrentWeaponCalibration();
	const int weaponType = GetCalibrationWeaponType();
	const int hand = gCalibrationEditHand == 0 ? 0 : 1;
	if(weaponType >= 0 && weaponType < WEAPONTYPE_TOTALWEAPONS)
		gWeaponCalibration[hand][weaponType].valid = false;
	char section[96];
	if(!GetCurrentWeaponCalibrationSection(section))
		return;
	WriteWeaponCalibrationValue(section, hand, "Configured", 1);
	WriteWeaponCalibrationValue(section, hand, "ValueScale", WEAPON_CALIBRATION_VALUE_SCALE);
	WriteWeaponCalibrationValue(section, hand, name, value);
}

bool IsSupportGripCalibrationConfigured(const char *section, int hand)
{
	char key[64];
	return GetPrivateProfileIntA(section,
		GetWeaponCalibrationKey(hand, "SupportGripConfigured", key), 0,
		".\\vr_settings.ini") != 0;
}

void CaptureCurrentSupportGripCalibration(SupportGripCalibration &calibration)
{
	calibration.offsetX = gSupportGripOffsetXCm;
	calibration.offsetY = gSupportGripOffsetYCm;
	calibration.offsetZ = gSupportGripOffsetZCm;
	calibration.valid = true;
}

void SaveCompleteSupportGripCalibration(const char *section, int hand)
{
	WriteWeaponCalibrationValue(section, hand, "SupportGripConfigured", 1);
	WriteWeaponCalibrationValue(section, hand, "SupportGripValueScale",
		WEAPON_CALIBRATION_VALUE_SCALE);
	WriteWeaponCalibrationValue(section, hand, "SupportGripOffsetX",
		gSupportGripOffsetXCm);
	WriteWeaponCalibrationValue(section, hand, "SupportGripOffsetY",
		gSupportGripOffsetYCm);
	WriteWeaponCalibrationValue(section, hand, "SupportGripOffsetZ",
		gSupportGripOffsetZCm);
}

void SyncCurrentSupportGripCalibration()
{
	const int weaponType = GetCalibrationWeaponType();
	const int hand = gCalibrationEditHand == 0 ? 0 : 1;
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS ||
	   (weaponType == gActiveSupportGripCalibration &&
	    hand == gActiveSupportGripCalibrationHand))
		return;
	char section[96];
	if(!GetCurrentWeaponCalibrationSection(section))
		return;
	gActiveSupportGripCalibration = weaponType;
	gActiveSupportGripCalibrationHand = hand;
	int readHand = hand;
	bool seedLeftFromRight = false;
	if(!IsSupportGripCalibrationConfigured(section, hand)){
		if(hand == 0 && IsSupportGripCalibrationConfigured(section, 1)){
			readHand = 1;
			seedLeftFromRight = true;
			BackupVrSettings("left_support_grip_seed");
		}else{
			// Support profiles are intentionally independent of the painstaking
			// weapon/laser calibration.  A new weapon always starts from a safe
			// foregrip 30 cm ahead and 5 cm below the primary controller.
			gSupportGripOffsetXCm = 0;
			gSupportGripOffsetYCm = 60;
			gSupportGripOffsetZCm = -10;
			BackupVrSettings("new_support_grip_profile");
			SaveCompleteSupportGripCalibration(section, hand);
			CaptureCurrentSupportGripCalibration(
				gSupportGripCalibration[hand][weaponType]);
			return;
		}
	}
	const int storedScale = ReadWeaponCalibrationValue(section, readHand,
		"SupportGripValueScale", 1);
	const int conversion = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	gSupportGripOffsetXCm = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"SupportGripOffsetX", 0)*conversion, -200), 200);
	gSupportGripOffsetYCm = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"SupportGripOffsetY", 30)*conversion, -200), 200);
	gSupportGripOffsetZCm = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"SupportGripOffsetZ", -5)*conversion, -200), 200);
	gSupportGripCalibration[hand][weaponType].valid = false;
	if(seedLeftFromRight || storedScale != WEAPON_CALIBRATION_VALUE_SCALE)
		SaveCompleteSupportGripCalibration(section, hand);
}

const SupportGripCalibration *GetSupportGripCalibration(int hand, int weaponType)
{
	if(hand < 0 || hand >= EYE_COUNT || weaponType < 0 ||
	   weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return nil;
	if(hand == gCalibrationEditHand && weaponType == GetCalibrationWeaponType()){
		SyncCurrentSupportGripCalibration();
		CaptureCurrentSupportGripCalibration(
			gSupportGripCalibration[hand][weaponType]);
		return &gSupportGripCalibration[hand][weaponType];
	}
	SupportGripCalibration &calibration =
		gSupportGripCalibration[hand][weaponType];
	if(calibration.valid)
		return &calibration;
	char section[96];
	if(!GetWeaponCalibrationSection(weaponType, section))
		return nil;
	int readHand = hand;
	if(!IsSupportGripCalibrationConfigured(section, hand)){
		if(hand == 0 && IsSupportGripCalibrationConfigured(section, 1))
			readHand = 1;
		else{
			calibration = SupportGripCalibration();
			calibration.valid = true;
			return &calibration;
		}
	}
	const int storedScale = ReadWeaponCalibrationValue(section, readHand,
		"SupportGripValueScale", 1);
	const int conversion = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	calibration.offsetX = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"SupportGripOffsetX", 0)*conversion, -200), 200);
	calibration.offsetY = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"SupportGripOffsetY", 30)*conversion, -200), 200);
	calibration.offsetZ = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"SupportGripOffsetZ", -5)*conversion, -200), 200);
	calibration.valid = true;
	return &calibration;
}

void SaveCurrentSupportGripCalibrationValue(const char *name, int value)
{
	SyncCurrentSupportGripCalibration();
	const int weaponType = GetCalibrationWeaponType();
	const int hand = gCalibrationEditHand == 0 ? 0 : 1;
	if(weaponType >= 0 && weaponType < WEAPONTYPE_TOTALWEAPONS)
		gSupportGripCalibration[hand][weaponType].valid = false;
	char section[96];
	if(!GetCurrentWeaponCalibrationSection(section))
		return;
	WriteWeaponCalibrationValue(section, hand, "SupportGripConfigured", 1);
	WriteWeaponCalibrationValue(section, hand, "SupportGripValueScale",
		WEAPON_CALIBRATION_VALUE_SCALE);
	WriteWeaponCalibrationValue(section, hand, name, value);
}

const char *FoveatedProfileName()
{
#ifdef RW_D3D12
	switch(gFixedFoveatedProfile){
	case rw::d3d12::FIXED_FOVEATED_OFF: return "OFF";
	case rw::d3d12::FIXED_FOVEATED_QUALITY: return "QUALITY";
	case rw::d3d12::FIXED_FOVEATED_BALANCED: return "BALANCED";
	case rw::d3d12::FIXED_FOVEATED_PERFORMANCE: return "PERFORMANCE";
	default: return "NA";
	}
#else
	return "NA";
#endif
}

const char *TemporalJitterModeName()
{
	switch(gTemporalJitterMode){
	case 0: return "OFF";
	case 2: return "X2";
	case 3: return "BOTH2";
	case 4: return "HALF";
	case 5: return "FLIPPED";
	default: return "NORMAL";
	}
}

const char *DrivingTypeName()
{
	switch(gDrivingType){
	case VR_DRIVING_IMMERSIVE: return "IMMERSIVE";
	case VR_DRIVING_MOTION: return "MOTION";
	default: return "DEFAULT";
	}
}

const char *MotionSteeringHandName()
{
	return gMotionSteeringHand == 0 ? "LEFT" : "RIGHT";
}

int MaxSupportedRenderScaleIndex()
{
	if(gEye[0].swapchain.width <= 0 || gEye[0].swapchain.height <= 0)
		return (int)ARRAY_SIZE(gRenderScaleOptions)-1;
#ifdef RW_D3D12
	// FULL SPS uses one double-wide texture. D3D12 limits either texture axis
	// to 16384 texels, so the admissible scale also depends on the OpenXR
	// resolution selected in Meta Link.
	const int maxTextureDimension = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	int maximum = 0;
	for(int index = 0; index < (int)ARRAY_SIZE(gRenderScaleOptions); index++){
		const int width = (int)(gEye[0].swapchain.width*gRenderScaleOptions[index]+0.5f)*EYE_COUNT;
		const int height = (int)(gEye[0].swapchain.height*gRenderScaleOptions[index]+0.5f);
		if(width > maxTextureDimension || height > maxTextureDimension)
			break;
		maximum = index;
	}
	return maximum;
#else
	return (int)ARRAY_SIZE(gRenderScaleOptions)-1;
#endif
}

RwRaster *gOriginalColor;
RwRaster *gOriginalDepth;
RwV2d gOriginalViewWindow;
RwV2d gOriginalViewOffset;
RwMatrix gOriginalFrameMatrix;
CMatrix gBaseCamera;
int gOriginalScreenWidth;
int gOriginalScreenHeight;
float gOriginalNearPlane;
float gOriginalDrawNear;

enum { VR_PERF_MAX_SAMPLES = 54000 };
struct PerfFrameSample
{
	double elapsedSeconds;
	float frameMs;
	float phaseMs[PERF_PHASE_COUNT];
	int visibleBuildings;
	int visibleObjects;
	int visiblePeds;
	int visibleVehicles;
	int entityRenderCalls;
	int requestedModels;
	uint64 streamingMemory;
	float slowStreamItemMs;
	int slowStreamItemId;
	int slowStreamItemType;
	float textureDefaultResourceMs;
	float textureDescriptorMs;
	float textureFootprintMs;
	float textureUploadResourceMs;
	float textureCpuCopyMs;
	float textureQueueMs;
	uint64 textureUploadBytes;
	int textureUploadCount;
	float xrWaitFrameMs;
	float xrBeginFrameMs;
	float xrAcquireMs;
	float xrSwapchainWaitMs;
	float xrReleaseMs;
	float xrEndFrameMs;
	float xrLocateViewsMs;
	float d3d12ExternalSubmitMs;
	float d3d12FrameFenceWaitMs;
	float d3d12FullGpuWaitMs;
	float geometryInstanceMs;
	float geometryBufferUploadMs;
	uint64 geometryBufferBytes;
	uint64 worldSubmittedIndices;
	uint32 geometryInstances;
	uint32 worldDrawCalls;
	float stereoBundleBuildMs;
	float stereoBundleWaitMs;
	uint32 stereoBundleDrawCalls;
	uint32 stereoBundleFallbacks;
	uint32 stereoSinglePassBegins;
	uint32 stereoSinglePassDrawCalls;
	uint64 stereoSinglePassIndices;
	uint32 stereoSinglePassFallbacks;
	uint32 fixedFoveatedBegins;
	uint32 fixedFoveatedFailures;
	uint32 fixedFoveatedProfile;
	uint32 fixedFoveatedTileSize;
	float playerX, playerY, playerZ;
};
PerfFrameSample gPerfSamples[VR_PERF_MAX_SAMPLES];
PerfFrameSample gPerfCurrent;
double gPerfFrameStartMs;
double gPerfRecordingStartMs;
double gPerfPhaseStartMs[PERF_PHASE_COUNT];
int gPerfRecordedSamples;
bool gPerfRecording;
bool gPerfFrameStarted;
double gPerfStreamItemStartMs;
int gPerfStreamItemId;
int gPerfStreamItemType;
FILE *gPerfLiveCsv;

bool XrOk(XrResult result, const char *operation)
{
	if(XR_SUCCEEDED(result))
		return true;
	char message[XR_MAX_RESULT_STRING_SIZE] = {};
	if(gInstance)
		xrResultToString(gInstance, result, message);
	debug("[OpenXR] %s failed (%d): %s\n", operation, result, message);
	VrLog("FAIL %s result=%d message=%s\n", operation, result, message);
	return false;
}

#ifndef RW_D3D12
GLuint CompileFxaaShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nil);
	glCompileShader(shader);
	GLint compiled = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if(compiled == GL_TRUE)
		return shader;
	char log[1024] = {};
	glGetShaderInfoLog(shader, sizeof(log), nil, log);
	debug("[OpenXR] FXAA shader compilation failed: %s\n", log);
	glDeleteShader(shader);
	return 0;
}

bool CreateFxaaProgram()
{
	static const char *vertexSource =
		"#version 330 core\n"
		"out vec2 uv;\n"
		"void main(){\n"
		" vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));\n"
		" uv=p; gl_Position=vec4(p*2.0-1.0,0.0,1.0);\n"
		"}\n";
	static const char *fragmentSource =
		"#version 330 core\n"
		"uniform sampler2D sourceTexture; uniform vec2 inverseScreenSize;\n"
		"uniform vec2 sourceUvScale; uniform vec2 sourceUvOffset;\n"
		"uniform int fxaaEnabled; uniform int colorMode; uniform vec4 blurColor;\n"
		"uniform vec3 contrastMult; uniform vec3 contrastAdd; in vec2 uv; out vec4 outColor;\n"
		"float luma(vec3 c){return dot(c,vec3(0.299,0.587,0.114));}\n"
		"vec3 s(vec2 p){vec2 sourceUv=p*sourceUvScale+sourceUvOffset;\n"
		" return texture(sourceTexture,clamp(sourceUv,vec2(0.0),vec2(1.0))).rgb;}\n"
		"void main(){\n"
		" vec3 m=s(uv),nw=s(uv+vec2(-1,-1)*inverseScreenSize),ne=s(uv+vec2(1,-1)*inverseScreenSize);\n"
		" vec3 sw=s(uv+vec2(-1,1)*inverseScreenSize),se=s(uv+vec2(1,1)*inverseScreenSize);\n"
		" float lm=luma(m),lnw=luma(nw),lne=luma(ne),lsw=luma(sw),lse=luma(se);\n"
		" float lmin=min(lm,min(min(lnw,lne),min(lsw,lse))),lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));\n"
		" vec2 d=vec2(-((lnw+lne)-(lsw+lse)),((lnw+lsw)-(lne+lse)));\n"
		" float reduce=max((lnw+lne+lsw+lse)*0.03125,0.0078125);\n"
		" d=clamp(d/(min(abs(d.x),abs(d.y))+reduce),vec2(-8),vec2(8))*inverseScreenSize;\n"
		" vec3 a=.5*(s(uv+d*(1.0/3.0-.5))+s(uv+d*(2.0/3.0-.5)));\n"
		" vec3 b=a*.5+.25*(s(uv+d*-.5)+s(uv+d*.5)); float lb=luma(b);\n"
		" vec3 color=fxaaEnabled!=0?((lb<lmin||lb>lmax)?a:b):m;\n"
		" if(colorMode==1){float alpha=blurColor.a;vec3 doubled=clamp(blurColor.rgb*2.0,0.0,1.0);\n"
		"  vec3 original=color,previous=color;for(int i=0;i<5;i++){vec3 f=original*(1.0-alpha)+previous*doubled*alpha;\n"
		"  f+=previous*blurColor.rgb*2.0;previous=clamp(f,0.0,1.0);}color=previous;}\n"
		" else if(colorMode==2)color=clamp(color*contrastMult+contrastAdd,0.0,1.0);\n"
		" outColor=vec4(color,1.0);}\n";
	GLuint vertex = CompileFxaaShader(GL_VERTEX_SHADER, vertexSource);
	GLuint fragment = CompileFxaaShader(GL_FRAGMENT_SHADER, fragmentSource);
	if(!vertex || !fragment){ if(vertex) glDeleteShader(vertex); if(fragment) glDeleteShader(fragment); return false; }
	gFxaaProgram = glCreateProgram();
	glAttachShader(gFxaaProgram, vertex); glAttachShader(gFxaaProgram, fragment); glLinkProgram(gFxaaProgram);
	glDeleteShader(vertex); glDeleteShader(fragment);
	GLint linked = GL_FALSE; glGetProgramiv(gFxaaProgram, GL_LINK_STATUS, &linked);
	if(linked != GL_TRUE){
		char log[1024] = {}; glGetProgramInfoLog(gFxaaProgram, sizeof(log), nil, log);
		debug("[OpenXR] FXAA program link failed: %s\n", log); glDeleteProgram(gFxaaProgram); gFxaaProgram=0; return false;
	}
	gFxaaTextureUniform=glGetUniformLocation(gFxaaProgram,"sourceTexture");
	gFxaaInverseSizeUniform=glGetUniformLocation(gFxaaProgram,"inverseScreenSize");
	gFxaaUvScaleUniform=glGetUniformLocation(gFxaaProgram,"sourceUvScale");
	gFxaaUvOffsetUniform=glGetUniformLocation(gFxaaProgram,"sourceUvOffset");
	gFxaaEnabledUniform=glGetUniformLocation(gFxaaProgram,"fxaaEnabled");
	gColorModeUniform=glGetUniformLocation(gFxaaProgram,"colorMode");
	gBlurColorUniform=glGetUniformLocation(gFxaaProgram,"blurColor");
	gContrastMultUniform=glGetUniformLocation(gFxaaProgram,"contrastMult");
	gContrastAddUniform=glGetUniformLocation(gFxaaProgram,"contrastAdd");
	glGenVertexArrays(1,&gFxaaVertexArray);
	return true;
}

void DestroyFxaaProgram()
{
	if(gFxaaVertexArray){ glDeleteVertexArrays(1,&gFxaaVertexArray); gFxaaVertexArray=0; }
	if(gFxaaProgram){ glDeleteProgram(gFxaaProgram); gFxaaProgram=0; }
}
#endif

double PerfNowMs()
{
	static LARGE_INTEGER frequency = {};
	if(!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart * 1000.0 / frequency.QuadPart;
}

void WritePerfCsvHeader(FILE *csv)
{
	fprintf(csv, "frame,elapsed_s,frame_ms,game_ms,stream_ms,slow_stream_item_ms,slow_stream_item_id,slow_stream_item_type,tex_default_resource_ms,tex_descriptor_ms,tex_footprint_ms,tex_upload_resource_ms,tex_cpu_copy_ms,tex_queue_ms,tex_upload_mb,tex_upload_count,xr_wait_frame_ms,xr_begin_frame_ms,xr_acquire_ms,xr_swapchain_wait_ms,xr_release_ms,xr_end_frame_ms,xr_locate_views_ms,d3d12_external_submit_ms,d3d12_frame_fence_wait_ms,d3d12_full_gpu_wait_ms,world_list_ms,pre_render_ms,desktop_render_ms,cinema_submit_ms,left_eye_ms,right_eye_ms,submit_ms,visible_buildings,visible_objects,visible_peds,visible_vehicles,entity_render_calls,requested_models,stream_memory_mb,player_x,player_y,player_z,audio_ms,scene_setup_ms,ui_ms,desktop_present_ms,geometry_instance_ms,geometry_buffer_upload_ms,geometry_buffer_mb,geometry_instances,world_draw_calls,world_indices,stereo_bundle_build_ms,stereo_bundle_wait_ms,stereo_bundle_draw_calls,stereo_bundle_fallbacks,stereo_single_pass_begins,stereo_single_pass_draw_calls,stereo_single_pass_indices,stereo_single_pass_fallbacks,vrs_begins,vrs_failures,vrs_profile,vrs_tile_size\n");
}

void WritePerfCsvSample(FILE *csv, int index, const PerfFrameSample &s)
{
	fprintf(csv, "%d,%.6f,%.4f,%.4f,%.4f,%.4f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%u,%llu,%.4f,%.4f,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%u\n",
		index, s.elapsedSeconds, s.frameMs, s.phaseMs[PERF_PHASE_GAME],
		s.phaseMs[PERF_PHASE_STREAMING], s.slowStreamItemMs,
		s.slowStreamItemId, s.slowStreamItemType,
		s.textureDefaultResourceMs, s.textureDescriptorMs,
		s.textureFootprintMs, s.textureUploadResourceMs,
		s.textureCpuCopyMs, s.textureQueueMs,
		s.textureUploadBytes/(1024.0*1024.0), s.textureUploadCount,
		s.xrWaitFrameMs, s.xrBeginFrameMs, s.xrAcquireMs,
		s.xrSwapchainWaitMs, s.xrReleaseMs, s.xrEndFrameMs,
		s.xrLocateViewsMs, s.d3d12ExternalSubmitMs,
		s.d3d12FrameFenceWaitMs, s.d3d12FullGpuWaitMs,
		s.phaseMs[PERF_PHASE_WORLD_LIST],
		s.phaseMs[PERF_PHASE_PRE_RENDER], s.phaseMs[PERF_PHASE_DESKTOP_RENDER],
		s.phaseMs[PERF_PHASE_CINEMA_SUBMIT], s.phaseMs[PERF_PHASE_LEFT_EYE],
		s.phaseMs[PERF_PHASE_RIGHT_EYE], s.phaseMs[PERF_PHASE_SUBMIT],
		s.visibleBuildings, s.visibleObjects, s.visiblePeds, s.visibleVehicles,
		s.entityRenderCalls, s.requestedModels, s.streamingMemory/(1024.0*1024.0),
		s.playerX, s.playerY, s.playerZ,
		s.phaseMs[PERF_PHASE_AUDIO], s.phaseMs[PERF_PHASE_SCENE_SETUP],
		s.phaseMs[PERF_PHASE_UI], s.phaseMs[PERF_PHASE_DESKTOP_PRESENT],
		s.geometryInstanceMs, s.geometryBufferUploadMs,
		s.geometryBufferBytes/(1024.0*1024.0), s.geometryInstances,
		s.worldDrawCalls, (unsigned long long)s.worldSubmittedIndices,
		s.stereoBundleBuildMs, s.stereoBundleWaitMs,
		s.stereoBundleDrawCalls, s.stereoBundleFallbacks,
		s.stereoSinglePassBegins, s.stereoSinglePassDrawCalls,
		(unsigned long long)s.stereoSinglePassIndices,
		s.stereoSinglePassFallbacks, s.fixedFoveatedBegins,
		s.fixedFoveatedFailures, s.fixedFoveatedProfile,
		s.fixedFoveatedTileSize);
}

void DumpPerfRecording()
{
	if(gPerfRecordedSamples <= 0){
		debug("[VR PERF] No complete OpenXR frames recorded\n");
		return;
	}
	time_t wallTime = time(nil);
	struct tm localTime = {};
	localtime_s(&localTime, &wallTime);
	char stamp[32], csvName[96], reportName[96];
	strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &localTime);
	sprintf(csvName, "vr_perf_openxr_%s.csv", stamp);
	sprintf(reportName, "vr_perf_openxr_%s.txt", stamp);
	FILE *csv = fopen(csvName, "w");
	if(csv){
		WritePerfCsvHeader(csv);
		for(int i = 0; i < gPerfRecordedSamples; i++)
			WritePerfCsvSample(csv, i, gPerfSamples[i]);
		fclose(csv);
	}
	double total = 0.0;
	float worst = 0.0f;
	for(int i = 0; i < gPerfRecordedSamples; i++){
		total += gPerfSamples[i].frameMs;
		worst = Max(worst, gPerfSamples[i].frameMs);
	}
	FILE *report = fopen(reportName, "w");
	if(report){
		const float average = (float)(total/gPerfRecordedSamples);
		fprintf(report, "Vice City VR OpenXR performance capture\n");
		fprintf(report, "Frames: %d  Duration: %.2f s\n", gPerfRecordedSamples,
			gPerfSamples[gPerfRecordedSamples-1].elapsedSeconds);
		fprintf(report, "Average: %.3f ms (%.1f FPS)  Worst: %.3f ms\n", average,
			average > 0.0f ? 1000.0f/average : 0.0f, worst);
		fclose(report);
	}
	debug("[VR PERF] Saved %d OpenXR frames to %s and %s\n", gPerfRecordedSamples, csvName, reportName);
}

void TogglePerfRecording()
{
	if(gPerfRecording){
		gPerfRecording = false;
		gPerfFrameStarted = false;
		if(gPerfLiveCsv){ fclose(gPerfLiveCsv); gPerfLiveCsv = nil; }
		DumpPerfRecording();
	}else{
		gPerfRecordedSamples = 0;
		gPerfFrameStarted = false;
		gPerfRecordingStartMs = PerfNowMs();
		gPerfLiveCsv = fopen("vr_perf_openxr_live.csv", "w");
		if(gPerfLiveCsv){ WritePerfCsvHeader(gPerfLiveCsv); fflush(gPerfLiveCsv); }
		gPerfRecording = true;
		debug("[VR PERF] OpenXR recording started; both grips + Y saves it\n");
	}
}

XrPath Path(const char *text)
{
	XrPath result = XR_NULL_PATH;
	XrOk(xrStringToPath(gInstance, text, &result), text);
	return result;
}

bool CreateAction(XrAction &action, XrActionType type, const char *name, const char *localized,
	bool perHand = true)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE-1);
	strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
	if(perHand){
		info.countSubactionPaths = EYE_COUNT;
		info.subactionPaths = gActions.hands;
	}
	return XrOk(xrCreateAction(gActions.set, &info, &action), name);
}

bool CreateActions()
{
	gActions.hands[0] = Path("/user/hand/left");
	gActions.hands[1] = Path("/user/hand/right");
	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(setInfo.actionSetName, "vice_city");
	strcpy(setInfo.localizedActionSetName, "Vice City VR");
	if(!XrOk(xrCreateActionSet(gInstance, &setInfo, &gActions.set), "xrCreateActionSet"))
		return false;
	if(!CreateAction(gActions.stick, XR_ACTION_TYPE_VECTOR2F_INPUT, "move_look", "Move and look") ||
	   !CreateAction(gActions.squeeze, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip") ||
	   !CreateAction(gActions.trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger") ||
	   !CreateAction(gActions.gripPose, XR_ACTION_TYPE_POSE_INPUT, "hand_grip_pose", "Hand grip pose") ||
	   !CreateAction(gActions.aimPose, XR_ACTION_TYPE_POSE_INPUT, "hand_aim_pose", "Hand aim pose") ||
	   !CreateAction(gActions.a, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A", false) ||
	   !CreateAction(gActions.b, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B", false) ||
	   !CreateAction(gActions.x, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X", false) ||
	   !CreateAction(gActions.y, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y", false) ||
	   !CreateAction(gActions.stickClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Stick click") ||
	   !CreateAction(gActions.menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", false))
		return false;

	struct BindingDef { XrAction action; const char *path; } defs[] = {
		{ gActions.stick, "/user/hand/left/input/thumbstick" },
		{ gActions.stick, "/user/hand/right/input/thumbstick" },
		{ gActions.squeeze, "/user/hand/left/input/squeeze/value" },
		{ gActions.squeeze, "/user/hand/right/input/squeeze/value" },
		{ gActions.trigger, "/user/hand/left/input/trigger/value" },
		{ gActions.trigger, "/user/hand/right/input/trigger/value" },
		{ gActions.gripPose, "/user/hand/left/input/grip/pose" },
		{ gActions.gripPose, "/user/hand/right/input/grip/pose" },
		{ gActions.aimPose, "/user/hand/left/input/aim/pose" },
		{ gActions.aimPose, "/user/hand/right/input/aim/pose" },
		{ gActions.a, "/user/hand/right/input/a/click" },
		{ gActions.b, "/user/hand/right/input/b/click" },
		{ gActions.x, "/user/hand/left/input/x/click" },
		{ gActions.y, "/user/hand/left/input/y/click" },
		{ gActions.stickClick, "/user/hand/left/input/thumbstick/click" },
		{ gActions.stickClick, "/user/hand/right/input/thumbstick/click" },
		{ gActions.menu, "/user/hand/left/input/menu/click" }
	};
	std::vector<XrActionSuggestedBinding> bindings;
	for(uint32 i = 0; i < ARRAY_SIZE(defs); i++){
		XrActionSuggestedBinding binding = { defs[i].action, Path(defs[i].path) };
		bindings.push_back(binding);
	}
	XrInteractionProfileSuggestedBinding suggestion = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggestion.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
	suggestion.countSuggestedBindings = (uint32_t)bindings.size();
	suggestion.suggestedBindings = bindings.data();
	if(!XrOk(xrSuggestInteractionProfileBindings(gInstance, &suggestion),
		"xrSuggestInteractionProfileBindings(Touch)"))
		return false;
	return true;
}

void DestroySwapchain(Swapchain &swapchain)
{
	if(&swapchain == &gCinemaSwapchain)
		gCinemaFrameValid = false;
	if(swapchain.handle)
		xrDestroySwapchain(swapchain.handle);
	swapchain.handle = XR_NULL_HANDLE;
	swapchain.images.clear();
	swapchain.width = swapchain.height = 0;
	swapchain.acquired = false;
}

bool CreateSwapchain(Swapchain &swapchain, int width, int height)
{
	VrLog("CreateSwapchain begin %dx%d format=%lld\n", width, height, (long long)gColorFormat);
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
		XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
#ifdef RW_D3D12
	info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
#endif
	info.format = gColorFormat;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if(!XrOk(xrCreateSwapchain(gSession, &info, &swapchain.handle), "xrCreateSwapchain"))
		return false;
	uint32_t count = 0;
	if(!XrOk(xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nil),
		"xrEnumerateSwapchainImages(count)"))
		return false;
#ifdef RW_D3D12
	swapchain.images.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
#else
	swapchain.images.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
#endif
	if(!XrOk(xrEnumerateSwapchainImages(swapchain.handle, count, &count,
		(XrSwapchainImageBaseHeader*)swapchain.images.data()), "xrEnumerateSwapchainImages"))
		return false;
	swapchain.width = width;
	swapchain.height = height;
	VrLog("CreateSwapchain ok %dx%d images=%u\n", width, height, count);
	return true;
}

bool AcquireSwapchain(Swapchain &swapchain)
{
	if(swapchain.acquired)
		return true;
	XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	double timingStart = PerfNowMs();
	XrResult result = xrAcquireSwapchainImage(
		swapchain.handle, &acquire, &swapchain.acquiredIndex);
	if(gPerfRecording && gPerfFrameStarted)
		gPerfCurrent.xrAcquireMs += (float)(PerfNowMs() - timingStart);
	if(!XrOk(result, "xrAcquireSwapchainImage"))
		return false;
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = XR_INFINITE_DURATION;
	timingStart = PerfNowMs();
	result = xrWaitSwapchainImage(swapchain.handle, &wait);
	if(gPerfRecording && gPerfFrameStarted)
		gPerfCurrent.xrSwapchainWaitMs += (float)(PerfNowMs() - timingStart);
	if(!XrOk(result, "xrWaitSwapchainImage"))
		return false;
	swapchain.acquired = true;
	return true;
}

void ReleaseSwapchain(Swapchain &swapchain)
{
	if(!swapchain.acquired)
		return;
	XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	const double timingStart = PerfNowMs();
	XrResult result = xrReleaseSwapchainImage(swapchain.handle, &release);
	if(gPerfRecording && gPerfFrameStarted)
		gPerfCurrent.xrReleaseMs += (float)(PerfNowMs() - timingStart);
	XrOk(result, "xrReleaseSwapchainImage");
	swapchain.acquired = false;
}

const uint8 *FindDebugGlyph(char character)
{
	if(character >= 'a' && character <= 'z')
		character = character-'a'+'A';
	for(uint32 i=0;i<ARRAY_SIZE(gDebugGlyphs);i++)
		if(gDebugGlyphs[i].character==character)
			return gDebugGlyphs[i].rows;
	return gDebugGlyphs[0].rows;
}

void PutDebugPixel(int x,int y,uint8 red,uint8 green,uint8 blue,uint8 alpha)
{
	if(x<0 || y<0 || x>=VR_DEBUG_WIDTH || y>=VR_DEBUG_HEIGHT) return;
#ifdef RW_D3D12
	const int offset=(y*VR_DEBUG_WIDTH+x)*4;
#else
	const int offset=((VR_DEBUG_HEIGHT-1-y)*VR_DEBUG_WIDTH+x)*4;
#endif
	gDebugPixels[offset+0]=red; gDebugPixels[offset+1]=green;
	gDebugPixels[offset+2]=blue; gDebugPixels[offset+3]=alpha;
}

void DrawDebugText(const char *value,int centreX,int y,int scale,uint8 red,uint8 green,uint8 blue)
{
	const int advance=scale*6;
	int x=centreX-(int)strlen(value)*advance/2;
	for(const char *ch=value;*ch;ch++,x+=advance){
		const uint8 *rows=FindDebugGlyph(*ch);
		for(int row=0;row<7;row++) for(int column=0;column<5;column++)
			if(rows[row]&(1<<(4-column)))
				for(int py=0;py<scale;py++) for(int px=0;px<scale;px++)
					PutDebugPixel(x+column*scale+px,y+row*scale+py,red,green,blue,255);
	}
}

void PutVrMenuPixel(int x, int y, uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	if(x < 0 || y < 0 || x >= VR_MENU_WIDTH || y >= VR_MENU_HEIGHT)
		return;
#ifdef RW_D3D12
	const int offset = (y*VR_MENU_WIDTH+x)*4;
#else
	const int offset = ((VR_MENU_HEIGHT-1-y)*VR_MENU_WIDTH+x)*4;
#endif
	gVrMenuPixels[offset+0] = red;
	gVrMenuPixels[offset+1] = green;
	gVrMenuPixels[offset+2] = blue;
	gVrMenuPixels[offset+3] = alpha;
}

void FillVrMenuRect(int left, int top, int right, int bottom,
	uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	left = Max(left, 0); top = Max(top, 0);
	right = Min(right, VR_MENU_WIDTH); bottom = Min(bottom, VR_MENU_HEIGHT);
	for(int y = top; y < bottom; y++)
		for(int x = left; x < right; x++)
			PutVrMenuPixel(x, y, red, green, blue, alpha);
}

void DrawVrMenuText(const char *value, int centreX, int y, int scale,
	uint8 red, uint8 green, uint8 blue)
{
	const int advance = scale*6;
	int x = centreX-(int)strlen(value)*advance/2;
	for(const char *ch = value; *ch; ch++, x += advance){
		const uint8 *rows = FindDebugGlyph(*ch);
		for(int row = 0; row < 7; row++) for(int column = 0; column < 5; column++)
			if(rows[row] & (1 << (4-column)))
				for(int py = 0; py < scale; py++) for(int px = 0; px < scale; px++)
					PutVrMenuPixel(x+column*scale+px, y+row*scale+py,
						red, green, blue, 255);
	}
}

RwImage *CreateVrWeaponIconImage(RwRaster *raster)
{
	if(raster == nil || raster->width <= 0 || raster->height <= 0)
		return nil;
#ifdef RW_D3D12
	// Weapon TXDs may retain their original BC compression.  The generic D3D12
	// Raster::toImage path expects BGRA8888, so decode only these tiny menu icons
	// locally rather than changing texture handling for the rest of the renderer.
	ID3D12Resource *resource = nil;
	if(rw::d3d12::getRasterResource(raster, &resource) && resource){
		const DXGI_FORMAT format = resource->GetDesc().Format;
		int dxt = 0;
		switch(format){
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB: dxt = 1; break;
		case DXGI_FORMAT_BC2_TYPELESS:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB: dxt = 3; break;
		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB: dxt = 5; break;
		default: break;
		}
		if(dxt != 0){
			RwImage *image = RwImageCreate(raster->width, raster->height, 32);
			if(image == nil || RwImageAllocatePixels(image) == nil){
				if(image) RwImageDestroy(image);
				return nil;
			}
			uint8 *blocks = raster->lock(0, rw::Raster::LOCKREAD);
			if(blocks == nil){
				RwImageDestroy(image);
				return nil;
			}
			image->setPixelsDXT(dxt, blocks);
			raster->unlock(0);
			return image;
		}
	}
#endif
	// All uncompressed textures produced by the modern backend are 32-bit.  The
	// stride guard keeps an unknown compressed backend format away from toImage.
	if(raster->depth != 32 || raster->stride < raster->width*4)
		return nil;
	return raster->toImage();
}

bool CacheVrWeaponIcon(int weaponType)
{
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return false;
	VrWeaponIconCache &cache = gVrWeaponIconCache[weaponType];
	if(!cache.rgba.empty())
		return true;
	RwTexture *texture = CHud::GetWeaponIconTexture(weaponType);
	RwRaster *raster = texture ? RwTextureGetRaster(texture) : nil;
	RwImage *image = CreateVrWeaponIconImage(raster);
	if(image == nil)
		return false;
	if(image->depth != 32)
		image->convertTo32();
	if(image->pixels == nil || image->width <= 0 || image->height <= 0 ||
	   image->depth != 32){
		RwImageDestroy(image);
		return false;
	}
	cache.width = image->width;
	cache.height = image->height;
	cache.rgba.resize(cache.width*cache.height*4);
	for(int y = 0; y < cache.height; y++)
		memcpy(&cache.rgba[y*cache.width*4], image->pixels+y*image->stride,
			cache.width*4);
	RwImageDestroy(image);
	return true;
}

void BlendVrMenuPixel(int x, int y, const uint8 *source)
{
	if(source == nil || x < 0 || y < 0 || x >= VR_MENU_WIDTH ||
	   y >= VR_MENU_HEIGHT || source[3] == 0)
		return;
#ifdef RW_D3D12
	const int offset = (y*VR_MENU_WIDTH+x)*4;
#else
	const int offset = ((VR_MENU_HEIGHT-1-y)*VR_MENU_WIDTH+x)*4;
#endif
	const uint32 alpha = source[3];
	const uint32 inverseAlpha = 255-alpha;
	for(int channel = 0; channel < 3; channel++)
		gVrMenuPixels[offset+channel] = (uint8)(
			(source[channel]*alpha+gVrMenuPixels[offset+channel]*inverseAlpha+127)/255);
	gVrMenuPixels[offset+3] = (uint8)Min(255U,
		alpha+(gVrMenuPixels[offset+3]*inverseAlpha+127)/255);
}

void DrawVrWeaponIcon(int weaponType, int left, int centreY,
	int maximumWidth, int maximumHeight)
{
	if(!CacheVrWeaponIcon(weaponType))
		return;
	const VrWeaponIconCache &cache = gVrWeaponIconCache[weaponType];
	if(cache.width <= 0 || cache.height <= 0)
		return;
	const float scale = Min((float)maximumWidth/cache.width,
		(float)maximumHeight/cache.height);
	const int width = Max(1, (int)(cache.width*scale+0.5f));
	const int height = Max(1, (int)(cache.height*scale+0.5f));
	const int top = centreY-height/2;
	FillVrMenuRect(left-3, top-3, left+width+3, top+height+3,
		4, 10, 18, 210);
	for(int y = 0; y < height; y++){
		const int sourceY = Min(cache.height-1, y*cache.height/height);
		for(int x = 0; x < width; x++){
			const int sourceX = Min(cache.width-1, x*cache.width/width);
			BlendVrMenuPixel(left+x, top+y,
				&cache.rgba[(sourceY*cache.width+sourceX)*4]);
		}
	}
}

bool UpdateVrMenuSwapchain()
{
	if((!gVrMenuVisible && !gCheatMenuVisible) || !gVrMenuSwapchain.handle || !AcquireSwapchain(gVrMenuSwapchain))
		return false;
	if(gVrBikeCalibrationMenuVisible &&
	   !IsImmersiveDrivingActiveInternal())
		gVrBikeCalibrationMenuVisible = false;
	FillVrMenuRect(0, 0, VR_MENU_WIDTH, VR_MENU_HEIGHT, 5, 10, 20, 238);
	FillVrMenuRect(28, 28, VR_MENU_WIDTH-28, VR_MENU_HEIGHT-28, 10, 22, 38, 245);
	DrawVrMenuText("VICE CITY VR", VR_MENU_WIDTH/2, 55, 7, 255, 120, 205);
	if(gCheatMenuVisible){
		DrawVrMenuText("CHEATS", VR_MENU_WIDTH/2, 118, 4, 100, 225, 255);
		const int count = GetVrCheatCount();
		const int itemsPerPage = 10;
		const int first = (gCheatMenuSelection/itemsPerPage)*itemsPerPage;
		for(int row = 0; row < itemsPerPage && first+row < count; row++){
			const int item = first+row;
			const int y = 158+row*50;
			if(item == gCheatMenuSelection)
				FillVrMenuRect(85, y-13, VR_MENU_WIDTH-85, y+37, 115, 42, 105, 245);
			DrawVrMenuText(GetVrCheatName(item), VR_MENU_WIDTH/2, y, 3,
				item == gCheatMenuSelection ? 255 : 205,
				item == gCheatMenuSelection ? 245 : 215,
				item == gCheatMenuSelection ? 110 : 225);
		}
		char page[48];
		sprintf(page, "PAGE %d OF %d", first/itemsPerPage+1,
			(count+itemsPerPage-1)/itemsPerPage);
		DrawVrMenuText(page, VR_MENU_WIDTH/2, 680, 3, 120, 220, 255);
		DrawVrMenuText("LEFT STICK SELECT   A ACTIVATE   B CLOSE", VR_MENU_WIDTH/2, 718, 3,
			170, 190, 210);
	}else if(gVrBikeCalibrationMenuVisible){
		CVehicle *vehicle = FindPlayerVehicle();
		const bool bike = vehicle && vehicle->IsBike();
		BikeHandleCalibration *calibration = vehicle ?
			(bike ?
			 GetBikeHandleCalibration(vehicle->GetModelIndex(),
				gBikeCalibrationEditHand) :
			 GetCarWheelCalibration(vehicle->GetModelIndex(),
				gBikeCalibrationEditHand)) : nil;
		BikeLeanCalibration *leanCalibration = bike ?
			GetBikeLeanCalibration(vehicle->GetModelIndex()) : nil;
		char heading[112];
		sprintf(heading, "VEHICLE CONTROLS - %s",
			GetActiveVrVehicleName());
		DrawVrMenuText(heading, VR_MENU_WIDTH/2, 108, 4,
			100, 225, 255);
		DrawVrMenuText("VALUES ARE SAVED FOR THIS VEHICLE MODEL",
			VR_MENU_WIDTH/2, 143, 2, 170, 190, 210);
		char rows[VR_BIKE_CALIBRATION_MENU_ITEM_COUNT][96];
		sprintf(rows[VR_BIKE_CAL_HAND], "EDIT HANDLE  < %s >",
			gBikeCalibrationEditHand == 0 ? "LEFT" : "RIGHT");
		sprintf(rows[VR_BIKE_CAL_OFFSET_X], "LOCAL X OFFSET  < %+.1f CM >",
			calibration ? (float)calibration->offsetX/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_OFFSET_Y], "LOCAL Y OFFSET  < %+.1f CM >",
			calibration ? (float)calibration->offsetY/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_OFFSET_Z], "LOCAL Z OFFSET  < %+.1f CM >",
			calibration ? (float)calibration->offsetZ/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_ROT_X], "LOCAL ROT X  < %+.1f DEG >",
			calibration ? (float)calibration->rotationX/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_ROT_Y], "LOCAL ROT Y  < %+.1f DEG >",
			calibration ? (float)calibration->rotationY/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_ROT_Z], "LOCAL ROT Z  < %+.1f DEG >",
			calibration ? (float)calibration->rotationZ/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_WHEELIE_HEIGHT],
			"WHEELIE HAND HEIGHT  < %d CM >",
			leanCalibration ? leanCalibration->wheelieHeightCm : 20);
		sprintf(rows[VR_BIKE_CAL_STAND_HEIGHT],
			"STAND HAND DROP  < %d CM >",
			leanCalibration ? leanCalibration->standHeightCm : 50);
		strcpy(rows[VR_BIKE_CAL_BACK], "BACK TO SETTINGS");
		const int rowCount = GetVehicleCalibrationMenuItemCount();
		for(int row = 0; row < rowCount; row++){
			const int item = GetVehicleCalibrationMenuItemForRow(row);
			const int y = 174+row*48;
			if(row == gVrBikeCalibrationMenuSelection)
				FillVrMenuRect(85, y-11, VR_MENU_WIDTH-85, y+34,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				row == gVrBikeCalibrationMenuSelection ? 255 : 205,
				row == gVrBikeCalibrationMenuSelection ? 245 : 215,
				row == gVrBikeCalibrationMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrHolsterMenuVisible){
		DrawVrMenuText("HOLSTER LOADOUT", VR_MENU_WIDTH/2, 112, 5, 100, 225, 255);
		DrawVrMenuText("SEVEN BODY POINTS - CENTER THROWABLE SLOT IS FIXED",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		for(int item = 0; item < HOLSTER_MENU_ITEM_COUNT; item++){
			const int y = 180+item*55;
			if(item == gVrHolsterMenuSelection)
				FillVrMenuRect(85, y-13, VR_MENU_WIDTH-85, y+37,
					25, 95, 135, 245);
			char row[112];
			if(item < HOLSTER_POINT_COUNT){
				char weapon[64];
				FormatHolsterSlotDisplayName(gHolsterPointWeaponSlot[item], weapon);
				if(item == HOLSTER_CHEST_CENTER)
					sprintf(row, "%s  [ %s ]", gHolsterPointNames[item], weapon);
				else
					sprintf(row, "%s  < %s >", gHolsterPointNames[item], weapon);
			}else
				strcpy(row, "BACK TO SETTINGS");
			DrawVrMenuText(row, VR_MENU_WIDTH/2, y, 3,
				item == gVrHolsterMenuSelection ? 255 : 205,
				item == gVrHolsterMenuSelection ? 245 : 215,
				item == gVrHolsterMenuSelection ? 110 : 225);
			if(item < HOLSTER_POINT_COUNT){
				const int slot = gHolsterPointWeaponSlot[item];
				if(slot >= 0 && IsVrWeaponSlotOwned(slot))
					DrawVrWeaponIcon(GetVrWeaponTypeForSlot(slot),
						VR_MENU_WIDTH-154, y+10, 48, 48);
			}
		}
		DrawVrMenuText("CLEAR THE OLD POINT BEFORE MOVING AN ASSIGNED SLOT",
			VR_MENU_WIDTH/2, 650, 2, 255, 180, 225);
		DrawVrMenuText("LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrCalibrationMenuVisible){
		SyncCurrentWeaponCalibration();
		SyncCurrentSupportGripCalibration();
		char calibrationHeading[112];
		sprintf(calibrationHeading, "WEAPON CALIBRATION - %s - %s HAND",
			GetVrWeaponName(GetCalibrationWeaponType()),
			gCalibrationEditHand == 0 ? "LEFT" : "RIGHT");
		DrawVrMenuText(calibrationHeading, VR_MENU_WIDTH/2, 110, 3,
			100, 225, 255);
		DrawVrMenuText("VALUES ARE SAVED PER WEAPON AND PER HAND",
			VR_MENU_WIDTH/2, 139, 2, 170, 190, 210);

		char rows[VR_CALIBRATION_MENU_ITEM_COUNT][96];
		sprintf(rows[VR_CAL_AIM_OFFSET_X], "AIM X OFFSET  < %+.1f CM >",
			(float)gWeaponAimOffsetXCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_AIM_OFFSET_Y], "AIM Y OFFSET  < %+.1f CM >",
			(float)gWeaponAimOffsetYCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_AIM_OFFSET_Z], "AIM Z OFFSET  < %+.1f CM >",
			(float)gWeaponAimOffsetZCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_AIM_ROT_X], "AIM LOCAL ROT X  < %+.1f DEG >",
			(float)gWeaponAimRotationXDeg/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_AIM_ROT_Y], "AIM LOCAL ROT Y  < %+.1f DEG >",
			(float)gWeaponAimRotationYDeg/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_AIM_ROT_Z], "AIM LOCAL ROT Z  < %+.1f DEG >",
			(float)gWeaponAimRotationZDeg/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_OFFSET_X], "WEAPON X OFFSET  < %+.1f CM >",
			(float)gWeaponOffsetXCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_OFFSET_Y], "WEAPON Y OFFSET  < %+.1f CM >",
			(float)gWeaponOffsetYCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_OFFSET_Z], "WEAPON Z OFFSET  < %+.1f CM >",
			(float)gWeaponOffsetZCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_ROT_X], "WEAPON LOCAL ROT X  < %+.1f DEG >",
			(float)gWeaponRotationXDeg/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_ROT_Y], "WEAPON LOCAL ROT Y  < %+.1f DEG >",
			(float)gWeaponRotationYDeg/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_ROT_Z], "WEAPON LOCAL ROT Z  < %+.1f DEG >",
			(float)gWeaponRotationZDeg/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_SUPPORT_OFFSET_X], "SUPPORT GRIP X  < %+.1f CM >",
			(float)gSupportGripOffsetXCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_SUPPORT_OFFSET_Y], "SUPPORT GRIP Y  < %+.1f CM >",
			(float)gSupportGripOffsetYCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_SUPPORT_OFFSET_Z], "SUPPORT GRIP Z  < %+.1f CM >",
			(float)gSupportGripOffsetZCm/WEAPON_CALIBRATION_VALUE_SCALE);
		strcpy(rows[VR_CAL_BACK], "BACK TO SETTINGS");

		for(int item = 0; item < VR_CALIBRATION_MENU_ITEM_COUNT; item++){
			const int y = 162+item*29;
			if(item == gVrCalibrationMenuSelection)
				FillVrMenuRect(85, y-5, VR_MENU_WIDTH-85, y+22,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 2,
				item == gVrCalibrationMenuSelection ? 255 : 205,
				item == gVrCalibrationMenuSelection ? 245 : 215,
				item == gVrCalibrationMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrVehicleMenuVisible){
		DrawVrMenuText("VEHICLE SETTINGS", VR_MENU_WIDTH/2, 112, 5,
			100, 225, 255);
		DrawVrMenuText("DRIVING CONTROLS AND PER-VEHICLE CALIBRATION",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_VEHICLE_MENU_ITEM_COUNT][112];
		sprintf(rows[VR_VEHICLE_DRIVING_TYPE], "DRIVING TYPE  < %s >",
			DrivingTypeName());
		sprintf(rows[VR_VEHICLE_DRIVING_Y],
			"DRIVING Y OFFSET  < %+d CM >", gDrivingYOffsetCm);
		sprintf(rows[VR_VEHICLE_MOTION_HAND],
			"MOTION STEERING HAND  < %s >", MotionSteeringHandName());
		sprintf(rows[VR_VEHICLE_HANDLE_HIGHLIGHTS],
			"VEHICLE GRIP HIGHLIGHTS  < %s >",
			gBikeHandleHighlightsEnabled ? "ON" : "OFF");
		const char *calibrationState =
			IsImmersiveDrivingActiveInternal() ? "OPEN" :
			(gDrivingType == VR_DRIVING_IMMERSIVE ?
				"ENTER VEHICLE" : "IMMERSIVE MODE ONLY");
		sprintf(rows[VR_VEHICLE_CALIBRATION],
			"CONTROL CALIBRATION  < %s >", calibrationState);
		strcpy(rows[VR_VEHICLE_BACK], "BACK TO SETTINGS");
		for(int item = 0; item < VR_VEHICLE_MENU_ITEM_COUNT; item++){
			const int y = 180+item*58;
			if(item == gVrVehicleMenuSelection)
				FillVrMenuRect(85, y-13, VR_MENU_WIDTH-85, y+39,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrVehicleMenuSelection ? 255 : 205,
				item == gVrVehicleMenuSelection ? 245 : 215,
				item == gVrVehicleMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else{
	DrawVrMenuText("SETTINGS", VR_MENU_WIDTH/2, 118, 4, 100, 225, 255);

	char rows[VR_MENU_ITEM_COUNT][96];
	const int requestedWidth = gEye[0].swapchain.width > 0 ?
		(int)(gEye[0].swapchain.width*gRenderScaleOptions[gRenderScaleIndex]+0.5f) : 0;
	const int requestedHeight = gEye[0].swapchain.height > 0 ?
		(int)(gEye[0].swapchain.height*gRenderScaleOptions[gRenderScaleIndex]+0.5f) : 0;
	sprintf(rows[VR_MAIN_RENDER_SCALE], "RENDER SCALE  < %d%% >",
		(int)(gRenderScaleOptions[gRenderScaleIndex]*100.0f+0.5f));
	sprintf(rows[VR_MAIN_VRS], "VRS  < %s >", FoveatedProfileName());
	sprintf(rows[VR_MAIN_DLAA], "DLAA  < %s >",
		!gDlaaEnabled ? "OFF" :
		(gDlaaStereoActivationFailed ? "ERROR" : "ON"));
	sprintf(rows[VR_MAIN_JITTER], "DLAA JITTER  < %s >", TemporalJitterModeName());
	sprintf(rows[VR_MAIN_FXAA], "FXAA FALLBACK  < %s >",
		gAntiAliasingEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_COLOR], "COLOR FILTER  < %s >",
		gLightingEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_HUD], "GAMEPLAY HUD  < %s >",
		gGameplayHudVisible ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_HANDS], "VR HANDS  < %s >",
		gVrHandsEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_LASER], "WEAPON LASER  < %s >",
		gWeaponLaserEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_HOLSTER_HIGHLIGHTS], "HOLSTER HIGHLIGHTS  < %s >",
		gWeaponHolsterHighlightsEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_MANUAL_RELOAD], "MANUAL RELOADING  < %s >",
		gManualReloadEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_SCOPE_AIM], "PHYSICAL SCOPE AIM  < %s >",
		gPhysicalScopeAimEnabled ? "ON" : "OFF");
	strcpy(rows[VR_MAIN_VEHICLE_SETTINGS], "VEHICLE SETTINGS  < OPEN >");
	sprintf(rows[VR_MAIN_DEBUG], "DEBUG OVERLAY  < %s >",
		gDebugVisible ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_GRIP_LOCK], "WEAPON GRIP LOCK  < %s >",
		gWeaponGripLockEnabled ? "ON" : "OFF");
	strcpy(rows[VR_MAIN_CALIBRATION], "WEAPON CALIBRATION  < OPEN >");
	strcpy(rows[VR_MAIN_HOLSTERS], "HOLSTER LOADOUT  < OPEN >");
	for(int item = 0; item < VR_MENU_ITEM_COUNT; item++){
		const int y = 145+item*22;
		if(item == gVrMenuSelection)
			FillVrMenuRect(85, y-2, VR_MENU_WIDTH-85, y+18, 25, 95, 135, 245);
		DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 2,
			item == gVrMenuSelection ? 255 : 205,
			item == gVrMenuSelection ? 245 : 215,
			item == gVrMenuSelection ? 110 : 225);
	}
	char resolution[96];
	sprintf(resolution, "PER EYE  %d X %d", requestedWidth, requestedHeight);
	DrawVrMenuText(resolution, VR_MENU_WIDTH/2, 608, 3, 120, 220, 255);
	char vrsStatus[96] = "VRS NOT SUPPORTED";
#ifdef RW_D3D12
	rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
	rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
	if(foveatedInfo.supported){
		if(foveatedInfo.profile == rw::d3d12::FIXED_FOVEATED_OFF)
			sprintf(vrsStatus, "VRS TIER %u READY - OFF", foveatedInfo.tier);
		else
			sprintf(vrsStatus, "VRS TIER %u ACTIVE   TILE %u", foveatedInfo.tier,
				foveatedInfo.tileSize);
	}
#endif
	DrawVrMenuText(vrsStatus, VR_MENU_WIDTH/2, 665, 3, 125, 255, 145);
	const char *dlaaStatus = !gDlaaEnabled ? "DLAA DISABLED" :
		(!Dlaa::IsSupported() ? "DLAA UNAVAILABLE" :
		(gDlaaStereoActivationFailed ? "DLAA INIT FAILED - FXAA" :
		(!gDlaaStereoActivationReady ? "DLAA PREPARING" :
		(Dlaa::WasLastEvaluationSuccessful() ? "DLAA ACTIVE" :
		"DLAA READY"))));
	DrawVrMenuText(dlaaStatus, VR_MENU_WIDTH/2, 637, 2, 255, 180, 225);
	DrawVrMenuText("LEFT STICK SELECT   L2 MINUS   R2 PLUS   A OPEN   B CLOSE", VR_MENU_WIDTH/2, 718, 2,
		170, 190, 210);
	}

#ifdef RW_D3D12
	if(!rw::d3d12::uploadRgbaToExternal(
	   gVrMenuSwapchain.images[gVrMenuSwapchain.acquiredIndex].texture, gVrMenuPixels,
	   VR_MENU_WIDTH*4, VR_MENU_WIDTH, VR_MENU_HEIGHT)){
		ReleaseSwapchain(gVrMenuSwapchain);
		return false;
	}
	return true;
#else
	GLint oldTexture = 0, oldAlignment = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlignment);
	glBindTexture(GL_TEXTURE_2D, gVrMenuSwapchain.images[gVrMenuSwapchain.acquiredIndex].image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VR_MENU_WIDTH, VR_MENU_HEIGHT,
		GL_RGBA, GL_UNSIGNED_BYTE, gVrMenuPixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlignment);
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	ReleaseSwapchain(gVrMenuSwapchain);
	return true;
#endif
}

bool UpdateDebugSwapchain()
{
	if(!gDebugVisible || !gDebugSwapchain.handle || !AcquireSwapchain(gDebugSwapchain))
		return false;
	for(int pixel=0;pixel<VR_DEBUG_WIDTH*VR_DEBUG_HEIGHT;pixel++){
		gDebugPixels[pixel*4+0]=0; gDebugPixels[pixel*4+1]=0;
		gDebugPixels[pixel*4+2]=0; gDebugPixels[pixel*4+3]=210;
	}
	char value[64];
	sprintf(value,"OPENXR FPS:%d",gDebugFps);
	DrawDebugText(value,VR_DEBUG_WIDTH/2,10,3,255,230,64);
	const char *aaMode = gDlaaEnabled && Dlaa::WasLastEvaluationSuccessful() ? "DLAA" :
		(gAntiAliasingEnabled ? "FXAA" : "OFF");
	sprintf(value,"AA:%s LIGHT:%s HUD:%s",aaMode,
		gLightingEnabled?"ON":"OFF",gGameplayHudVisible?"ON":"OFF");
	DrawDebugText(value,VR_DEBUG_WIDTH/2,48,3,96,220,255);
	char vrsMode[8] = "NA";
#ifdef RW_D3D12
	rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
	rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
	if(foveatedInfo.supported)
		sprintf(vrsMode, "%u", foveatedInfo.profile);
#endif
	if(gPerfRecording) sprintf(value,"REC:%d SPS:%s VRS:%s",gPerfRecordedSamples,
		gFullStereoSinglePass?"FULL":"HYBRID",vrsMode);
	else sprintf(value,"SPS:%s VRS:%s JIT:%s",gFullStereoSinglePass?"FULL":"HYBRID",
		vrsMode,TemporalJitterModeName());
	DrawDebugText(value,VR_DEBUG_WIDTH/2,86,3,128,255,128);

#ifdef RW_D3D12
	if(!rw::d3d12::uploadRgbaToExternal(
	   gDebugSwapchain.images[gDebugSwapchain.acquiredIndex].texture, gDebugPixels,
	   VR_DEBUG_WIDTH*4, VR_DEBUG_WIDTH, VR_DEBUG_HEIGHT)){
		ReleaseSwapchain(gDebugSwapchain);
		return false;
	}
	// The image is released together with the eye and HUD images after the
	// shared D3D12 command list has completed.
	return true;
#else
	GLint oldTexture=0,oldAlignment=0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
	glGetIntegerv(GL_UNPACK_ALIGNMENT,&oldAlignment);
	glBindTexture(GL_TEXTURE_2D,gDebugSwapchain.images[gDebugSwapchain.acquiredIndex].image);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT,1);
	glTexSubImage2D(GL_TEXTURE_2D,0,0,0,VR_DEBUG_WIDTH,VR_DEBUG_HEIGHT,
		GL_RGBA,GL_UNSIGNED_BYTE,gDebugPixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT,oldAlignment);
	glBindTexture(GL_TEXTURE_2D,oldTexture);
	ReleaseSwapchain(gDebugSwapchain);
	return true;
#endif
}

#ifdef RW_D3D12
ID3D12Resource *AcquiredTexture(const Swapchain &swapchain)
{
	return swapchain.images[swapchain.acquiredIndex].texture;
}
#else
GLuint AcquiredTexture(const Swapchain &swapchain)
{
	return swapchain.images[swapchain.acquiredIndex].image;
}
#endif

void DestroyStereoRenderTargets()
{
#ifdef RW_D3D12
	Dlaa::ReleaseResources();
#endif
	for(int eye = 0; eye < EYE_COUNT; eye++){
		if(gEye[eye].depth) RwRasterDestroy(gEye[eye].depth);
		if(gEye[eye].color) RwRasterDestroy(gEye[eye].color);
		gEye[eye].depth = gEye[eye].color = nil;
	}
#ifdef RW_D3D12
	if(gStereoDepth) RwRasterDestroy(gStereoDepth);
	if(gStereoColor) RwRasterDestroy(gStereoColor);
	gStereoDepth = gStereoColor = nil;
#endif
}

bool ReplaceStereoRenderTargets(float scale, bool synchronizeOldTargets)
{
	int renderWidth[EYE_COUNT], renderHeight[EYE_COUNT];
	for(int eye = 0; eye < EYE_COUNT; eye++){
		renderWidth[eye] = (int)(gEye[eye].swapchain.width*scale+0.5f);
		renderHeight[eye] = (int)(gEye[eye].swapchain.height*scale+0.5f);
	}
#ifdef RW_D3D12
	if(renderWidth[0] != renderWidth[1] || renderHeight[0] != renderHeight[1])
		return false;
	RwRaster *newStereoColor = RwRasterCreate(renderWidth[0]*EYE_COUNT, renderHeight[0], 32,
		rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
	RwRaster *newStereoDepth = RwRasterCreate(renderWidth[0]*EYE_COUNT, renderHeight[0], 0,
		rwRASTERTYPEZBUFFER);
	RwRaster *newColor[EYE_COUNT] = {};
	RwRaster *newDepth[EYE_COUNT] = {};
	bool created = newStereoColor && newStereoDepth;
	for(int eye = 0; eye < EYE_COUNT && created; eye++){
		newColor[eye] = RwRasterCreate(0, 0, 0,
			rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888 | rwRASTERDONTALLOCATE);
		newDepth[eye] = RwRasterCreate(0, 0, 0,
			rwRASTERTYPEZBUFFER | rwRASTERDONTALLOCATE);
		created = newColor[eye] && newDepth[eye];
		if(created){
			rw::Rect rect = { eye*renderWidth[eye], 0, renderWidth[eye], renderHeight[eye] };
			((rw::Raster*)newColor[eye])->subRaster((rw::Raster*)newStereoColor, &rect);
			((rw::Raster*)newDepth[eye])->subRaster((rw::Raster*)newStereoDepth, &rect);
		}
	}
	if(!created || (synchronizeOldTargets && !rw::d3d12::submitAndWaitForExternal())){
		for(int eye = 0; eye < EYE_COUNT; eye++){
			if(newDepth[eye]) RwRasterDestroy(newDepth[eye]);
			if(newColor[eye]) RwRasterDestroy(newColor[eye]);
		}
		if(newStereoDepth) RwRasterDestroy(newStereoDepth);
		if(newStereoColor) RwRasterDestroy(newStereoColor);
		return false;
	}
	DestroyStereoRenderTargets();
	gStereoColor = newStereoColor;
	gStereoDepth = newStereoDepth;
	for(int eye = 0; eye < EYE_COUNT; eye++){
		gEye[eye].color = newColor[eye];
		gEye[eye].depth = newDepth[eye];
		gEye[eye].renderWidth = renderWidth[eye];
		gEye[eye].renderHeight = renderHeight[eye];
	}
	gDlaaStereoActivationReady = false;
	gDlaaStereoActivationFailed = false;
	gDlaaStereoWarmupFrames = DLAA_ACTIVATION_WARMUP_FRAMES;
	VrLog("D3D12 double-wide stereo target %dx%d (%dx%d per eye, %.0f%%)\n",
		renderWidth[0]*EYE_COUNT, renderHeight[0], renderWidth[0], renderHeight[0], scale*100.0f);
#else
	RwRaster *newColor[EYE_COUNT] = {};
	RwRaster *newDepth[EYE_COUNT] = {};
	bool created = true;
	for(int eye = 0; eye < EYE_COUNT && created; eye++){
		newColor[eye] = RwRasterCreate(renderWidth[eye], renderHeight[eye], 32,
			rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
		newDepth[eye] = RwRasterCreate(renderWidth[eye], renderHeight[eye], 0,
			rwRASTERTYPEZBUFFER);
		created = newColor[eye] && newDepth[eye];
	}
	if(!created){
		for(int eye = 0; eye < EYE_COUNT; eye++){
			if(newDepth[eye]) RwRasterDestroy(newDepth[eye]);
			if(newColor[eye]) RwRasterDestroy(newColor[eye]);
		}
		return false;
	}
	DestroyStereoRenderTargets();
	for(int eye = 0; eye < EYE_COUNT; eye++){
		gEye[eye].color = newColor[eye];
		gEye[eye].depth = newDepth[eye];
		gEye[eye].renderWidth = renderWidth[eye];
		gEye[eye].renderHeight = renderHeight[eye];
	}
#endif
	gRenderScale = scale;
	return true;
}

void ApplyPendingRenderScale()
{
	if(!gRenderScaleChangePending || !gSession || gFrameBegun || gFramePrepared)
		return;
	gRenderScaleChangePending = false;
	const float requestedScale = gRenderScaleOptions[gRenderScaleIndex];
	if(ReplaceStereoRenderTargets(requestedScale, true)){
		SaveVrSetting("RenderScale", gRenderScaleIndex);
		debug("[OpenXR] Render scale: %.0f%% (%dx%d per eye)\n", requestedScale*100.0f,
			gEye[0].renderWidth, gEye[0].renderHeight);
	}else{
		int nearest = 0;
		for(int i = 1; i < (int)ARRAY_SIZE(gRenderScaleOptions); i++)
			if(fabsf(gRenderScaleOptions[i]-gRenderScale) <
			   fabsf(gRenderScaleOptions[nearest]-gRenderScale)) nearest = i;
		gRenderScaleIndex = nearest;
		debug("[OpenXR] Could not allocate the requested render scale; keeping %.0f%%\n",
			gRenderScale*100.0f);
	}
}

void DestroyStartupCapture()
{
	if(gStartupCaptureDc){
		if(gStartupCaptureOldBitmap)
			SelectObject(gStartupCaptureDc, gStartupCaptureOldBitmap);
		if(gStartupCaptureBitmap) DeleteObject(gStartupCaptureBitmap);
		DeleteDC(gStartupCaptureDc);
	}
	gStartupCaptureDc = nil;
	gStartupCaptureBitmap = nil;
	gStartupCaptureOldBitmap = nil;
	gStartupCaptureBits = nil;
}

void DestroySessionResources()
{
#ifdef RW_D3D12
	// Without an OpenXR session the desktop swapchain owns frame pacing again.
	rw::d3d12::setPresentInterval(1);
#endif
	DestroyStereoRenderTargets();
	for(int eye = 0; eye < EYE_COUNT; eye++){
		DestroySwapchain(gEye[eye].swapchain);
	}
	if(gHudDepth) RwRasterDestroy(gHudDepth);
	if(gHudColor) RwRasterDestroy(gHudColor);
	gHudDepth = gHudColor = nil;
	DestroySwapchain(gHudSwapchain);
	DestroySwapchain(gCinemaSwapchain);
	DestroySwapchain(gDebugSwapchain);
	DestroySwapchain(gVrMenuSwapchain);
	DestroyStartupCapture();
#ifndef RW_D3D12
	DestroyFxaaProgram();
	if(gCopyFramebuffer){ glDeleteFramebuffers(1, &gCopyFramebuffer); gCopyFramebuffer = 0; }
#endif
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(gActions.gripSpace[hand]) xrDestroySpace(gActions.gripSpace[hand]);
		if(gActions.aimSpace[hand]) xrDestroySpace(gActions.aimSpace[hand]);
		gActions.gripSpace[hand] = XR_NULL_HANDLE;
		gActions.aimSpace[hand] = XR_NULL_HANDLE;
		gTrackedHandPoseValid[hand] = false;
		gTrackedHandAimPoseValid[hand] = false;
	}
	if(gGameplaySpace) xrDestroySpace(gGameplaySpace);
	if(gLocalSpace) xrDestroySpace(gLocalSpace);
	if(gViewSpace) xrDestroySpace(gViewSpace);
	gGameplaySpace = gLocalSpace = gViewSpace = XR_NULL_HANDLE;
	gCinemaAnchorValid = false;
	if(gSession){
		if(gSessionRunning) xrEndSession(gSession);
		xrDestroySession(gSession);
	}
	gSession = XR_NULL_HANDLE;
	gSessionRunning = false;
	gFrameBegun = false;
	gFramePrepared = false;
	gWasSubmitting = false;
	gTrackingCenterValid = false;
	gRecenterRequested = false;
}

void DestroyRuntime()
{
	DestroySessionResources();
	if(gActions.set) xrDestroyActionSet(gActions.set);
	gActions = Actions();
	if(gInstance) xrDestroyInstance(gInstance);
	gInstance = XR_NULL_HANDLE;
	gSystemId = XR_NULL_SYSTEM_ID;
	gExitRequested = false;
}

bool InitializeRuntime()
{
	LoadVrSettings();
	if(gInstance)
		return true;
	VrLog("InitializeRuntime begin backend=D3D12\n");
#ifdef RW_D3D12
	const char *extensions[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
#else
	const char *extensions[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
#endif
	XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy(info.applicationInfo.applicationName, "Vice City VR");
	info.applicationInfo.applicationVersion = 1;
#ifdef RW_D3D12
	strcpy(info.applicationInfo.engineName, "reVC librw D3D12");
#else
	strcpy(info.applicationInfo.engineName, "reVC librw GL3");
#endif
	info.applicationInfo.engineVersion = 1;
	info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	info.enabledExtensionCount = ARRAY_SIZE(extensions);
	info.enabledExtensionNames = extensions;
	if(!XrOk(xrCreateInstance(&info, &gInstance), "xrCreateInstance"))
		return false;
	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if(!XrOk(xrGetSystem(gInstance, &systemInfo, &gSystemId), "xrGetSystem")){
		DestroyRuntime();
		return false;
	}
	if(!CreateActions()){
		DestroyRuntime();
		return false;
	}
	debug("[OpenXR] Runtime initialized (loader API 1.1, requested API 1.0)\n");
	VrLog("InitializeRuntime ok system=%llu\n", (unsigned long long)gSystemId);
	return true;
}

bool ChooseColorFormat()
{
	uint32_t count = 0;
	if(!XrOk(xrEnumerateSwapchainFormats(gSession, 0, &count, nil),
		"xrEnumerateSwapchainFormats(count)"))
		return false;
	std::vector<int64_t> formats(count);
	if(!XrOk(xrEnumerateSwapchainFormats(gSession, count, &count, formats.data()),
		"xrEnumerateSwapchainFormats"))
		return false;
#ifdef RW_D3D12
	VrLog("Swapchain formats count=%u", count);
	for(uint32 i = 0; i < count; i++)
		VrLog(" %lld", (long long)formats[i]);
	VrLog("\n");
	// The game produces display-referred (sRGB-encoded) colour values. Asking the
	// compositor for a linear UNORM swapchain makes those values look washed out.
	const int64_t preferred[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
		DXGI_FORMAT_R8G8B8A8_UNORM };
#else
	const int64_t preferred[] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
#endif
	for(uint32 p = 0; p < ARRAY_SIZE(preferred); p++)
		for(uint32 i = 0; i < count; i++)
			if(formats[i] == preferred[p]){ gColorFormat = preferred[p]; return true; }
	debug("[OpenXR] Runtime exposes no compatible RGBA8 swapchain format\n");
	return false;
}

bool CreateSession()
{
	if(!InitializeRuntime())
		return false;
#ifdef RW_D3D12
	PFN_xrGetD3D12GraphicsRequirementsKHR getRequirements = nil;
	if(!XrOk(xrGetInstanceProcAddr(gInstance, "xrGetD3D12GraphicsRequirementsKHR",
		(PFN_xrVoidFunction*)&getRequirements), "xrGetD3D12GraphicsRequirementsKHR") || !getRequirements)
		return false;
	XrGraphicsRequirementsD3D12KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
	if(!XrOk(getRequirements(gInstance, gSystemId, &requirements), "D3D12 graphics requirements"))
		return false;
	ID3D12Device *device = rw::d3d12::getDevice();
	ID3D12CommandQueue *queue = rw::d3d12::getCommandQueue();
	if(!device || !queue){
		debug("[OpenXR] librw D3D12 device or queue is unavailable\n");
		return false;
	}
	const LUID deviceLuid = device->GetAdapterLuid();
	VrLog("Graphics requirements feature=%u runtimeLuid=%08X:%08X deviceLuid=%08X:%08X\n",
		(unsigned)requirements.minFeatureLevel,
		(unsigned)requirements.adapterLuid.HighPart, (unsigned)requirements.adapterLuid.LowPart,
		(unsigned)deviceLuid.HighPart, (unsigned)deviceLuid.LowPart);
	if(deviceLuid.HighPart != requirements.adapterLuid.HighPart ||
	   deviceLuid.LowPart != requirements.adapterLuid.LowPart){
		debug("[OpenXR] D3D12 device does not use the runtime-requested adapter\n");
		return false;
	}
	XrGraphicsBindingD3D12KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
	binding.device = device;
	binding.queue = queue;
#else
	PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nil;
	if(!XrOk(xrGetInstanceProcAddr(gInstance, "xrGetOpenGLGraphicsRequirementsKHR",
		(PFN_xrVoidFunction*)&getRequirements), "xrGetOpenGLGraphicsRequirementsKHR") || !getRequirements)
		return false;
	XrGraphicsRequirementsOpenGLKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	if(!XrOk(getRequirements(gInstance, gSystemId, &requirements), "OpenGL graphics requirements"))
		return false;
	XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	binding.hDC = wglGetCurrentDC();
	binding.hGLRC = wglGetCurrentContext();
	if(!binding.hDC || !binding.hGLRC){
		debug("[OpenXR] No current Win32 OpenGL context\n");
		return false;
	}
#endif
	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = gSystemId;
	if(!XrOk(xrCreateSession(gInstance, &sessionInfo, &gSession), "xrCreateSession"))
		return false;
	VrLog("xrCreateSession ok\n");
	if(!ChooseColorFormat()){
		DestroySessionResources();
		return false;
	}
	uint32_t viewCount = 0;
	if(!XrOk(xrEnumerateViewConfigurationViews(gInstance, gSystemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, EYE_COUNT, &viewCount, gViewConfig),
		"xrEnumerateViewConfigurationViews") || viewCount != EYE_COUNT){
		DestroySessionResources();
		return false;
	}
	for(int eye = 0; eye < EYE_COUNT; eye++){
		const int width = (int)gViewConfig[eye].recommendedImageRectWidth;
		const int height = (int)gViewConfig[eye].recommendedImageRectHeight;
		if(!CreateSwapchain(gEye[eye].swapchain, width, height)){
			DestroySessionResources(); return false;
		}
	}
	const int maximumRenderScaleIndex = MaxSupportedRenderScaleIndex();
	if(gRenderScaleIndex > maximumRenderScaleIndex){
		gRenderScaleIndex = maximumRenderScaleIndex;
		gRenderScale = gRenderScaleOptions[gRenderScaleIndex];
		SaveVrSetting("RenderScale", gRenderScaleIndex);
		debug("[OpenXR] Saved render scale exceeded the double-wide D3D12 target limit; capped at %.0f%%\n",
			gRenderScale*100.0f);
	}
	if(!ReplaceStereoRenderTargets(gRenderScale, false)){
		DestroySessionResources(); return false;
	}
#ifdef RW_D3D12
	rw::d3d12::setFixedFoveatedRenderingProfile(gFixedFoveatedProfile);
	rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
	rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
	debug("[OpenXR] D3D12 VRS Tier %u: %s, tile %u, profile %u\n",
		foveatedInfo.tier, foveatedInfo.supported ? "fixed foveation ready" : "unsupported",
		foveatedInfo.tileSize, foveatedInfo.profile);
	VrLog("D3D12 VRS tier=%u supported=%d additionalRates=%d tile=%u profile=%u\n",
		foveatedInfo.tier, foveatedInfo.supported, foveatedInfo.additionalRates,
		foveatedInfo.tileSize, foveatedInfo.profile);
#endif
	if(!CreateSwapchain(gHudSwapchain, VR_HUD_WIDTH, VR_HUD_HEIGHT)){
		DestroySessionResources(); return false;
	}
	if(!CreateSwapchain(gDebugSwapchain, VR_DEBUG_WIDTH, VR_DEBUG_HEIGHT)){
		DestroySessionResources(); return false;
	}
	if(!CreateSwapchain(gVrMenuSwapchain, VR_MENU_WIDTH, VR_MENU_HEIGHT)){
		DestroySessionResources(); return false;
	}
	gHudColor = RwRasterCreate(VR_HUD_WIDTH, VR_HUD_HEIGHT, 32,
		rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
	gHudDepth = RwRasterCreate(VR_HUD_WIDTH, VR_HUD_HEIGHT, 0, rwRASTERTYPEZBUFFER);
	if(!gHudColor || !gHudDepth){ DestroySessionResources(); return false; }
	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if(!XrOk(xrCreateReferenceSpace(gSession, &spaceInfo, &gLocalSpace), "xrCreateReferenceSpace(local)")){
		DestroySessionResources(); return false;
	}
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if(!XrOk(xrCreateReferenceSpace(gSession, &spaceInfo, &gViewSpace), "xrCreateReferenceSpace(view)")){
		DestroySessionResources(); return false;
	}
	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &gActions.set;
	if(!XrOk(xrAttachSessionActionSets(gSession, &attach), "xrAttachSessionActionSets")){
		DestroySessionResources(); return false;
	}
	for(int hand = 0; hand < EYE_COUNT; hand++){
		XrActionSpaceCreateInfo handSpaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		handSpaceInfo.action = gActions.gripPose;
		handSpaceInfo.subactionPath = gActions.hands[hand];
		handSpaceInfo.poseInActionSpace.orientation.w = 1.0f;
		if(!XrOk(xrCreateActionSpace(gSession, &handSpaceInfo, &gActions.gripSpace[hand]),
			"xrCreateActionSpace(hand grip)")){
			DestroySessionResources(); return false;
		}
		handSpaceInfo.action = gActions.aimPose;
		if(!XrOk(xrCreateActionSpace(gSession, &handSpaceInfo, &gActions.aimSpace[hand]),
			"xrCreateActionSpace(hand aim)")){
			DestroySessionResources(); return false;
		}
	}
#ifndef RW_D3D12
	glGenFramebuffers(1, &gCopyFramebuffer);
	if(!CreateFxaaProgram())
		debug("[OpenXR] FXAA unavailable; using linear supersample resolve\n");
#endif
#ifdef RW_D3D12
	const char *backendName = "D3D12";
#else
	const char *backendName = "GL3";
#endif
	debug("[OpenXR] %s stereo session created: submit %dx%d, render %dx%d (%.0f%%)\n",
		backendName,
		gEye[0].swapchain.width, gEye[0].swapchain.height,
		gEye[0].renderWidth, gEye[0].renderHeight, gRenderScale*100.0f);
	VrLog("CreateSession ok eye=%dx%d hud=%dx%d\n",
		gEye[0].renderWidth, gEye[0].renderHeight, VR_HUD_WIDTH, VR_HUD_HEIGHT);
#ifdef RW_D3D12
	// xrWaitFrame is the authoritative limiter while VR is active.  Do not
	// block a second time on the desktop window's vertical blank.
	rw::d3d12::setPresentInterval(0);
#endif
	return true;
}

bool PollEvents()
{
	if(!gInstance)
		return false;
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while(xrPollEvent(gInstance, &event) == XR_SUCCESS){
		if(event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){
			const XrEventDataSessionStateChanged *changed =
				(const XrEventDataSessionStateChanged*)&event;
			gSessionState = changed->state;
			VrLog("Session state=%d\n", (int)changed->state);
			if(changed->state == XR_SESSION_STATE_READY && !gSessionRunning){
				XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if(XrOk(xrBeginSession(gSession, &begin), "xrBeginSession"))
					gSessionRunning = true;
			}else if(changed->state == XR_SESSION_STATE_STOPPING && gSessionRunning){
				xrEndSession(gSession);
				gSessionRunning = false;
				gWasSubmitting = false;
			}else if(changed->state == XR_SESSION_STATE_EXITING ||
			          changed->state == XR_SESSION_STATE_LOSS_PENDING){
				gExitRequested = true;
			}
		}else if(event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING){
			const XrEventDataReferenceSpaceChangePending *changed =
				(const XrEventDataReferenceSpaceChangePending*)&event;
			if(changed->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL){
				// Meta's system recenter changes LOCAL underneath the game. Rebuild
				// our head-anchored space on the next stereo frame so the saved
				// positional origin cannot pull the camera away from Tommy.
				gRecenterRequested = true;
				gCinemaAnchorValid = false;
				VrLog("LOCAL reference space changed; gameplay head anchor reset\n");
			}
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
	if(gExitRequested){
		DestroyRuntime();
		gRetryFrames = 300;
		return false;
	}
	return true;
}

bool EnsureSession()
{
	if(!gSession){
		if(gRetryFrames > 0){ --gRetryFrames; return false; }
		if(!CreateSession()){
			VrLog("CreateSession failed; retry delayed\n");
			gRetryFrames = 300;
			return false;
		}
	}
	return PollEvents();
}

bool BeginXrFrame()
{
	if(!EnsureSession() || gFrameBegun)
		return false;
	ApplyPendingRenderScale();
	if(!gSessionRunning)
		return false;
	XrFrameWaitInfo wait = { XR_TYPE_FRAME_WAIT_INFO };
	gFrameState = { XR_TYPE_FRAME_STATE };
	double timingStart = PerfNowMs();
	XrResult result = xrWaitFrame(gSession, &wait, &gFrameState);
	if(gPerfRecording && gPerfFrameStarted)
		gPerfCurrent.xrWaitFrameMs += (float)(PerfNowMs() - timingStart);
	if(!XrOk(result, "xrWaitFrame"))
		return false;
	XrFrameBeginInfo begin = { XR_TYPE_FRAME_BEGIN_INFO };
	timingStart = PerfNowMs();
	result = xrBeginFrame(gSession, &begin);
	if(gPerfRecording && gPerfFrameStarted)
		gPerfCurrent.xrBeginFrameMs += (float)(PerfNowMs() - timingStart);
	if(!XrOk(result, "xrBeginFrame"))
		return false;
	const double now=PerfNowMs();
	if(gDebugPreviousFrameMs>0.0){
		const float frameMs=(float)(now-gDebugPreviousFrameMs);
		gDebugSmoothedFrameMs=gDebugSmoothedFrameMs>0.0f ?
			gDebugSmoothedFrameMs*0.9f+frameMs*0.1f : frameMs;
		gDebugFps=gDebugSmoothedFrameMs>0.0f ? (int)(1000.0f/gDebugSmoothedFrameMs+0.5f) : 0;
	}
	gDebugPreviousFrameMs=now;
	gFrameBegun = true;
	if(gVrLoggedFrames < 10 || (gFrameState.shouldRender && gVrLoggedRenderableFrames < 10))
		VrLog("Begin frame %d shouldRender=%u predicted=%lld\n", gVrLoggedFrames,
			(unsigned)gFrameState.shouldRender, (long long)gFrameState.predictedDisplayTime);
	return true;
}

bool EndXrFrame(const XrCompositionLayerBaseHeader *const *layers, uint32_t count)
{
	if(!gFrameBegun)
		return false;
	XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
	end.displayTime = gFrameState.predictedDisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = count;
	end.layers = layers;
	const double timingStart = PerfNowMs();
	XrResult result = xrEndFrame(gSession, &end);
	if(gPerfRecording && gPerfFrameStarted)
		gPerfCurrent.xrEndFrameMs += (float)(PerfNowMs() - timingStart);
	const bool ok = XrOk(result, "xrEndFrame");
	if(gVrLoggedFrames < 10 || (count > 0 && gVrLoggedRenderableFrames < 10)){
		VrLog("End frame %d layers=%u ok=%d\n", gVrLoggedFrames, count, ok ? 1 : 0);
		if(count > 0)
			gVrLoggedRenderableFrames++;
	}
	gVrLoggedFrames++;
	gFrameBegun = false;
	if(ok && count > 0) gWasSubmitting = true;
	return ok;
}

void RestoreCamera(RwCamera *camera)
{
	if(!gFramePrepared)
		return;
	RwCameraSetRaster(camera, gOriginalColor);
	RwCameraSetZRaster(camera, gOriginalDepth);
	RwCameraSetViewWindow(camera, &gOriginalViewWindow);
	RwCameraSetViewOffset(camera, &gOriginalViewOffset);
	RwCameraSetNearClipPlane(camera, gOriginalNearPlane);
	CDraw::SetNearClipZ(gOriginalDrawNear);
	RsGlobal.width = gOriginalScreenWidth;
	RsGlobal.height = gOriginalScreenHeight;
	RwFrame *frame = RwCameraGetFrame(camera);
	*RwFrameGetMatrix(frame) = gOriginalFrameMatrix;
	RwMatrixUpdate(RwFrameGetMatrix(frame));
	RwFrameUpdateObjects(frame);
	RwFrameOrthoNormalize(frame);
	gFramePrepared = false;
}

XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v)
{
	const XrVector3f t = { 2.0f*(q.y*v.z-q.z*v.y), 2.0f*(q.z*v.x-q.x*v.z),
		2.0f*(q.x*v.y-q.y*v.x) };
	const XrVector3f cross = { q.y*t.z-q.z*t.y, q.z*t.x-q.x*t.z, q.x*t.y-q.y*t.x };
	return { v.x+q.w*t.x+cross.x, v.y+q.w*t.y+cross.y, v.z+q.w*t.z+cross.z };
}

void ApplyCinemaQuadPose(XrCompositionLayerQuad *layer)
{
	if(!layer)
		return;
	if(!gCinemaAnchorValid && gViewSpace && gLocalSpace){
		XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
		if(XrOk(xrLocateSpace(gViewSpace, gLocalSpace,
		   gFrameState.predictedDisplayTime, &location), "xrLocateSpace(cinema)") &&
		   (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
		   (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0){
			const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
			const XrVector3f headForward = Rotate(
				location.pose.orientation, localForward);
			const float yaw = atan2f(-headForward.x, -headForward.z);
			gCinemaAnchorPose = {};
			gCinemaAnchorPose.orientation.y = sinf(yaw*0.5f);
			gCinemaAnchorPose.orientation.w = cosf(yaw*0.5f);
			const XrVector3f localScreenOffset = { 0.0f, 0.0f, -2.0f };
			const XrVector3f screenOffset = Rotate(
				gCinemaAnchorPose.orientation, localScreenOffset);
			gCinemaAnchorPose.position.x = location.pose.position.x+screenOffset.x;
			gCinemaAnchorPose.position.y = location.pose.position.y;
			gCinemaAnchorPose.position.z = location.pose.position.z+screenOffset.z;
			gCinemaAnchorValid = true;
		}
	}
	if(gCinemaAnchorValid){
		layer->space = gLocalSpace;
		layer->pose = gCinemaAnchorPose;
		return;
	}
	// Tracking can be unavailable for the very first submitted frame. Keep that
	// single frame visible and retry the world-locked anchor on the next one.
	layer->space = gViewSpace;
	layer->pose = {};
	layer->pose.orientation.w = 1.0f;
	layer->pose.position.z = -2.0f;
}

CVector ToGameVector(const XrVector3f &v)
{
	return gBaseCamera.GetRight()*(-v.x) + gBaseCamera.GetUp()*v.y + gBaseCamera.GetForward()*(-v.z);
}

CVector RotateAroundAxis(const CVector &vector, const CVector &axis, float angle)
{
	const float cosine = cosf(angle);
	const float sine = sinf(angle);
	return vector*cosine + CrossProduct(axis, vector)*sine +
		axis*(DotProduct(axis, vector)*(1.0f-cosine));
}

bool IsBikeSidearmTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_PYTHON:
	case WEAPONTYPE_TEC9:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SILENCED_INGRAM:
		return true;
	default:
		return false;
	}
}

void RestrictImmersiveVehicleWeaponsToSidearms(CPlayerPed *player)
{
	if(!player)
		return;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const int slot = gHeldWeaponSlot[hand];
		const int weaponType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if(slot >= 0 && !IsBikeSidearmTypeInternal(weaponType)){
			ClearWeaponSupportForHand(hand);
			gHeldWeaponSlot[hand] = -1;
			gWeaponHolsterSelection[hand] = -1;
			gTrackedWeaponRenderMatrixSlot[hand] = -1;
			ClearDroppedWeapon(hand);
		}
	}
	uint32 sidearmMask = 0;
	for(int slot = WEAPONSLOT_HANDGUN; slot <= WEAPONSLOT_SUBMACHINEGUN; slot++){
		if(!player->HasWeaponSlot(slot))
			continue;
		const int weaponType = GetVrWeaponTypeForSlot(slot);
		if(IsBikeSidearmTypeInternal(weaponType))
			sidearmMask |= 1u << slot;
	}
	for(int hand = 0; hand < EYE_COUNT; hand++)
		if(gHeldWeaponSlot[hand] >= 0)
			sidearmMask &= ~(1u << gHeldWeaponSlot[hand]);
	gWeaponHolsterMask = sidearmMask;
}

void RotateBikeHandleCalibration(CMatrix *matrix,
	const BikeHandleCalibration &calibration)
{
	if(!matrix)
		return;
	const float pitch = DEGTORAD((float)calibration.rotationX/
		WEAPON_CALIBRATION_VALUE_SCALE);
	const float yaw = DEGTORAD((float)calibration.rotationY/
		WEAPON_CALIBRATION_VALUE_SCALE);
	const float roll = DEGTORAD((float)calibration.rotationZ/
		WEAPON_CALIBRATION_VALUE_SCALE);
	if(pitch != 0.0f){
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			matrix->GetRight(), pitch);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			matrix->GetRight(), pitch);
	}
	if(yaw != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			matrix->GetUp(), yaw);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			matrix->GetUp(), yaw);
	}
	if(roll != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			matrix->GetForward(), roll);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			matrix->GetForward(), roll);
	}
	matrix->GetRight().Normalise();
	matrix->GetForward().Normalise();
	matrix->GetUp().Normalise();
}

bool BuildBikeHandleMatrixInternal(int hand, CMatrix *matrix,
	bool applySteering)
{
	if(!matrix || hand < 0 || hand >= EYE_COUNT ||
	   !IsImmersiveBikeDrivingActiveInternal())
		return false;
	CBike *bike = GetActivePlayerBike();
	BikeHandleCalibration *calibration =
		GetBikeHandleCalibration(bike->GetModelIndex(), hand);
	if(!calibration)
		return false;
	CVector bikeRight = bike->GetRight();
	CVector bikeForward = bike->GetForward();
	CVector bikeUp = bike->GetUp();
	bikeRight.Normalise();
	bikeForward.Normalise();
	bikeUp.Normalise();
	CVector center = bike->GetPosition()+bikeForward*0.30f+bikeUp*0.82f;
	if(bike->m_aBikeNodes[BIKE_HANDLEBARS]){
		RwMatrix *handleLtm =
			RwFrameGetLTM(bike->m_aBikeNodes[BIKE_HANDLEBARS]);
		if(handleLtm)
			center = CVector(handleLtm->pos);
	}
	matrix->SetUnity();
	// RenderVrTrackedHand derives its palm normal from Matrix::Right and its
	// pointing direction from the aim ray. This mirrored controller basis keeps
	// both palms naturally resting on the bar before per-model calibration.
	matrix->GetRight() = bikeUp*(hand == 0 ? -1.0f : 1.0f);
	matrix->GetForward() = bikeForward;
	matrix->GetUp() = CrossProduct(matrix->GetRight(),
		matrix->GetForward());
	matrix->GetUp().Normalise();
	matrix->GetPosition() = center +
		bikeRight*((float)calibration->offsetX/200.0f) +
		bikeForward*((float)calibration->offsetY/200.0f) +
		bikeUp*((float)calibration->offsetZ/200.0f);
	RotateBikeHandleCalibration(matrix, *calibration);
	if(applySteering && bike->m_fWheelAngle != 0.0f){
		// Follow the angle that the game actually applied to the front fork.
		// Using the raw controller input here made the hands and the rendered
		// handlebar diverge because bike steering has its own response curve.
		const float steeringAngle = bike->m_fWheelAngle;
		matrix->GetPosition() = center+RotateAroundAxis(
			matrix->GetPosition()-center, bikeUp, steeringAngle);
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			bikeUp, steeringAngle);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			bikeUp, steeringAngle);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			bikeUp, steeringAngle);
	}
	return true;
}

bool BuildCarWheelCenter(CAutomobile *car, CVector *center,
	CVector *axis)
{
	if(!car || !center || !axis)
		return false;
	CVehicleModelInfo *model =
		(CVehicleModelInfo*)CModelInfo::GetModelInfo(car->GetModelIndex());
	if(!model)
		return false;
	CVector local = model->GetFrontSeatPosn();
	// The authored front-seat point is the passenger side. Mirroring X yields
	// the driver's shoulder line; the small forward/up offsets put the neutral
	// calibration origin at the steering wheel instead of inside Tommy's chest.
	local.x = -local.x;
	local.y += 0.33f;
	local.z += 0.30f;
	*center = car->GetPosition()+Multiply3x3(car->GetMatrix(), local);
	*axis = car->GetForward();
	if(axis->MagnitudeSqr() < 0.0001f)
		return false;
	axis->Normalise();
	return true;
}

bool BuildCarWheelMatrixInternal(int hand, CMatrix *matrix,
	bool applySteering)
{
	if(!matrix || hand < 0 || hand >= EYE_COUNT ||
	   !IsVrCarDrivingActiveInternal())
		return false;
	CAutomobile *car = GetActivePlayerCar();
	BikeHandleCalibration *calibration =
		GetCarWheelCalibration(car->GetModelIndex(), hand);
	if(!calibration)
		return false;
	CVector center, axis;
	if(!BuildCarWheelCenter(car, &center, &axis))
		return false;
	CVector carRight = car->GetRight();
	CVector carForward = car->GetForward();
	CVector carUp = car->GetUp();
	carRight.Normalise();
	carForward.Normalise();
	carUp.Normalise();
	matrix->SetUnity();
	// This is the same palm-facing convention as motorcycle grips, except both
	// sockets lie in the steering-wheel plane (car right/up, normal forward).
	matrix->GetRight() = carUp*(hand == 0 ? -1.0f : 1.0f);
	matrix->GetForward() = carForward;
	matrix->GetUp() = CrossProduct(matrix->GetRight(),
		matrix->GetForward());
	matrix->GetUp().Normalise();
	matrix->GetPosition() = center +
		carRight*((float)calibration->offsetX/200.0f) +
		carForward*((float)calibration->offsetY/200.0f) +
		carUp*((float)calibration->offsetZ/200.0f);
	RotateBikeHandleCalibration(matrix, *calibration);
	const float physicalAngle = IsImmersiveCarDrivingActiveInternal() ?
		gImmersiveCarPhysicalAngle : gMotionVehiclePhysicalAngle;
	if(applySteering && physicalAngle != 0.0f){
		// PlanarCarWheelAngle and Rodrigues rotation use opposite signs in
		// Vice City's car basis. This sign is visual only: vehicle steering is
		// passed through independently in GetImmersiveCarSteering.
		const float angle = -physicalAngle;
		matrix->GetPosition() = center+RotateAroundAxis(
			matrix->GetPosition()-center, axis, angle);
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			axis, angle);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			axis, angle);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			axis, angle);
	}
	return true;
}

bool ProjectCarWheelVector(CVector vector, const CVector &axis,
	CVector *projected)
{
	if(!projected)
		return false;
	vector -= axis*DotProduct(vector, axis);
	if(vector.MagnitudeSqr() < 0.0001f)
		return false;
	vector.Normalise();
	*projected = vector;
	return true;
}

float SignedCarWheelAngle(const CVector &neutral,
	const CVector &actual, const CVector &axis)
{
	CVector projectedNeutral, projectedActual;
	if(!ProjectCarWheelVector(neutral, axis, &projectedNeutral) ||
	   !ProjectCarWheelVector(actual, axis, &projectedActual))
		return 0.0f;
	return atan2f(DotProduct(axis,
		CrossProduct(projectedNeutral, projectedActual)),
		clamp(DotProduct(projectedNeutral, projectedActual),
			-1.0f, 1.0f));
}

float WrapCarWheelAngle(float angle)
{
	const float pi = 3.14159265358979323846f;
	while(angle > pi) angle -= 2.0f*pi;
	while(angle < -pi) angle += 2.0f*pi;
	return angle;
}

float UnwrapCarWheelAngle(float angle, float reference)
{
	const float pi = 3.14159265358979323846f;
	while(angle-reference > pi) angle -= 2.0f*pi;
	while(angle-reference < -pi) angle += 2.0f*pi;
	return angle;
}

float PlanarCarWheelAngle(const CVector &vector,
	const CVector &right, const CVector &up)
{
	return atan2f(DotProduct(vector, up), DotProduct(vector, right));
}

bool GetTrackedHornHandPosition(int hand, CVector *position)
{
	if(!position || hand < 0 || hand >= EYE_COUNT ||
	   !gTrackedHandPoseValid[hand])
		return false;
	*position = gBaseCamera.GetPosition()+
		ToGameVector(gTrackedHandPose[hand].position);
	return true;
}

void UpdateImmersiveCarHorn(CAutomobile *car,
	const CVector &center, const CVector &axis, uint32 blockedHands)
{
	(void)axis;
	if(!car){
		gImmersiveCarHornPressed = false;
		for(int hand = 0; hand < EYE_COUNT; hand++){
			gCarHornContact[hand] = false;
			gCarHornArmed[hand] = false;
			gCarHornPreviousDistance[hand] = 1000.0f;
		}
		return;
	}
	bool pressed = false;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			gCarWheelGrabbed[hand] ||
			gHeldWeaponSlot[hand] >= 0 ||
			IsWeaponSupportHandInternal(hand) ||
			IsHandBusyWithReload(hand);
		CVector handPosition;
		if(unavailable || !GetTrackedHornHandPosition(hand, &handPosition)){
			gCarHornContact[hand] = false;
			gCarHornArmed[hand] = false;
			gCarHornPreviousDistance[hand] = 1000.0f;
			continue;
		}
		const float distance = (handPosition-center).Magnitude();
		const float previousDistance = gCarHornPreviousDistance[hand];
		const float enterDistance = 0.105f;
		const float leaveDistance = 0.15f;
		const float armNearDistance = 0.12f;
		const float armFarDistance = 0.26f;
		if(gCarHornContact[hand]){
			if(distance > leaveDistance)
				gCarHornContact[hand] = false;
		}else{
			// Pointing at the wheel must never sound the horn. Arm it only while
			// the tracked hand is moving toward the centre, then require an
			// actual inward crossing of the small contact sphere.
			if(distance >= armNearDistance &&
			   distance <= armFarDistance &&
			   previousDistance < 999.0f &&
			   distance < previousDistance-0.002f)
				gCarHornArmed[hand] = true;
			if(distance > armFarDistance)
				gCarHornArmed[hand] = false;
			if(gCarHornArmed[hand] &&
			   distance <= enterDistance &&
			   previousDistance < 999.0f &&
			   distance < previousDistance){
				gCarHornContact[hand] = true;
				gCarHornArmed[hand] = false;
			}
		}
		gCarHornPreviousDistance[hand] = distance;
		pressed = pressed || gCarHornContact[hand];
	}
	gImmersiveCarHornPressed = pressed;
}

uint32 UpdateImmersiveCarInput(const float *grips, uint32 blockedHands)
{
	if(!grips || !IsImmersiveCarDrivingActiveInternal()){
		ResetImmersiveCarInteraction();
		return 0;
	}
	CAutomobile *car = GetActivePlayerCar();
	CPlayerPed *player = FindPlayerPed();
	if(!car || !player){
		ResetImmersiveCarInteraction();
		return 0;
	}
	RestrictImmersiveVehicleWeaponsToSidearms(player);
	CMatrix anchors[EYE_COUNT];
	CVector handPositions[EYE_COUNT];
	bool anchorValid[EYE_COUNT] = {};
	bool justGrabbed[EYE_COUNT] = {};
	for(int hand = 0; hand < EYE_COUNT; hand++){
		anchorValid[hand] =
			BuildCarWheelMatrixInternal(hand, &anchors[hand], false);
		if(gTrackedHandPoseValid[hand])
			handPositions[hand] = gBaseCamera.GetPosition()+
				ToGameVector(gTrackedHandPose[hand].position);
		else
			handPositions[hand] = CVector(0.0f, 0.0f, 0.0f);
		gCarWheelDistance[hand] =
			anchorValid[hand] && gTrackedHandPoseValid[hand] ?
			(handPositions[hand]-anchors[hand].GetPosition()).Magnitude() :
			1000.0f;
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			!anchorValid[hand] || !gTrackedHandPoseValid[hand] ||
			gHeldWeaponSlot[hand] >= 0 ||
			IsWeaponSupportHandInternal(hand) ||
			IsHandBusyWithReload(hand);
		if(gCarWheelGrabbed[hand] &&
		   (unavailable || grips[hand] <= 0.30f)){
			gCarWheelGrabbed[hand] = false;
			gCarWheelAngleValid[hand] = false;
		}
		if(!gCarWheelGrabbed[hand] && !unavailable &&
		   grips[hand] >= 0.65f && !gCarWheelGripDown[hand] &&
		   gCarWheelDistance[hand] <= 0.23f){
			gCarWheelGrabbed[hand] = true;
			justGrabbed[hand] = true;
			debug("[OpenXR] %s car wheel grip grabbed on %s\n",
				hand == 0 ? "Left" : "Right",
				GetActiveVrVehicleName());
		}
		if(grips[hand] <= 0.30f)
			gCarWheelGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gCarWheelGripDown[hand] = true;
	}

	const bool left = gCarWheelGrabbed[0] && anchorValid[0];
	const bool right = gCarWheelGrabbed[1] && anchorValid[1];
	CVector center, axis;
	if(!BuildCarWheelCenter(car, &center, &axis)){
		ResetImmersiveCarInteraction();
		return 0;
	}
	// Use the midpoint of the actual calibrated sockets as the logical wheel
	// centre. This keeps one-handed steering correct even for asymmetric cabin
	// geometry or intentionally asymmetric user calibration.
	if(anchorValid[0] && anchorValid[1])
		center = (anchors[0].GetPosition()+
			anchors[1].GetPosition())*0.5f;
	CVector carRight = car->GetRight();
	CVector carUp = car->GetUp();
	carRight.Normalise();
	carUp.Normalise();
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(!justGrabbed[hand])
			continue;
		const float currentHandAngle = PlanarCarWheelAngle(
			handPositions[hand]-center, carRight, carUp);
		// A second hand may join an already turned wheel. Capture it relative to
		// the current virtual angle so adding that hand never snaps the steering.
		gCarWheelGrabReferenceAngle[hand] = WrapCarWheelAngle(
			currentHandAngle-gImmersiveCarPhysicalAngle);
		gCarWheelContinuousAngle[hand] =
			gImmersiveCarPhysicalAngle;
		gCarWheelAngleValid[hand] = true;
	}
	float steeringAngle = 0.0f;
	if(left && right){
		// With both hands on the wheel, their labelled left-to-right chord is the
		// only stable steering reference. Averaging two independent polar angles
		// allowed one controller to wrap first near a strong turn and visually
		// exchange the hands even though the car kept steering correctly.
		const float chordAngle = PlanarCarWheelAngle(
			handPositions[1]-handPositions[0], carRight, carUp);
		if(!gCarWheelTwoHandAngleValid ||
		   justGrabbed[0] || justGrabbed[1]){
			gCarWheelTwoHandReferenceAngle = WrapCarWheelAngle(
				chordAngle-gImmersiveCarPhysicalAngle);
			gCarWheelTwoHandContinuousAngle =
				gImmersiveCarPhysicalAngle;
			gCarWheelTwoHandAngleValid = true;
		}
		steeringAngle = WrapCarWheelAngle(
			chordAngle-gCarWheelTwoHandReferenceAngle);
		steeringAngle = UnwrapCarWheelAngle(steeringAngle,
			gCarWheelTwoHandContinuousAngle);
		gCarWheelTwoHandContinuousAngle = steeringAngle;
		// Re-anchor both one-hand paths continuously. Releasing either grip at
		// full lock therefore cannot produce a one-frame snap.
		for(int hand = 0; hand < EYE_COUNT; hand++){
			const float currentHandAngle = PlanarCarWheelAngle(
				handPositions[hand]-center, carRight, carUp);
			gCarWheelGrabReferenceAngle[hand] = WrapCarWheelAngle(
				currentHandAngle-steeringAngle);
			gCarWheelContinuousAngle[hand] = steeringAngle;
			gCarWheelAngleValid[hand] = true;
		}
	}else{
		gCarWheelTwoHandAngleValid = false;
		for(int hand = 0; hand < EYE_COUNT; hand++){
			if(!gCarWheelGrabbed[hand])
				continue;
			float handAngle = WrapCarWheelAngle(
				PlanarCarWheelAngle(handPositions[hand]-center,
					carRight, carUp)-
				gCarWheelGrabReferenceAngle[hand]);
			if(gCarWheelAngleValid[hand])
				handAngle = UnwrapCarWheelAngle(handAngle,
					gCarWheelContinuousAngle[hand]);
			gCarWheelContinuousAngle[hand] = handAngle;
			gCarWheelAngleValid[hand] = true;
			steeringAngle = handAngle;
			break;
		}
	}
	// Reach full vehicle lock before fixed 9-and-3 grips can cross sides. This
	// keeps hand identity readable without reducing the car's steering range.
	const float maxSteering = DEGTORAD(80.0f);
	gImmersiveCarPhysicalAngle =
		clamp(steeringAngle, -maxSteering, maxSteering);
	float steering =
		clamp(steeringAngle/maxSteering, -1.0f, 1.0f);
	const float deadZone = 0.03f;
	if(Abs(steering) <= deadZone)
		steering = 0.0f;
	else
		steering = (steering > 0.0f ? 1.0f : -1.0f)*
			(Abs(steering)-deadZone)/(1.0f-deadZone);
	// Hand/controller ownership and the rendered wheel orientation are already
	// correct here. Pass the physical wheel direction through unchanged; swapping
	// hands or sockets to compensate would make the grips impossible to reach.
	gImmersiveCarSteering = steering;
	const uint32 capturedHands =
		(left ? 1u : 0u) | (right ? 2u : 0u);
	UpdateImmersiveCarHorn(car, center, axis,
		blockedHands | capturedHands);
	return capturedHands;
}

bool GetMotionSteeringHeading(float *heading)
{
	const int hand = Min(Max(gMotionSteeringHand, 0), EYE_COUNT-1);
	if(!heading || !gTrackedHandAimPoseValid[hand])
		return false;
	// OpenXR is Y-up. Project the selected controller's aim vector onto the
	// horizontal X/Z plane, which is the direct equivalent of the Unreal
	// Z-axis yaw used by common UEVR motion-steering scripts.
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f forward =
		Rotate(gTrackedHandAimPose[hand].orientation, localForward);
	const float horizontalLengthSqr =
		forward.x*forward.x+forward.z*forward.z;
	if(horizontalLengthSqr < 0.01f)
		return false;
	*heading = atan2f(forward.x, -forward.z);
	return true;
}

void UpdateMotionDrivingInput()
{
	if(!IsMotionDrivingEnvironmentActive() ||
	   gVrMenuVisible || gCheatMenuVisible){
		ResetMotionSteeringInteraction();
		return;
	}
	CVehicle *vehicle = FindPlayerVehicle();
	CPlayerPed *player = FindPlayerPed();
	if(!vehicle || !player ||
	   (!IsVrCarDrivingActiveInternal(vehicle) &&
	    !IsVrBikeDrivingActiveInternal(vehicle))){
		ResetMotionSteeringInteraction();
		return;
	}
	RestrictImmersiveVehicleWeaponsToSidearms(player);
	if(gMotionSteeringVehicle != vehicle){
		gMotionSteeringVehicle = vehicle;
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		gMotionSteeringReferenceValid = false;
		gMotionSteeringReferenceHeading = 0.0f;
	}
	float heading;
	if(!GetMotionSteeringHeading(&heading)){
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		return;
	}

	// The driver's real controller pose at the first accelerator press is the
	// steering centre. This avoids assuming that an OpenXR controller has one
	// universal "forward" pose: the physical grip angle differs between users
	// and can also change while entering a vehicle. Keep this centre for the
	// whole ride so releasing and pressing the accelerator in a turn cannot
	// recenter the wheel unexpectedly.
	const bool acceleratorPressed = gTrackedHandTrigger[1] >= 0.15f;
	if(!gMotionSteeringReferenceValid){
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		if(!acceleratorPressed)
			return;
		gMotionSteeringReferenceHeading = heading;
		gMotionSteeringReferenceValid = true;
		debug("[OpenXR] Motion steering centred on R2 at %.2f degrees\n",
			RADTODEG(gMotionSteeringReferenceHeading));
	}

	// Turning the controller clockwise/right produces negative GTA steering,
	// matching the already validated physical-wheel convention.
	const float maxAngle = DEGTORAD(90.0f);
	const float midAngle = DEGTORAD(30.0f);
	const float fineScale = 0.5f;
	float angle = clamp(-WrapCarWheelAngle(
		heading-gMotionSteeringReferenceHeading), -maxAngle, maxAngle);
	if(Abs(angle) < DEGTORAD(3.0f))
		angle = 0.0f;
	gMotionVehiclePhysicalAngle = angle;
	const float absAngle = Abs(angle);
	float steering;
	if(absAngle <= midAngle)
		steering = angle/midAngle*fineScale;
	else{
		const float sign = angle >= 0.0f ? 1.0f : -1.0f;
		steering = sign*(fineScale+
			(absAngle-midAngle)/(maxAngle-midAngle)*(1.0f-fineScale));
	}
	gMotionVehicleSteering =
		Abs(steering) < 0.01f ? 0.0f :
		clamp(steering, -1.0f, 1.0f);
}

float WrapBikeSteeringAngle(float angle)
{
	const float pi = 3.14159265358979323846f;
	while(angle > pi) angle -= 2.0f*pi;
	while(angle < -pi) angle += 2.0f*pi;
	return angle;
}

float PlanarBikeHandleAngle(const CVector &vector, const CVector &right,
	const CVector &forward)
{
	return atan2f(DotProduct(vector, forward), DotProduct(vector, right));
}

bool GetBikeThrottleOrientation(CVector *orientation, const CVector &axis)
{
	if(!orientation || !gTrackedHandPoseValid[1])
		return false;
	const XrPosef &pose = gTrackedHandAimPoseValid[1] ?
		gTrackedHandAimPose[1] : gTrackedHandPose[1];
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	*orientation = ToGameVector(Rotate(pose.orientation, localForward));
	*orientation -= axis*DotProduct(*orientation, axis);
	if(orientation->MagnitudeSqr() < 0.01f){
		const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
		*orientation = ToGameVector(Rotate(pose.orientation, localUp));
		*orientation -= axis*DotProduct(*orientation, axis);
	}
	if(orientation->MagnitudeSqr() < 0.01f)
		return false;
	orientation->Normalise();
	return true;
}

void UpdateImmersiveBikeThrottle(CBike *bike, bool rightHandleGrabbed)
{
	const bool triggerHeld = gTrackedHandTrigger[1] >= 0.45f;
	if(!bike || !rightHandleGrabbed || !triggerHeld){
		gImmersiveBikeThrottle = 0.0f;
		gBikeThrottleReferenceValid = false;
		return;
	}

	CVector throttleAxis = bike->GetRight();
	CVector bikeUp = bike->GetUp();
	throttleAxis = RotateAroundAxis(throttleAxis, bikeUp,
		bike->m_fWheelAngle);
	throttleAxis.Normalise();
	CVector current;
	if(!GetBikeThrottleOrientation(&current, throttleAxis)){
		gImmersiveBikeThrottle = 0.0f;
		gBikeThrottleReferenceValid = false;
		return;
	}
	if(!gBikeThrottleReferenceValid){
		gBikeThrottleReference = current;
		gBikeThrottleReferenceValid = true;
		gImmersiveBikeThrottle = 0.0f;
		return;
	}

	const float twist = atan2f(DotProduct(throttleAxis,
		CrossProduct(gBikeThrottleReference, current)),
		clamp(DotProduct(gBikeThrottleReference, current), -1.0f, 1.0f));
	// Ignore the first two degrees so a resting wrist does not make the bike
	// creep, and reach full throttle after a natural 45-degree twist.
	const float throttleAngle = Max(twist-DEGTORAD(2.0f), 0.0f);
	gImmersiveBikeThrottle = clamp(
		throttleAngle/DEGTORAD(43.0f), 0.0f, 1.0f);
}

void UpdateImmersiveBikeLean(CBike *bike, const CMatrix *anchors,
	const CVector *handPositions, bool left, bool right,
	float physicalSteeringAngle)
{
	if(!bike || !anchors || !handPositions || (!left && !right)){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		for(int hand = 0; hand < EYE_COUNT; hand++)
			gBikeLeanReferenceValid[hand] = false;
		return;
	}

	CVector bikeUp = bike->GetUp();
	bikeUp.Normalise();
	const CVector center =
		(anchors[0].GetPosition()+anchors[1].GetPosition())*0.5f;
	float heightTotal = 0.0f;
	int heightHands = 0;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const bool grabbed = hand == 0 ? left : right;
		if(!grabbed){
			gBikeLeanReferenceValid[hand] = false;
			continue;
		}
		const CVector expected = center+RotateAroundAxis(
			anchors[hand].GetPosition()-center, bikeUp,
			physicalSteeringAngle);
		const CVector displacement = handPositions[hand]-expected;
		const float currentUp = DotProduct(displacement, bikeUp);
		if(!gBikeLeanReferenceValid[hand]){
			gBikeLeanReferenceUp[hand] = currentUp;
			gBikeLeanReferenceValid[hand] = true;
			continue;
		}
		heightTotal += currentUp-gBikeLeanReferenceUp[hand];
		heightHands++;
	}
	if(heightHands == 0){
		gImmersiveBikeLean = 0.0f;
		return;
	}

	BikeLeanCalibration *calibration =
		GetBikeLeanCalibration(bike->GetModelIndex());
	if(!calibration){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		return;
	}
	const float handHeight = heightTotal/(float)heightHands;
	const float wheelieHeight =
		(float)calibration->wheelieHeightCm/100.0f;
	const float standDepth =
		(float)calibration->standHeightCm/100.0f;

	// Each bike has explicit vertical trigger distances. Raising the hands from
	// their captured neutral height requests a wheelie; lowering them requests
	// the forward standing/stoppie pose. Hysteresis keeps the state stable near
	// the configured boundary while CBike performs its normal smooth blending.
	if(gBikeLeanGestureState == 0){
		if(handHeight >= wheelieHeight)
			gBikeLeanGestureState = -1;
		else if(handHeight <= -standDepth)
			gBikeLeanGestureState = 1;
	}else if(gBikeLeanGestureState < 0){
		if(handHeight <= wheelieHeight*0.70f)
			gBikeLeanGestureState = 0;
	}else{
		if(handHeight >= -standDepth*0.70f)
			gBikeLeanGestureState = 0;
	}
	gImmersiveBikeLean = (float)gBikeLeanGestureState;
}

uint32 UpdateImmersiveBikeInput(const float *grips, uint32 blockedHands)
{
	if(!grips || !IsImmersiveBikeDrivingActiveInternal()){
		ResetImmersiveBikeInteraction();
		return 0;
	}
	CBike *bike = GetActivePlayerBike();
	CPlayerPed *player = FindPlayerPed();
	if(!bike || !player){
		ResetImmersiveBikeInteraction();
		return 0;
	}

	// Only compact one-handed firearms remain available while physically
	// driving. A long gun brought into the seat returns to its body slot.
	RestrictImmersiveVehicleWeaponsToSidearms(player);

	CMatrix anchors[EYE_COUNT];
	CVector handPositions[EYE_COUNT];
	bool anchorValid[EYE_COUNT] = {};
	for(int hand = 0; hand < EYE_COUNT; hand++){
		anchorValid[hand] =
			BuildBikeHandleMatrixInternal(hand, &anchors[hand], false);
		if(gTrackedHandPoseValid[hand])
			handPositions[hand] = gBaseCamera.GetPosition()+
				ToGameVector(gTrackedHandPose[hand].position);
		else
			handPositions[hand] = CVector(0.0f, 0.0f, 0.0f);
		gBikeHandleDistance[hand] =
			anchorValid[hand] && gTrackedHandPoseValid[hand] ?
			(handPositions[hand]-anchors[hand].GetPosition()).Magnitude() :
			1000.0f;
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			!anchorValid[hand] || !gTrackedHandPoseValid[hand] ||
			gHeldWeaponSlot[hand] >= 0 ||
			IsWeaponSupportHandInternal(hand) ||
			IsHandBusyWithReload(hand);
		if(gBikeHandleGrabbed[hand] &&
		   (unavailable || grips[hand] <= 0.30f)){
			gBikeHandleGrabbed[hand] = false;
			gBikeLeanReferenceValid[hand] = false;
		}
		if(!gBikeHandleGrabbed[hand] && !unavailable &&
		   grips[hand] >= 0.65f && !gBikeHandleGripDown[hand] &&
		   gBikeHandleDistance[hand] <= 0.17f){
			gBikeHandleGrabbed[hand] = true;
			gBikeLeanReferenceValid[hand] = false;
			debug("[OpenXR] %s motorcycle handle grabbed on %s\n",
				hand == 0 ? "Left" : "Right", GetActiveVrBikeName());
		}
		if(grips[hand] <= 0.30f)
			gBikeHandleGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gBikeHandleGripDown[hand] = true;
	}

	const bool left = gBikeHandleGrabbed[0] && anchorValid[0];
	const bool right = gBikeHandleGrabbed[1] && anchorValid[1];
	float steeringAngle = 0.0f;
	CVector bikeRight = bike->GetRight();
	CVector bikeForward = bike->GetForward();
	bikeRight.Normalise();
	bikeForward.Normalise();
	if(left && right){
		const CVector neutral =
			anchors[1].GetPosition()-anchors[0].GetPosition();
		const CVector actual = handPositions[1]-handPositions[0];
		steeringAngle = WrapBikeSteeringAngle(
			PlanarBikeHandleAngle(actual, bikeRight, bikeForward)-
			PlanarBikeHandleAngle(neutral, bikeRight, bikeForward));
	}else if(left || right){
		const int hand = left ? 0 : 1;
		const CVector center =
			(anchors[0].GetPosition()+anchors[1].GetPosition())*0.5f;
		const CVector neutral = anchors[hand].GetPosition()-center;
		const CVector actual = handPositions[hand]-center;
		steeringAngle = WrapBikeSteeringAngle(
			PlanarBikeHandleAngle(actual, bikeRight, bikeForward)-
			PlanarBikeHandleAngle(neutral, bikeRight, bikeForward));
	}
	const float maxSteering = DEGTORAD(35.0f);
	float steering = clamp(steeringAngle/maxSteering, -1.0f, 1.0f);
	const float deadZone = 0.035f;
	if(Abs(steering) <= deadZone)
		steering = 0.0f;
	else
		steering = (steering > 0.0f ? 1.0f : -1.0f)*
			(Abs(steering)-deadZone)/(1.0f-deadZone);
	gImmersiveBikeSteering = steering;
	UpdateImmersiveBikeLean(bike, anchors, handPositions, left, right,
		clamp(steeringAngle, -maxSteering, maxSteering));
	UpdateImmersiveBikeThrottle(bike, right);
	return (left ? 1u : 0u) | (right ? 2u : 0u);
}

bool IsPhysicalScopeWeaponTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_LASERSCOPE:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_CAMERA:
		return true;
	default:
		return false;
	}
}

float GetTrackedScopeZoomInternal()
{
	switch(gTrackedScopeWeaponType){
	case WEAPONTYPE_SNIPERRIFLE: return 2.5f;
	case WEAPONTYPE_LASERSCOPE: return 3.0f;
	case WEAPONTYPE_ROCKETLAUNCHER: return 1.8f;
	case WEAPONTYPE_CAMERA: return 1.6f;
	default: return 1.0f;
	}
}

void ApplyTrackedScopeZoomToUv(float *scaleX, float *scaleY,
	float *offsetX, float *offsetY)
{
	if(!scaleX || !scaleY || !offsetX || !offsetY)
		return;
	const float zoom = GetTrackedScopeZoomInternal();
	if(zoom <= 1.0f)
		return;
	// Crop around the stable centre of the symmetric source render.  This keeps
	// the OpenXR eye poses/FOV untouched, so activating an optic cannot reintroduce
	// the old stereo warp or make RenderWare cull a zoomed eye differently.
	*scaleX /= zoom;
	*scaleY /= zoom;
	*offsetX = 0.5f+(*offsetX-0.5f)/zoom;
	*offsetY = 0.5f+(*offsetY-0.5f)/zoom;
}

bool IsPhysicalGunTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_PYTHON:
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN:
	case WEAPONTYPE_STUBBY_SHOTGUN:
	case WEAPONTYPE_TEC9:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SILENCED_INGRAM:
	case WEAPONTYPE_MP5:
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER:
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_LASERSCOPE:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_FLAMETHROWER:
	case WEAPONTYPE_M60:
	case WEAPONTYPE_MINIGUN:
	case WEAPONTYPE_CAMERA:
		return true;
	default:
		return false;
	}
}

bool IsPhysicalMeleeTypeInternal(int weaponType)
{
	// Vice City's authored melee block is contiguous. Unarmed and brass
	// knuckles live in slot zero and are driven directly by a closed fist; the
	// remaining types share the physical MELEE holster slot.
	return weaponType >= WEAPONTYPE_UNARMED &&
		weaponType <= WEAPONTYPE_CHAINSAW;
}

bool IsPhysicalThrowableTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_GRENADE:
	case WEAPONTYPE_DETONATOR_GRENADE:
	case WEAPONTYPE_TEARGAS:
	case WEAPONTYPE_MOLOTOV:
		return true;
	default:
		return false;
	}
}

CVector BuildTrackedThrowableVelocity(int weaponType, CVector direction)
{
	if(!IsPhysicalThrowableTypeInternal(weaponType) ||
	   direction.MagnitudeSqr() < 0.0001f)
		return CVector(0.0f, 0.0f, 0.0f);
	direction.Normalise();
	// Game physics stores velocity in world units per normalized 30 Hz tick.
	// A fixed launch speed makes the preview stable while R2 is held and lets the
	// player choose the landing point by moving the controller, rather than by
	// timing an invisible legacy animation charge.
	return direction*0.46f;
}

bool BuildTrackedThrowableLaunch(int weaponType, const CVector &requestedSource,
	CVector direction, CVector *source, CVector *velocity)
{
	if(!source || !velocity || !IsPhysicalThrowableTypeInternal(weaponType) ||
	   direction.MagnitudeSqr() < 0.0001f)
		return false;
	direction.Normalise();
	*source = requestedSource;
	*velocity = BuildTrackedThrowableVelocity(weaponType, direction);

	// A tracked controller can physically pass through a wall. Clamp the spawn
	// point to the visible side of any geometry between the player's head and the
	// requested hand/muzzle point; the preview and the real projectile both call
	// this helper, so they cannot disagree about that correction.
	CPlayerPed *player = FindPlayerPed();
	if(player){
		const CVector anchor = gBaseCamera.GetPosition();
		CVector reach = requestedSource-anchor;
		const float reachLength = reach.Magnitude();
		if(reachLength > 0.001f){
			reach /= reachLength;
			CColPoint point;
			CEntity *hitEntity = nil;
			CEntity *savedIgnoreEntity = CWorld::pIgnoreEntity;
			CWorld::pIgnoreEntity = player;
			const bool hit = CWorld::ProcessLineOfSight(anchor,
				requestedSource, point, hitEntity, true, true, true, true,
				true, false, false, false);
			CWorld::pIgnoreEntity = savedIgnoreEntity;
			if(hit){
				const float safeDistance = Max(0.0f,
					(point.point-anchor).Magnitude()-0.08f);
				*source = anchor+reach*safeDistance;
			}
		}
	}
	return velocity->MagnitudeSqr() > 0.0001f;
}

bool IsPhysicalWeaponTypeInternal(int weaponType)
{
	return IsPhysicalGunTypeInternal(weaponType) ||
		IsPhysicalMeleeTypeInternal(weaponType) ||
		IsPhysicalThrowableTypeInternal(weaponType);
}

bool IsManualReloadWeaponTypeInternal(int weaponType)
{
	// The first physical-reload pass covers detachable magazines which can be
	// manipulated with one free hand.  Python is deliberately excluded: a
	// revolver needs a separate cylinder/speed-loader interaction rather than a
	// rectangular magazine pretending to fit into its grip.
	switch(weaponType){
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_TEC9:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SILENCED_INGRAM:
		return true;
	default:
		return false;
	}
}

bool IsTwoHandedWeaponTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN:
	case WEAPONTYPE_STUBBY_SHOTGUN:
	case WEAPONTYPE_MP5:
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER:
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_LASERSCOPE:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_FLAMETHROWER:
	case WEAPONTYPE_M60:
	case WEAPONTYPE_MINIGUN:
		return true;
	default:
		return false;
	}
}

float GetOneHandAimSwayDegrees(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_MP5: return 1.25f;
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN: return 1.50f;
	case WEAPONTYPE_STUBBY_SHOTGUN: return 2.00f;
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER: return 1.75f;
	case WEAPONTYPE_SNIPERRIFLE: return 2.25f;
	case WEAPONTYPE_LASERSCOPE: return 2.00f;
	case WEAPONTYPE_ROCKETLAUNCHER: return 3.00f;
	case WEAPONTYPE_FLAMETHROWER: return 2.25f;
	case WEAPONTYPE_M60: return 2.50f;
	case WEAPONTYPE_MINIGUN: return 3.25f;
	default: return 0.0f;
	}
}

bool IsWeaponSupportHandInternal(int hand)
{
	for(int primary = 0; primary < EYE_COUNT; primary++)
		if(gWeaponSupportHand[primary] == hand)
			return true;
	return false;
}

void ClearWeaponSupportForHand(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gWeaponSupportHand[hand] = -1;
	for(int primary = 0; primary < EYE_COUNT; primary++)
		if(gWeaponSupportHand[primary] == hand)
			gWeaponSupportHand[primary] = -1;
}

bool BuildTrackedWeaponHandBasis(int hand, CVector *position,
	CVector *right, CVector *up, CVector *forward)
{
	if(hand < 0 || hand >= EYE_COUNT || !position || !right || !up ||
	   !forward || !gTrackedHandPoseValid[hand])
		return false;
	const XrPosef &gripPose = gTrackedHandPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const int modelHand = 1-hand;
	*up = ToGameVector(Rotate(gripPose.orientation, localRight))*
		(modelHand == 0 ? 1.0f : -1.0f);
	if(gTrackedHandAimPoseValid[hand])
		*forward = ToGameVector(Rotate(
			gTrackedHandAimPose[hand].orientation, localForward));
	else
		*forward = ToGameVector(Rotate(gripPose.orientation, localUp));
	forward->Normalise();
	*up -= *forward*DotProduct(*up, *forward);
	if(up->MagnitudeSqr() < 0.0001f)
		*up = ToGameVector(Rotate(gripPose.orientation, localRight))*
			(modelHand == 0 ? 1.0f : -1.0f);
	up->Normalise();
	*right = CrossProduct(*up, *forward);
	right->Normalise();
	const CVector gripForward =
		ToGameVector(Rotate(gripPose.orientation, localForward));
	if(DotProduct(*right, gripForward) < 0.0f)
		*right *= -1.0f;
	*position = gBaseCamera.GetPosition()+ToGameVector(gripPose.position);
	return true;
}

bool BuildSupportGripVector(int primaryHand, int weaponType,
	CVector *pivot, CVector *expected)
{
	CVector right, up, forward;
	if(!pivot || !expected || !IsTwoHandedWeaponTypeInternal(weaponType) ||
	   !BuildTrackedWeaponHandBasis(primaryHand, pivot, &right, &up, &forward))
		return false;
	const SupportGripCalibration *calibration =
		GetSupportGripCalibration(primaryHand, weaponType);
	if(!calibration)
		return false;
	*expected = right*((float)calibration->offsetX/200.0f) +
		forward*((float)calibration->offsetY/200.0f) +
		up*((float)calibration->offsetZ/200.0f);
	return expected->MagnitudeSqr() >= 0.0001f;
}

bool BuildTwoHandRotation(int primaryHand, int weaponType, CVector *pivot,
	CVector *axis, float *angle)
{
	if(!pivot || !axis || !angle || primaryHand < 0 ||
	   primaryHand >= EYE_COUNT)
		return false;
	const int supportHand = gWeaponSupportHand[primaryHand];
	if(supportHand < 0 || supportHand >= EYE_COUNT ||
	   !gTrackedHandPoseValid[supportHand])
		return false;
	CVector expected;
	if(!BuildSupportGripVector(primaryHand, weaponType, pivot, &expected))
		return false;
	const CVector supportPosition = gBaseCamera.GetPosition()+
		ToGameVector(gTrackedHandPose[supportHand].position);
	CVector actual = supportPosition-*pivot;
	if(actual.MagnitudeSqr() < 0.0001f)
		return false;
	expected.Normalise();
	actual.Normalise();
	const float dot = Min(Max(DotProduct(expected, actual), -1.0f), 1.0f);
	*axis = CrossProduct(expected, actual);
	if(axis->MagnitudeSqr() < 0.000001f){
		if(dot > 0.9999f){
			*axis = CVector(0.0f, 0.0f, 1.0f);
			*angle = 0.0f;
			return true;
		}
		CVector reference = fabsf(expected.z) < 0.9f ?
			CVector(0.0f, 0.0f, 1.0f) : CVector(0.0f, 1.0f, 0.0f);
		*axis = CrossProduct(expected, reference);
		axis->Normalise();
		*angle = 3.14159265358979323846f;
		return true;
	}
	axis->Normalise();
	*angle = acosf(dot);
	return true;
}

bool BuildWeaponHolsterMatrix(int slot, CMatrix *matrix)
{
	if(!matrix || slot <= WEAPONSLOT_UNARMED || slot >= TOTAL_WEAPON_SLOTS)
		return false;
	if(IsVrDrivingActiveInternal() &&
	   !IsBikeSidearmTypeInternal(GetVrWeaponTypeForSlot(slot)))
		return false;
	const int point = FindHolsterPointForSlot(slot);
	if(point < 0)
		return false;
	static const float lateral[HOLSTER_POINT_COUNT] = {
		-0.27f, 0.27f, -0.21f, 0.21f, 0.0f, 0.24f, -0.24f
	};
	static const float vertical[HOLSTER_POINT_COUNT] = {
		-0.58f, -0.58f, -0.30f, -0.30f, -0.36f, -0.14f, -0.14f
	};
	static const float depth[HOLSTER_POINT_COUNT] = {
		0.07f, 0.07f, 0.12f, 0.12f, 0.14f, -0.23f, -0.23f
	};
	const bool behind = point == HOLSTER_BACK_LEFT ||
		point == HOLSTER_BACK_RIGHT;
	matrix->SetUnity();
	// Holstered guns hang barrel-down with their broad side facing away from the
	// body on both the front and back planes. This basis deliberately includes
	// the inverse of the legacy weapon frame's fixed authored adjustment.
	matrix->GetRight() = gBaseCamera.GetForward()*(behind ? 1.0f : -1.0f);
	matrix->GetForward() = gBaseCamera.GetUp();
	matrix->GetUp() = gBaseCamera.GetRight()*(behind ? 1.0f : -1.0f);
	matrix->GetPosition() = gBaseCamera.GetPosition() +
		gBaseCamera.GetRight()*lateral[point] +
		gBaseCamera.GetUp()*vertical[point] +
		gBaseCamera.GetForward()*depth[point];
	return true;
}

#ifdef RW_D3D12
bool AreCopyCompatibleFormats(DXGI_FORMAT source, DXGI_FORMAT target)
{
	if(source == target)
		return true;
	return (source == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
	        source == DXGI_FORMAT_R8G8B8A8_UNORM ||
	        source == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
	       (target == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
	        target == DXGI_FORMAT_R8G8B8A8_UNORM ||
	        target == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
}

bool CopyRasterToSwapchain(RwRaster *source, int sourceWidth, int sourceHeight,
	Swapchain &target, uint32)
{
	if(!source || !AcquireSwapchain(target))
		return false;
	rw::Raster *raster = ((rw::Raster*)source)->parent;
	ID3D12Resource *sourceResource = nil;
	ID3D12Resource *targetResource = AcquiredTexture(target);
	ID3D12GraphicsCommandList *list = rw::d3d12::getCommandList();
	if(!raster || !targetResource || !list ||
	   !rw::d3d12::getRasterResource(raster, &sourceResource) || !sourceResource){
		VrLog("CopyRaster unavailable raster=%p target=%p list=%p source=%p\n",
			(void*)raster, (void*)targetResource, (void*)list, (void*)sourceResource);
		return false;
	}
	D3D12_RESOURCE_DESC sourceDesc = sourceResource->GetDesc();
	D3D12_RESOURCE_DESC targetDesc = targetResource->GetDesc();
	if((int)sourceDesc.Width != sourceWidth || (int)sourceDesc.Height != sourceHeight ||
	   sourceDesc.Width != targetDesc.Width || sourceDesc.Height != targetDesc.Height ||
	   !AreCopyCompatibleFormats(sourceDesc.Format, targetDesc.Format) ||
	   sourceDesc.SampleDesc.Count != targetDesc.SampleDesc.Count){
		VrLog("CopyRaster mismatch source=%llux%u fmt=%u samples=%u target=%llux%u fmt=%u samples=%u\n",
			(unsigned long long)sourceDesc.Width, sourceDesc.Height, (unsigned)sourceDesc.Format,
			sourceDesc.SampleDesc.Count, (unsigned long long)targetDesc.Width, targetDesc.Height,
			(unsigned)targetDesc.Format, targetDesc.SampleDesc.Count);
		return false;
	}
	if(!rw::d3d12::transitionRaster(raster, D3D12_RESOURCE_STATE_COPY_SOURCE))
		return false;
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = targetResource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	list->ResourceBarrier(1, &barrier);
	list->CopyResource(targetResource, sourceResource);
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	list->ResourceBarrier(1, &barrier);
	rw::d3d12::transitionRaster(raster, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if(gVrLoggedRenderableFrames < 10)
		VrLog("CopyRaster ok %dx%d targetIndex=%u\n", sourceWidth, sourceHeight,
			target.acquiredIndex);
	return true;
}

bool DrawEyeFxaa(EyeBuffer &eye, int eyeIndex)
{
	if(eyeIndex < 0 || eyeIndex >= EYE_COUNT ||
	   !AcquireSwapchain(eye.swapchain))
		return false;
	const XrFovf &fov = gRenderFov[eyeIndex];
	const float left = -tanf(fov.angleLeft);
	const float right = tanf(fov.angleRight);
	const float up = tanf(fov.angleUp);
	const float down = -tanf(fov.angleDown);
	float uvScaleX = (left + right) / (2.0f * gSourceTanX);
	float uvScaleY = (up + down) / (2.0f * gSourceTanY);
	float uvOffsetX = (gSourceTanX - left) / (2.0f * gSourceTanX);
	// D3D textures use a top-left origin, unlike the GL source used below.
	float uvOffsetY = (gSourceTanY - up) / (2.0f * gSourceTanY);
	ApplyTrackedScopeZoomToUv(&uvScaleX, &uvScaleY, &uvOffsetX, &uvOffsetY);
	uint32 colorMode = 0;
	float blurColor[4] = { 0.0f, 0.0f, 0.0f, 30.0f/255.0f };
	float contrastMult[3] = { 1.0f, 1.0f, 1.0f };
	float contrastAdd[3] = { 0.0f, 0.0f, 0.0f };
#ifdef EXTENDED_COLOURFILTER
	if(gLightingEnabled && TheCamera.m_BlurType != MOTION_BLUR_NONE){
		if(CPostFX::EffectSwitch == CPostFX::POSTFX_NORMAL)
			colorMode = 1;
		else if(CPostFX::EffectSwitch == CPostFX::POSTFX_MOBILE)
			colorMode = 2;
	}
	const float red = (float)TheCamera.m_BlurRed;
	const float green = (float)TheCamera.m_BlurGreen;
	const float blue = (float)TheCamera.m_BlurBlue;
	blurColor[0] = red*CPostFX::Intensity/255.0f;
	blurColor[1] = green*CPostFX::Intensity/255.0f;
	blurColor[2] = blue*CPostFX::Intensity/255.0f;
	contrastMult[0] = (red-64.0f)/256.0f+1.4f;
	contrastMult[1] = (green-64.0f)/256.0f+1.4f;
	contrastMult[2] = (blue-64.0f)/256.0f+1.4f;
	contrastAdd[0] = red/1536.0f-0.05f;
	contrastAdd[1] = green/1536.0f-0.05f;
	contrastAdd[2] = blue/1536.0f-0.05f;
#endif
	rw::Raster *source = (rw::Raster*)eye.color;
	const bool resolved = rw::d3d12::resolveRasterToExternal(
		source, AcquiredTexture(eye.swapchain),
		eye.swapchain.width, eye.swapchain.height,
		uvScaleX, uvScaleY, uvOffsetX, uvOffsetY,
		gAntiAliasingEnabled, colorMode, blurColor,
		contrastMult, contrastAdd) != 0;
	if(!resolved){
		VrLog("D3D12 eye resolve failed eye=%d scale=(%.5f,%.5f) offset=(%.5f,%.5f)\n",
			eyeIndex, uvScaleX, uvScaleY, uvOffsetX, uvOffsetY);
		ReleaseSwapchain(eye.swapchain);
	}
	return resolved;
}

bool DrawEyeDlaa(EyeBuffer &eye, int eyeIndex, RwCamera *camera)
{
	if(!gDlaaEnabled || !Dlaa::IsSupported() || !camera ||
	   eyeIndex < 0 || eyeIndex >= EYE_COUNT || !eye.color || !eye.depth)
		return false;
	rw::Raster *colorRaster = ((rw::Raster*)eye.color)->parent;
	rw::Raster *depthRaster = ((rw::Raster*)eye.depth)->parent;
	ID3D12Resource *colorResource = nil;
	ID3D12Resource *depthResource = nil;
	D3D12_GPU_DESCRIPTOR_HANDLE depthView = {};
	if(!colorRaster || !depthRaster ||
	   !rw::d3d12::getRasterResource(colorRaster, &colorResource) || !colorResource ||
	   !rw::d3d12::transitionRaster(colorRaster, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
	   !rw::d3d12::getDepthTextureView(depthRaster, &depthResource, &depthView) ||
	   !depthResource || depthView.ptr == 0 ||
	   !rw::d3d12::transitionDepthRaster(depthRaster, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
		return false;

	Dlaa::EyeInput input = {};
	input.color = colorResource;
	input.depth = depthResource;
	input.depthShaderResourceView = depthView.ptr;
	input.width = eye.renderWidth;
	input.height = eye.renderHeight;
	input.sourceLeft = ((rw::Raster*)eye.color)->offsetX;
	if(!rw::d3d12::getStereoWorldCamera(eyeIndex, input.view, input.projection))
		return false;
	input.nearPlane = gFirstPersonEnabled ? 0.05f : gOriginalNearPlane;
	input.farPlane = RwCameraGetFarClipPlane(camera);
	Dlaa::EyeOutput output = {};
	if(!Dlaa::EvaluateEye(eyeIndex, input, &output) || !output.color ||
	   output.shaderResourceView == 0 || !AcquireSwapchain(eye.swapchain))
		return false;

	const XrFovf &fov = gRenderFov[eyeIndex];
	const float left = -tanf(fov.angleLeft);
	const float right = tanf(fov.angleRight);
	const float up = tanf(fov.angleUp);
	const float down = -tanf(fov.angleDown);
	float uvScaleX = (left + right) / (2.0f * gSourceTanX);
	float uvScaleY = (up + down) / (2.0f * gSourceTanY);
	float uvOffsetX = (gSourceTanX - left) / (2.0f * gSourceTanX);
	float uvOffsetY = (gSourceTanY - up) / (2.0f * gSourceTanY);
	ApplyTrackedScopeZoomToUv(&uvScaleX, &uvScaleY, &uvOffsetX, &uvOffsetY);
	uint32 colorMode = 0;
	float blurColor[4] = { 0.0f, 0.0f, 0.0f, 30.0f/255.0f };
	float contrastMult[3] = { 1.0f, 1.0f, 1.0f };
	float contrastAdd[3] = { 0.0f, 0.0f, 0.0f };
#ifdef EXTENDED_COLOURFILTER
	if(gLightingEnabled && TheCamera.m_BlurType != MOTION_BLUR_NONE){
		if(CPostFX::EffectSwitch == CPostFX::POSTFX_NORMAL)
			colorMode = 1;
		else if(CPostFX::EffectSwitch == CPostFX::POSTFX_MOBILE)
			colorMode = 2;
	}
	const float red = (float)TheCamera.m_BlurRed;
	const float green = (float)TheCamera.m_BlurGreen;
	const float blue = (float)TheCamera.m_BlurBlue;
	blurColor[0] = red*CPostFX::Intensity/255.0f;
	blurColor[1] = green*CPostFX::Intensity/255.0f;
	blurColor[2] = blue*CPostFX::Intensity/255.0f;
	contrastMult[0] = (red-64.0f)/256.0f+1.4f;
	contrastMult[1] = (green-64.0f)/256.0f+1.4f;
	contrastMult[2] = (blue-64.0f)/256.0f+1.4f;
	contrastAdd[0] = red/1536.0f-0.05f;
	contrastAdd[1] = green/1536.0f-0.05f;
	contrastAdd[2] = blue/1536.0f-0.05f;
#endif
	D3D12_GPU_DESCRIPTOR_HANDLE outputView = { output.shaderResourceView };
	const bool resolved = rw::d3d12::resolveTextureToExternal(
		(ID3D12Resource*)output.color, outputView, AcquiredTexture(eye.swapchain),
		output.width, output.height, eye.swapchain.width, eye.swapchain.height,
		uvScaleX, uvScaleY, uvOffsetX, uvOffsetY, false, colorMode, blurColor,
		contrastMult, contrastAdd) != 0;
	if(!resolved){
		VrLog("D3D12 DLAA eye resolve failed eye=%d\n", eyeIndex);
		ReleaseSwapchain(eye.swapchain);
	}
	return resolved;
}

bool FinishD3D12SwapchainWrites()
{
	// The command list must be submitted before releasing OpenXR images, but a
	// CPU-side fence wait is unnecessary. The runtime synchronizes its use of
	// the D3D12 queue, while librw fences allocator reuse independently.
	const double timingStart = PerfNowMs();
	const bool completed = rw::d3d12::submitForExternal() != 0;
	if(gPerfRecording && gPerfFrameStarted)
		gPerfCurrent.d3d12ExternalSubmitMs +=
			(float)(PerfNowMs() - timingStart);
	if(!completed)
		VrLog("submitAndWaitForExternal failed\n");
	for(int eye = 0; eye < EYE_COUNT; eye++)
		ReleaseSwapchain(gEye[eye].swapchain);
	ReleaseSwapchain(gHudSwapchain);
	ReleaseSwapchain(gDebugSwapchain);
	ReleaseSwapchain(gVrMenuSwapchain);
	ReleaseSwapchain(gCinemaSwapchain);
	return completed;
}
#else
bool CopyRasterToSwapchain(RwRaster *source, int sourceWidth, int sourceHeight,
	Swapchain &target, GLenum filter)
{
	if(!AcquireSwapchain(target))
		return false;
	rw::Raster *raster = ((rw::Raster*)source)->parent;
	rw::gl3::Gl3Raster *native = PLUGINOFFSET(rw::gl3::Gl3Raster, raster, rw::gl3::nativeRasterOffset);
	GLint oldDraw = 0, oldRead = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldRead);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, native->fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCopyFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		AcquiredTexture(target), 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	const bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	if(complete)
		glBlitFramebuffer(0, 0, sourceWidth, sourceHeight, 0, 0, target.width, target.height,
			GL_COLOR_BUFFER_BIT, filter);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDraw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, oldRead);
	ReleaseSwapchain(target);
	return complete;
}

bool DrawEyeFxaa(EyeBuffer &eye, int eyeIndex)
{
	if(!gFxaaProgram || !gFxaaVertexArray || !AcquireSwapchain(eye.swapchain))
		return false;
	int colorMode=0;
	float blurColor[4]={0.0f,0.0f,0.0f,30.0f/255.0f};
	float contrastMult[3]={1.0f,1.0f,1.0f}, contrastAdd[3]={0.0f,0.0f,0.0f};
#ifdef EXTENDED_COLOURFILTER
	if(gLightingEnabled && TheCamera.m_BlurType!=MOTION_BLUR_NONE){
		if(CPostFX::EffectSwitch==CPostFX::POSTFX_NORMAL) colorMode=1;
		else if(CPostFX::EffectSwitch==CPostFX::POSTFX_MOBILE) colorMode=2;
	}
	const float red=(float)TheCamera.m_BlurRed, green=(float)TheCamera.m_BlurGreen, blue=(float)TheCamera.m_BlurBlue;
	blurColor[0]=red*CPostFX::Intensity/255.0f; blurColor[1]=green*CPostFX::Intensity/255.0f;
	blurColor[2]=blue*CPostFX::Intensity/255.0f;
	contrastMult[0]=(red-64.0f)/256.0f+1.4f; contrastMult[1]=(green-64.0f)/256.0f+1.4f;
	contrastMult[2]=(blue-64.0f)/256.0f+1.4f;
	contrastAdd[0]=red/1536.0f-0.05f; contrastAdd[1]=green/1536.0f-0.05f; contrastAdd[2]=blue/1536.0f-0.05f;
#endif
	rw::Raster *raster=((rw::Raster*)eye.color)->parent;
	rw::gl3::Gl3Raster *native=PLUGINOFFSET(rw::gl3::Gl3Raster,raster,rw::gl3::nativeRasterOffset);
	GLint oldDraw=0,oldRead=0,oldProgram=0,oldVao=0,oldActive=0,oldTexture=0,oldViewport[4]={};
	GLboolean oldMask[4]={GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
	glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram); glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&oldVao);
	glGetIntegerv(GL_ACTIVE_TEXTURE,&oldActive); glGetIntegerv(GL_VIEWPORT,oldViewport); glGetBooleanv(GL_COLOR_WRITEMASK,oldMask);
	const GLboolean blend=glIsEnabled(GL_BLEND),depth=glIsEnabled(GL_DEPTH_TEST),cull=glIsEnabled(GL_CULL_FACE);
	const GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST),srgb=glIsEnabled(GL_FRAMEBUFFER_SRGB);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gCopyFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,AcquiredTexture(eye.swapchain),0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldDraw); glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);
		ReleaseSwapchain(eye.swapchain); return false;
	}
	glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture); glBindTexture(GL_TEXTURE_2D,native->texid);
	GLint oldMin=GL_NEAREST,oldMag=GL_NEAREST; glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,&oldMin);
	glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,&oldMag);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,gAntiAliasingEnabled?GL_LINEAR:GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,gAntiAliasingEnabled?GL_LINEAR:GL_NEAREST);
	glViewport(0,0,eye.swapchain.width,eye.swapchain.height); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
	glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB);
	glUseProgram(gFxaaProgram); glBindVertexArray(gFxaaVertexArray);
	const XrFovf &fov=gRenderFov[eyeIndex];
	const float left=-tanf(fov.angleLeft), right=tanf(fov.angleRight);
	const float up=tanf(fov.angleUp), down=-tanf(fov.angleDown);
	float uvScaleX=(left+right)/(2.0f*gSourceTanX);
	float uvScaleY=(up+down)/(2.0f*gSourceTanY);
	float uvOffsetX=(gSourceTanX-left)/(2.0f*gSourceTanX);
	float uvOffsetY=(gSourceTanY-down)/(2.0f*gSourceTanY);
	ApplyTrackedScopeZoomToUv(&uvScaleX,&uvScaleY,&uvOffsetX,&uvOffsetY);
	glUniform1i(gFxaaTextureUniform,0);
	glUniform2f(gFxaaInverseSizeUniform,1.0f/eye.swapchain.width,1.0f/eye.swapchain.height);
	glUniform2f(gFxaaUvScaleUniform,uvScaleX,uvScaleY);
	glUniform2f(gFxaaUvOffsetUniform,uvOffsetX,uvOffsetY);
	glUniform1i(gFxaaEnabledUniform,gAntiAliasingEnabled?1:0); glUniform1i(gColorModeUniform,colorMode);
	glUniform4fv(gBlurColorUniform,1,blurColor); glUniform3fv(gContrastMultUniform,1,contrastMult); glUniform3fv(gContrastAddUniform,1,contrastAdd);
	glDrawArrays(GL_TRIANGLES,0,3);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,oldMin); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,oldMag);
	glBindTexture(GL_TEXTURE_2D,oldTexture); glActiveTexture(oldActive); glBindVertexArray(oldVao); glUseProgram(oldProgram);
	glViewport(oldViewport[0],oldViewport[1],oldViewport[2],oldViewport[3]); glColorMask(oldMask[0],oldMask[1],oldMask[2],oldMask[3]);
	if(blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); if(depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if(cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE); if(scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	if(srgb) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldDraw); glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);
	ReleaseSwapchain(eye.swapchain);
	return true;
}
#endif

bool UpdateHudSwapchain(RwCamera *camera)
{
	if((!gGameplayHudVisible && !IsTrackedScopeActive()) ||
	   !gHudColor || !gHudDepth)
		return false;
	RwRaster *oldColor = RwCameraGetRaster(camera);
	RwRaster *oldDepth = RwCameraGetZRaster(camera);
	const int oldWidth = RsGlobal.width, oldHeight = RsGlobal.height;
	RwCameraSetRaster(camera, gHudColor);
	RwCameraSetZRaster(camera, gHudDepth);
	RsGlobal.width = VR_HUD_WIDTH;
	RsGlobal.height = VR_HUD_HEIGHT;
	RwRGBA transparent = { 0, 0, 0, 0 };
	RwCameraClear(camera, &transparent, rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
	bool rendered = RwCameraBeginUpdate(camera) != nil;
	if(rendered){ RenderVrGameplayHud(); RwCameraEndUpdate(camera); }
	RwCameraSetRaster(camera, oldColor);
	RwCameraSetZRaster(camera, oldDepth);
	RsGlobal.width = oldWidth;
	RsGlobal.height = oldHeight;
	return rendered && CopyRasterToSwapchain(gHudColor, VR_HUD_WIDTH, VR_HUD_HEIGHT,
#ifdef RW_D3D12
		gHudSwapchain, 0);
#else
		gHudSwapchain, GL_NEAREST);
#endif
}

int16 AxisValue(float value) { return (int16)(clamp(value, -1.0f, 1.0f)*128.0f); }
int16 TriggerValue(float value) { return (int16)(clamp(value, 0.0f, 1.0f)*255.0f); }
void MergeAxis(int16 &destination, int16 value) { if(Abs(value)>Abs(destination)) destination=value; }
void MergeButton(int16 &destination, bool pressed) { if(pressed) destination=255; }

bool ReadVector(XrAction action, int hand, XrVector2f &value)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action; get.subactionPath = gActions.hands[hand];
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
	if(XR_FAILED(xrGetActionStateVector2f(gSession, &get, &state)) || !state.isActive) return false;
	value = state.currentState; return true;
}

float ReadFloat(XrAction action, int hand)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action; get.subactionPath = gActions.hands[hand];
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
	return XR_SUCCEEDED(xrGetActionStateFloat(gSession, &get, &state)) && state.isActive ? state.currentState : 0.0f;
}

bool ReadBool(XrAction action, int hand = -1)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action; get.subactionPath = hand >= 0 ? gActions.hands[hand] : XR_NULL_PATH;
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
	return XR_SUCCEEDED(xrGetActionStateBoolean(gSession, &get, &state)) && state.isActive && state.currentState;
}

bool IsHandPoseActive(XrAction action, int hand)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action;
	get.subactionPath = gActions.hands[hand];
	XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
	return XR_SUCCEEDED(xrGetActionStatePose(gSession, &get, &state)) && state.isActive;
}

bool LocateHandAction(XrAction action, XrSpace space, int hand, XrPosef &pose,
	XrVector3f *linearVelocity = nil, XrVector3f *angularVelocity = nil)
{
	if(linearVelocity)
		*linearVelocity = { 0.0f, 0.0f, 0.0f };
	if(angularVelocity)
		*angularVelocity = { 0.0f, 0.0f, 0.0f };
	if(!space || !gGameplaySpace || !IsHandPoseActive(action, hand))
		return false;
	XrSpaceVelocity velocity = { XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if(linearVelocity || angularVelocity)
		location.next = &velocity;
	if(XR_FAILED(xrLocateSpace(space, gGameplaySpace,
		gFrameState.predictedDisplayTime, &location)))
		return false;
	const XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT |
		XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if((location.locationFlags & required) != required)
		return false;
	pose = location.pose;
	if(linearVelocity &&
	   (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0)
		*linearVelocity = velocity.linearVelocity;
	if(angularVelocity &&
	   (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0)
		*angularVelocity = velocity.angularVelocity;
	return true;
}

void LocateTrackedHands()
{
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gTrackedHandPoseValid[hand] = LocateHandAction(gActions.gripPose,
			gActions.gripSpace[hand], hand, gTrackedHandPose[hand],
			&gTrackedHandLinearVelocity[hand],
			&gTrackedHandAngularVelocity[hand]);
		gTrackedHandAimPoseValid[hand] = LocateHandAction(gActions.aimPose,
			gActions.aimSpace[hand], hand, gTrackedHandAimPose[hand]);
	}
}

float Halton(uint32 index, uint32 base)
{
	float result = 0.0f;
	float fraction = 1.0f;
	while(index){
		fraction /= (float)base;
		result += fraction*(float)(index % base);
		index /= base;
	}
	return result;
}
}

void PerfBeginFrame()
{
	if(!gPerfRecording || gPerfFrameStarted || gPerfRecordedSamples >= VR_PERF_MAX_SAMPLES) return;
	gPerfCurrent = {};
	gPerfCurrent.slowStreamItemId = -1;
	gPerfCurrent.slowStreamItemType = -1;
	gPerfStreamItemStartMs = 0.0;
#ifdef RW_D3D12
	rw::d3d12::resetFrameSyncProfile();
	rw::d3d12::resetWorldRenderProfile();
#endif
	gPerfFrameStartMs = PerfNowMs();
	gPerfFrameStarted = true;
}
void PerfAbortFrame() { gPerfFrameStarted = false; gPerfStreamItemStartMs = 0.0; }
void PerfEndFrame(float x, float y, float z)
{
	if(!gPerfRecording || !gPerfFrameStarted) return;
#ifdef RW_D3D12
	rw::d3d12::FrameSyncProfile syncProfile = {};
	rw::d3d12::getFrameSyncProfile(&syncProfile);
	gPerfCurrent.d3d12FrameFenceWaitMs = syncProfile.frameFenceWaitMs;
	gPerfCurrent.d3d12FullGpuWaitMs = syncProfile.fullGpuWaitMs;
	rw::d3d12::WorldRenderProfile worldProfile = {};
	rw::d3d12::getWorldRenderProfile(&worldProfile);
	gPerfCurrent.geometryInstanceMs = worldProfile.geometryInstanceMs;
	gPerfCurrent.geometryBufferUploadMs = worldProfile.bufferUploadMs;
	gPerfCurrent.geometryBufferBytes = worldProfile.bufferBytes;
	gPerfCurrent.worldSubmittedIndices = worldProfile.submittedIndices;
	gPerfCurrent.geometryInstances = worldProfile.geometryInstances;
	gPerfCurrent.worldDrawCalls = worldProfile.drawCalls;
	gPerfCurrent.stereoBundleBuildMs = worldProfile.stereoBundleBuildMs;
	gPerfCurrent.stereoBundleWaitMs = worldProfile.stereoBundleWaitMs;
	gPerfCurrent.stereoBundleDrawCalls = worldProfile.stereoBundleDrawCalls;
	gPerfCurrent.stereoBundleFallbacks = worldProfile.stereoBundleFallbacks;
	gPerfCurrent.stereoSinglePassBegins = worldProfile.stereoSinglePassBegins;
	gPerfCurrent.stereoSinglePassDrawCalls = worldProfile.stereoSinglePassDrawCalls;
	gPerfCurrent.stereoSinglePassIndices = worldProfile.stereoSinglePassIndices;
	gPerfCurrent.stereoSinglePassFallbacks = worldProfile.stereoSinglePassFallbacks;
	gPerfCurrent.fixedFoveatedBegins = worldProfile.fixedFoveatedBegins;
	gPerfCurrent.fixedFoveatedFailures = worldProfile.fixedFoveatedFailures;
	rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
	rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
	gPerfCurrent.fixedFoveatedProfile = foveatedInfo.profile;
	gPerfCurrent.fixedFoveatedTileSize = foveatedInfo.tileSize;
#endif
	gPerfCurrent.frameMs = (float)(PerfNowMs()-gPerfFrameStartMs);
	gPerfCurrent.elapsedSeconds = (PerfNowMs()-gPerfRecordingStartMs)/1000.0;
	gPerfCurrent.playerX=x; gPerfCurrent.playerY=y; gPerfCurrent.playerZ=z;
	gPerfSamples[gPerfRecordedSamples] = gPerfCurrent;
	if(gPerfLiveCsv){
		WritePerfCsvSample(gPerfLiveCsv, gPerfRecordedSamples, gPerfCurrent);
		if((gPerfRecordedSamples % 30) == 29)
			fflush(gPerfLiveCsv);
	}
	gPerfRecordedSamples++;
	gPerfFrameStarted = false;
	gPerfStreamItemStartMs = 0.0;
}
void PerfBeginPhase(ePerfPhase phase)
{
	if(gPerfRecording && gPerfFrameStarted && phase >= 0 && phase < PERF_PHASE_COUNT)
		gPerfPhaseStartMs[phase] = PerfNowMs();
}
void PerfEndPhase(ePerfPhase phase)
{
	if(gPerfRecording && gPerfFrameStarted && phase >= 0 && phase < PERF_PHASE_COUNT)
		gPerfCurrent.phaseMs[phase] += (float)(PerfNowMs()-gPerfPhaseStartMs[phase]);
}
void PerfSetStreamingStats(int requested, uint64 memory)
{
	if(gPerfRecording && gPerfFrameStarted){ gPerfCurrent.requestedModels=requested; gPerfCurrent.streamingMemory=memory; }
}
void PerfBeginStreamItem(int streamId, int streamType)
{
	if(!gPerfRecording || !gPerfFrameStarted){ gPerfStreamItemStartMs = 0.0; return; }
#ifdef RW_D3D12
	rw::d3d12::resetTextureUploadProfile();
#endif
	gPerfStreamItemId = streamId;
	gPerfStreamItemType = streamType;
	gPerfStreamItemStartMs = PerfNowMs();
}
void PerfEndStreamItem()
{
	if(gPerfStreamItemStartMs <= 0.0 || !gPerfRecording || !gPerfFrameStarted){
		gPerfStreamItemStartMs = 0.0;
		return;
	}
	const float elapsed = (float)(PerfNowMs() - gPerfStreamItemStartMs);
	if(elapsed > gPerfCurrent.slowStreamItemMs){
		gPerfCurrent.slowStreamItemMs = elapsed;
		gPerfCurrent.slowStreamItemId = gPerfStreamItemId;
		gPerfCurrent.slowStreamItemType = gPerfStreamItemType;
#ifdef RW_D3D12
		rw::d3d12::TextureUploadProfile profile = {};
		rw::d3d12::getTextureUploadProfile(&profile);
		gPerfCurrent.textureDefaultResourceMs = profile.defaultResourceMs;
		gPerfCurrent.textureDescriptorMs = profile.descriptorMs;
		gPerfCurrent.textureFootprintMs = profile.footprintMs;
		gPerfCurrent.textureUploadResourceMs = profile.uploadResourceMs;
		gPerfCurrent.textureCpuCopyMs = profile.cpuCopyMs;
		gPerfCurrent.textureQueueMs = profile.queueMs;
		gPerfCurrent.textureUploadBytes = profile.uploadBytes;
		gPerfCurrent.textureUploadCount = profile.uploads;
#endif
	}
	gPerfStreamItemStartMs = 0.0;
}
void PerfCountVisibleEntity(ePerfVisibleType type)
{
	if(!gPerfRecording || !gPerfFrameStarted) return;
	if(type==PERF_VISIBLE_BUILDING) gPerfCurrent.visibleBuildings++;
	else if(type==PERF_VISIBLE_OBJECT) gPerfCurrent.visibleObjects++;
	else if(type==PERF_VISIBLE_PED) gPerfCurrent.visiblePeds++;
	else if(type==PERF_VISIBLE_VEHICLE) gPerfCurrent.visibleVehicles++;
}
void PerfCountEntityRender() { if(gPerfRecording && gPerfFrameStarted) gPerfCurrent.entityRenderCalls++; }

static int WrapWeaponRotation(int degrees)
{
	while(degrees > 360) degrees -= 720;
	while(degrees < -360) degrees += 720;
	return degrees;
}

void ChangeVrMenuValue(int direction)
{
	if(gVrBikeCalibrationMenuVisible){
		CVehicle *vehicle = FindPlayerVehicle();
		if(!vehicle || !IsImmersiveDrivingActiveInternal()){
			gVrBikeCalibrationMenuVisible = false;
			return;
		}
		const bool bike = vehicle->IsBike();
		const int model = vehicle->GetModelIndex();
		const int item = GetVehicleCalibrationMenuItemForRow(
			gVrBikeCalibrationMenuSelection);
		if(item == VR_BIKE_CAL_HAND){
			gBikeCalibrationEditHand = 1-gBikeCalibrationEditHand;
			return;
		}
		BikeHandleCalibration *calibration =
			bike ?
			GetBikeHandleCalibration(model, gBikeCalibrationEditHand) :
			GetCarWheelCalibration(model, gBikeCalibrationEditHand);
		BikeLeanCalibration *leanCalibration = bike ?
			GetBikeLeanCalibration(model) : nil;
		if(!calibration)
			return;
		switch(item){
		case VR_BIKE_CAL_OFFSET_X:
			calibration->offsetX =
				Min(Max(calibration->offsetX+direction, -300), 300);
			if(bike)
				SaveBikeHandleCalibrationValue(model,
					gBikeCalibrationEditHand, "OffsetX",
					calibration->offsetX);
			else
				SaveCarWheelCalibrationValue(model,
					gBikeCalibrationEditHand, "OffsetX",
					calibration->offsetX);
			break;
		case VR_BIKE_CAL_OFFSET_Y:
			calibration->offsetY =
				Min(Max(calibration->offsetY+direction, -300), 300);
			if(bike)
				SaveBikeHandleCalibrationValue(model,
					gBikeCalibrationEditHand, "OffsetY",
					calibration->offsetY);
			else
				SaveCarWheelCalibrationValue(model,
					gBikeCalibrationEditHand, "OffsetY",
					calibration->offsetY);
			break;
		case VR_BIKE_CAL_OFFSET_Z:
			calibration->offsetZ =
				Min(Max(calibration->offsetZ+direction, -300), 300);
			if(bike)
				SaveBikeHandleCalibrationValue(model,
					gBikeCalibrationEditHand, "OffsetZ",
					calibration->offsetZ);
			else
				SaveCarWheelCalibrationValue(model,
					gBikeCalibrationEditHand, "OffsetZ",
					calibration->offsetZ);
			break;
		case VR_BIKE_CAL_ROT_X:
			calibration->rotationX = Min(Max(
				calibration->rotationX+direction, -720), 720);
			if(bike)
				SaveBikeHandleCalibrationValue(model,
					gBikeCalibrationEditHand, "RotationX",
					calibration->rotationX);
			else
				SaveCarWheelCalibrationValue(model,
					gBikeCalibrationEditHand, "RotationX",
					calibration->rotationX);
			break;
		case VR_BIKE_CAL_ROT_Y:
			calibration->rotationY = Min(Max(
				calibration->rotationY+direction, -720), 720);
			if(bike)
				SaveBikeHandleCalibrationValue(model,
					gBikeCalibrationEditHand, "RotationY",
					calibration->rotationY);
			else
				SaveCarWheelCalibrationValue(model,
					gBikeCalibrationEditHand, "RotationY",
					calibration->rotationY);
			break;
		case VR_BIKE_CAL_ROT_Z:
			calibration->rotationZ = Min(Max(
				calibration->rotationZ+direction, -720), 720);
			if(bike)
				SaveBikeHandleCalibrationValue(model,
					gBikeCalibrationEditHand, "RotationZ",
					calibration->rotationZ);
			else
				SaveCarWheelCalibrationValue(model,
					gBikeCalibrationEditHand, "RotationZ",
					calibration->rotationZ);
			break;
		case VR_BIKE_CAL_WHEELIE_HEIGHT:
			if(!leanCalibration)
				break;
			leanCalibration->wheelieHeightCm = Min(Max(
				leanCalibration->wheelieHeightCm+direction, 5), 100);
			SaveBikeLeanCalibrationValue(model, "WheelieHeightCm",
				leanCalibration->wheelieHeightCm);
			break;
		case VR_BIKE_CAL_STAND_HEIGHT:
			if(!leanCalibration)
				break;
			leanCalibration->standHeightCm = Min(Max(
				leanCalibration->standHeightCm+direction, 5), 100);
			SaveBikeLeanCalibrationValue(model, "StandHeightCm",
				leanCalibration->standHeightCm);
			break;
		case VR_BIKE_CAL_BACK:
			gVrBikeCalibrationMenuVisible = false;
			break;
		}
		return;
	}
	if(gVrHolsterMenuVisible){
		if(gVrHolsterMenuSelection < HOLSTER_POINT_COUNT)
			CycleHolsterPointSlot(gVrHolsterMenuSelection, direction);
		else
			gVrHolsterMenuVisible = false;
		return;
	}
	if(gVrCalibrationMenuVisible){
		SyncCurrentWeaponCalibration();
		SyncCurrentSupportGripCalibration();
		switch(gVrCalibrationMenuSelection){
		case VR_CAL_AIM_OFFSET_X:
			gWeaponAimOffsetXCm = Min(Max(gWeaponAimOffsetXCm+direction, -100), 100);
			SaveCurrentWeaponCalibrationValue("AimOffsetX", gWeaponAimOffsetXCm);
			break;
		case VR_CAL_AIM_OFFSET_Y:
			gWeaponAimOffsetYCm = Min(Max(gWeaponAimOffsetYCm+direction, -100), 100);
			SaveCurrentWeaponCalibrationValue("AimOffsetY", gWeaponAimOffsetYCm);
			break;
		case VR_CAL_AIM_OFFSET_Z:
			gWeaponAimOffsetZCm = Min(Max(gWeaponAimOffsetZCm+direction, -100), 100);
			SaveCurrentWeaponCalibrationValue("AimOffsetZ", gWeaponAimOffsetZCm);
			break;
		case VR_CAL_AIM_ROT_X:
			gWeaponAimRotationXDeg = WrapWeaponRotation(gWeaponAimRotationXDeg+direction);
			SaveCurrentWeaponCalibrationValue("AimRotationX", gWeaponAimRotationXDeg);
			break;
		case VR_CAL_AIM_ROT_Y:
			gWeaponAimRotationYDeg = WrapWeaponRotation(gWeaponAimRotationYDeg+direction);
			SaveCurrentWeaponCalibrationValue("AimRotationY", gWeaponAimRotationYDeg);
			break;
		case VR_CAL_AIM_ROT_Z:
			gWeaponAimRotationZDeg = WrapWeaponRotation(gWeaponAimRotationZDeg+direction);
			SaveCurrentWeaponCalibrationValue("AimRotationZ", gWeaponAimRotationZDeg);
			break;
		case VR_CAL_WEAPON_OFFSET_X:
			gWeaponOffsetXCm = Min(Max(gWeaponOffsetXCm+direction, -100), 100);
			SaveCurrentWeaponCalibrationValue("OffsetX", gWeaponOffsetXCm);
			break;
		case VR_CAL_WEAPON_OFFSET_Y:
			gWeaponOffsetYCm = Min(Max(gWeaponOffsetYCm+direction, -100), 100);
			SaveCurrentWeaponCalibrationValue("OffsetY", gWeaponOffsetYCm);
			break;
		case VR_CAL_WEAPON_OFFSET_Z:
			gWeaponOffsetZCm = Min(Max(gWeaponOffsetZCm+direction, -100), 100);
			SaveCurrentWeaponCalibrationValue("OffsetZ", gWeaponOffsetZCm);
			break;
		case VR_CAL_WEAPON_ROT_X:
			gWeaponRotationXDeg = WrapWeaponRotation(gWeaponRotationXDeg+direction);
			SaveCurrentWeaponCalibrationValue("RotationX", gWeaponRotationXDeg);
			break;
		case VR_CAL_WEAPON_ROT_Y:
			gWeaponRotationYDeg = WrapWeaponRotation(gWeaponRotationYDeg+direction);
			SaveCurrentWeaponCalibrationValue("RotationY", gWeaponRotationYDeg);
			break;
		case VR_CAL_WEAPON_ROT_Z:
			gWeaponRotationZDeg = WrapWeaponRotation(gWeaponRotationZDeg+direction);
			SaveCurrentWeaponCalibrationValue("RotationZ", gWeaponRotationZDeg);
			break;
		case VR_CAL_SUPPORT_OFFSET_X:
			gSupportGripOffsetXCm = Min(Max(gSupportGripOffsetXCm+direction, -200), 200);
			SaveCurrentSupportGripCalibrationValue("SupportGripOffsetX",
				gSupportGripOffsetXCm);
			break;
		case VR_CAL_SUPPORT_OFFSET_Y:
			gSupportGripOffsetYCm = Min(Max(gSupportGripOffsetYCm+direction, -200), 200);
			SaveCurrentSupportGripCalibrationValue("SupportGripOffsetY",
				gSupportGripOffsetYCm);
			break;
		case VR_CAL_SUPPORT_OFFSET_Z:
			gSupportGripOffsetZCm = Min(Max(gSupportGripOffsetZCm+direction, -200), 200);
			SaveCurrentSupportGripCalibrationValue("SupportGripOffsetZ",
				gSupportGripOffsetZCm);
			break;
		case VR_CAL_BACK:
			gVrCalibrationMenuVisible = false;
			break;
		}
		return;
	}
	if(gVrVehicleMenuVisible){
		switch(gVrVehicleMenuSelection){
		case VR_VEHICLE_DRIVING_TYPE:
			gDrivingType =
				(gDrivingType+VR_DRIVING_TYPE_COUNT+direction) %
				VR_DRIVING_TYPE_COUNT;
			SaveVrSetting("DrivingType", gDrivingType);
			ResetImmersiveDrivingInteraction();
			if(gDrivingType != VR_DRIVING_IMMERSIVE)
				gVrBikeCalibrationMenuVisible = false;
			break;
		case VR_VEHICLE_DRIVING_Y:
			gDrivingYOffsetCm =
				Min(Max(gDrivingYOffsetCm+direction*5, -100), 150);
			SaveVrSetting("DrivingYOffsetCm", gDrivingYOffsetCm);
			break;
		case VR_VEHICLE_MOTION_HAND:
			gMotionSteeringHand = 1-gMotionSteeringHand;
			SaveVrSetting("MotionSteeringHand", gMotionSteeringHand);
			ResetMotionSteeringInteraction();
			break;
		case VR_VEHICLE_HANDLE_HIGHLIGHTS:
			gBikeHandleHighlightsEnabled =
				!gBikeHandleHighlightsEnabled;
			SaveVrSetting("BikeHandleHighlights",
				gBikeHandleHighlightsEnabled);
			break;
		case VR_VEHICLE_CALIBRATION:
			if(IsImmersiveDrivingActiveInternal()){
				gVrBikeCalibrationMenuVisible = true;
				gVrBikeCalibrationMenuSelection = 0;
				gBikeCalibrationEditHand = 0;
			}
			break;
		case VR_VEHICLE_BACK:
			gVrVehicleMenuVisible = false;
			break;
		}
		return;
	}
	switch(gVrMenuSelection){
	case VR_MAIN_RENDER_SCALE:
		gRenderScaleIndex = Min(Max(gRenderScaleIndex+direction, 0),
			MaxSupportedRenderScaleIndex());
		gRenderScaleChangePending = true;
		break;
	case VR_MAIN_VRS:
#ifdef RW_D3D12
		gFixedFoveatedProfile = (gFixedFoveatedProfile+
			rw::d3d12::FIXED_FOVEATED_PROFILE_COUNT+direction) %
			rw::d3d12::FIXED_FOVEATED_PROFILE_COUNT;
		rw::d3d12::setFixedFoveatedRenderingProfile(gFixedFoveatedProfile);
		SaveVrSetting("VRS", gFixedFoveatedProfile);
#endif
		break;
	case VR_MAIN_DLAA:
		gDlaaEnabled = !gDlaaEnabled;
		SaveVrSetting("DLAA", gDlaaEnabled);
		Dlaa::ResetHistory();
		if(gDlaaEnabled && !gDlaaStereoActivationReady){
			gDlaaStereoActivationFailed = false;
			gDlaaStereoWarmupFrames = DLAA_ACTIVATION_WARMUP_FRAMES;
		}
		break;
	case VR_MAIN_JITTER:
		gTemporalJitterMode = (gTemporalJitterMode+6+direction)%6;
		SaveVrSetting("TemporalJitter", gTemporalJitterMode);
		Dlaa::ResetHistory();
		break;
	case VR_MAIN_FXAA:
		gAntiAliasingEnabled = !gAntiAliasingEnabled;
		SaveVrSetting("AntiAliasing", gAntiAliasingEnabled);
		break;
	case VR_MAIN_COLOR:
		gLightingEnabled = !gLightingEnabled;
		SaveVrSetting("ViceCityColor", gLightingEnabled);
		break;
	case VR_MAIN_HUD:
		gGameplayHudVisible = !gGameplayHudVisible;
		SaveVrSetting("GameplayHud", gGameplayHudVisible);
		break;
	case VR_MAIN_HANDS:
		gVrHandsEnabled = !gVrHandsEnabled;
		if(!gVrHandsEnabled)
			for(int hand = 0; hand < EYE_COUNT; hand++)
				ClearWeaponSupportForHand(hand);
		SaveVrSetting("VrHands", gVrHandsEnabled);
		break;
	case VR_MAIN_LASER:
		gWeaponLaserEnabled = !gWeaponLaserEnabled;
		SaveVrSetting("WeaponLaser", gWeaponLaserEnabled);
		break;
	case VR_MAIN_HOLSTER_HIGHLIGHTS:
		gWeaponHolsterHighlightsEnabled = !gWeaponHolsterHighlightsEnabled;
		SaveVrSetting("HolsterHighlights", gWeaponHolsterHighlightsEnabled);
		break;
	case VR_MAIN_MANUAL_RELOAD:
		gManualReloadEnabled = !gManualReloadEnabled;
		if(!gManualReloadEnabled)
			for(int hand = 0; hand < EYE_COUNT; hand++)
				gManualReload[hand] = ManualReloadState();
		SaveVrSetting("ManualReloading", gManualReloadEnabled);
		break;
	case VR_MAIN_SCOPE_AIM:
		gPhysicalScopeAimEnabled = !gPhysicalScopeAimEnabled;
		SaveVrSetting("PhysicalScopeAim", gPhysicalScopeAimEnabled);
		ResetTrackedScopeState();
		break;
	case VR_MAIN_VEHICLE_SETTINGS:
		gVrVehicleMenuVisible = true;
		gVrCalibrationMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrVehicleMenuSelection = 0;
		break;
	case VR_MAIN_DEBUG:
		gDebugVisible = !gDebugVisible;
		break;
	case VR_MAIN_GRIP_LOCK:
		gWeaponGripLockEnabled = !gWeaponGripLockEnabled;
		SaveVrSetting("WeaponGripLock", gWeaponGripLockEnabled);
		break;
	case VR_MAIN_CALIBRATION:
		gVrCalibrationMenuVisible = true;
		gVrVehicleMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrCalibrationMenuSelection = 0;
		SyncCurrentWeaponCalibration();
		SyncCurrentSupportGripCalibration();
		break;
	case VR_MAIN_HOLSTERS:
		gVrHolsterMenuVisible = true;
		gVrVehicleMenuVisible = false;
		gVrCalibrationMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrHolsterMenuSelection = 0;
		break;
	}
}

bool IsVrMenuValueRepeatable()
{
	if(gVrBikeCalibrationMenuVisible){
		const int item = GetVehicleCalibrationMenuItemForRow(
			gVrBikeCalibrationMenuSelection);
		return item >= VR_BIKE_CAL_OFFSET_X &&
			item <= (IsImmersiveCarDrivingActiveInternal() ?
				VR_BIKE_CAL_ROT_Z : VR_BIKE_CAL_STAND_HEIGHT);
	}
	if(gVrCalibrationMenuVisible)
		return gVrCalibrationMenuSelection >= VR_CAL_AIM_OFFSET_X &&
			gVrCalibrationMenuSelection <= VR_CAL_SUPPORT_OFFSET_Z;
	if(gVrHolsterMenuVisible)
		return false;
	if(gVrVehicleMenuVisible)
		return gVrVehicleMenuSelection == VR_VEHICLE_DRIVING_Y;
	return gVrMenuSelection == VR_MAIN_RENDER_SCALE;
}

bool ShouldApplyVrMenuValueInput(bool down, bool wasDown,
	ULONGLONG now, ULONGLONG *nextRepeatAt)
{
	if(!nextRepeatAt)
		return down && !wasDown;
	if(!down){
		*nextRepeatAt = 0;
		return false;
	}
	if(!wasDown){
		// A quick tap is always exactly one step. Holding begins repeating only
		// after a deliberate pause, which keeps fine calibration practical.
		*nextRepeatAt = now+420ULL;
		return true;
	}
	if(!IsVrMenuValueRepeatable() || *nextRepeatAt == 0 || now < *nextRepeatAt)
		return false;
	*nextRepeatAt = now+85ULL;
	return true;
}

void HandleVrMenuInput(const XrVector2f &stick, bool decrease, bool increase,
	bool select, bool back)
{
	const bool vertical = fabsf(stick.y) >= 0.65f;
	int *selection;
	int itemCount;
	if(gVrBikeCalibrationMenuVisible){
		selection = &gVrBikeCalibrationMenuSelection;
		itemCount = GetVehicleCalibrationMenuItemCount();
	}else if(gVrHolsterMenuVisible){
		selection = &gVrHolsterMenuSelection;
		itemCount = HOLSTER_MENU_ITEM_COUNT;
	}else if(gVrCalibrationMenuVisible){
		selection = &gVrCalibrationMenuSelection;
		itemCount = VR_CALIBRATION_MENU_ITEM_COUNT;
	}else if(gVrVehicleMenuVisible){
		selection = &gVrVehicleMenuSelection;
		itemCount = VR_VEHICLE_MENU_ITEM_COUNT;
	}else{
		selection = &gVrMenuSelection;
		itemCount = VR_MENU_ITEM_COUNT;
	}
	if(vertical && !gVrMenuVerticalDown){
		*selection += stick.y > 0.0f ? -1 : 1;
		if(*selection < 0) *selection = itemCount-1;
		if(*selection >= itemCount) *selection = 0;
	}
	const ULONGLONG menuInputNow = GetTickCount64();
	if(ShouldApplyVrMenuValueInput(decrease, gVrMenuDecreaseDown,
	   menuInputNow, &gVrMenuDecreaseRepeatAt))
		ChangeVrMenuValue(-1);
	if(ShouldApplyVrMenuValueInput(increase, gVrMenuIncreaseDown,
	   menuInputNow, &gVrMenuIncreaseRepeatAt))
		ChangeVrMenuValue(1);
	const bool mainAction = !gVrBikeCalibrationMenuVisible &&
		!gVrHolsterMenuVisible &&
		!gVrCalibrationMenuVisible &&
		!gVrVehicleMenuVisible &&
		(gVrMenuSelection == VR_MAIN_CALIBRATION ||
		 gVrMenuSelection == VR_MAIN_HOLSTERS ||
		 gVrMenuSelection == VR_MAIN_VEHICLE_SETTINGS);
	if(select && !gVrMenuSelectDown &&
	   (gVrBikeCalibrationMenuVisible || gVrHolsterMenuVisible ||
	    gVrCalibrationMenuVisible || gVrVehicleMenuVisible || mainAction))
		ChangeVrMenuValue(1);
	if(back && !gVrMenuBackDown){
		if(gVrBikeCalibrationMenuVisible)
			gVrBikeCalibrationMenuVisible = false;
		else if(gVrHolsterMenuVisible)
			gVrHolsterMenuVisible = false;
		else if(gVrCalibrationMenuVisible)
			gVrCalibrationMenuVisible = false;
		else if(gVrVehicleMenuVisible)
			gVrVehicleMenuVisible = false;
		else
			gVrMenuVisible = false;
	}
	gVrMenuVerticalDown = vertical;
	gVrMenuHorizontalDown = false;
	gVrMenuSelectDown = select;
	gVrMenuBackDown = back;
	gVrMenuDecreaseDown = decrease;
	gVrMenuIncreaseDown = increase;
}

void HandleCheatMenuInput(const XrVector2f &stick, bool select, bool back)
{
	const int count = GetVrCheatCount();
	const bool vertical = fabsf(stick.y) >= 0.65f;
	if(vertical && !gVrMenuVerticalDown && count > 0){
		gCheatMenuSelection += stick.y > 0.0f ? -1 : 1;
		if(gCheatMenuSelection < 0) gCheatMenuSelection = count-1;
		if(gCheatMenuSelection >= count) gCheatMenuSelection = 0;
	}
	if(select && !gVrMenuSelectDown){
		const bool activated = ActivateVrCheat(gCheatMenuSelection);
		debug("[OpenXR] Cheat %s: %s\n", GetVrCheatName(gCheatMenuSelection),
			activated ? "activated" : "not available in frontend");
	}
	if(back && !gVrMenuBackDown)
		gCheatMenuVisible = false;
	gVrMenuVerticalDown = vertical;
	gVrMenuHorizontalDown = false;
	gVrMenuSelectDown = select;
	gVrMenuBackDown = back;
}

void ToggleCheatMenu()
{
	gCheatMenuVisible = !gCheatMenuVisible;
	gVrMenuVisible = false;
	gVrVehicleMenuVisible = false;
	gVrHolsterMenuVisible = false;
	gVrCalibrationMenuVisible = false;
	gVrBikeCalibrationMenuVisible = false;
	gCheatMenuSelection = 0;
	gVrMenuVerticalDown = gVrMenuHorizontalDown = false;
	gVrMenuSelectDown = gVrMenuBackDown = false;
	gVrMenuDecreaseDown = gVrMenuIncreaseDown = false;
	debug("[OpenXR] Cheat menu: %s\n", gCheatMenuVisible ? "open" : "closed");
}

void ResetManualReloadState(ManualReloadState &state)
{
	state = ManualReloadState();
}

bool BuildManualReloadSpotMatrix(int slot, CMatrix *matrix)
{
	if(!matrix)
		return false;
	CMatrix holster;
	const bool assigned = BuildWeaponHolsterMatrix(slot, &holster);
	// The weapon leaves this body spot when it is grabbed.  Present the fresh
	// magazine at that now-empty spot, upright against the player's chest. If
	// the player edits this slot to EMPTY while still holding it, keep reload
	// usable at a fixed emergency chest point instead of losing the magazine.
	matrix->SetUnity();
	matrix->GetRight() = gBaseCamera.GetRight();
	matrix->GetUp() = gBaseCamera.GetUp();
	matrix->GetForward() = gBaseCamera.GetForward();
	matrix->GetPosition() = assigned ? holster.GetPosition() :
		gBaseCamera.GetPosition() + gBaseCamera.GetUp()*-0.34f +
		gBaseCamera.GetForward()*0.14f;
	return true;
}

bool BuildManualReloadHeldMatrix(int hand, CMatrix *matrix)
{
	if(!matrix || hand < 0 || hand >= EYE_COUNT ||
	   !gTrackedHandPoseValid[hand])
		return false;
	const XrPosef &pose = gTrackedHandPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	CVector right = ToGameVector(Rotate(pose.orientation, localRight));
	CVector up = ToGameVector(Rotate(pose.orientation, localUp));
	CVector forward = ToGameVector(Rotate(pose.orientation, localForward));
	right.Normalise();
	up.Normalise();
	forward.Normalise();
	matrix->SetUnity();
	// A magazine is pinched along the controller's pointing axis.  The exact
	// orientation is intentionally not part of the insertion test yet; position
	// is forgiving while the procedural hand/model alignment is still evolving.
	matrix->GetRight() = right;
	matrix->GetUp() = forward;
	matrix->GetForward() = up;
	matrix->GetPosition() = gBaseCamera.GetPosition() +
		ToGameVector(pose.position) + forward*0.035f - up*0.015f;
	return true;
}

bool BuildManualReloadSocketPosition(int weaponHand, CVector *position)
{
	if(!position || weaponHand < 0 || weaponHand >= EYE_COUNT)
		return false;
	const XrPosef *pose = nil;
	if(gTrackedHandAimPoseValid[weaponHand])
		pose = &gTrackedHandAimPose[weaponHand];
	else if(gTrackedHandPoseValid[weaponHand])
		pose = &gTrackedHandPose[weaponHand];
	if(!pose)
		return false;
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	CVector up = ToGameVector(Rotate(pose->orientation, localUp));
	CVector forward = ToGameVector(Rotate(pose->orientation, localForward));
	up.Normalise();
	forward.Normalise();
	// The calibrated weapon origin lies inside the gripping hand.  A generous
	// socket just below and slightly ahead of it matches the pistol/SMG magwell
	// without requiring weapon-specific DFF sockets in this first pass.
	*position = gBaseCamera.GetPosition() + ToGameVector(pose->position) -
		up*0.060f + forward*0.020f;
	return true;
}

bool IsMagazineHandInUse(int hand)
{
	for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++)
		if(gManualReload[weaponHand].active &&
		   gManualReload[weaponHand].magazineHand == hand)
			return true;
	return false;
}

uint32 UpdateManualReloadInput(float leftGrip, float rightGrip,
	uint32 blockedHands)
{
	const float grips[EYE_COUNT] = { leftGrip, rightGrip };
	uint32 capturedHands = 0;
	const ULONGLONG now = GetTickCount64();

	if(!ShouldUseManualReload()){
		for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++)
			ResetManualReloadState(gManualReload[weaponHand]);
		for(int hand = 0; hand < EYE_COUNT; hand++)
			gManualReloadGripDown[hand] = grips[hand] >= 0.45f;
		return 0;
	}

	// Reject stale targets before accepting any grip.  The final request is
	// validated again against the exact inventory slot by CPlayerPed.
	for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++){
		ManualReloadState &reload = gManualReload[weaponHand];
		if(reload.active &&
		   (gHeldWeaponSlot[weaponHand] != reload.slot ||
		    !gTrackedHandPoseValid[weaponHand]))
			ResetManualReloadState(reload);
		if(reload.active && reload.magazineHand >= 0)
			capturedHands |= 1u << reload.magazineHand;
	}

	// A held magazine follows either hand.  Moving it away from the body spot
	// once prevents an accidental instant reload when the gun is also near the
	// chest; bringing it into the magwell completes insertion immediately.
	for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++){
			ManualReloadState &reload = gManualReload[weaponHand];
			const int magazineHand = reload.magazineHand;
			if(!reload.active || reload.requested || magazineHand < 0 ||
			   (blockedHands & (1u << magazineHand)) != 0)
				continue;
			capturedHands |= 1u << magazineHand;
			if(grips[magazineHand] <= 0.30f){
				reload.magazineHand = -1;
				reload.movedAwayFromSpot = false;
				reload.grabbedAt = 0;
				debug("[OpenXR] Reload magazine returned to slot %d\n", reload.slot);
				continue;
			}
			CMatrix heldMatrix, spotMatrix;
			CVector socketPosition;
			if(!BuildManualReloadHeldMatrix(magazineHand, &heldMatrix) ||
			   !BuildManualReloadSpotMatrix(reload.slot, &spotMatrix) ||
			   !BuildManualReloadSocketPosition(weaponHand, &socketPosition))
				continue;
			if((heldMatrix.GetPosition()-spotMatrix.GetPosition()).Magnitude() >= 0.08f)
				reload.movedAwayFromSpot = true;
			if(reload.movedAwayFromSpot && now-reload.grabbedAt >= 150 &&
			   (heldMatrix.GetPosition()-socketPosition).Magnitude() <= 0.115f){
				reload.requested = true;
				debug("[OpenXR] Reload magazine inserted for %s hand slot %d\n",
					weaponHand == 0 ? "left" : "right", reload.slot);
			}
	}

	// A free hand can grab the closest available magazine.  Magazine grips
	// take priority over neighbouring weapon holsters.
	for(int hand = 0; hand < EYE_COUNT; hand++){
			if((capturedHands & (1u << hand)) != 0 ||
			   (blockedHands & (1u << hand)) != 0 ||
			   gHeldWeaponSlot[hand] >= 0 || !gTrackedHandPoseValid[hand] ||
			   grips[hand] < 0.65f || gManualReloadGripDown[hand] ||
			   IsMagazineHandInUse(hand))
				continue;
			const CVector handPosition = gBaseCamera.GetPosition() +
				ToGameVector(gTrackedHandPose[hand].position);
			float closestDistance = 0.20f;
			int closestWeaponHand = -1;
			for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++){
				ManualReloadState &reload = gManualReload[weaponHand];
				if(!reload.active || reload.requested || reload.magazineHand >= 0 ||
				   hand == weaponHand)
					continue;
				CMatrix spotMatrix;
				if(!BuildManualReloadSpotMatrix(reload.slot, &spotMatrix))
					continue;
				const float distance =
					(handPosition-spotMatrix.GetPosition()).Magnitude();
				if(distance < closestDistance){
					closestDistance = distance;
					closestWeaponHand = weaponHand;
				}
			}
			if(closestWeaponHand >= 0){
				ManualReloadState &reload = gManualReload[closestWeaponHand];
				reload.magazineHand = hand;
				reload.movedAwayFromSpot = false;
				reload.grabbedAt = now;
				capturedHands |= 1u << hand;
				debug("[OpenXR] Reload magazine grabbed by %s hand for slot %d\n",
					hand == 0 ? "left" : "right", reload.slot);
			}
	}

	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(grips[hand] <= 0.30f)
			gManualReloadGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gManualReloadGripDown[hand] = true;
	}
	return capturedHands;
}

bool IsWeaponSlotOwnedByOtherHand(int hand, int slot)
{
	for(int other = 0; other < EYE_COUNT; other++)
		if(other != hand && (gHeldWeaponSlot[other] == slot || gDroppedWeaponSlot[other] == slot))
			return true;
	return false;
}

void ResetPhysicalMeleeMotion(int hand);

void InvalidatePhysicalMeleeWeaponMatrix(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gTrackedWeaponContactMatrixSlot[hand] = -1;
	gTrackedWeaponContactMatrixType[hand] = -1;
	gTrackedWeaponContactMatrixFrame[hand] = 0;
}

void StartDroppedWeapon(int hand, int slot)
{
	if(hand < 0 || hand >= EYE_COUNT || slot < 0)
		return;
	if(!gTrackedHandPoseValid[hand]){
		InvalidatePhysicalMeleeWeaponMatrix(hand);
		ResetPhysicalMeleeMotion(hand);
		return;
	}
	ClearWeaponSupportForHand(hand);
	const XrPosef &pose = gTrackedHandPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	gDroppedWeaponMatrix[hand].SetUnity();
	gDroppedWeaponMatrix[hand].GetRight() = ToGameVector(Rotate(pose.orientation, localRight));
	gDroppedWeaponMatrix[hand].GetForward() = ToGameVector(Rotate(pose.orientation, localForward));
	gDroppedWeaponMatrix[hand].GetUp() = ToGameVector(Rotate(pose.orientation, localUp));
	gDroppedWeaponMatrix[hand].GetRight().Normalise();
	gDroppedWeaponMatrix[hand].GetForward().Normalise();
	gDroppedWeaponMatrix[hand].GetUp().Normalise();
	if(gTrackedWeaponRenderMatrixSlot[hand] == slot)
		gDroppedWeaponMatrix[hand] = gTrackedWeaponRenderMatrix[hand];
	gDroppedWeaponStartPosition[hand] = gTrackedWeaponRenderMatrixSlot[hand] == slot ?
		gDroppedWeaponMatrix[hand].GetPosition() :
		gBaseCamera.GetPosition()+ToGameVector(pose.position);
	gDroppedWeaponMatrix[hand].GetPosition() = gDroppedWeaponStartPosition[hand];
	gDroppedWeaponGravityUp[hand] = gBaseCamera.GetUp();
	gDroppedWeaponGravityUp[hand].Normalise();
	gDroppedWeaponLinearVelocity[hand] = ToGameVector(gTrackedHandLinearVelocity[hand]);
	const float linearSpeed = gDroppedWeaponLinearVelocity[hand].Magnitude();
	if(linearSpeed > 6.0f)
		gDroppedWeaponLinearVelocity[hand] *= 6.0f/linearSpeed;
	gDroppedWeaponAngularVelocity[hand] = ToGameVector(gTrackedHandAngularVelocity[hand]);
	const float angularSpeed = gDroppedWeaponAngularVelocity[hand].Magnitude();
	if(angularSpeed > 12.0f)
		gDroppedWeaponAngularVelocity[hand] *= 12.0f/angularSpeed;
	gDroppedWeaponSlot[hand] = slot;
	gDroppedWeaponStartTime[hand] = GetTickCount64();
	gTrackedWeaponRenderMatrixSlot[hand] = -1;
	InvalidatePhysicalMeleeWeaponMatrix(hand);
	ResetPhysicalMeleeMotion(hand);
}

enum { DROPPED_WEAPON_LIFETIME_MS = 1800 };

void ClearDroppedWeapon(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gDroppedWeaponSlot[hand] = -1;
	gDroppedWeaponStartTime[hand] = 0;
	gDroppedWeaponLinearVelocity[hand] = CVector(0.0f, 0.0f, 0.0f);
	gDroppedWeaponAngularVelocity[hand] = CVector(0.0f, 0.0f, 0.0f);
}

bool BuildDroppedWeaponMatrix(int hand, ULONGLONG now, CMatrix *matrix)
{
	if(!matrix || hand < 0 || hand >= EYE_COUNT ||
	   gDroppedWeaponSlot[hand] < 0 || gDroppedWeaponStartTime[hand] == 0)
		return false;
	const double elapsedMs = (double)(now-gDroppedWeaponStartTime[hand]);
	if(elapsedMs < 0.0 || elapsedMs >= DROPPED_WEAPON_LIFETIME_MS)
		return false;
	const float elapsed = (float)(elapsedMs/1000.0);
	*matrix = gDroppedWeaponMatrix[hand];
	const float angularSpeed = gDroppedWeaponAngularVelocity[hand].Magnitude();
	if(angularSpeed > 0.001f){
		CVector spinAxis = gDroppedWeaponAngularVelocity[hand];
		spinAxis *= 1.0f/angularSpeed;
		const float angle = angularSpeed*elapsed;
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(), spinAxis, angle);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(), spinAxis, angle);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(), spinAxis, angle);
	}
	// Vice City's world is deliberately small, so Earth-like gravity also reads
	// better at VR scale. Keep the 1.8 second catch window unchanged while making
	// the vertical fall 1.5x faster than the first toss implementation.
	const float fallDistance = 0.5f*4.2f*elapsed*elapsed;
	matrix->GetPosition() = gDroppedWeaponStartPosition[hand] +
		gDroppedWeaponLinearVelocity[hand]*elapsed -
		gDroppedWeaponGravityUp[hand]*fallDistance;
	return true;
}

void GiveWeaponToHand(int hand, int slot, const char *reason)
{
	if(hand < 0 || hand >= EYE_COUNT || slot < 0)
		return;
	ClearWeaponSupportForHand(hand);
	gHeldWeaponSlot[hand] = slot;
	gWeaponHolsterSelection[hand] = slot;
	gTrackedWeaponRenderMatrixSlot[hand] = -1;
	InvalidatePhysicalMeleeWeaponMatrix(hand);
	ResetPhysicalMeleeMotion(hand);
	// A deliberate holster/catch grab is already a safe neutral origin.  Arm the
	// first valid tracked sample immediately instead of requiring the player to
	// hold perfectly still after a respawn before the weapon starts working.
	gPhysicalMeleeFreshGrab[hand] = true;
	if(slot == WEAPONSLOT_PROJECTILE &&
	   GetVrWeaponTypeForSlot(slot) == WEAPONTYPE_DETONATOR_GRENADE){
		// C4 stays in the grabbing hand; its controller is calibrated and used
		// independently in the opposite hand.
		gTrackedDetonatorHand = 1-hand;
		gCalibrationEditHand = gTrackedDetonatorHand;
		gTrackedDetonatorWasActive[gTrackedDetonatorHand] = false;
	}else
		gCalibrationEditHand = hand;
	debug("[OpenXR] Weapon %s by %s hand slot %d\n",
		reason, hand == 0 ? "left" : "right", slot);
}

void UpdateWeaponHolsterInput(float leftGrip, float rightGrip, uint32 blockedHands)
{
	const ULONGLONG now = GetTickCount64();
	const float grips[EYE_COUNT] = { leftGrip, rightGrip };
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(gDroppedWeaponSlot[hand] >= 0 &&
		   (double)(now-gDroppedWeaponStartTime[hand]) >= DROPPED_WEAPON_LIFETIME_MS)
			ClearDroppedWeapon(hand);
	}
	if(FindPlayerVehicle() != nil &&
	   !IsVrDrivingActiveInternal()){
		for(int hand = 0; hand < EYE_COUNT; hand++){
			ClearWeaponSupportForHand(hand);
			gHeldWeaponSlot[hand] = -1;
			gTrackedWeaponRenderMatrixSlot[hand] = -1;
			ClearDroppedWeapon(hand);
			gWeaponHolsterGripDown[hand] = grips[hand] >= 0.45f;
		}
		return;
	}

	uint32 handledHands = blockedHands;
	// Maintain an existing support grip before considering a fresh transfer.
	// Releasing the support hand only detaches it. Releasing the primary while
	// the support hand remains squeezed promotes that hand to primary, which
	// makes natural hand-to-hand passes possible without dropping the weapon.
	for(int primary = 0; primary < EYE_COUNT; primary++){
		const int support = gWeaponSupportHand[primary];
		if(support < 0)
			continue;
		const int slot = gHeldWeaponSlot[primary];
		const int weaponType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if(slot < 0 || gHeldWeaponSlot[support] >= 0 ||
		   !IsTwoHandedWeaponTypeInternal(weaponType) ||
		   !gTrackedHandPoseValid[primary] || !gTrackedHandPoseValid[support]){
			gWeaponSupportHand[primary] = -1;
			continue;
		}
		if(grips[support] <= 0.30f){
			gWeaponSupportHand[primary] = -1;
			debug("[OpenXR] Support grip released from %s hand slot %d\n",
				support == 0 ? "left" : "right", slot);
			continue;
		}
		handledHands |= 1u << support;
		if(grips[primary] <= 0.30f && !gWeaponGripLockEnabled){
			gHeldWeaponSlot[primary] = -1;
			gWeaponHolsterSelection[primary] = -1;
			gTrackedWeaponRenderMatrixSlot[primary] = -1;
			gWeaponSupportHand[primary] = -1;
			GiveWeaponToHand(support, slot, "promoted from support to");
			handledHands |= 1u << primary;
		}
	}

	// A fresh squeeze at the saved foregrip socket creates a real two-handed
	// hold. The supporting hand never owns the inventory slot and therefore
	// cannot fire a duplicate shot or render a duplicate weapon.
	for(int primary = 0; primary < EYE_COUNT; primary++){
		const int support = 1-primary;
		const int slot = gHeldWeaponSlot[primary];
		const int weaponType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if(slot < 0 || gWeaponSupportHand[primary] >= 0 ||
		   gHeldWeaponSlot[support] >= 0 ||
		   (handledHands & (1u << support)) != 0 ||
		   !IsTwoHandedWeaponTypeInternal(weaponType) ||
		   !gTrackedHandPoseValid[support] || grips[support] < 0.65f ||
		   gWeaponHolsterGripDown[support])
			continue;
		CVector pivot, expected;
		if(!BuildSupportGripVector(primary, weaponType, &pivot, &expected))
			continue;
		const CVector supportPosition = gBaseCamera.GetPosition()+
			ToGameVector(gTrackedHandPose[support].position);
		if((supportPosition-(pivot+expected)).Magnitude() > 0.18f)
			continue;
		gWeaponSupportHand[primary] = support;
		gCalibrationEditHand = primary;
		handledHands |= 1u << support;
		debug("[OpenXR] Two-hand support grip: %s supports %s hand slot %d\n",
			support == 0 ? "left" : "right",
			primary == 0 ? "left" : "right", slot);
		if(grips[primary] <= 0.30f && !gWeaponGripLockEnabled){
			// Both edges can arrive in the same OpenXR sample (release primary,
			// squeeze support). Promote immediately so the generic drop pass below
			// cannot destroy the newly established hand-off.
			gHeldWeaponSlot[primary] = -1;
			gWeaponHolsterSelection[primary] = -1;
			gTrackedWeaponRenderMatrixSlot[primary] = -1;
			gWeaponSupportHand[primary] = -1;
			GiveWeaponToHand(support, slot, "promoted from fresh support to");
			handledHands |= 1u << primary;
		}
	}

	// A fresh squeeze close to the gun in the other hand performs a direct
	// hand-off.  It deliberately works while grip lock is enabled: the lock only
	// suppresses an accidental release, never an explicit second-hand grab.
	for(int receiver = 0; receiver < EYE_COUNT; receiver++){
		const int source = 1-receiver;
		if((handledHands & (1u << receiver)) != 0 ||
		   IsWeaponSupportHandInternal(receiver) ||
		   gHeldWeaponSlot[receiver] >= 0 || gHeldWeaponSlot[source] < 0 ||
		   !gTrackedHandPoseValid[receiver] || !gTrackedHandPoseValid[source] ||
		   grips[receiver] < 0.65f || gWeaponHolsterGripDown[receiver])
			continue;
		const CVector receiverPosition = gBaseCamera.GetPosition()+
			ToGameVector(gTrackedHandPose[receiver].position);
		const int sourceSlot = gHeldWeaponSlot[source];
		const CVector sourcePosition = gTrackedWeaponRenderMatrixSlot[source] == sourceSlot ?
			gTrackedWeaponRenderMatrix[source].GetPosition() :
			gBaseCamera.GetPosition()+ToGameVector(gTrackedHandPose[source].position);
		if((receiverPosition-sourcePosition).Magnitude() > 0.20f)
			continue;
		const int slot = gHeldWeaponSlot[source];
		CMatrix transferredMatrix;
		const bool hasTransferredMatrix =
			gTrackedWeaponRenderMatrixSlot[source] == slot;
		if(hasTransferredMatrix)
			transferredMatrix = gTrackedWeaponRenderMatrix[source];
		ClearWeaponSupportForHand(source);
		gHeldWeaponSlot[source] = -1;
		gTrackedWeaponRenderMatrixSlot[source] = -1;
		gWeaponHolsterSelection[source] = -1;
		GiveWeaponToHand(receiver, slot, "transferred to");
		if(hasTransferredMatrix){
			gTrackedWeaponRenderMatrix[receiver] = transferredMatrix;
			gTrackedWeaponRenderMatrixSlot[receiver] = slot;
		}
		handledHands |= (1u << source) | (1u << receiver);
	}

	// A flying gun can be caught with the other free hand.  Unlike a holster or
	// direct hand-off, the grip may already be held while waiting for the gun to
	// enter the catch radius, which makes actual toss-and-catch gestures reliable.
	for(int receiver = 0; receiver < EYE_COUNT; receiver++){
		const int source = 1-receiver;
		if((handledHands & (1u << receiver)) != 0 ||
		   IsWeaponSupportHandInternal(receiver) ||
		   gHeldWeaponSlot[receiver] >= 0 || grips[receiver] < 0.55f ||
		   !gTrackedHandPoseValid[receiver] || gDroppedWeaponSlot[source] < 0)
			continue;
		CMatrix flyingWeapon;
		if(!BuildDroppedWeaponMatrix(source, now, &flyingWeapon))
			continue;
		const CVector receiverPosition = gBaseCamera.GetPosition()+
			ToGameVector(gTrackedHandPose[receiver].position);
		if((receiverPosition-flyingWeapon.GetPosition()).Magnitude() > 0.22f)
			continue;
		const int slot = gDroppedWeaponSlot[source];
		ClearDroppedWeapon(source);
		GiveWeaponToHand(receiver, slot, "caught by");
		gTrackedWeaponRenderMatrix[receiver] = flyingWeapon;
		gTrackedWeaponRenderMatrixSlot[receiver] = slot;
		handledHands |= 1u << receiver;
	}

	for(int hand = 0; hand < EYE_COUNT; hand++){
		// A remote grenade grabbed by the previous hand in this same loop
		// immediately reserves its opposite controller hand.
		handledHands |= GetTrackedDetonatorHandMaskInternal();
		const float grip = grips[hand];
		if(IsWeaponSupportHandInternal(hand))
			handledHands |= 1u << hand;
		if((handledHands & (1u << hand)) != 0){
			// Keep the holster edge latch in sync while this grip belongs to a
			// magazine.  The still-held grip must not immediately grab a gun after
			// insertion; a fresh grab requires a real release first.
			if(grip <= 0.30f)
				gWeaponHolsterGripDown[hand] = false;
			else if(grip >= 0.65f)
				gWeaponHolsterGripDown[hand] = true;
			continue;
		}
		// Grip hysteresis prevents a slightly noisy analogue squeeze from dropping
		// a gun that the player is still physically holding.
		if(gHeldWeaponSlot[hand] >= 0 && grip <= 0.30f &&
		   !gWeaponGripLockEnabled){
			const int releasedSlot = gHeldWeaponSlot[hand];
			gHeldWeaponSlot[hand] = -1;
			StartDroppedWeapon(hand, releasedSlot);
			debug("[OpenXR] Weapon released from %s hand slot %d\n",
				hand == 0 ? "left" : "right", releasedSlot);
		}
		if(gHeldWeaponSlot[hand] < 0 && !IsWeaponSupportHandInternal(hand) &&
		   grip >= 0.65f &&
		   !gWeaponHolsterGripDown[hand] && gTrackedHandPoseValid[hand]){
			const CVector handPosition = gBaseCamera.GetPosition()+
				ToGameVector(gTrackedHandPose[hand].position);
			float closestDistance = 0.24f;
			int closestSlot = -1;
			for(int slot = WEAPONSLOT_MELEE; slot < TOTAL_WEAPON_SLOTS; slot++){
				if((gWeaponHolsterMask & (1u << slot)) == 0 ||
				   IsWeaponSlotOwnedByOtherHand(hand, slot))
					continue;
				CMatrix holster;
				if(!BuildWeaponHolsterMatrix(slot, &holster))
					continue;
				const float distance = (handPosition-holster.GetPosition()).Magnitude();
				if(distance < closestDistance){
					closestDistance = distance;
					closestSlot = slot;
				}
			}
			if(closestSlot >= 0){
				ClearDroppedWeapon(hand);
				GiveWeaponToHand(hand, closestSlot, "gripped from holster by");
			}
		}
		if(grip <= 0.30f)
			gWeaponHolsterGripDown[hand] = false;
		else if(grip >= 0.65f)
			gWeaponHolsterGripDown[hand] = true;
	}
}

float XrVectorLength(const XrVector3f &value)
{
	return sqrtf(value.x*value.x + value.y*value.y + value.z*value.z);
}

XrVector3f AddXrVector(const XrVector3f &left, const XrVector3f &right)
{
	return { left.x+right.x, left.y+right.y, left.z+right.z };
}

XrVector3f SubtractXrVector(const XrVector3f &left, const XrVector3f &right)
{
	return { left.x-right.x, left.y-right.y, left.z-right.z };
}

XrVector3f ScaleXrVector(const XrVector3f &value, float scale)
{
	return { value.x*scale, value.y*scale, value.z*scale };
}

XrVector3f CrossXrVector(const XrVector3f &left, const XrVector3f &right)
{
	return {
		left.y*right.z-left.z*right.y,
		left.z*right.x-left.x*right.z,
		left.x*right.y-left.y*right.x
	};
}

void ResetPhysicalMeleeMotion(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gPhysicalMeleeMotion[hand] = PhysicalMeleeMotion();
	gPhysicalMeleeStrike[hand].pending = false;
	gPhysicalMeleeFreshGrab[hand] = false;
}

bool IsPhysicalGameplayAvailable()
{
	CPlayerPed *player = FindPlayerPed();
	return player && player->m_rwObject && !player->DyingOrDead() &&
		CWorld::Players[CWorld::PlayerInFocus].m_WBState == WBSTATE_PLAYING;
}

bool IsTrackedScopeGameplaySafe()
{
	return gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!FrontEndMenuManager.m_bWantToRestart &&
		!FrontEndMenuManager.m_bWantToLoad &&
		!FrontEndMenuManager.m_bMenuActive &&
		IsPhysicalGameplayAvailable() && !CTimer::GetIsPaused() &&
		!CGame::playingIntro && !CCutsceneMgr::IsRunning() &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!gVrMenuVisible && !gCheatMenuVisible;
}

void ResetPhysicalWeaponStateForGameplayLoss(const float *grips)
{
	for(int hand = 0; hand < EYE_COUNT; hand++){
		ClearWeaponSupportForHand(hand);
		gHeldWeaponSlot[hand] = -1;
		gWeaponHolsterSelection[hand] = -1;
		gTrackedWeaponRenderMatrixSlot[hand] = -1;
		InvalidatePhysicalMeleeWeaponMatrix(hand);
		ClearDroppedWeapon(hand);
		ResetManualReloadState(gManualReload[hand]);
		gManualReloadGripDown[hand] = grips && grips[hand] >= 0.45f;
		// Preserve the physical edge latch. A grip still held through the death
		// screen must be released once before it can grab a hospital holster.
		gWeaponHolsterGripDown[hand] = grips && grips[hand] >= 0.45f;
		gTrackedWeaponTriggerPressed[hand] = false;
		gTrackedWeaponTriggerJustPressed[hand] = false;
		gTrackedWeaponTriggerJustReleased[hand] = false;
		gTrackedThrowablePreviewActive[hand] = false;
		gTrackedAimCacheValid[hand] = false;
		ResetPhysicalMeleeMotion(hand);
	}
	gWeaponHolsterMask = 0;
	gActiveTrackedFireAimValid = false;
	gTrackedScopeReticleTargetValid = false;
	// A cutscene or world-state transition can temporarily suspend physical
	// gameplay without removing planted C4. Preserve the remembered off-hand;
	// the authoritative projectile query decides whether it should reappear.
	ResetTrackedDetonatorInteraction(false);
	ResetImmersiveDrivingInteraction();
}

bool IsHandBusyWithReload(int hand)
{
	for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++)
		if(gManualReload[weaponHand].active &&
		   gManualReload[weaponHand].magazineHand == hand)
			return true;
	return false;
}

int GetTrackedRemoteGrenadeHandInternal()
{
	for(int hand = 0; hand < EYE_COUNT; hand++)
		if(gHeldWeaponSlot[hand] == WEAPONSLOT_PROJECTILE &&
		   GetVrWeaponTypeForSlot(WEAPONSLOT_PROJECTILE) ==
			WEAPONTYPE_DETONATOR_GRENADE)
			return hand;
	return -1;
}

bool HasTrackedRemoteChargesInternal()
{
	CPlayerPed *player = FindPlayerPed();
	return player && CProjectileInfo::HasDetonatorProjectile(player);
}

bool IsTrackedDetonatorHandReservedInternal(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT || !gVrHandsEnabled ||
	   !gSessionRunning || FindPlayerVehicle() != nil ||
	   !IsPhysicalGameplayAvailable())
		return false;
	const int grenadeHand = GetTrackedRemoteGrenadeHandInternal();
	if(grenadeHand < 0 && !HasTrackedRemoteChargesInternal())
		return false;
	const int desiredHand = grenadeHand >= 0 ? 1-grenadeHand :
		gTrackedDetonatorHand;
	return desiredHand == hand && gHeldWeaponSlot[hand] < 0 &&
		!IsWeaponSupportHandInternal(hand) && !IsHandBusyWithReload(hand);
}

bool IsTrackedDetonatorActiveInternal(int hand)
{
	return IsTrackedDetonatorHandReservedInternal(hand) &&
		gTrackedHandPoseValid[hand] && gTrackedHandAimPoseValid[hand];
}

uint32 GetTrackedDetonatorHandMaskInternal()
{
	uint32 mask = 0;
	for(int hand = 0; hand < EYE_COUNT; hand++)
		if(IsTrackedDetonatorHandReservedInternal(hand))
			mask |= 1u << hand;
	return mask;
}

void ResetTrackedDetonatorInteraction(bool clearCharges)
{
	if(clearCharges){
		gTrackedDetonatorHand = -1;
	}
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gTrackedDetonatorWasActive[hand] = false;
		gTrackedDetonatorWaitForTriggerRelease[hand] = false;
		gTrackedDetonatorTriggerJustPressed[hand] = false;
	}
}

void UpdateTrackedDetonatorTriggerInput(const float *triggers, bool blocked)
{
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const bool active = IsTrackedDetonatorActiveInternal(hand);
		gTrackedDetonatorTriggerJustPressed[hand] = false;
		if(!active){
			gTrackedDetonatorWasActive[hand] = false;
			gTrackedDetonatorWaitForTriggerRelease[hand] = false;
			continue;
		}
		const float trigger = triggers ? triggers[hand] : 0.0f;
		if(!gTrackedDetonatorWasActive[hand])
			gTrackedDetonatorWaitForTriggerRelease[hand] = trigger > 0.30f;
		gTrackedDetonatorWasActive[hand] = true;
		if(blocked){
			if(trigger > 0.30f)
				gTrackedDetonatorWaitForTriggerRelease[hand] = true;
			continue;
		}
		if(gTrackedDetonatorWaitForTriggerRelease[hand]){
			if(trigger <= 0.30f)
				gTrackedDetonatorWaitForTriggerRelease[hand] = false;
			continue;
		}
		gTrackedDetonatorTriggerJustPressed[hand] =
			gTrackedWeaponTriggerJustPressed[hand];
	}
}

float GetPhysicalMeleeReach(int weaponType)
{
	if(weaponType == WEAPONTYPE_UNARMED ||
	   weaponType == WEAPONTYPE_BRASSKNUCKLE)
		return 0.11f;
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo((eWeaponType)weaponType);
	if(!info)
		return 0.45f;
	// weapon.dat's forward fire offset is the authored contact reach (for
	// example 0.8 m for the bat and 1.2 m for the katana).
	return Max(0.18f, Abs(info->m_vecFireOffset.y));
}

CVector GetPhysicalMeleeModelTip(int weaponType)
{
	// Measured from the shipped DFF bounds. Vice City's hand-held melee models
	// extend along local +Z (the final RenderWare matrix's Up axis), not along
	// weapon.dat's animated-ped fire-offset axis. This makes contact follow the
	// visible calibrated blade/club exactly.
	switch(weaponType){
	case WEAPONTYPE_SCREWDRIVER: return CVector(0.160f, -0.017f, 0.277f);
	case WEAPONTYPE_GOLFCLUB: return CVector(0.029f, 0.0f, 0.848f);
	case WEAPONTYPE_NIGHTSTICK: return CVector(0.050f, 0.030f, 0.451f);
	case WEAPONTYPE_KNIFE: return CVector(0.133f, 0.040f, 0.416f);
	case WEAPONTYPE_BASEBALLBAT: return CVector(0.068f, -0.019f, 0.680f);
	case WEAPONTYPE_HAMMER: return CVector(0.045f, 0.036f, 0.317f);
	case WEAPONTYPE_CLEAVER: return CVector(0.075f, 0.036f, 0.270f);
	case WEAPONTYPE_MACHETE: return CVector(0.079f, 0.023f, 0.572f);
	// Authored katana.dff tip.  The whole blade is offset from the model origin;
	// tracing local +Z made the physical edge sit visibly below the rendered one.
	case WEAPONTYPE_KATANA: return CVector(0.0468f, -0.0246f, 0.9847f);
	case WEAPONTYPE_CHAINSAW: return CVector(0.865f, 0.033f, 0.335f);
	default: return CVector(0.0f, 0.0f, GetPhysicalMeleeReach(weaponType));
	}
}

CVector GetPhysicalMeleeModelRoot(int weaponType)
{
	// Start immediately beyond each authored handle/guard. The DFF meshes are
	// offset several centimetres from their frame origins, so a generic zero root
	// makes the collision capsule visibly miss the bat, blade or chainsaw bar.
	switch(weaponType){
	case WEAPONTYPE_SCREWDRIVER: return CVector(0.078f, 0.027f, 0.0f);
	case WEAPONTYPE_GOLFCLUB: return CVector(0.064f, 0.035f, 0.0f);
	case WEAPONTYPE_NIGHTSTICK: return CVector(0.081f, 0.030f, 0.0f);
	case WEAPONTYPE_KNIFE: return CVector(0.089f, 0.038f, 0.0f);
	case WEAPONTYPE_BASEBALLBAT: return CVector(0.066f, 0.035f, 0.0f);
	case WEAPONTYPE_HAMMER: return CVector(0.078f, 0.035f, 0.0f);
	case WEAPONTYPE_CLEAVER: return CVector(0.073f, 0.036f, 0.0f);
	case WEAPONTYPE_MACHETE: return CVector(0.073f, 0.018f, 0.0f);
	case WEAPONTYPE_KATANA: return CVector(0.0725f, 0.0245f, 0.0467f);
	case WEAPONTYPE_CHAINSAW: return CVector(0.485f, 0.038f, 0.160f);
	default: return CVector(0.0f, 0.0f, 0.0f);
	}
}

CVector ToPhysicalMeleeTrackingSpace(const CVector &worldPoint)
{
	CVector right = gBaseCamera.GetRight();
	CVector forward = gBaseCamera.GetForward();
	CVector up = gBaseCamera.GetUp();
	right.Normalise();
	forward.Normalise();
	up.Normalise();
	const CVector relative = worldPoint-gBaseCamera.GetPosition();
	return CVector(DotProduct(relative, right), DotProduct(relative, forward),
		DotProduct(relative, up));
}

void UpdatePhysicalMeleeInput(uint32 blockedHands)
{
	const uint32 now = CTimer::GetTimeInMillisecondsNonClipped();
	const uint32 frame = CTimer::GetFrameCounter();
	const float dt = Max(0.001f, CTimer::GetTimeStepNonClippedInSeconds());
	const bool playerUnavailable = !IsPhysicalGameplayAvailable();
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(gPhysicalMeleeStrike[hand].pending &&
		   frame-gPhysicalMeleeStrike[hand].frame > 1U)
			gPhysicalMeleeStrike[hand].pending = false;
		if(playerUnavailable || (blockedHands & (1u << hand)) != 0 ||
		   gVrMenuVisible || gCheatMenuVisible ||
		   FindPlayerVehicle() != nil || !gVrHandsEnabled ||
		   !gTrackedHandPoseValid[hand] ||
		   CTimer::GetIsPaused() || CGame::playingIntro ||
		   CCutsceneMgr::IsRunning() || CCutsceneMgr::IsCutsceneProcessing() ||
		   IsWeaponSupportHandInternal(hand) ||
		   IsHandBusyWithReload(hand)){
			ResetPhysicalMeleeMotion(hand);
			continue;
		}

		int slot = gHeldWeaponSlot[hand];
		int weaponType = -1;
		bool strikeEnabled = false;
		if(slot >= 0){
			weaponType = GetVrWeaponTypeForSlot(slot);
			strikeEnabled = IsPhysicalMeleeTypeInternal(weaponType);
		}else{
			slot = WEAPONSLOT_UNARMED;
			weaponType = GetVrWeaponTypeForSlot(slot);
			// A real fist closes both the three grip fingers and the index finger.
			strikeEnabled = IsPhysicalMeleeTypeInternal(weaponType) &&
				gTrackedHandGrip[hand] >= 0.65f &&
				gTrackedHandTrigger[hand] >= 0.45f;
		}
		if(!IsPhysicalMeleeTypeInternal(weaponType)){
			ResetPhysicalMeleeMotion(hand);
			continue;
		}

		const XrPosef &pose = gTrackedHandAimPoseValid[hand] ?
			gTrackedHandAimPose[hand] : gTrackedHandPose[hand];
		const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
		XrVector3f forward = Rotate(pose.orientation, localForward);
		const float reach = GetPhysicalMeleeReach(weaponType);
		const XrVector3f reachVector = ScaleXrVector(forward, reach);
		const XrVector3f fallbackPoint = AddXrVector(pose.position, reachVector);
		const CVector basePosition = gBaseCamera.GetPosition();
		// Keep controller/handle motion separate from the authored damaging blade.
		// The former drives gesture velocity; the latter is stored in the world
		// sweep so calibration/model offsets cannot move collision off the mesh.
		CVector worldMotionRoot = basePosition+ToGameVector(pose.position);
		CVector worldBladeRoot = worldMotionRoot;
		CVector worldPoint = basePosition+ToGameVector(fallbackPoint);
		// The renderer exposes the exact final matrix after hand calibration,
		// authored model-axis conversion and the optional two-hand transform.
		// Using its authored fire offset keeps the physical blade/bat tip attached
		// to the visible model instead of an uncalibrated controller ray.
		bool usedContactMatrix = false;
		if(slot != WEAPONSLOT_UNARMED &&
		   gTrackedWeaponContactMatrixSlot[hand] == slot &&
		   gTrackedWeaponContactMatrixType[hand] == weaponType &&
		   frame-gTrackedWeaponContactMatrixFrame[hand] <= 2U){
			worldMotionRoot = gTrackedWeaponContactMatrix[hand].GetPosition();
			worldBladeRoot = gTrackedWeaponContactMatrix[hand]*
				GetPhysicalMeleeModelRoot(weaponType);
			worldPoint = gTrackedWeaponContactMatrix[hand]*
				GetPhysicalMeleeModelTip(weaponType);
			usedContactMatrix = true;
		}
		// Keep a second copy relative to the gameplay camera origin. The world
		// positions are required for collision, but include Tommy's locomotion and
		// walking bob; those must never count as a physical controller swing.
		const CVector trackingRoot = ToPhysicalMeleeTrackingSpace(worldMotionRoot);
		const CVector trackingPoint = ToPhysicalMeleeTrackingSpace(worldPoint);
		const int supportHand = slot == WEAPONSLOT_UNARMED ? -1 :
			gWeaponSupportHand[hand];

		PhysicalMeleeMotion &motion = gPhysicalMeleeMotion[hand];
		if(!motion.valid || motion.slot != slot ||
		   motion.weaponType != weaponType ||
		   motion.usedContactMatrix != usedContactMatrix ||
		   motion.supportHand != supportHand ||
		   frame-motion.previousFrame > 2U || dt > 0.05f){
			const bool armFreshGrab = gPhysicalMeleeFreshGrab[hand] &&
				usedContactMatrix;
			motion = PhysicalMeleeMotion();
			motion.valid = true;
			motion.slot = slot;
			motion.weaponType = weaponType;
			motion.usedContactMatrix = usedContactMatrix;
			motion.supportHand = supportHand;
			motion.previousFrame = frame;
			motion.calmSinceTime = now;
			motion.previousWorldPoint = worldPoint;
			motion.previousWorldRoot = worldBladeRoot;
			motion.previousTrackingPoint = trackingPoint;
			motion.previousTrackingRoot = trackingRoot;
			motion.armed = armFreshGrab;
			continue;
		}

		const CVector trackingTravelled =
			trackingPoint-motion.previousTrackingPoint;
		const float trackingTravelDistance = trackingTravelled.Magnitude();
		const float speed = trackingTravelDistance/dt;
		const CVector trackingRootTravelled =
			trackingRoot-motion.previousTrackingRoot;
		const float rootSpeed = trackingRootTravelled.Magnitude()/dt;
		const CVector previousBlade = motion.previousTrackingPoint-
			motion.previousTrackingRoot;
		const CVector currentBlade = trackingPoint-trackingRoot;
		const float angularTipSpeed = (currentBlade-previousBlade).Magnitude()/dt;
		const float strikeSpeed = slot == WEAPONSLOT_UNARMED ? 1.25f : 0.70f;
		const bool calmForRearm = slot == WEAPONSLOT_UNARMED ?
			speed <= 0.48f : speed <= 0.40f && rootSpeed <= 0.30f;
		if(calmForRearm){
			if(motion.calmSinceTime == 0)
				motion.calmSinceTime = now;
			if(slot == WEAPONSLOT_UNARMED ||
			   now-motion.calmSinceTime >= 70U)
				motion.armed = true;
		}else{
			motion.calmSinceTime = 0;
		}
		// A collision (especially against a moving vehicle) can leave the tracked
		// tip above the calm threshold indefinitely because of tiny controller and
		// contact-matrix jitter. Never allow one resolved hit to permanently lock
		// melee input. Player locomotion has already been removed from `speed`, so
		// this fallback cannot bring back passive hits while merely walking.
		if(!motion.armed && motion.lastStrikeTime != 0 &&
		   now-motion.lastStrikeTime >= 450U){
			motion.armed = true;
			motion.calmSinceTime = 0;
		}

		// Large discontinuities are tracking/recenter events, never superhuman
		// attacks. Keep the new sample but require a calm frame before rearming.
		if(trackingTravelDistance > 0.65f){
			motion.armed = false;
			motion.previousWorldPoint = worldPoint;
			motion.previousWorldRoot = worldBladeRoot;
			motion.previousTrackingPoint = trackingPoint;
			motion.previousTrackingRoot = trackingRoot;
			motion.previousFrame = frame;
			continue;
		}

		// Require motion produced by the tracked hand or by deliberate blade
		// rotation. Merely walking Tommy (or an NPC walking into a stationary
		// weapon) changes world coordinates but cannot satisfy this local gesture.
		const bool deliberateMotion = slot == WEAPONSLOT_UNARMED ?
			rootSpeed >= 0.75f : rootSpeed >= 0.25f || angularTipSpeed >= 0.50f;
		if(motion.strikeInProgress && now > motion.strikeContinueUntil){
			motion.strikeInProgress = false;
			motion.strikePeakSpeed = 0.0f;
		}
		const bool fastStrikeSample = strikeEnabled && motion.armed &&
			speed >= strikeSpeed && deliberateMotion &&
			now-motion.lastStrikeTime >= 220U;
		if(fastStrikeSample && weaponType == WEAPONTYPE_KATANA){
			motion.strikeInProgress = true;
			motion.strikePeakSpeed = Max(motion.strikePeakSpeed, speed);
			motion.strikeContinueUntil = now+180U;
		}
		// Once a genuine fast swing has started, keep publishing its slower tail.
		// The broad torso hull can be entered first while the visible edge reaches
		// the head a few frames later; head classification must see those frames.
		const bool continuationSample = weaponType == WEAPONTYPE_KATANA &&
			motion.strikeInProgress &&
			now <= motion.strikeContinueUntil && speed >= 0.08f;
		if(strikeEnabled && motion.armed &&
		   (fastStrikeSample || continuationSample) &&
		   !gPhysicalMeleeStrike[hand].pending){
			PhysicalMeleeStrike &strike = gPhysicalMeleeStrike[hand];
			strike.pending = true;
			strike.slot = slot;
			strike.weaponType = weaponType;
			strike.sweepStart = motion.previousWorldPoint;
			strike.sweepEnd = worldPoint;
			strike.rootStart = motion.previousWorldRoot;
			strike.rootEnd = worldBladeRoot;
			strike.speed = Max(speed, motion.strikePeakSpeed);
			strike.frame = frame;
		}
		motion.previousWorldPoint = worldPoint;
		motion.previousWorldRoot = worldBladeRoot;
		motion.previousTrackingPoint = trackingPoint;
		motion.previousTrackingRoot = trackingRoot;
		motion.previousFrame = frame;
		if(gPhysicalMeleeFreshGrab[hand] && usedContactMatrix)
			gPhysicalMeleeFreshGrab[hand] = false;
	}
}

bool ApplyTouchInput(CControllerState *state)
{
	if(!state || !gSession || !gSessionRunning || !PollEvents()) return false;
	XrActiveActionSet active = { gActions.set, XR_NULL_PATH };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	sync.countActiveActionSets=1; sync.activeActionSets=&active;
	if(XR_FAILED(xrSyncActions(gSession, &sync))) return false;
	XrVector2f sticks[EYE_COUNT] = {};
	const bool leftActive = ReadVector(gActions.stick, 0, sticks[0]);
	const bool rightActive = ReadVector(gActions.stick, 1, sticks[1]);
	const bool connected = leftActive || rightActive;
	if(connected != gTouchWasConnected){
		debug("[OpenXR] Touch controllers %s\n", connected ? "connected" : "disconnected");
		if(connected) debug("[OpenXR] Shortcuts: grips+Menu VR settings, grips+Y profiler, grips+A debug, grips+B cheats, grips+right stick left/right weapons, grips+left stick click+left trigger SPS mode, grips+right stick click+right trigger VRS, grips+both sticks recenter\n");
		gTouchWasConnected = connected;
	}
	if(!connected){
		for(int hand = 0; hand < EYE_COUNT; hand++){
			ClearWeaponSupportForHand(hand);
			gTrackedWeaponTriggerPressed[hand]=false;
			gTrackedWeaponTriggerJustPressed[hand]=false;
			gTrackedWeaponTriggerJustReleased[hand]=false;
			gTrackedThrowablePreviewActive[hand]=false;
			gHeldWeaponSlot[hand]=-1;
			gTrackedWeaponRenderMatrixSlot[hand]=-1;
			ClearDroppedWeapon(hand);
			ResetManualReloadState(gManualReload[hand]);
			gManualReloadGripDown[hand]=false;
			ResetPhysicalMeleeMotion(hand);
		}
		// Preserve already planted charges across a transient controller loss, but
		// require a fresh R2 press after tracking returns.
		ResetTrackedDetonatorInteraction(false);
		ResetImmersiveDrivingInteraction();
		return false;
	}
	const float leftGrip=ReadFloat(gActions.squeeze,0), rightGrip=ReadFloat(gActions.squeeze,1);
	const float leftTrigger=ReadFloat(gActions.trigger,0), rightTrigger=ReadFloat(gActions.trigger,1);
	gTrackedHandGrip[0]=leftGrip; gTrackedHandGrip[1]=rightGrip;
	gTrackedHandTrigger[0]=leftTrigger; gTrackedHandTrigger[1]=rightTrigger;
	const float physicalGrips[EYE_COUNT] = { leftGrip, rightGrip };
	const bool physicalGameplayAvailable = IsPhysicalGameplayAvailable();
	if(!physicalGameplayAvailable){
		if(gPhysicalGameplayWasAvailable)
			ResetPhysicalWeaponStateForGameplayLoss(physicalGrips);
		else{
			// Keep latches synchronized throughout the death screen so a released
			// grip is ready for a deliberate new squeeze after respawn.
			for(int hand = 0; hand < EYE_COUNT; hand++)
				gWeaponHolsterGripDown[hand] = physicalGrips[hand] >= 0.45f;
		}
	}
	gPhysicalGameplayWasAvailable = physicalGameplayAvailable;
	const bool a=ReadBool(gActions.a), b=ReadBool(gActions.b);
	const bool x=ReadBool(gActions.x), y=ReadBool(gActions.y);
	const bool leftStickClick=ReadBool(gActions.stickClick,0);
	const bool rightStickClick=ReadBool(gActions.stickClick,1);
	const bool menu=ReadBool(gActions.menu);
	// Physical grabs win over a plain two-grip squeeze, so two holsters can be
	// grabbed simultaneously. An explicit service button still wins, preserving
	// access to the VR/debug menus even when both hands happen to be near slots.
	// Trigger-only two-grip chords conflict with making two real fists and with
	// punching while the other hand holds a weapon. Keep service shortcuts
	// explicit: trigger diagnostics now also require their matching stick click.
	const bool serviceChord = leftGrip >= 0.75f && rightGrip >= 0.75f &&
		(menu || a || b || y || (leftStickClick && rightStickClick) ||
		 (leftStickClick && leftTrigger >= 0.75f) ||
		 (rightStickClick && rightTrigger >= 0.75f));
	const bool vrRadioPressed =
		FindPlayerVehicle() != nil &&
		!gVrMenuVisible && !gCheatMenuVisible &&
		!serviceChord && x;
	gVrRadioChangeJustPressed =
		vrRadioPressed && !gVrRadioButtonDown;
	gVrRadioButtonDown = vrRadioPressed;
	const uint32 allTrackedHands = (1u << EYE_COUNT)-1u;
	uint32 trackedDetonatorHands = GetTrackedDetonatorHandMaskInternal();
	uint32 vehicleCapturedHands = UpdateImmersiveBikeInput(physicalGrips,
		(serviceChord || gVrMenuVisible || gCheatMenuVisible) ?
			allTrackedHands : 0);
	vehicleCapturedHands |= UpdateImmersiveCarInput(physicalGrips,
		(serviceChord || gVrMenuVisible || gCheatMenuVisible) ?
			allTrackedHands : 0);
	UpdateMotionDrivingInput();
	uint32 reloadCapturedHands = 0;
	if(physicalGameplayAvailable && !gVrMenuVisible && !gCheatMenuVisible){
		reloadCapturedHands = UpdateManualReloadInput(
			leftGrip, rightGrip,
			(serviceChord ? allTrackedHands : trackedDetonatorHands) |
				vehicleCapturedHands);
		if(!serviceChord)
			UpdateWeaponHolsterInput(leftGrip, rightGrip,
				reloadCapturedHands | trackedDetonatorHands |
				vehicleCapturedHands);
	}
	// A grab can make a remote grenade (and therefore its opposite-hand
	// controller) active during UpdateWeaponHolsterInput.
	trackedDetonatorHands = GetTrackedDetonatorHandMaskInternal();
	const bool modifier=gHeldWeaponSlot[0] < 0 && gHeldWeaponSlot[1] < 0 &&
		trackedDetonatorHands == 0 &&
		leftGrip>=0.75f && rightGrip>=0.75f;
	const float triggers[EYE_COUNT] = { leftTrigger, rightTrigger };
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const bool weaponTriggerPressed = !modifier &&
			(gTrackedWeaponTriggerPressed[hand] ? triggers[hand] >= 0.45f : triggers[hand] >= 0.55f);
		gTrackedWeaponTriggerJustReleased[hand]=!weaponTriggerPressed &&
			gTrackedWeaponTriggerPressed[hand];
		gTrackedWeaponTriggerJustPressed[hand]=weaponTriggerPressed &&
			!gTrackedWeaponTriggerPressed[hand];
		gTrackedWeaponTriggerPressed[hand]=weaponTriggerPressed;
	}
	UpdatePhysicalMeleeInput(serviceChord ? allTrackedHands :
		(trackedDetonatorHands | vehicleCapturedHands));
	const bool vrMenuShortcut=serviceChord && menu;
	const bool vrMenuToggled=vrMenuShortcut && !gTouchVrMenuShortcutDown;
	if(vrMenuToggled){
		gVrMenuVisible=!gVrMenuVisible;
		gVrVehicleMenuVisible=false;
		gVrHolsterMenuVisible=false;
		gVrCalibrationMenuVisible=false;
		gVrBikeCalibrationMenuVisible=false;
		gCheatMenuVisible=false;
		if(IsTrackedDetonatorHandReservedInternal(0))
			gCalibrationEditHand = 0;
		else if(IsTrackedDetonatorHandReservedInternal(1))
			gCalibrationEditHand = 1;
		else if(gHeldWeaponSlot[0] >= 0 && gHeldWeaponSlot[1] < 0)
			gCalibrationEditHand = 0;
		else if(gHeldWeaponSlot[1] >= 0 && gHeldWeaponSlot[0] < 0)
			gCalibrationEditHand = 1;
		gVrMenuSelection=0;
		gVrMenuVerticalDown=gVrMenuHorizontalDown=false;
		gVrMenuSelectDown=gVrMenuBackDown=false;
		gVrMenuDecreaseDown=gVrMenuIncreaseDown=false;
		debug("[OpenXR] VR settings: %s\n",gVrMenuVisible?"open":"closed");
	}
	gTouchVrMenuShortcutDown=vrMenuShortcut;
	const bool cheatMenuShortcut=modifier && b;
	const bool cheatMenuToggled=cheatMenuShortcut && !gTouchWeatherShortcutDown;
	if(cheatMenuToggled){
		gCheatMenuVisible=!gCheatMenuVisible;
		gVrMenuVisible=false;
		gVrVehicleMenuVisible=false;
		gVrHolsterMenuVisible=false;
		gVrCalibrationMenuVisible=false;
		gVrBikeCalibrationMenuVisible=false;
		gCheatMenuSelection=0;
		gVrMenuVerticalDown=gVrMenuHorizontalDown=false;
		gVrMenuSelectDown=false; gVrMenuBackDown=b;
		gVrMenuDecreaseDown=gVrMenuIncreaseDown=false;
		debug("[OpenXR] Cheat menu: %s\n",gCheatMenuVisible?"open":"closed");
	}
	gTouchWeatherShortcutDown=cheatMenuShortcut;
	UpdateTrackedDetonatorTriggerInput(triggers,
		serviceChord || gVrMenuVisible || gCheatMenuVisible ||
		!physicalGameplayAvailable);
	if(gVrMenuVisible){
		for(int hand = 0; hand < EYE_COUNT; hand++){
			gTrackedWeaponTriggerPressed[hand]=false;
			gTrackedWeaponTriggerJustPressed[hand]=false;
			gTrackedWeaponTriggerJustReleased[hand]=false;
			gTrackedThrowablePreviewActive[hand]=false;
		}
		HandleVrMenuInput(sticks[0], leftTrigger>=0.55f,
			rightTrigger>=0.55f, a, b);
		return true;
	}
	if(gCheatMenuVisible){
		for(int hand = 0; hand < EYE_COUNT; hand++){
			gTrackedWeaponTriggerPressed[hand]=false;
			gTrackedWeaponTriggerJustPressed[hand]=false;
			gTrackedWeaponTriggerJustReleased[hand]=false;
			gTrackedThrowablePreviewActive[hand]=false;
		}
		HandleCheatMenuInput(sticks[0],a,b);
		return true;
	}
	if(vrMenuToggled || cheatMenuToggled)
		return true;
	const bool perfShortcut=modifier && y;
	const bool debugShortcut=modifier && a;
	const bool spsShortcut=modifier && leftStickClick && leftTrigger>=0.75f;
	bool vrsShortcut=false;
#ifdef RW_D3D12
	vrsShortcut=modifier && rightStickClick && rightTrigger>=0.75f;
#endif
	const bool recenterShortcut=modifier && leftStickClick && rightStickClick;
	const bool weaponStickHorizontal=fabsf(sticks[1].x)>=0.65f &&
		fabsf(sticks[1].x)>fabsf(sticks[1].y);
	const bool weaponLeftShortcut=!gVrHandsEnabled && modifier && FindPlayerVehicle()==nil &&
		weaponStickHorizontal && sticks[1].x<0.0f;
	const bool weaponRightShortcut=!gVrHandsEnabled && modifier && FindPlayerVehicle()==nil &&
		weaponStickHorizontal && sticks[1].x>0.0f;
	if(perfShortcut && !gTouchPerfShortcutDown) TogglePerfRecording();
	if(debugShortcut && !gTouchDebugShortcutDown){
		gDebugVisible=!gDebugVisible;
		debug("[OpenXR] Debug overlay: %s\n",gDebugVisible?"visible":"hidden");
	}
	if(spsShortcut && !gTouchSpsShortcutDown){
		gFullStereoSinglePass=!gFullStereoSinglePass;
		debug("[OpenXR] Stereo tail mode: %s\n",
			gFullStereoSinglePass?"full SPS":"hybrid packet replay");
	}
#ifdef RW_D3D12
	if(vrsShortcut && !gTouchVrsShortcutDown){
		gFixedFoveatedProfile = (gFixedFoveatedProfile+1) %
			rw::d3d12::FIXED_FOVEATED_PROFILE_COUNT;
		rw::d3d12::setFixedFoveatedRenderingProfile(gFixedFoveatedProfile);
		rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
		rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
		SaveVrSetting("VRS", gFixedFoveatedProfile);
		debug("[OpenXR] Fixed foveated rendering profile %u (%s, Tier %u, tile %u)\n",
			foveatedInfo.profile, foveatedInfo.supported ? "ready" : "unsupported",
			foveatedInfo.tier, foveatedInfo.tileSize);
	}
#endif
	if(recenterShortcut && !gTouchRecenterShortcutDown){
		gRecenterRequested=true;
		gCinemaAnchorValid=false;
		debug("[OpenXR] Gameplay view recenter requested\n");
	}
	gTouchPerfShortcutDown=perfShortcut;
	gTouchDebugShortcutDown=debugShortcut;
	gTouchSpsShortcutDown=spsShortcut;
	gTouchVrsShortcutDown=vrsShortcut;
	gTouchRecenterShortcutDown=recenterShortcut;
	MergeAxis(state->LeftStickX, AxisValue(sticks[0].x));
	MergeAxis(state->LeftStickY, AxisValue(-sticks[0].y));
	if(!weaponLeftShortcut && !weaponRightShortcut){
		MergeAxis(state->RightStickX, AxisValue(sticks[1].x));
		MergeAxis(state->RightStickY, AxisValue(-sticks[1].y));
	}
	MergeButton(state->LeftShoulder2, weaponLeftShortcut);
	MergeButton(state->RightShoulder2, weaponRightShortcut);
	const int padMode=CPad::GetPad(0)->GetMode();
	if(gImmersiveCarHornPressed){
		switch(padMode){
		case 1:
			state->LeftShoulder1 =
				Max(state->LeftShoulder1, (int16)255);
			break;
		case 2:
			state->RightShoulder1 =
				Max(state->RightShoulder1, (int16)255);
			break;
		default:
			state->LeftShock = Max(state->LeftShock, (int16)255);
			break;
		}
	}
	if(!modifier){
		if(gHeldWeaponSlot[0] < 0 && !IsWeaponSupportHandInternal(0) &&
		   (reloadCapturedHands & 1u) == 0 &&
		   (trackedDetonatorHands & 1u) == 0 &&
		   (vehicleCapturedHands & 1u) == 0 &&
		   !IsVrCarDrivingActiveInternal())
			state->LeftShoulder1=Max(state->LeftShoulder1,TriggerValue(leftGrip));
		// A grip holding a physical weapon belongs exclusively to that hand.
		if(padMode != 3 && gHeldWeaponSlot[1] < 0 &&
		   !IsWeaponSupportHandInternal(1) &&
		   (reloadCapturedHands & 2u) == 0 &&
		   (trackedDetonatorHands & 2u) == 0 &&
		   (vehicleCapturedHands & 2u) == 0 &&
		   !IsVrCarDrivingActiveInternal())
			state->RightShoulder1=Max(state->RightShoulder1,TriggerValue(rightGrip));
	}
	const bool inVehicle = FindPlayerVehicle() != nil;
	if(!spsShortcut && (inVehicle || !gVrHandsEnabled))
		state->Square=Max(state->Square,TriggerValue(leftTrigger));
	if(!vrsShortcut){
		const int16 rightTriggerValue=TriggerValue(rightTrigger);
		if(inVehicle){
			// Keep the original accelerator binding while driving.
			state->Cross=Max(state->Cross,rightTriggerValue);
		}else if(!gVrHandsEnabled){
			const int weaponType = GetVrCurrentWeaponType();
			const bool directVrFirearm =
				(weaponType >= WEAPONTYPE_COLT45 && weaponType <= WEAPONTYPE_MINIGUN) ||
				weaponType == WEAPONTYPE_HELICANNON;
			if(!directVrFirearm){
				// Melee, thrown weapons, detonator and camera still use their
				// legacy action state until their physical VR interactions exist.
				switch(padMode){
				case 2: state->Cross=Max(state->Cross,rightTriggerValue); break;
				case 3: state->RightShoulder1=Max(state->RightShoulder1,rightTriggerValue); break;
				default: state->Circle=Max(state->Circle,rightTriggerValue); break;
				}
			}
		}
		// Firearms are deliberately not translated to a legacy pad button.
		// CPlayerPed consumes their OpenXR trigger directly, so SetAttack cannot
		// produce a second delayed bullet from the same physical press.
	}
	if(!modifier){
		// Suppress whichever face button the selected classic control mode
		// treats as fire. Other face-button actions remain available.
		if(padMode != 2) MergeButton(state->Cross,a);
		if(padMode != 0 && padMode != 1) MergeButton(state->Circle,b);
		// X is the dedicated radio button in every VR vehicle. L2 still feeds
		// Square above, so braking/reverse remains available in every pad mode.
		if(!inVehicle)
			MergeButton(state->Square,x);
		MergeButton(state->Triangle,y);
	}
	if(!modifier){
		MergeButton(state->LeftShock,leftStickClick);
		if(inVehicle)
			MergeButton(state->RightShock,rightStickClick);
		else if(rightStickClick){
			// R3 is sprint on foot. Do not also pass it as RightShock there:
			// Vice City maps that state to look-behind.
			if(padMode == 2)
				state->Circle = Max(state->Circle, (int16)255);
			else
				state->Cross = Max(state->Cross, (int16)255);
		}
	}
	if(!vrMenuShortcut)
		MergeButton(state->Start,menu);
	return true;
}

bool BeginStereoFrame(RwCamera *camera, const CMatrix &baseCamera)
{
	if(!camera || !BeginXrFrame()) return false;
	if(!gFrameState.shouldRender){ EndXrFrame(nil,0); return false; }
	// The next frontend or cutscene should anchor a fresh static theater screen
	// in front of the viewer instead of reusing a pre-gameplay location.
	gCinemaAnchorValid = false;
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF4)){
		gLightingEnabled=!gLightingEnabled;
		SaveVrSetting("ViceCityColor", gLightingEnabled);
		debug("[OpenXR] Vice City color filter: %s\n",gLightingEnabled?"enabled":"disabled");
	}
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF5)){
		gAntiAliasingEnabled=!gAntiAliasingEnabled;
		SaveVrSetting("AntiAliasing", gAntiAliasingEnabled);
		debug("[OpenXR] Anti-aliasing comparison: %s\n",gAntiAliasingEnabled?"enabled":"disabled");
	}
	gBaseCamera=baseCamera;
	gOriginalColor=RwCameraGetRaster(camera); gOriginalDepth=RwCameraGetZRaster(camera);
	gOriginalViewWindow=*RwCameraGetViewWindow(camera); gOriginalViewOffset=*RwCameraGetViewOffset(camera);
	gOriginalFrameMatrix=*RwFrameGetMatrix(RwCameraGetFrame(camera));
	gOriginalScreenWidth=RsGlobal.width; gOriginalScreenHeight=RsGlobal.height;
	gOriginalNearPlane=RwCameraGetNearClipPlane(camera); gOriginalDrawNear=CDraw::GetNearClipZ();
	gFramePrepared=true;
	if(gRecenterRequested){
		if(gGameplaySpace) xrDestroySpace(gGameplaySpace);
		gGameplaySpace=XR_NULL_HANDLE;
		gTrackingCenterValid=false;
		gRecenterRequested=false;
		ResetMotionSteeringInteraction();
		Dlaa::ResetHistory();
	}
	XrViewLocateInfo locate={XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime=gFrameState.predictedDisplayTime;
	auto locateViews=[&](XrSpace space)->bool {
		locate.space=space;
		XrViewState viewState={XR_TYPE_VIEW_STATE}; uint32_t count=0;
		for(int i=0;i<EYE_COUNT;i++) gLocatedViews[i]={XR_TYPE_VIEW};
		const double timingStart = PerfNowMs();
		XrResult result = xrLocateViews(
			gSession,&locate,&viewState,EYE_COUNT,&count,gLocatedViews);
		if(gPerfRecording && gPerfFrameStarted)
			gPerfCurrent.xrLocateViewsMs +=
				(float)(PerfNowMs() - timingStart);
		return XrOk(result,"xrLocateViews") &&
			count==EYE_COUNT &&
			(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)!=0 &&
			(viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)!=0;
	};
	if(!locateViews(gGameplaySpace ? gGameplaySpace : gLocalSpace)){
		RestoreCamera(camera); EndXrFrame(nil,0); return false;
	}
	if(!gGameplaySpace){
		const XrVector3f centre={
			(gLocatedViews[0].pose.position.x+gLocatedViews[1].pose.position.x)*0.5f,
			(gLocatedViews[0].pose.position.y+gLocatedViews[1].pose.position.y)*0.5f,
			(gLocatedViews[0].pose.position.z+gLocatedViews[1].pose.position.z)*0.5f };
		const XrVector3f localForward={0.0f,0.0f,-1.0f};
		const XrVector3f forward=Rotate(gLocatedViews[0].pose.orientation,localForward);
		const float yaw=atan2f(-forward.x,-forward.z);
		XrReferenceSpaceCreateInfo gameplayInfo={XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		gameplayInfo.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
		gameplayInfo.poseInReferenceSpace.position=centre;
		gameplayInfo.poseInReferenceSpace.orientation.y=sinf(yaw*0.5f);
		gameplayInfo.poseInReferenceSpace.orientation.w=cosf(yaw*0.5f);
		if(!XrOk(xrCreateReferenceSpace(gSession,&gameplayInfo,&gGameplaySpace),"xrCreateReferenceSpace(gameplay)") ||
		   !locateViews(gGameplaySpace)){
			RestoreCamera(camera); EndXrFrame(nil,0); return false;
		}
		debug("[OpenXR] Gameplay view recentered\n");
	}
	LocateTrackedHands();
	for(int eye=0;eye<EYE_COUNT;eye++){
		gRenderPose[eye]=gLocatedViews[eye].pose;
		// Unlike the old LibOVR calibration path, OpenXR already supplies the exact
		// asymmetric optical projection for each physical eye.  Rendering a wider
		// synthetic symmetric frustum and then describing it to the compositor as a
		// real view creates a direction-dependent lens warp on Quest.
		gRenderFov[eye]=gLocatedViews[eye].fov;
	}
	// RenderWare gets one stable symmetric projection for both eyes. The final
	// backend resolve pass remaps that source into each eye's exact asymmetric
	// OpenXR frustum.
	gSourceTanX=0.0f; gSourceTanY=0.0f;
	for(int eye=0;eye<EYE_COUNT;eye++){
		gSourceTanX=Max(gSourceTanX,Max(-tanf(gRenderFov[eye].angleLeft),tanf(gRenderFov[eye].angleRight)));
		gSourceTanY=Max(gSourceTanY,Max(tanf(gRenderFov[eye].angleUp),-tanf(gRenderFov[eye].angleDown)));
	}
	XrVector3f center={ (gRenderPose[0].position.x+gRenderPose[1].position.x)*0.5f,
		(gRenderPose[0].position.y+gRenderPose[1].position.y)*0.5f,
		(gRenderPose[0].position.z+gRenderPose[1].position.z)*0.5f };
	if(gFirstPersonEnabled && !gTrackingCenterValid){ gTrackingCenterOrigin=center; gTrackingCenterValid=true; }
	XrVector3f tracking={ gFirstPersonEnabled?center.x-gTrackingCenterOrigin.x:0.0f,
		gFirstPersonEnabled?center.y-gTrackingCenterOrigin.y:0.0f,
		gFirstPersonEnabled?center.z-gTrackingCenterOrigin.z:0.0f };
	// OpenXR supplies the physically correct per-eye poses.  Keep the scale and
	// direction fixed so an accidental input can never invalidate stereo again.
	const float scale=1.0f;
	for(int eye=0;eye<EYE_COUNT;eye++){
		gRenderPose[eye].position.x=(gRenderPose[eye].position.x-center.x)*scale+tracking.x;
		gRenderPose[eye].position.y=(gRenderPose[eye].position.y-center.y)*scale+tracking.y;
		gRenderPose[eye].position.z=(gRenderPose[eye].position.z-center.z)*scale+tracking.z;
	}
	UpdateTrackedScopeState();
	gTemporalJitterX = gTemporalJitterY = 0.0f;
	gTemporalJitterClipX = gTemporalJitterClipY = 0.0f;
#ifdef RW_D3D12
	if(gDlaaEnabled && Dlaa::IsSupported() && gTemporalJitterMode != 0 &&
	   gEye[0].renderWidth > 0 && gEye[0].renderHeight > 0){
		const uint32 sequence = (gTemporalFrameIndex++ % 8U) + 1U;
		const float projectionJitterX = Halton(sequence, 2U)-0.5f;
		const float projectionJitterY = Halton(sequence, 3U)-0.5f;
		float reportedScaleX = 1.0f;
		float reportedScaleY = 1.0f;
		if(gTemporalJitterMode == 2)
			reportedScaleX = 2.0f;
		else if(gTemporalJitterMode == 3)
			reportedScaleX = reportedScaleY = 2.0f;
		else if(gTemporalJitterMode == 4)
			reportedScaleX = reportedScaleY = 0.5f;
		else if(gTemporalJitterMode == 5)
			reportedScaleX = reportedScaleY = -1.0f;
		gTemporalJitterX = projectionJitterX*reportedScaleX;
		gTemporalJitterY = projectionJitterY*reportedScaleY;
		gTemporalJitterClipX = 2.0f*projectionJitterX/(float)gEye[0].renderWidth;
		gTemporalJitterClipY = -2.0f*projectionJitterY/(float)gEye[0].renderHeight;
	}
#endif
	return true;
}

bool GetEyeCamera(int eye, CMatrix *eyeCamera)
{
	if(!gFramePrepared || !eyeCamera || eye<0 || eye>=EYE_COUNT) return false;
	const XrVector3f localUp={0,1,0}, localForward={0,0,-1}; const XrPosef &pose=gRenderPose[eye];
	*eyeCamera=gBaseCamera;
	CVector forward=ToGameVector(Rotate(pose.orientation,localForward));
	CVector upVector=ToGameVector(Rotate(pose.orientation,localUp));
	forward.Normalise(); CVector leftVector=CrossProduct(upVector,forward); leftVector.Normalise();
	upVector=CrossProduct(forward,leftVector); upVector.Normalise();
	eyeCamera->GetRight()=leftVector; eyeCamera->GetUp()=upVector; eyeCamera->GetForward()=forward;
	eyeCamera->GetPosition()=gBaseCamera.GetPosition()+ToGameVector(pose.position);
	if(FindPlayerVehicle())
		eyeCamera->GetPosition().z += (float)gDrivingYOffsetCm/100.0f;
	return true;
}

bool BeginEye(RwCamera *camera, int eye, CMatrix *eyeCamera, float *horizontalFov)
{
	if(!gFramePrepared || !camera || !GetEyeCamera(eye, eyeCamera)) return false;
#ifdef RW_D3D12
	rw::d3d12::setStereoWorldEye(eye);
#endif
	RwV2d window={gSourceTanX,gSourceTanY};
	RwV2d offset={0.0f,0.0f};
	RwCameraSetRaster(camera,gEye[eye].color); RwCameraSetZRaster(camera,gEye[eye].depth);
	RwCameraSetViewWindow(camera,&window); RwCameraSetViewOffset(camera,&offset);
	if(gFirstPersonEnabled){ RwCameraSetNearClipPlane(camera,0.05f); CDraw::SetNearClipZ(0.05f); }
	RsGlobal.width=gEye[eye].renderWidth; RsGlobal.height=gEye[eye].renderHeight;
	RwMatrix *matrix=RwFrameGetMatrix(RwCameraGetFrame(camera));
	*RwMatrixGetRight(matrix)=eyeCamera->GetRight(); *RwMatrixGetUp(matrix)=eyeCamera->GetUp();
	*RwMatrixGetAt(matrix)=eyeCamera->GetForward(); *RwMatrixGetPos(matrix)=eyeCamera->GetPosition();
	RwMatrixUpdate(matrix); RwFrameUpdateObjects(RwCameraGetFrame(camera)); RwFrameOrthoNormalize(RwCameraGetFrame(camera));
	if(horizontalFov) *horizontalFov=RADTODEG(2.0f*atanf(gSourceTanX));
	return true;
}

void GetTemporalJitterClip(float *x, float *y)
{
	if(x) *x = gTemporalJitterClipX;
	if(y) *y = gTemporalJitterClipY;
}

int GetStereoScalePercent(){ return 100; }
bool IsStereoReversed(){ return false; }
bool IsFirstPersonEnabled(){ return gFirstPersonEnabled; }
bool CanSkipDesktopGameplayRender(){ return gSession!=XR_NULL_HANDLE && gWasSubmitting; }
bool UseFullStereoSinglePass(){ return gFullStereoSinglePass; }
bool AreTrackedHandsEnabled(){ return gVrHandsEnabled; }
bool AreWeaponHolsterHighlightsEnabled(){ return gWeaponHolsterHighlightsEnabled; }

void BeginNewGameCinemaHold()
{
	gNewGameCinemaHold = true;
	gNewGameCinemaHoldStartedAt = GetTickCount64();
	debug("[OpenXR] Holding loading cinema until first cutscene frame\n");
}

bool IsNewGameCinemaHoldActive()
{
	if(gNewGameCinemaHold &&
	   GetTickCount64()-gNewGameCinemaHoldStartedAt > 15000ULL){
		gNewGameCinemaHold = false;
		debug("[OpenXR] New-game cinema hold timed out; releasing gameplay\n");
	}
	return gNewGameCinemaHold;
}

void EndNewGameCinemaHold()
{
	if(gNewGameCinemaHold)
		debug("[OpenXR] First cutscene frame ready; releasing cinema hold\n");
	gNewGameCinemaHold = false;
}
bool ShouldUseTrackedHands()
{
	return IsTrackedHandReady(0) || IsTrackedHandReady(1);
}
bool IsTrackedHandReady(int hand)
{
	return hand >= 0 && hand < EYE_COUNT && gVrHandsEnabled && gFramePrepared &&
		gTrackedHandPoseValid[hand] &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}
bool IsTrackedWeaponLaserEnabled(){ return gWeaponLaserEnabled; }
bool IsTrackedScopeActive()
{
	return gTrackedScopeHand >= 0 && gTrackedScopeWeaponType >= 0;
}
bool IsTrackedScopeActiveForHand(int hand)
{
	return IsTrackedScopeActive() && gTrackedScopeHand == hand;
}
int GetTrackedScopeWeaponType()
{
	return IsTrackedScopeActive() ? gTrackedScopeWeaponType : -1;
}
bool ShouldUseTrackedWeapon(int hand)
{
	return IsTrackedHandReady(hand) && gHeldWeaponSlot[hand] >= 0 &&
		gTrackedHandAimPoseValid[hand];
}

bool IsTrackedWeaponTriggerPressed(int hand)
{
	return hand >= 0 && hand < EYE_COUNT && gVrHandsEnabled &&
		gHeldWeaponSlot[hand] >= 0 && gSessionRunning &&
		gTrackedWeaponTriggerPressed[hand] &&
		gTrackedHandAimPoseValid[hand] &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}

bool IsTrackedWeaponTriggerJustPressed(int hand)
{
	return IsTrackedWeaponTriggerPressed(hand) && gTrackedWeaponTriggerJustPressed[hand];
}

bool IsTrackedWeaponTriggerJustReleased(int hand)
{
	return hand >= 0 && hand < EYE_COUNT && gVrHandsEnabled &&
		gHeldWeaponSlot[hand] >= 0 && gSessionRunning &&
		gTrackedWeaponTriggerJustReleased[hand] &&
		gTrackedHandAimPoseValid[hand] &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}

bool IsTrackedDetonatorActive(int hand)
{
	return IsTrackedDetonatorActiveInternal(hand);
}

bool IsTrackedDetonatorTriggerJustPressed(int hand)
{
	return HasTrackedRemoteChargesInternal() &&
		IsTrackedDetonatorActiveInternal(hand) &&
		gTrackedDetonatorTriggerJustPressed[hand];
}

bool IsTrackedRemoteGrenadeFireActive()
{
	return gActiveTrackedFireAimValid && gActiveTrackedFireHand >= 0 &&
		gActiveTrackedFireWeaponType == WEAPONTYPE_DETONATOR_GRENADE;
}

bool ShouldKeepTrackedRemoteCharges(CEntity *source)
{
	return source != nil && source == FindPlayerPed() &&
		CProjectileInfo::HasDetonatorProjectile(source);
}

void NotifyTrackedRemoteGrenadeThrown(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gTrackedDetonatorHand = 1-hand;
	gCalibrationEditHand = gTrackedDetonatorHand;
	debug("[OpenXR] Remote charge armed; detonator assigned to %s hand\n",
		gTrackedDetonatorHand == 0 ? "left" : "right");
}

void NotifyTrackedDetonatorActivated(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gTrackedDetonatorTriggerJustPressed[hand] = false;
	gTrackedDetonatorWaitForTriggerRelease[hand] = true;
	debug("[OpenXR] Remote charges detonated from %s hand\n",
		hand == 0 ? "left" : "right");
}

bool IsManualReloadWeaponType(int weaponType)
{
	return IsManualReloadWeaponTypeInternal(weaponType);
}

void SetManualReloadWeaponState(int weaponHand, int slot, int weaponType,
	bool available)
{
	if(weaponHand < 0 || weaponHand >= EYE_COUNT)
		return;
	ManualReloadState &reload = gManualReload[weaponHand];
	available = available && ShouldUseManualReload() &&
		IsManualReloadWeaponTypeInternal(weaponType) &&
		slot > WEAPONSLOT_UNARMED && slot < TOTAL_WEAPON_SLOTS &&
		gHeldWeaponSlot[weaponHand] == slot;
	if(!available){
		ResetManualReloadState(reload);
		return;
	}
	// An empty detachable magazine needs the other hand.  Release a long-gun
	// support grip as soon as physical reload becomes available rather than
	// allowing the same controller to own both interactions.
	if(gWeaponSupportHand[weaponHand] >= 0)
		gWeaponSupportHand[weaponHand] = -1;
	if(!reload.active || reload.slot != slot ||
	   reload.weaponType != weaponType){
		ResetManualReloadState(reload);
		reload.active = true;
		reload.weaponHand = weaponHand;
		reload.slot = slot;
		reload.weaponType = weaponType;
		debug("[OpenXR] Physical reload available for %s hand slot %d type %d\n",
			weaponHand == 0 ? "left" : "right", slot, weaponType);
	}
}

bool ShouldUseManualReload()
{
	return gManualReloadEnabled && gVrHandsEnabled && gSessionRunning &&
		FindPlayerVehicle() == nil;
}

bool ConsumeManualReloadRequest(int weaponHand, int slot, int weaponType)
{
	if(weaponHand < 0 || weaponHand >= EYE_COUNT)
		return false;
	ManualReloadState &reload = gManualReload[weaponHand];
	const bool requested = reload.active && reload.requested &&
		reload.weaponHand == weaponHand && reload.slot == slot &&
		reload.weaponType == weaponType &&
		gHeldWeaponSlot[weaponHand] == slot;
	if(requested)
		ResetManualReloadState(reload);
	return requested;
}

bool GetManualReloadMagazineMatrix(int weaponHand, CMatrix *matrix,
	int *weaponType, bool *held)
{
	if(!matrix || weaponHand < 0 || weaponHand >= EYE_COUNT)
		return false;
	ManualReloadState &reload = gManualReload[weaponHand];
	if(!reload.active || reload.requested || !ShouldUseManualReload())
		return false;
	const bool magazineHeld = reload.magazineHand >= 0;
	const bool ready = magazineHeld ?
		BuildManualReloadHeldMatrix(reload.magazineHand, matrix) :
		BuildManualReloadSpotMatrix(reload.slot, matrix);
	if(!ready)
		return false;
	if(weaponType)
		*weaponType = reload.weaponType;
	if(held)
		*held = magazineHeld;
	return true;
}

void SetWeaponHolsterMask(uint32 mask)
{
	const uint32 ownedMask = mask;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(gHeldWeaponSlot[hand] >= 0 &&
		   (ownedMask & (1u << gHeldWeaponSlot[hand])) == 0){
			// Death/arrest can clear Vice City's inventory while the OpenXR hand
			// still remembers the old slot.  Drop that stale ownership before it
			// can hide a newly restored holster or reuse pre-death melee state.
			ClearWeaponSupportForHand(hand);
			gHeldWeaponSlot[hand] = -1;
			gWeaponHolsterSelection[hand] = -1;
			gTrackedWeaponRenderMatrixSlot[hand] = -1;
			InvalidatePhysicalMeleeWeaponMatrix(hand);
			ResetPhysicalMeleeMotion(hand);
		}
		if(gDroppedWeaponSlot[hand] >= 0 &&
		   (ownedMask & (1u << gDroppedWeaponSlot[hand])) == 0)
			ClearDroppedWeapon(hand);
		if(gHeldWeaponSlot[hand] >= 0)
			mask &= ~(1u << gHeldWeaponSlot[hand]);
		if(gDroppedWeaponSlot[hand] >= 0)
			mask &= ~(1u << gDroppedWeaponSlot[hand]);
	}
	gWeaponHolsterMask = mask;
}

bool ConsumeWeaponHolsterSelection(int hand, int *slot)
{
	if(hand < 0 || hand >= EYE_COUNT || !slot)
		return false;
	*slot = gWeaponHolsterSelection[hand];
	gWeaponHolsterSelection[hand] = -1;
	return *slot >= 0;
}

bool GetWeaponHolsterMatrix(int slot, CMatrix *matrix)
{
	return gVrHandsEnabled && gFramePrepared &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal()) &&
		(gWeaponHolsterMask & (1u << slot)) != 0 &&
		BuildWeaponHolsterMatrix(slot, matrix);
}

bool IsPhysicalGunType(int weaponType)
{
	return IsPhysicalGunTypeInternal(weaponType);
}

bool IsPhysicalMeleeType(int weaponType)
{
	return IsPhysicalMeleeTypeInternal(weaponType);
}

bool IsPhysicalThrowableType(int weaponType)
{
	return IsPhysicalThrowableTypeInternal(weaponType);
}

bool IsPhysicalWeaponType(int weaponType)
{
	return IsPhysicalWeaponTypeInternal(weaponType);
}

bool ConsumePhysicalMeleeStrike(int hand, int *slot, int *weaponType,
	CVector *sweepStart, CVector *sweepEnd, float *speed,
	CVector *rootStart, CVector *rootEnd)
{
	if(hand < 0 || hand >= EYE_COUNT || !slot || !weaponType ||
	   !sweepStart || !sweepEnd || !gPhysicalMeleeStrike[hand].pending)
		return false;
	PhysicalMeleeStrike &strike = gPhysicalMeleeStrike[hand];
	if(CTimer::GetFrameCounter()-strike.frame > 1U){
		strike.pending = false;
		return false;
	}
	*slot = strike.slot;
	*weaponType = strike.weaponType;
	*sweepStart = strike.sweepStart;
	*sweepEnd = strike.sweepEnd;
	if(rootStart)
		*rootStart = strike.rootStart;
	if(rootEnd)
		*rootEnd = strike.rootEnd;
	if(speed)
		*speed = strike.speed;
	strike.pending = false;
	return true;
}

void ResolvePhysicalMeleeStrike(int hand, bool contact)
{
	if(hand < 0 || hand >= EYE_COUNT || !contact)
		return;
	// A miss leaves the swing live, allowing the following 90 Hz sweep segment
	// to reach the target. A real contact latches until the hand slows down,
	// guaranteeing one damage event per deliberate swing.
	gPhysicalMeleeMotion[hand].armed = false;
	gPhysicalMeleeMotion[hand].strikeInProgress = false;
	gPhysicalMeleeMotion[hand].strikePeakSpeed = 0.0f;
	gPhysicalMeleeMotion[hand].strikeContinueUntil = 0;
	gPhysicalMeleeMotion[hand].calmSinceTime = 0;
	gPhysicalMeleeMotion[hand].lastStrikeTime =
		CTimer::GetTimeInMillisecondsNonClipped();
}

bool IsPhysicalWeaponInteractionActive()
{
	// Do not depend on a single frame's pose validity: a transient tracking loss
	// must not let the legacy ammo-empty switch select an invisible inventory gun.
	return gVrHandsEnabled && gSessionRunning &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}

bool IsTrackedWeaponHeld(int hand)
{
	return hand >= 0 && hand < EYE_COUNT && gVrHandsEnabled &&
		gHeldWeaponSlot[hand] >= 0 && gSessionRunning &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}

int GetHeldWeaponSlot(int hand)
{
	return hand >= 0 && hand < EYE_COUNT ? gHeldWeaponSlot[hand] : -1;
}

void ReleaseTrackedWeaponAfterUse(int hand, int slot)
{
	if(hand < 0 || hand >= EYE_COUNT || slot < 0 ||
	   gHeldWeaponSlot[hand] != slot)
		return;
	ClearWeaponSupportForHand(hand);
	gHeldWeaponSlot[hand] = -1;
	gWeaponHolsterSelection[hand] = -1;
	gTrackedWeaponRenderMatrixSlot[hand] = -1;
	InvalidatePhysicalMeleeWeaponMatrix(hand);
	ResetPhysicalMeleeMotion(hand);
	ClearDroppedWeapon(hand);
	ResetManualReloadState(gManualReload[hand]);
	gTrackedThrowablePreviewActive[hand] = false;
	// The controller is normally still squeezing grip after releasing R2. Keep
	// the body-point edge latched until a real grip release, so the next grenade
	// cannot jump into the same hand immediately.
	gWeaponHolsterGripDown[hand] = true;
}

void SetTrackedWeaponRenderMatrix(int hand, int slot, int weaponType,
	const CMatrix *matrix, const CMatrix *contactMatrix)
{
	if(hand < 0 || hand >= EYE_COUNT || slot < 0 || !matrix)
		return;
	gTrackedWeaponRenderMatrix[hand] = *matrix;
	gTrackedWeaponRenderMatrixSlot[hand] = slot;
	if(contactMatrix){
		gTrackedWeaponContactMatrix[hand] = *contactMatrix;
		gTrackedWeaponContactMatrixSlot[hand] = slot;
		gTrackedWeaponContactMatrixType[hand] = weaponType;
		gTrackedWeaponContactMatrixFrame[hand] = CTimer::GetFrameCounter();
	}
}

int GetDroppedWeaponSlot(int hand)
{
	return hand >= 0 && hand < EYE_COUNT ? gDroppedWeaponSlot[hand] : -1;
}

bool GetDroppedWeaponMatrix(int slot, CMatrix *matrix)
{
	if(!matrix)
		return false;
	const ULONGLONG now = GetTickCount64();
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(slot != gDroppedWeaponSlot[hand])
			continue;
		return BuildDroppedWeaponMatrix(hand, now, matrix);
	}
	return false;
}

void GetTrackedWeaponOffset(float *offsetX, float *offsetY, float *offsetZ)
{
	GetTrackedWeaponOffsetForType(gCalibrationEditHand, GetCalibrationWeaponType(),
		offsetX, offsetY, offsetZ);
}

void GetTrackedWeaponRotation(float *rotationX, float *rotationY, float *rotationZ)
{
	GetTrackedWeaponRotationForType(gCalibrationEditHand, GetCalibrationWeaponType(),
		rotationX, rotationY, rotationZ);
}

void GetTrackedWeaponOffsetForType(int hand, int weaponType,
	float *offsetX, float *offsetY, float *offsetZ)
{
	const WeaponCalibration *calibration = GetWeaponCalibration(hand, weaponType);
	if(offsetX) *offsetX = calibration ? (float)calibration->offsetX/200.0f : 0.0f;
	if(offsetY) *offsetY = calibration ? (float)calibration->offsetY/200.0f : 0.0f;
	if(offsetZ) *offsetZ = calibration ? (float)calibration->offsetZ/200.0f : 0.0f;
}

void GetTrackedWeaponRotationForType(int hand, int weaponType,
	float *rotationX, float *rotationY, float *rotationZ)
{
	const WeaponCalibration *calibration = GetWeaponCalibration(hand, weaponType);
	if(rotationX) *rotationX = calibration ? (float)calibration->rotationX/WEAPON_CALIBRATION_VALUE_SCALE : 0.0f;
	if(rotationY) *rotationY = calibration ? (float)calibration->rotationY/WEAPON_CALIBRATION_VALUE_SCALE : 0.0f;
	if(rotationZ) *rotationZ = calibration ? (float)calibration->rotationZ/WEAPON_CALIBRATION_VALUE_SCALE : 0.0f;
}

bool ApplyTrackedWeaponTwoHandTransform(int primaryHand, int weaponType,
	CMatrix *matrix)
{
	if(!matrix)
		return false;
	CVector pivot, axis;
	float angle;
	if(!BuildTwoHandRotation(primaryHand, weaponType, &pivot, &axis, &angle))
		return false;
	if(angle != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(), axis, angle);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(), axis, angle);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(), axis, angle);
		matrix->GetPosition() = pivot+
			RotateAroundAxis(matrix->GetPosition()-pivot, axis, angle);
	}
	return true;
}

bool GetTrackedWeaponSupportAnchor(int primaryHand, int weaponType,
	CVector *position, bool *engaged)
{
	if(!position || primaryHand < 0 || primaryHand >= EYE_COUNT ||
	   gHeldWeaponSlot[primaryHand] < 0)
		return false;
	CVector pivot, expected;
	if(!BuildSupportGripVector(primaryHand, weaponType, &pivot, &expected))
		return false;
	const int supportHand = gWeaponSupportHand[primaryHand] >= 0 ?
		gWeaponSupportHand[primaryHand] : 1-primaryHand;
	const bool supportEngaged = gWeaponSupportHand[primaryHand] == supportHand;
	if(supportEngaged){
		CVector rotationPivot, axis;
		float angle;
		if(!BuildTwoHandRotation(primaryHand, weaponType, &rotationPivot,
		   &axis, &angle))
			return false;
		if(angle != 0.0f)
			expected = RotateAroundAxis(expected, axis, angle);
	}else{
		if(supportHand < 0 || supportHand >= EYE_COUNT ||
		   gHeldWeaponSlot[supportHand] >= 0 ||
		   !gTrackedHandPoseValid[supportHand])
			return false;
		const CVector supportPosition = gBaseCamera.GetPosition()+
			ToGameVector(gTrackedHandPose[supportHand].position);
		if((supportPosition-(pivot+expected)).Magnitude() > 0.45f)
			return false;
	}
	*position = pivot+expected;
	if(engaged)
		*engaged = supportEngaged;
	return true;
}

bool ApplyTrackedScopeReticleAim(int hand, int weaponType,
	const CVector &muzzle, CVector *direction)
{
	if(!direction || gTrackedScopeHand != hand ||
	   gTrackedScopeWeaponType != weaponType ||
	   (!gPhysicalScopeAimEnabled && weaponType != WEAPONTYPE_CAMERA) ||
	   !gTrackedScopeReticleTargetValid || !IsTrackedScopeGameplaySafe())
		return false;
	CVector convergedDirection = gTrackedScopeReticleTarget-muzzle;
	if(convergedDirection.MagnitudeSqr() < 0.0001f)
		return false;
	convergedDirection.Normalise();
	*direction = convergedDirection;
	return true;
}

void UpdateTrackedScopeReticleTarget()
{
	gTrackedScopeReticleTargetValid = false;
	if(gTrackedScopeHand < 0 || gTrackedScopeWeaponType < 0 ||
	   (!gPhysicalScopeAimEnabled &&
	    gTrackedScopeWeaponType != WEAPONTYPE_CAMERA) ||
	   !gFramePrepared || !IsTrackedScopeGameplaySafe())
		return;
	const XrVector3f eyeCentre = {
		(gLocatedViews[0].pose.position.x+gLocatedViews[1].pose.position.x)*0.5f,
		(gLocatedViews[0].pose.position.y+gLocatedViews[1].pose.position.y)*0.5f,
		(gLocatedViews[0].pose.position.z+gLocatedViews[1].pose.position.z)*0.5f
	};
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const CVector reticleOrigin = gBaseCamera.GetPosition()+
		ToGameVector(eyeCentre);
	CVector reticleForward = ToGameVector(Rotate(
		gLocatedViews[0].pose.orientation, localForward));
	// During a frontend/world transition OpenXR can briefly retain a located-view
	// flag while the pose or restored game camera is being rebuilt. Never pass a
	// non-finite ray into the legacy sector traversal: unlike modern collision
	// code it assumes valid bounded coordinates and can otherwise loop forever.
	if(!_finite(reticleOrigin.x) || !_finite(reticleOrigin.y) ||
	   !_finite(reticleOrigin.z) || !_finite(reticleForward.x) ||
	   !_finite(reticleForward.y) || !_finite(reticleForward.z) ||
	   reticleForward.MagnitudeSqr() < 0.0001f ||
	   reticleOrigin.x <= WORLD_MIN_X+1.0f ||
	   reticleOrigin.x >= WORLD_MAX_X-1.0f ||
	   reticleOrigin.y <= WORLD_MIN_Y+1.0f ||
	   reticleOrigin.y >= WORLD_MAX_Y-1.0f)
		return;
	reticleForward.Normalise();
	CVector reticleTarget = reticleOrigin+reticleForward*150.0f;
	reticleTarget.x = Max(WORLD_MIN_X+1.0f,
		Min(WORLD_MAX_X-1.0f, reticleTarget.x));
	reticleTarget.y = Max(WORLD_MIN_Y+1.0f,
		Min(WORLD_MAX_Y-1.0f, reticleTarget.y));
	CColPoint hitPoint;
	CEntity *hitEntity = nil;
	CEntity *savedIgnoreEntity = CWorld::pIgnoreEntity;
	CWorld::pIgnoreEntity = FindPlayerPed();
	if(CWorld::ProcessLineOfSight(reticleOrigin, reticleTarget, hitPoint,
	   hitEntity, true, true, true, true, true, false, false, false))
		reticleTarget = hitPoint.point;
	CWorld::pIgnoreEntity = savedIgnoreEntity;
	gTrackedScopeReticleTarget = reticleTarget;
	gTrackedScopeReticleTargetValid = true;
}

bool BuildTrackedWeaponAimInternal(int hand, int weaponType, CVector *source,
	CVector *direction, bool applyOneHandSway)
{
	if(!source || !direction || hand < 0 || hand >= EYE_COUNT ||
	   !gVrHandsEnabled || gHeldWeaponSlot[hand] < 0 || !gSessionRunning ||
	   !gTrackedHandAimPoseValid[hand] ||
	   (FindPlayerVehicle() != nil &&
	    !IsVrDrivingActiveInternal()))
		return false;
	const WeaponCalibration *calibration = GetWeaponCalibration(hand, weaponType);
	if(!calibration)
		return false;
	const XrPosef &pose = gTrackedHandAimPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	CVector aimForward = ToGameVector(Rotate(pose.orientation, localForward));
	CVector aimUp = ToGameVector(Rotate(pose.orientation, localUp));
	CVector aimRight = ToGameVector(Rotate(pose.orientation, localRight));
	aimForward.Normalise();
	// Preserve the reflected OpenXR basis while removing numerical skew. Matrix
	// Euler multiplication here previously made calibration axes overlap.
	aimUp -= aimForward*DotProduct(aimUp, aimForward);
	aimUp.Normalise();
	aimRight -= aimForward*DotProduct(aimRight, aimForward);
	aimRight -= aimUp*DotProduct(aimRight, aimUp);
	aimRight.Normalise();
	const float pitch = DEGTORAD((float)calibration->aimRotationX/WEAPON_CALIBRATION_VALUE_SCALE);
	const float yaw = DEGTORAD((float)calibration->aimRotationY/WEAPON_CALIBRATION_VALUE_SCALE);
	const float roll = DEGTORAD((float)calibration->aimRotationZ/WEAPON_CALIBRATION_VALUE_SCALE);
	if(pitch != 0.0f){
		aimForward = RotateAroundAxis(aimForward, aimRight, pitch);
		aimUp = RotateAroundAxis(aimUp, aimRight, pitch);
	}
	if(yaw != 0.0f){
		aimForward = RotateAroundAxis(aimForward, aimUp, yaw);
		aimRight = RotateAroundAxis(aimRight, aimUp, yaw);
	}
	if(roll != 0.0f){
		aimRight = RotateAroundAxis(aimRight, aimForward, roll);
		aimUp = RotateAroundAxis(aimUp, aimForward, roll);
	}
	aimForward.Normalise();
	aimUp -= aimForward*DotProduct(aimUp, aimForward);
	aimUp.Normalise();
	aimRight -= aimForward*DotProduct(aimRight, aimForward);
	aimRight -= aimUp*DotProduct(aimRight, aimUp);
	aimRight.Normalise();
	*direction = aimForward;
	// OpenXR aim pose starts inside the Touch controller. The per-weapon local
	// rotation and XYZ muzzle offsets below are shared by the visible laser and
	// the actual shot, so calibration can never make the two disagree.
	*source = gBaseCamera.GetPosition() + ToGameVector(pose.position) +
		aimRight*((float)calibration->aimOffsetX/200.0f) +
		aimUp*((float)calibration->aimOffsetY/200.0f) +
		*direction*(0.18f + (float)calibration->aimOffsetZ/200.0f);
	CVector pivot, axis;
	float angle;
	const bool supported = BuildTwoHandRotation(hand, weaponType,
		&pivot, &axis, &angle);
	if(supported && angle != 0.0f){
		*source = pivot+RotateAroundAxis(*source-pivot, axis, angle);
		*direction = RotateAroundAxis(*direction, axis, angle);
		direction->Normalise();
	}
	if(applyOneHandSway && IsTwoHandedWeaponTypeInternal(weaponType) && !supported){
		// Long guns deliberately become substantially less stable in one hand.
		// The 1.5 multiplier is shared by their real shot and visible hip-fire
		// laser, while scope proximity itself uses the unswayed optical axis.
		const float amplitude = GetOneHandAimSwayDegrees(weaponType)*1.5f;
		const float time = (float)(CTimer::GetTimeInMillisecondsNonClipped() %
			120000U)*0.001f;
		const float phase = weaponType*0.731f + hand*2.173f;
		const float yaw = DEGTORAD(amplitude*(
			0.68f*sinf(3.35f*time+phase) +
			0.32f*sinf(8.91f*time+1.73f*phase+0.4f)));
		const float pitch = DEGTORAD(0.78f*amplitude*(
			0.62f*sinf(2.87f*time+1.31f*phase+1.2f) +
			0.38f*sinf(7.43f*time+0.77f*phase)));
		CVector swayedRight = RotateAroundAxis(aimRight, aimUp, yaw);
		*direction = RotateAroundAxis(*direction, aimUp, yaw);
		*direction = RotateAroundAxis(*direction, swayedRight, pitch);
		direction->Normalise();
	}
	return true;
}

bool GetTrackedWeaponAim(int hand, int weaponType, CVector *source, CVector *direction)
{
	if(!source || !direction || hand < 0 || hand >= EYE_COUNT)
		return false;
	const uint32 frame = CTimer::GetFrameCounter();
	if(gTrackedAimCacheValid[hand] && gTrackedAimCacheFrame[hand] == frame &&
	   gTrackedAimCacheWeaponType[hand] == weaponType){
		*source = gTrackedAimCacheSource[hand];
		*direction = gTrackedAimCacheDirection[hand];
		return true;
	}
	if(!BuildTrackedWeaponAimInternal(hand, weaponType, source, direction, true))
		return false;
	// Scope activation continues to use the untouched physical barrel above, but
	// the final shot/laser converges on the world point under the HMD reticle.
	// Apply it only here, after raw pose/sway and before this frame's aim is cached.
	ApplyTrackedScopeReticleAim(hand, weaponType, *source, direction);
	gTrackedAimCacheValid[hand] = true;
	gTrackedAimCacheFrame[hand] = frame;
	gTrackedAimCacheWeaponType[hand] = weaponType;
	gTrackedAimCacheSource[hand] = *source;
	gTrackedAimCacheDirection[hand] = *direction;
	return true;
}

bool GetTrackedScopeMetrics(int hand, int weaponType, bool relaxed,
	float *score)
{
	if(!IsPhysicalScopeWeaponTypeInternal(weaponType) ||
	   (!gPhysicalScopeAimEnabled && weaponType != WEAPONTYPE_CAMERA) ||
	   !gFramePrepared)
		return false;
	CVector sightPosition, sightDirection;
	if(!BuildTrackedWeaponAimInternal(hand, weaponType, &sightPosition,
	   &sightDirection, false))
		return false;
	if(IsTwoHandedWeaponTypeInternal(weaponType)){
		// A long-gun optic only becomes usable after the saved foregrip socket is
		// actually held by the other controller. Merely moving the free hand near
		// the gun is not sufficient: UpdateWeaponHolsterInput owns that grip state.
		const int supportHand = gWeaponSupportHand[hand];
		if(supportHand < 0 || supportHand >= EYE_COUNT || supportHand == hand ||
		   !gTrackedHandPoseValid[supportHand])
			return false;
	}
	sightDirection.Normalise();
	// Controller poses are located in raw gGameplaySpace. gRenderPose has already
	// been recentered for the game camera, so mixing it with a raw hand pose makes
	// the apparent HMD-to-controller distance include the tracking-origin offset.
	// Use the unmodified located views here so both ends share one XR space.
	const XrVector3f eyeCentre = {
		(gLocatedViews[0].pose.position.x+gLocatedViews[1].pose.position.x)*0.5f,
		(gLocatedViews[0].pose.position.y+gLocatedViews[1].pose.position.y)*0.5f,
		(gLocatedViews[0].pose.position.z+gLocatedViews[1].pose.position.z)*0.5f
	};
	const CVector headPosition = gBaseCamera.GetPosition()+
		ToGameVector(eyeCentre);
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	CVector headForward = ToGameVector(Rotate(
		gLocatedViews[0].pose.orientation, localForward));
	CVector headUp = ToGameVector(Rotate(
		gLocatedViews[0].pose.orientation, localUp));
	CVector headRight = ToGameVector(Rotate(
		gLocatedViews[0].pose.orientation, localRight));
	headForward.Normalise();
	headUp.Normalise();
	headRight.Normalise();
	// Use the controller's physical proximity to the HMD as the primary gesture.
	// The muzzle/laser origin includes per-weapon calibration and can be tens of
	// centimetres away from the actual grip, which made a strict ray-to-eye test
	// impossible to satisfy for several otherwise correctly calibrated weapons.
	const CVector controllerPosition = gBaseCamera.GetPosition()+
		ToGameVector(gTrackedHandAimPose[hand].position);
	const CVector headToController = controllerPosition-headPosition;
	const float controllerDistance = headToController.Magnitude();
	const float controllerForward = DotProduct(headToController, headForward);
	const float controllerHeight = DotProduct(headToController, headUp);
	const float controllerLateral = DotProduct(headToController, headRight);
	const float alignment = DotProduct(headForward, sightDirection);
	const float minimumDistance = relaxed ? 0.04f : 0.06f;
	const float maximumDistance = relaxed ? 0.70f : 0.58f;
	const float minimumForward = relaxed ? -0.03f : 0.04f;
	const float maximumForward = relaxed ? 0.62f : 0.50f;
	// The trigger/grip sits below the optical tube on long guns, so the valid eye
	// band is intentionally asymmetric while still excluding a weapon at chest or
	// waist height. The wider relaxed band prevents flicker after activation.
	const float minimumHeight = relaxed ? -0.32f : -0.24f;
	const float maximumHeight = relaxed ? 0.16f : 0.10f;
	const float maximumLateral = relaxed ? 0.34f : 0.25f;
	const float minimumAlignment = relaxed ? 0.80f : 0.90f;
	if(controllerDistance < minimumDistance || controllerDistance > maximumDistance ||
	   controllerForward < minimumForward || controllerForward > maximumForward ||
	   controllerHeight < minimumHeight || controllerHeight > maximumHeight ||
	   fabsf(controllerLateral) > maximumLateral ||
	   alignment < minimumAlignment)
		return false;
	if(score)
		*score = fabsf(controllerHeight)*1.40f+fabsf(controllerLateral)+
			(1.0f-alignment)*0.35f+
			Max(0.0f, controllerForward-0.38f)*0.20f;
	return true;
}

void SetTrackedScopeStateInternal(int hand, int weaponType)
{
	if(gTrackedScopeHand == hand && gTrackedScopeWeaponType == weaponType)
		return;
	const bool wasActive = gTrackedScopeHand >= 0;
	gTrackedScopeHand = hand;
	gTrackedScopeWeaponType = weaponType;
	gTrackedScopeInvalidSince = 0;
	gTrackedScopeCandidateHand = -1;
	gTrackedScopeCandidateWeaponType = -1;
	gTrackedScopeCandidateSince = 0;
	gTrackedScopeReticleTargetValid = false;
	for(int aimHand = 0; aimHand < EYE_COUNT; aimHand++)
		gTrackedAimCacheValid[aimHand] = false;
	Dlaa::ResetHistory();
	if(hand >= 0)
		debug("[OpenXR] Physical optic active: hand=%d weapon=%d\n",
			hand, weaponType);
	else if(wasActive)
		debug("[OpenXR] Physical optic released\n");
}

void ResetTrackedScopeState()
{
	SetTrackedScopeStateInternal(-1, -1);
	gTrackedScopeCandidateHand = -1;
	gTrackedScopeCandidateWeaponType = -1;
	gTrackedScopeCandidateSince = 0;
	gTrackedScopeInvalidSince = 0;
}

void UpdateTrackedScopeState()
{
	if(!gVrHandsEnabled || !IsTrackedScopeGameplaySafe() ||
	   FindPlayerVehicle() != nil){
		ResetTrackedScopeState();
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if(gTrackedScopeHand >= 0){
		const int slot = gTrackedScopeHand < EYE_COUNT ?
			gHeldWeaponSlot[gTrackedScopeHand] : -1;
		const int heldType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		const bool valid = heldType == gTrackedScopeWeaponType &&
			GetTrackedScopeMetrics(gTrackedScopeHand,
				gTrackedScopeWeaponType, true, nil);
		if(valid){
			gTrackedScopeInvalidSince = 0;
			return;
		}
		if(gTrackedScopeInvalidSince == 0)
			gTrackedScopeInvalidSince = now;
		else if(now-gTrackedScopeInvalidSince >= 150ULL)
			ResetTrackedScopeState();
		return;
	}

	int bestHand = -1;
	int bestWeaponType = -1;
	float bestScore = 1000000.0f;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const int slot = gHeldWeaponSlot[hand];
		if(slot < 0 || slot >= TOTAL_WEAPON_SLOTS)
			continue;
		const int weaponType = GetVrWeaponTypeForSlot(slot);
		float score = 0.0f;
		if(!GetTrackedScopeMetrics(hand, weaponType, false, &score) ||
		   score >= bestScore)
			continue;
		bestScore = score;
		bestHand = hand;
		bestWeaponType = weaponType;
	}
	if(bestHand < 0){
		gTrackedScopeCandidateHand = -1;
		gTrackedScopeCandidateWeaponType = -1;
		gTrackedScopeCandidateSince = 0;
		return;
	}
	if(gTrackedScopeCandidateHand != bestHand ||
	   gTrackedScopeCandidateWeaponType != bestWeaponType){
		gTrackedScopeCandidateHand = bestHand;
		gTrackedScopeCandidateWeaponType = bestWeaponType;
		gTrackedScopeCandidateSince = now;
		return;
	}
	if(now-gTrackedScopeCandidateSince >= 35ULL)
		SetTrackedScopeStateInternal(bestHand, bestWeaponType);
}

bool GetTrackedThrowableLaunch(int hand, int weaponType, CVector *source,
	CVector *velocity)
{
	if(!source || !velocity || !IsPhysicalThrowableTypeInternal(weaponType))
		return false;
	CVector direction;
	CVector requestedSource;
	if(!GetTrackedWeaponAim(hand, weaponType, &requestedSource, &direction))
		return false;
	return BuildTrackedThrowableLaunch(weaponType, requestedSource, direction,
		source, velocity);
}

void SetTrackedThrowablePreviewActive(int hand, bool active)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gTrackedThrowablePreviewActive[hand] = active &&
		gHeldWeaponSlot[hand] == WEAPONSLOT_PROJECTILE;
}

bool IsTrackedThrowablePreviewActive(int hand)
{
	return hand >= 0 && hand < EYE_COUNT &&
		gTrackedThrowablePreviewActive[hand] &&
		IsTrackedWeaponTriggerPressed(hand);
}

void BeginTrackedWeaponFire(int hand, int weaponType, const CVector &source,
	const CVector &direction)
{
	gActiveTrackedFireHand = hand;
	gActiveTrackedFireWeaponType = weaponType;
	gActiveTrackedFireSource = source;
	gActiveTrackedFireDirection = direction;
	gActiveTrackedFireDirection.Normalise();
	gActiveTrackedFireAimValid = true;
}

void EndTrackedWeaponFire()
{
	gActiveTrackedFireHand = -1;
	gActiveTrackedFireWeaponType = -1;
	gActiveTrackedFireAimValid = false;
}

bool GetActiveTrackedWeaponAim(CVector *source, CVector *direction)
{
	if(!source || !direction || gActiveTrackedFireHand < 0 ||
	   !gActiveTrackedFireAimValid)
		return false;
	*source = gActiveTrackedFireSource;
	*direction = gActiveTrackedFireDirection;
	return true;
}

bool GetActiveTrackedThrowableLaunch(CVector *source, CVector *velocity)
{
	if(!source || !velocity || gActiveTrackedFireHand < 0 ||
	   !gActiveTrackedFireAimValid ||
	   !IsPhysicalThrowableTypeInternal(gActiveTrackedFireWeaponType))
		return false;
	return BuildTrackedThrowableLaunch(gActiveTrackedFireWeaponType,
		gActiveTrackedFireSource, gActiveTrackedFireDirection, source, velocity);
}

bool GetTrackedHandMatrix(int hand, CMatrix *handMatrix, float *grip, float *trigger)
{
	if(!handMatrix || hand < 0 || hand >= EYE_COUNT || !gVrHandsEnabled ||
	   !gFramePrepared || !gTrackedHandPoseValid[hand])
		return false;
	if(gBikeHandleGrabbed[hand] &&
	   BuildBikeHandleMatrixInternal(hand, handMatrix, true)){
		if(grip) *grip = gTrackedHandGrip[hand];
		if(trigger) *trigger = gTrackedHandTrigger[hand];
		return true;
	}
	if(gCarWheelGrabbed[hand] &&
	   BuildCarWheelMatrixInternal(hand, handMatrix, true)){
		if(grip) *grip = gTrackedHandGrip[hand];
		if(trigger) *trigger = gTrackedHandTrigger[hand];
		return true;
	}
	const XrPosef &pose = gTrackedHandPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	*handMatrix = gBaseCamera;
	handMatrix->GetRight() = ToGameVector(Rotate(pose.orientation, localRight));
	handMatrix->GetUp() = ToGameVector(Rotate(pose.orientation, localUp));
	handMatrix->GetForward() = ToGameVector(Rotate(pose.orientation, localForward));
	handMatrix->GetRight().Normalise();
	handMatrix->GetUp().Normalise();
	handMatrix->GetForward().Normalise();
	handMatrix->GetPosition() = gBaseCamera.GetPosition() + ToGameVector(pose.position);
	// A real two-handed weapon cannot stretch when the controllers are held at
	// slightly different radii. The raw support controller still steers the gun,
	// while its rendered hand is pinned to the calibrated foregrip socket.
	for(int primary = 0; primary < EYE_COUNT; primary++){
		if(gWeaponSupportHand[primary] != hand)
			continue;
		const int slot = gHeldWeaponSlot[primary];
		const int weaponType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		CVector supportAnchor;
		bool engaged = false;
		if(GetTrackedWeaponSupportAnchor(primary, weaponType, &supportAnchor,
		   &engaged) && engaged)
			handMatrix->GetPosition() = supportAnchor;
		break;
	}
	if(grip) *grip = gTrackedHandGrip[hand];
	if(trigger) *trigger = gTrackedHandTrigger[hand];
	return true;
}

bool GetTrackedHandAimRay(int hand, CVector *origin, CVector *direction)
{
	if(!origin || !direction || hand < 0 || hand >= EYE_COUNT || !gVrHandsEnabled ||
	   !gFramePrepared || !gTrackedHandAimPoseValid[hand])
		return false;
	CMatrix handle;
	if(gBikeHandleGrabbed[hand] &&
	   BuildBikeHandleMatrixInternal(hand, &handle, true)){
		*origin = handle.GetPosition();
		*direction = handle.GetForward();
		direction->Normalise();
		return true;
	}
	if(gCarWheelGrabbed[hand] &&
	   BuildCarWheelMatrixInternal(hand, &handle, true)){
		*origin = handle.GetPosition();
		*direction = handle.GetForward();
		direction->Normalise();
		return true;
	}
	const XrPosef &pose = gTrackedHandAimPose[hand];
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	*origin = gBaseCamera.GetPosition() + ToGameVector(pose.position);
	*direction = ToGameVector(Rotate(pose.orientation, localForward));
	direction->Normalise();
	return true;
}

bool IsImmersiveBikeDrivingActive()
{
	return IsImmersiveBikeDrivingActiveInternal();
}

bool IsImmersiveDrivingActive()
{
	return IsVrDrivingActiveInternal();
}

bool IsImmersiveCarDrivingActive()
{
	return IsImmersiveCarDrivingActiveInternal();
}

bool IsVrBikeDrivingActive()
{
	return IsVrBikeDrivingActiveInternal();
}

bool IsVrCarDrivingActive()
{
	return IsVrCarDrivingActiveInternal();
}

bool IsVrRadioControlActive()
{
	return gVrHandsEnabled && gSessionRunning &&
		FindPlayerVehicle() != nil;
}

bool ConsumeVrRadioChange()
{
	if(!IsVrRadioControlActive()){
		gVrRadioChangeJustPressed = false;
		return false;
	}
	const bool pressed = gVrRadioChangeJustPressed;
	gVrRadioChangeJustPressed = false;
	return pressed;
}

bool GetImmersiveCarSteering(CVehicle *car, float *steering)
{
	if(!steering)
		return false;
	if(IsImmersiveCarDrivingActiveInternal(car)){
		*steering = gImmersiveCarSteering;
		return true;
	}
	if(IsMotionDrivingEnvironmentActive() &&
	   IsVrCarDrivingActiveInternal(car)){
		*steering = gMotionVehicleSteering;
		return true;
	}
	return false;
}

bool GetImmersiveBikeSteering(CVehicle *bike, float *steering)
{
	if(!steering)
		return false;
	if(IsImmersiveBikeDrivingActiveInternal(bike)){
		*steering = gImmersiveBikeSteering;
		return true;
	}
	if(IsMotionDrivingEnvironmentActive() &&
	   IsVrBikeDrivingActiveInternal(bike)){
		*steering = gMotionVehicleSteering;
		return true;
	}
	return false;
}

bool GetImmersiveBikeThrottle(CVehicle *bike, float *throttle)
{
	if(!throttle || !IsImmersiveBikeDrivingActiveInternal(bike))
		return false;
	*throttle = gImmersiveBikeThrottle;
	return true;
}

bool GetImmersiveBikeLean(CVehicle *bike, float *lean)
{
	if(!lean || !IsImmersiveBikeDrivingActiveInternal(bike))
		return false;
	*lean = gImmersiveBikeLean;
	return true;
}

bool GetImmersiveBikeHandleMatrix(int hand, CMatrix *matrix)
{
	return BuildBikeHandleMatrixInternal(hand, matrix, true);
}

bool IsImmersiveBikeHandleGrabbed(int hand)
{
	return hand >= 0 && hand < EYE_COUNT &&
		IsImmersiveBikeDrivingActiveInternal() &&
		gBikeHandleGrabbed[hand];
}

bool ShouldRenderImmersiveBikeHandleMarker(int hand)
{
	return hand >= 0 && hand < EYE_COUNT &&
		IsImmersiveBikeDrivingActiveInternal() &&
		(gVrBikeCalibrationMenuVisible ||
		 (gBikeHandleHighlightsEnabled &&
		  (gBikeHandleGrabbed[hand] ||
		   gBikeHandleDistance[hand] <= 0.23f)));
}

bool GetImmersiveSteeringHandleMatrix(int hand, CMatrix *matrix)
{
	if(IsVrCarDrivingActiveInternal())
		return BuildCarWheelMatrixInternal(hand, matrix, true);
	return BuildBikeHandleMatrixInternal(hand, matrix, true);
}

bool IsImmersiveSteeringHandleGrabbed(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return false;
	if(IsImmersiveCarDrivingActiveInternal())
		return gCarWheelGrabbed[hand];
	return IsImmersiveBikeHandleGrabbed(hand);
}

bool ShouldRenderImmersiveSteeringHandleMarker(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return false;
	if(IsImmersiveCarDrivingActiveInternal())
		return gVrBikeCalibrationMenuVisible ||
			gBikeHandleHighlightsEnabled;
	return ShouldRenderImmersiveBikeHandleMarker(hand);
}

bool IsImmersiveBikeSidearm(int weaponType)
{
	return IsBikeSidearmTypeInternal(weaponType);
}

bool IsImmersiveVehicleSidearm(int weaponType)
{
	return IsBikeSidearmTypeInternal(weaponType);
}

bool SubmitStereoFrame(RwCamera *camera)
{
#ifdef RW_D3D12
	rw::d3d12::setStereoWorldEye(-1);
#endif
	if(!gFramePrepared || !gFrameBegun) return false;
	if(gVrLoggedRenderableFrames < 10)
		VrLog("SubmitStereoFrame begin\n");
	// World rendering is complete and its collision lists are stable here. Cache
	// the point under the HMD reticle for next-frame weapon input; never perform
	// this query from loading/menu weapon updates while the world is rebuilding.
	UpdateTrackedScopeReticleTarget();
	RestoreCamera(camera);
#ifdef RW_D3D12
	const bool dlaaWarmupFrame =
		gDlaaEnabled && !gDlaaStereoActivationReady;
	if(dlaaWarmupFrame && gDlaaStereoWarmupFrames > 0)
		gDlaaStereoWarmupFrames--;
	const bool dlaaFrame = gDlaaEnabled &&
		gDlaaStereoActivationReady && !gDlaaStereoActivationFailed &&
		Dlaa::BeginFrame(
		gTemporalJitterX, gTemporalJitterY);
#endif
	for(int eye=0;eye<EYE_COUNT;eye++){
		bool copied = false;
#ifdef RW_D3D12
		if(dlaaFrame)
			copied = DrawEyeDlaa(gEye[eye], eye, camera);
#endif
		if(!copied)
			copied=DrawEyeFxaa(gEye[eye],eye);
#ifndef RW_D3D12
		if(!copied)
			copied=CopyRasterToSwapchain(gEye[eye].color,gEye[eye].renderWidth,gEye[eye].renderHeight,
				gEye[eye].swapchain,gAntiAliasingEnabled?GL_LINEAR:GL_NEAREST);
#endif
		if(!copied){
#ifdef RW_D3D12
			FinishD3D12SwapchainWrites();
#endif
			EndXrFrame(nil,0); return false;
		}
	}
#ifdef RW_D3D12
	if(dlaaFrame && !Dlaa::WasLastEvaluationSuccessful()){
		gDlaaStereoActivationReady = false;
		gDlaaStereoActivationFailed = true;
		VrLog("DLAA real evaluation failed: %s; retaining FXAA fallback\n",
			Dlaa::GetStatus());
	}
#endif
	const bool showHud=UpdateHudSwapchain(camera);
	const bool showDebug=UpdateDebugSwapchain();
	const bool showVrMenu=UpdateVrMenuSwapchain();
	if(gVrLoggedRenderableFrames < 10)
		VrLog("SubmitStereoFrame copies done hud=%d debug=%d\n", showHud ? 1 : 0,
			showDebug ? 1 : 0);
#ifdef RW_D3D12
	const bool prepareDlaaAfterSubmit =
		dlaaWarmupFrame && gDlaaStereoWarmupFrames == 0 &&
		!gDlaaStereoActivationFailed;
	if(!FinishD3D12SwapchainWrites()){
		EndXrFrame(nil,0);
		return false;
	}
	if(prepareDlaaAfterSubmit){
		// Streamline creates the DLAA feature lazily on the first real
		// evaluation. Let the cinema-to-world transition settle first, submit
		// several ordinary stereo frames, then drain the queue. The next frame
		// follows the original working EvaluateEye path with valid color/depth
		// tags while the GPU starts from a known idle state.
		gDlaaStereoActivationReady = rw::d3d12::waitForGpu() != 0;
		gDlaaStereoActivationFailed = !gDlaaStereoActivationReady;
		if(gDlaaStereoActivationReady)
			VrLog("DLAA activation barrier complete; real evaluation starts next frame\n");
		else
			VrLog("DLAA activation barrier failed; retaining FXAA fallback\n");
	}
#endif
	XrCompositionLayerProjectionView projectionViews[EYE_COUNT];
	for(int eye=0;eye<EYE_COUNT;eye++){
		projectionViews[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
		projectionViews[eye].pose=gLocatedViews[eye].pose;
		projectionViews[eye].fov=gRenderFov[eye];
		projectionViews[eye].subImage.swapchain=gEye[eye].swapchain.handle;
		projectionViews[eye].subImage.imageRect.offset={0,0};
		projectionViews[eye].subImage.imageRect.extent={gEye[eye].swapchain.width,gEye[eye].swapchain.height};
		projectionViews[eye].subImage.imageArrayIndex=0;
	}
	XrCompositionLayerProjection projection={XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	projection.space=gGameplaySpace; projection.viewCount=EYE_COUNT; projection.views=projectionViews;
	XrCompositionLayerQuad hud={XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(showHud){
		hud.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		hud.space=gViewSpace; hud.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
		hud.subImage.swapchain=gHudSwapchain.handle; hud.subImage.imageRect.extent={VR_HUD_WIDTH,VR_HUD_HEIGHT};
		hud.pose.orientation.w=1.0f;
		const bool trackedScope = IsTrackedScopeActive();
		hud.pose.position.x=trackedScope?0.0f:0.06f;
		hud.pose.position.y=trackedScope?0.0f:-0.15f;
		hud.pose.position.z=-1.6f;
		if(trackedScope){
			// Cover the complete Quest view while an opaque optic mask is active.
			hud.size.width=5.7f;
			hud.size.height=3.2f;
		}else{
			hud.size.width=1.8f;
			hud.size.height=1.8f*(float)VR_HUD_HEIGHT/VR_HUD_WIDTH;
		}
	}
	XrCompositionLayerQuad debugLayer={XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(showDebug){
		debugLayer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		debugLayer.space=gViewSpace; debugLayer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
		debugLayer.subImage.swapchain=gDebugSwapchain.handle;
		debugLayer.subImage.imageRect.extent={VR_DEBUG_WIDTH,VR_DEBUG_HEIGHT};
		debugLayer.pose.orientation.w=1.0f;
		debugLayer.pose.position.y=-0.24f; debugLayer.pose.position.z=-1.5f;
		debugLayer.size.width=1.2f;
		debugLayer.size.height=1.2f*(float)VR_DEBUG_HEIGHT/VR_DEBUG_WIDTH;
	}
	XrCompositionLayerQuad vrMenuLayer={XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(showVrMenu){
		vrMenuLayer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		vrMenuLayer.space=gViewSpace; vrMenuLayer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
		vrMenuLayer.subImage.swapchain=gVrMenuSwapchain.handle;
		vrMenuLayer.subImage.imageRect.extent={VR_MENU_WIDTH,VR_MENU_HEIGHT};
		vrMenuLayer.pose.orientation.w=1.0f;
		vrMenuLayer.pose.position.z=-1.65f;
		vrMenuLayer.size.width=2.15f;
		vrMenuLayer.size.height=2.15f*(float)VR_MENU_HEIGHT/VR_MENU_WIDTH;
	}
	const XrCompositionLayerBaseHeader *layers[4]; uint32_t count=0;
	layers[count++]=(const XrCompositionLayerBaseHeader*)&projection;
	if(showHud) layers[count++]=(const XrCompositionLayerBaseHeader*)&hud;
	if(showDebug) layers[count++]=(const XrCompositionLayerBaseHeader*)&debugLayer;
	if(showVrMenu) layers[count++]=(const XrCompositionLayerBaseHeader*)&vrMenuLayer;
	return EndXrFrame(layers,count);
}

bool SubmitCinemaFrame(RwCamera *camera, bool holdLastFrame)
{
	// A cinema/loading frame marks a hard boundary for physical optics. Never
	// carry an old weapon/reticle target into a frontend restart or world rebuild.
	ResetTrackedScopeState();
	Dlaa::ResetHistory();
	if(!gDlaaStereoActivationReady && !gDlaaStereoActivationFailed)
		gDlaaStereoWarmupFrames = DLAA_ACTIVATION_WARMUP_FRAMES;
	if(!camera || !BeginXrFrame()) return false;
	if(!gFrameState.shouldRender){ EndXrFrame(nil,0); return false; }
	if(gVrLoggedRenderableFrames < 10)
		VrLog("SubmitCinemaFrame begin\n");
	if(holdLastFrame){
		if(!gCinemaFrameValid || !gCinemaSwapchain.handle ||
		   gCinemaSwapchain.width <= 0 || gCinemaSwapchain.height <= 0){
			// Never reveal the partially unloaded world. A missing previous cinema
			// image is rare (first frame/session loss), and black is the safe fallback.
			EndXrFrame(nil, 0);
			return false;
		}
		XrCompositionLayerQuad cinema = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		ApplyCinemaQuadPose(&cinema);
		cinema.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		cinema.subImage.swapchain = gCinemaSwapchain.handle;
		cinema.subImage.imageRect.extent = {
			gCinemaSwapchain.width, gCinemaSwapchain.height
		};
		cinema.size.width = 3.2f;
		cinema.size.height = 3.2f*(float)gCinemaSwapchain.height/
			(float)gCinemaSwapchain.width;
		const XrCompositionLayerBaseHeader *layers[] = {
			(const XrCompositionLayerBaseHeader*)&cinema
		};
		return EndXrFrame(layers, 1);
	}
#ifdef RW_D3D12
	int width = 0, height = 0;
	rw::d3d12::getPresentSize(&width, &height);
#else
	GLint viewport[4]={}; glGetIntegerv(GL_VIEWPORT,viewport);
	const int width=viewport[2], height=viewport[3];
#endif
	if(width<=0 || height<=0){ EndXrFrame(nil,0); return false; }
	if(!gCinemaSwapchain.handle || gCinemaSwapchain.width!=width || gCinemaSwapchain.height!=height){
		DestroySwapchain(gCinemaSwapchain);
		if(!CreateSwapchain(gCinemaSwapchain,width,height)){ EndXrFrame(nil,0); return false; }
	}
	if(!AcquireSwapchain(gCinemaSwapchain)){ EndXrFrame(nil,0); return false; }
	gCinemaFrameValid = false;
	bool showVrMenu = false;
#ifdef RW_D3D12
	const bool cinemaCopied = rw::d3d12::copyCurrentBackBufferToExternal(
		AcquiredTexture(gCinemaSwapchain)) != 0;
	showVrMenu = UpdateVrMenuSwapchain();
	const bool cinemaGpuSubmitted = FinishD3D12SwapchainWrites();
	const bool cinemaSubmitted = cinemaCopied && cinemaGpuSubmitted;
	if(gVrLoggedRenderableFrames < 10)
		VrLog("SubmitCinemaFrame copy=%d gpu=%d size=%dx%d\n",
			cinemaCopied ? 1 : 0, cinemaSubmitted ? 1 : 0, width, height);
	if(!cinemaSubmitted){
		ReleaseSwapchain(gCinemaSwapchain);
		EndXrFrame(nil,0);
		return false;
	}
	gCinemaFrameValid = true;
#else
	GLint oldDraw=0,oldRead=0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
	glBindFramebuffer(GL_READ_FRAMEBUFFER,0); glReadBuffer(GL_BACK);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gCopyFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,AcquiredTexture(gCinemaSwapchain),0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glBlitFramebuffer(viewport[0],viewport[1],viewport[0]+width,viewport[1]+height,0,0,width,height,GL_COLOR_BUFFER_BIT,GL_LINEAR);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldDraw); glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);
	ReleaseSwapchain(gCinemaSwapchain);
	gCinemaFrameValid = true;
	showVrMenu = UpdateVrMenuSwapchain();
#endif
	XrCompositionLayerQuad cinema={XR_TYPE_COMPOSITION_LAYER_QUAD};
	ApplyCinemaQuadPose(&cinema); cinema.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
	cinema.subImage.swapchain=gCinemaSwapchain.handle; cinema.subImage.imageRect.extent={width,height};
	cinema.size.width=3.2f; cinema.size.height=3.2f*(float)height/width;
	XrCompositionLayerQuad vrMenuLayer={XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(showVrMenu){
		vrMenuLayer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		vrMenuLayer.space=gViewSpace; vrMenuLayer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
		vrMenuLayer.subImage.swapchain=gVrMenuSwapchain.handle;
		vrMenuLayer.subImage.imageRect.extent={VR_MENU_WIDTH,VR_MENU_HEIGHT};
		vrMenuLayer.pose.orientation.w=1.0f; vrMenuLayer.pose.position.z=-1.65f;
		vrMenuLayer.size.width=2.15f;
		vrMenuLayer.size.height=2.15f*(float)VR_MENU_HEIGHT/VR_MENU_WIDTH;
	}
	const XrCompositionLayerBaseHeader *layers[2]; uint32 count=0;
	layers[count++]=(const XrCompositionLayerBaseHeader*)&cinema;
	if(showVrMenu) layers[count++]=(const XrCompositionLayerBaseHeader*)&vrMenuLayer;
	return EndXrFrame(layers,count);
}

bool CaptureStartupWindow(HWND window)
{
	if(CaptureMovieFrameRGBA(gStartupPixels, VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT))
		return true;
	if(!window || !IsWindow(window))
		return false;
	if(!gStartupCaptureDc){
		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = VR_STARTUP_WIDTH;
		info.bmiHeader.biHeight = -VR_STARTUP_HEIGHT;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		gStartupCaptureDc = CreateCompatibleDC(nil);
		gStartupCaptureBitmap = CreateDIBSection(gStartupCaptureDc, &info,
			DIB_RGB_COLORS, &gStartupCaptureBits, nil, 0);
		if(!gStartupCaptureDc || !gStartupCaptureBitmap || !gStartupCaptureBits){
			DestroyStartupCapture();
			return false;
		}
		gStartupCaptureOldBitmap = SelectObject(gStartupCaptureDc, gStartupCaptureBitmap);
	}
	RECT client = {};
	if(!GetClientRect(window, &client) || client.right <= client.left || client.bottom <= client.top)
		return false;
	POINT origin = { client.left, client.top };
	if(!ClientToScreen(window, &origin))
		return false;
	HDC desktop = GetDC(nil);
	if(!desktop)
		return false;
	SetStretchBltMode(gStartupCaptureDc, HALFTONE);
	SetBrushOrgEx(gStartupCaptureDc, 0, 0, nil);
	const BOOL copied = StretchBlt(gStartupCaptureDc, 0, 0,
		VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT, desktop, origin.x, origin.y,
		client.right-client.left, client.bottom-client.top, SRCCOPY | CAPTUREBLT);
	ReleaseDC(nil, desktop);
	if(!copied)
		return false;
	const uint8 *source = (const uint8*)gStartupCaptureBits;
	for(int pixel = 0; pixel < VR_STARTUP_WIDTH*VR_STARTUP_HEIGHT; pixel++){
		gStartupPixels[pixel*4+0] = source[pixel*4+2];
		gStartupPixels[pixel*4+1] = source[pixel*4+1];
		gStartupPixels[pixel*4+2] = source[pixel*4+0];
		gStartupPixels[pixel*4+3] = 255;
	}
	return true;
}

void InitializeTemporalAAEarly()
{
	Dlaa::InitializeEarly();
#ifdef RW_D3D12
	rw::d3d12::setDeviceCreatedCallback(Dlaa::AttachDevice);
#endif
}

void StartEarly()
{
	Dlaa::AttachDevice();
	debug("[OpenXR] %s\n", Dlaa::GetStatus());
	EnsureSession();
}

bool SubmitStartupFrame(void *nativeWindow)
{
	if(!BeginXrFrame())
		return false;
	if(!gFrameState.shouldRender){
		EndXrFrame(nil, 0);
		return false;
	}
	if(!gCinemaSwapchain.handle || gCinemaSwapchain.width != VR_STARTUP_WIDTH ||
	   gCinemaSwapchain.height != VR_STARTUP_HEIGHT){
		DestroySwapchain(gCinemaSwapchain);
		if(!CreateSwapchain(gCinemaSwapchain, VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT)){
			EndXrFrame(nil, 0);
			return false;
		}
	}
	if(!CaptureStartupWindow((HWND)nativeWindow) || !AcquireSwapchain(gCinemaSwapchain)){
		EndXrFrame(nil, 0);
		return false;
	}
	gCinemaFrameValid = false;
#ifdef RW_D3D12
	const bool uploaded = rw::d3d12::uploadRgbaToExternal(
		AcquiredTexture(gCinemaSwapchain), gStartupPixels,
		VR_STARTUP_WIDTH*4, VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT) != 0;
	const bool submitted = FinishD3D12SwapchainWrites();
	if(!uploaded || !submitted){
		EndXrFrame(nil, 0);
		return false;
	}
	gCinemaFrameValid = true;
#else
	GLint oldTexture = 0, oldAlignment = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlignment);
	glBindTexture(GL_TEXTURE_2D, AcquiredTexture(gCinemaSwapchain));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT,
		GL_RGBA, GL_UNSIGNED_BYTE, gStartupPixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlignment);
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	ReleaseSwapchain(gCinemaSwapchain);
	gCinemaFrameValid = true;
#endif
	XrCompositionLayerQuad cinema = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	ApplyCinemaQuadPose(&cinema);
	cinema.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	cinema.subImage.swapchain = gCinemaSwapchain.handle;
	cinema.subImage.imageRect.extent = { VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT };
	cinema.size.width = 3.2f;
	cinema.size.height = 1.8f;
	const XrCompositionLayerBaseHeader *layers[] = {
		(const XrCompositionLayerBaseHeader*)&cinema
	};
	return EndXrFrame(layers, 1);
}

void CancelStereoFrame(RwCamera *camera){
#ifdef RW_D3D12
	rw::d3d12::setStereoWorldEye(-1);
#endif
	Dlaa::ResetHistory();
	RestoreCamera(camera);
#ifdef RW_D3D12
	FinishD3D12SwapchainWrites();
#endif
	if(gFrameBegun) EndXrFrame(nil,0);
}
void SetInactive(){
#ifdef RW_D3D12
	rw::d3d12::setStereoWorldEye(-1);
#endif
	Dlaa::ResetHistory();
	ResetTrackedScopeState();
	gWasSubmitting=false;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		ClearWeaponSupportForHand(hand);
		gHeldWeaponSlot[hand]=-1;
		gTrackedWeaponRenderMatrixSlot[hand]=-1;
		ClearDroppedWeapon(hand);
		gWeaponHolsterSelection[hand]=-1;
		gWeaponHolsterGripDown[hand]=false;
		gTrackedWeaponTriggerPressed[hand]=false;
		gTrackedWeaponTriggerJustPressed[hand]=false;
		gTrackedWeaponTriggerJustReleased[hand]=false;
		gTrackedThrowablePreviewActive[hand]=false;
		gTrackedAimCacheValid[hand]=false;
		ResetManualReloadState(gManualReload[hand]);
		gManualReloadGripDown[hand]=false;
		ResetPhysicalMeleeMotion(hand);
	}
	gTrackedHandPoseValid[0]=gTrackedHandPoseValid[1]=false;
	gTrackedHandAimPoseValid[0]=gTrackedHandAimPoseValid[1]=false;
	gActiveTrackedFireAimValid=false;
	// SetInactive is also used for recoverable OpenXR frame/session hiccups.
	// Do not orphan live C4 just because the headset skipped a frame.
	ResetTrackedDetonatorInteraction(false);
}
void Shutdown()
{
	ResetTrackedScopeState();
	for(int hand = 0; hand < EYE_COUNT; hand++){
		ClearWeaponSupportForHand(hand);
		gHeldWeaponSlot[hand]=-1;
		gTrackedWeaponRenderMatrixSlot[hand]=-1;
		ClearDroppedWeapon(hand);
		gWeaponHolsterSelection[hand]=-1;
		gTrackedAimCacheValid[hand]=false;
		ResetManualReloadState(gManualReload[hand]);
		gManualReloadGripDown[hand]=false;
		ResetPhysicalMeleeMotion(hand);
	}
	gActiveTrackedFireAimValid=false;
	ResetTrackedDetonatorInteraction(true);
	if(gPerfRecording){
		gPerfRecording=false;
		if(gPerfLiveCsv){
			fclose(gPerfLiveCsv);
			gPerfLiveCsv=nil;
		}
		DumpPerfRecording();
	}
	DestroyRuntime();
	Dlaa::Shutdown();
	debug("[OpenXR] Runtime shut down\n");
}
}

#endif
