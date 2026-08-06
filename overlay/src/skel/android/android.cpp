#include <time.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform_android.h"

#include "common.h"
#include "crossplatform.h"
#include "platform.h"
#include "skeleton.h"
#include "android.h"

#include "main.h"
#include "FileMgr.h"
#include "Frontend.h"
#include "Game.h"
#include "Pad.h"
#include "Text.h"
#include "Timer.h"
#include "Camera.h"
#include "DMAudio.h"
#include "MemoryMgr.h"
#include "Lists.h"	// CPtrList, needed by PlayerInfo.h
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "CutsceneMgr.h"
#include "Vehicle.h"
#include "World.h"
#include "Draw.h"
#include "vulkan/rwvk.h"
#ifdef GTA_VR_WEAPONS
#include "OculusVR.h"
#endif

#include <android/log.h>
#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "MiamiVR", __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MiamiVR", __VA_ARGS__)

// ---------------------------------------------------------------------------
// Platform globals
// ---------------------------------------------------------------------------

psGlobalType PsGlobal;

extern RsGlobalType RsGlobal;

// Owned by the skeleton on every platform; the desktop builds define these in
// win.cpp / glfw.cpp.
RwUInt32 gGameState;
long _dwOperatingSystemVersion;

// CStreaming sizes its budget as (_dwMemAvailPhys - 10MB)/2. Quest 3 has 8 GB,
// but an Android app never gets to use all of it and the OpenXR compositor
// needs headroom, so the figure reported here is capped well below the
// physical total.
size_t _dwMemAvailPhys;

static bool gRwInitialised = false;
static bool gForegroundApp = true;

// ---------------------------------------------------------------------------
// ps* interface
// ---------------------------------------------------------------------------

#ifdef USE_CUSTOM_ALLOCATOR
extern RwMemoryFunctions memFuncs;
#endif

RwMemoryFunctions *
psGetMemoryFunctions(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	return &memFuncs;
#else
	return nil;
#endif
}

RwBool
psInstallFileSystem(void)
{
	return TRUE;
}

RwBool
psNativeTextureSupport(void)
{
	return TRUE;
}

double
psTimer(void)
{
	// While frames are flowing, game time follows the compositor's predicted
	// display time: it advances in exact display intervals, where the wall
	// clock jitters with thread scheduling -- that jitter was directly
	// visible as vehicles pulsing forward.
	//
	// When frames STOP (blocking loads, paused session) the display time
	// freezes, and a frozen clock hangs anything that waits for time to pass
	// -- New Game stalled forever in exactly that way. So after a short
	// grace the clock switches to advancing by wall time from the frozen
	// value, and a monotonicity clamp bridges the switch back when frames
	// resume.
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const double wallNow = ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;

	extern long long gVrFrameTimeNs;
	static double maxReturned;
	double result;
	if(gVrFrameTimeNs != 0){
		static long long lastSeenNs;
		static double wallAtChange;
		if(gVrFrameTimeNs != lastSeenNs){
			lastSeenNs = gVrFrameTimeNs;
			wallAtChange = wallNow;
		}
		const double frozenFor = wallNow - wallAtChange;
		result = gVrFrameTimeNs / 1000000.0;
		if(frozenFor > 50.0)
			result += frozenFor - 50.0;
	}else
		result = wallNow;

	if(result < maxReturned)
		result = maxReturned;
	maxReturned = result;
	return result;
}

void
psMouseSetPos(RwV2d *pos)
{
	// No pointer device on a headset.
	(void)pos;
}

RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	// The OpenXR layer has already opened the multiview render pass by the time
	// the game reaches here, so the camera update is unconditional.
	if(!RwCameraBeginUpdate(camera)){
		gForegroundApp = FALSE;
		return FALSE;
	}
	return TRUE;
}

void
psCameraShowRaster(RwCamera *camera)
{
	// Presentation belongs to xrEndFrame; nothing to flip here.
	(void)camera;
}

RwImage *
psGrabScreen(RwCamera *camera)
{
	(void)camera;
	return nil;
}

RwBool
psSelectDevice(void)
{
	RwVideoMode videoMode;
	RwEngineGetVideoModeInfo(&videoMode, RwEngineGetCurrentVideoMode());
	if(videoMode.flags & rwVIDEOMODEEXCLUSIVE)
		PsGlobal.fullScreen = TRUE;
	return TRUE;
}

RwInt32
_psGetNumVideModes(void)
{
	// The headset owns the resolution; there is exactly one mode.
	return 1;
}

RwChar **
_psGetVideoModeList(void)
{
	static RwChar *modeName = nil;
	static RwChar *modeList[1];
	if(modeName == nil){
		modeName = (RwChar*)malloc(32);
		snprintf(modeName, 32, "%dx%d", RsGlobal.maximumWidth, RsGlobal.maximumHeight);
	}
	modeList[0] = modeName;
	return modeList;
}

void
_psSelectScreenVM(RwInt32 videoMode)
{
	(void)videoMode;
}

RwBool
_psSetVideoMode(RwInt32 subSystem, RwInt32 videoMode)
{
	(void)subSystem;
	(void)videoMode;
	return TRUE;
}

void
_InputTranslateShiftKeyUpDown(RsKeyCodes *rs)
{
	(void)rs;
}

long
_InputInitialiseMouse(bool exclusive)
{
	(void)exclusive;
	return 0;
}

void _InputShutdownMouse(void) {}
bool _InputMouseNeedsExclusive(void) { return false; }
void _InputInitialiseJoys(void) {}

void
HandleExit(void)
{
	RsGlobal.quit = TRUE;
}

// Saves and settings live beside the staged game data, in the app-specific
// external directory, so they survive an app update and are reachable over adb
// without any storage permission.
const char *
_psGetUserFilesFolder(void)
{
	static char path[512] = "";
	if(path[0] == '\0'){
		const char *root = platform::storageRoot();
		snprintf(path, sizeof(path), "%s/userfiles",
		         root != nil && root[0] != '\0' ? root : ".");
		mkdir(path, 0770);
	}
	return path;
}

static androidgame::PadInput gPadInput;

// Read by psTimer; written once per frame from the XR layer.
long long gVrFrameTimeNs;

namespace androidgame {
void
SetPadInput(const PadInput &input)
{
	gPadInput = input;
}

const PadInput &
GetPadInput(void)
{
	return gPadInput;
}

void
SetFrameTimeNs(long long ns)
{
	gVrFrameTimeNs = ns;
}

void SetEyeFovDeg(float fov);
}

float gVrEyeFovDeg;

void
androidgame::SetEyeFovDeg(float fov)
{
	gVrEyeFovDeg = fov;
}

// Updated once the camera's view window has been derived for the frame.
static float gVrViewWindowX = 0.6f, gVrViewWindowY = 0.6f;

void
androidgame::GetIm2DViewWindow(float *x, float *y)
{
	*x = gVrViewWindowX;
	*y = gVrViewWindowY;
}

void
VrCaptureViewWindow(void)
{
	const RwV2d *window = RwCameraGetViewWindow(Scene.camera);
	if(window != nil && window->x > 0.01f && window->y > 0.01f){
		gVrViewWindowX = window->x;
		gVrViewWindowY = window->y;
	}
}
static float gVrSavedFov = -1.0f;

// Called at the top of DoRWStuffStartOfFrame_Horizon, before CameraSize
// derives the view window: during first person rendering the game runs on
// the eye's real field of view, exactly as the desktop BeginEye does, which
// is what sizes coronas and other screen-space sprites correctly.
void
VrApplyEyeFov(void)
{
	extern bool gVrFirstPersonActive;
	if(gVrFirstPersonActive && gVrEyeFovDeg > 1.0f){
		if(gVrSavedFov < 0.0f)
			gVrSavedFov = CDraw::GetFOV();
		CDraw::SetFOV(gVrEyeFovDeg);
	}
}

// Called when the frame's game step is done: game logic outside rendering
// keeps the original fov, mirroring the desktop's save/restore.
void
VrRestoreFov(void)
{
	if(gVrSavedFov > 0.0f){
		CDraw::SetFOV(gVrSavedFov);
		gVrSavedFov = -1.0f;
	}
}

// First person state, consumed by CRenderer to hide the player's own body.
// The body is replaced by tracked hands on foot and during physical vehicle
// driving, but DEFAULT driving retains Vice City's animated in-car Tommy.
// Set every frame below, next to the pad capture, which runs before rendering.
bool gVrFirstPersonActive;
CEntity *gVrPlayerEntity;
static bool gVrInVehicle;
bool gVrHidePlayerBody = true;

bool
androidgame::VrShouldUseTheaterMode(void)
{
	// This is the Android equivalent of the desktop build's showGameplay
	// predicate. IsRunning covers formal cutscenes, while WideScreenOn covers
	// scripted cinematics such as the opening drive to the Ocean View hotel.
	// The original scripts also use playingIntro while they remove/rebuild the
	// world between opening scenes.  That interval has no valid immersive
	// camera or world to display, so it belongs to the same static theater
	// path instead of exposing the clear-colour sky for one or more frames.
	return gGameState != GS_PLAYING_GAME ||
		FrontEndMenuManager.m_bGameNotLoaded ||
		FrontEndMenuManager.m_bMenuActive ||
		FrontEndMenuManager.m_bWantToRestart ||
		FrontEndMenuManager.m_bWantToLoad ||
		CGame::playingIntro ||
		FindPlayerPed() == nil ||
		CCutsceneMgr::IsCutsceneProcessing() ||
		CCutsceneMgr::IsRunning() ||
		TheCamera.m_WideScreenOn;
}

// Decides whether this frame plays from inside the character's head and, if
// so, anchors the view there. Menus, cutscenes and widescreen sequences keep
// the regular camera anchor -- they are flat content and the cinema treatment
// comes with the full VR layer port.
//
// Called twice per frame: from CapturePad so the stick rotation has a fresh
// view yaw, and again right before the scene renders (after physics) so the
// anchor position is this frame's, not last frame's -- anchoring on the
// pre-physics position made the view trail a moving car into the seat back.
void
VrUpdateFirstPersonAnchor(bool postPhysics)
{
	CPlayerPed *player = FindPlayerPed();
	const bool remoteMode =
		CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode();
	const bool wantFirstPerson =
		gGameState == GS_PLAYING_GAME &&
		player != nil &&
		!remoteMode &&
		!FrontEndMenuManager.m_bMenuActive &&
		!CGame::playingIntro &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!CCutsceneMgr::IsRunning() &&
		!TheCamera.m_WideScreenOn;

	gVrFirstPersonActive = wantFirstPerson;
	gVrPlayerEntity = (CEntity*)player;
	gVrInVehicle = wantFirstPerson && player->InVehicle() &&
		player->m_pMyVehicle != nil;
	// Match the desktop policy: DEFAULT driving uses the original animated
	// vehicle occupant, while on-foot VR and IMMERSIVE/MOTION driving replace
	// Tommy with tracked hands.  CRenderer and CPed both consume this flag, so
	// decide it once here instead of letting their early Vulkan exits hide the
	// DEFAULT occupant unconditionally.
#ifdef GTA_VR_WEAPONS
	gVrHidePlayerBody = !gVrInVehicle ||
		OculusVR::IsImmersiveDrivingActive();
#else
	gVrHidePlayerBody = true;
#endif

	// Verbatim port of the desktop VR base camera (RenderVrStereoFrame in
	// main.cpp): the anchor is the player's HEAD BONE -- animated, seated in
	// vehicles by the sitting animation, updated after physics -- nudged
	// 0.12m forward, oriented by the ped's own basis. The desktop's numbers,
	// nothing reinvented. During death/respawn, or when the clump is gone,
	// the regular game camera takes over, exactly as the desktop does.
	if(wantFirstPerson && player->m_rwObject != nil &&
	   !player->DyingOrDead() && player->m_pFrames[PED_HEAD] != nil){
		CVector forward = player->GetForward();
		forward.Normalise();
		CVector horizontalForward = forward;
		horizontalForward.z = 0.0f;
		if(horizontalForward.MagnitudeSqr() > 0.0001f){
			horizontalForward.Normalise();

			// PlayerControlZelda derives on-foot movement from
			// TheCamera.Orientation.  Quest writes the tracked view straight
			// into the RenderWare camera, so the old chase-camera heading
			// otherwise survives here and turns "forward" by 90 degrees.
			// Keep the renderer anchor below in its standard atan2(y, x)
			// convention; the game camera uses atan2(x, y).
			if(!gVrInVehicle)
				TheCamera.Orientation =
					Atan2(horizontalForward.x, horizontalForward.y);
		}

		CVector head(0.0f, 0.0f, 0.0f);
		player->m_pedIK.GetComponentPosition(head, PED_HEAD);
		head += forward * 0.12f;
#ifdef GTA_VR_WEAPONS
		if(gVrInVehicle){
			// The desktop vehicle view applies both the global vertical seat
			// adjustment and the per-model fore/aft calibration before the
			// OpenXR eye pose. Quest anchors directly on the animated head
			// bone, so fold the same offsets into that anchor here.
			CMatrix vehicleView = player->GetMatrix();
			vehicleView.GetPosition() = head;
			OculusVR::ApplyQuestVehicleViewOffset(&vehicleView);
			head = vehicleView.GetPosition();
		}
#endif

		// Diagnostic: where the anchor actually lands relative to the ped.
		{
			static int n;
			if((n++ % 300) == 0){
				const CVector p = player->GetPosition();
				ALOG("[probe] fp anchor: head %.1f %.1f %.1f | ped %.1f %.1f "
				     "%.1f | inVeh %d", head.x, head.y, head.z,
				     p.x, p.y, p.z, gVrInVehicle);
			}
		}

		bool fullVehicleBasis = false;
#ifdef GTA_VR_WEAPONS
		CVehicle *viewVehicle =
			gVrInVehicle ? player->m_pMyVehicle : nil;
		if(viewVehicle){
			CVector vehicleForward = viewVehicle->GetForward();
			// Unlocked vehicles inherit the complete authored vehicle basis.
			// Bike horizon lock preserves the bike's forward vector (including
			// pitch over hills/wheelies) and replaces only its rolled up vector
			// with world-up. Feeding both through the same basis API also keeps
			// controller/hand conversion consistent with the rendered eyes.
			CVector vehicleUp =
				viewVehicle->IsBike() &&
			 OculusVR::IsQuestBikeHorizonLocked() ?
				CVector(0.0f, 0.0f, 1.0f) :
				viewVehicle->GetUp();
			if(vehicleForward.MagnitudeSqr() > 0.0001f &&
			   vehicleUp.MagnitudeSqr() > 0.0001f){
				vehicleForward.Normalise();
				vehicleUp.Normalise();
				CVector vehicleRight =
					CrossProduct(vehicleForward, vehicleUp);
				if(vehicleRight.MagnitudeSqr() > 0.0001f){
					vehicleRight.Normalise();
					vehicleUp = CrossProduct(
						vehicleRight, vehicleForward);
					vehicleUp.Normalise();
					rw::vulkan::setFirstPersonAnchorBasis(
						&head.x, &vehicleRight.x,
						&vehicleUp.x, &vehicleForward.x,
						Atan2(vehicleForward.y,
						      vehicleForward.x), 1);
					fullVehicleBasis = true;
				}
			}
		}
#endif
		if(!fullVehicleBasis)
			rw::vulkan::setFirstPersonAnchor(&head.x,
				Atan2(forward.y, forward.x), 1, 1);

#ifdef GTA_VR_WEAPONS
		// CapturePad converts controller poses before physics so interactions
		// are evaluated once per game step. The anchor above is the post-
		// physics player/vehicle position used for this rendered frame. Rebase
		// only the visual hand matrices now, otherwise they trail a running
		// player or moving vehicle by one simulation step.
		if(postPhysics)
			OculusVR::RefreshQuestTrackedHandWorldPosesForRender();
#endif

		// Write the actual view into the RenderWare camera frame, as the
		// desktop BeginEye does. Everything that projects against the game
		// camera -- corona placement, vehicle interior visibility angles,
		// sprite screen mathematics -- then sees where the player really
		// looks instead of the chase camera.
		if(postPhysics){
			rw::float32 vr[3], vu[3], va[3], vp[3];
			if(rw::vulkan::getFirstPersonViewFrame(vr, vu, va, vp)){
				RwMatrix *m = RwFrameGetMatrix(RwCameraGetFrame(Scene.camera));
				RwMatrixGetRight(m)->x = vr[0];
				RwMatrixGetRight(m)->y = vr[1];
				RwMatrixGetRight(m)->z = vr[2];
				RwMatrixGetUp(m)->x = vu[0];
				RwMatrixGetUp(m)->y = vu[1];
				RwMatrixGetUp(m)->z = vu[2];
				RwMatrixGetAt(m)->x = va[0];
				RwMatrixGetAt(m)->y = va[1];
				RwMatrixGetAt(m)->z = va[2];
				RwMatrixGetPos(m)->x = vp[0];
				RwMatrixGetPos(m)->y = vp[1];
				RwMatrixGetPos(m)->z = vp[2];
				RwMatrixUpdate(m);
				RwFrameUpdateObjects(RwCameraGetFrame(Scene.camera));
				RwFrameOrthoNormalize(RwCameraGetFrame(Scene.camera));
				TheCamera.m_viewMatrix.Update();
			}
		}
	}else{
		// Diagnostic: which gate dropped the view onto the chase camera.
		{
			static int n;
			if((n++ % 300) == 0)
				ALOG("[probe] fp OFF: want %d rw %p dying %d headframe %p",
				     wantFirstPerson,
				     player ? (void*)player->m_rwObject : nil,
				     player ? (int)player->DyingOrDead() : -1,
				     player ? (void*)player->m_pFrames[PED_HEAD] : nil);
		}
		rw::vulkan::setFirstPersonAnchor(nil, 0.0f, 0, 0);
	}
}

// XINPUT is off on this platform, so CPad routes through CapturePad. This is
// the same seam the desktop build uses to inject tracked-controller state.
static bool gQuestSnapTurnStickLatched;

static void
ApplyQuestSnapTurn(float stickX)
{
	if(!androidgame::VrUsesSnapTurn() || FindPlayerVehicle() ||
	   CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode() ||
	   gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bGameNotLoaded ||
	   CGame::playingIntro ||
	   CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn){
		gQuestSnapTurnStickLatched = fabsf(stickX) >= 0.35f;
		return;
	}
	if(fabsf(stickX) <= 0.35f){
		gQuestSnapTurnStickLatched = false;
		return;
	}
	if(gQuestSnapTurnStickLatched || fabsf(stickX) < 0.70f)
		return;

	gQuestSnapTurnStickLatched = true;
	const float delta = -copysignf(
		DEGTORAD((float)androidgame::VrSnapTurnAngleDegrees()), stickX);
	CCam &cam = TheCamera.Cams[TheCamera.ActiveCam];
	cam.Beta += delta;
	cam.m_fTrueBeta += delta;
	cam.m_fTargetBeta += delta;
	while(cam.Beta >= PI) cam.Beta -= TWOPI;
	while(cam.Beta < -PI) cam.Beta += TWOPI;
	while(cam.m_fTrueBeta >= PI) cam.m_fTrueBeta -= TWOPI;
	while(cam.m_fTrueBeta < -PI) cam.m_fTrueBeta += TWOPI;
	while(cam.m_fTargetBeta >= PI) cam.m_fTargetBeta -= TWOPI;
	while(cam.m_fTargetBeta < -PI) cam.m_fTargetBeta += TWOPI;

	CPlayerPed *player = FindPlayerPed();
	if(player){
		player->m_fRotationCur += delta;
		player->m_fRotationDest += delta;
		player->SetHeading(player->m_fRotationCur);
	}
}

void
CapturePad(RwInt32 padID)
{
	if(padID != 0)
		return;

	VrUpdateFirstPersonAnchor(false);

	CControllerState &state = CPad::GetPad(0)->PCTempJoyState;
	state.Clear();

	const androidgame::PadInput &in = gPadInput;
	if(androidgame::VrMenuConsumesInput())
		return;

	// Thumbstick Y is up-positive on OpenXR and down-positive in the game.
	//
	// Full deflection is 128, not the int16 range: the game divides these by
	// 128 to get a fraction. Scaling to 32767 made every axis read 256 times
	// too far, which spun the camera a full turn per frame and threw the pitch
	// past vertical -- the world appeared to tumble and the player to walk up
	// walls. The 0.3 deadzone and the leave-alone-when-inside behaviour match
	// the desktop skeletons in glfw.cpp and win.cpp.
	const float stickDeadzone = 0.3f;
	float rightStickX = clamp(in.rightStickX, -1.0f, 1.0f);
	const bool remoteMode =
		CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode();
	ApplyQuestSnapTurn(rightStickX);
#ifdef GTA_VR_WEAPONS
	const bool trackedScopeActive = OculusVR::IsTrackedScopeActive();
#else
	const bool trackedScopeActive = false;
#endif
	const bool onFootGameplay =
		gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!FrontEndMenuManager.m_bMenuActive &&
		!CGame::playingIntro &&
		!FindPlayerVehicle() &&
		!remoteMode &&
		!CCutsceneMgr::IsRunning() &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!trackedScopeActive &&
		!TheCamera.m_WideScreenOn;
	float localHeadYaw = 0.0f;
	const bool localHeadYawValid =
		onFootGameplay &&
		rw::vulkan::getFirstPersonLocalHeadYaw(&localHeadYaw);
	if(onFootGameplay){
		if(androidgame::VrUsesSnapTurn())
			rightStickX = 0.0f;
		else
			rightStickX = clamp(
				rightStickX*androidgame::VrSmoothTurnScale(),
				-1.0f, 1.0f);
		if(localHeadYawValid &&
		   androidgame::VrUsesExperimentalHeadTurning()){
			const float moveMagnitudeSqr =
				in.leftStickX*in.leftStickX+
				in.leftStickY*in.leftStickY;
			if(moveMagnitudeSqr >= 0.20f*0.20f){
				const float deadZone = DEGTORAD(10.0f);
				const float fullSpeedAngle = DEGTORAD(55.0f);
				const float absoluteYaw = fabsf(localHeadYaw);
				if(absoluteYaw > deadZone){
					float axis = clamp(
						(absoluteYaw-deadZone)/
						(fullSpeedAngle-deadZone),
						0.0f, 1.0f);
					axis = axis*axis*(3.0f-2.0f*axis);
					axis = copysignf(axis, -localHeadYaw);
					rightStickX = clamp(
						rightStickX+
						axis*androidgame::VrHeadTurnScale(),
						-1.0f, 1.0f);
				}
			}
		}
	}
	float moveStickX = clamp(in.leftStickX, -1.0f, 1.0f);
	float moveStickY = clamp(in.leftStickY, -1.0f, 1.0f);
	if(localHeadYawValid && androidgame::VrUsesHeadRelativeMovement()){
		// Rotate only by local HMD yaw. Pitch/roll never alter locomotion and
		// the latched facing makes recentering a true new forward direction.
		const float cosine = cosf(localHeadYaw);
		const float sine = sinf(localHeadYaw);
		const float headX = moveStickX*cosine-moveStickY*sine;
		const float headY = moveStickY*cosine+moveStickX*sine;
		moveStickX = headX;
		moveStickY = headY;
	}
	const float sticks[4] = {
		moveStickX,
		-moveStickY,
		rightStickX,
		clamp(-in.rightStickY, -1.0f, 1.0f)
	};
	int16 *const axes[4] = {
		&state.LeftStickX, &state.LeftStickY,
		&state.RightStickX, &state.RightStickY
	};
	for(int i = 0; i < 4; i++)
		if(Abs(sticks[i]) > stickDeadzone)
			*axes[i] = (int16)(sticks[i] * 128.0f);

	// Left movement and vertical camera input remain the classic pad mapping.
	// Horizontal turn is the same configurable smooth/snap path as desktop.

	const int padMode = CPad::GetPad(0)->GetMode();
	// Remote-control missions keep A for the stock vehicle-fire action (the
	// demolition helicopter drops its bomb through that path). The accelerator
	// remains on R2 below, so A must not also leak into Cross in remote mode.
	state.Cross    = !remoteMode && in.a ? 255 : 0;
	state.Circle   = in.b ? 255 : 0;
	state.Square   = in.x ? 255 : 0;
	state.Triangle = in.y ? 255 : 0;
	state.Start    = in.menu ? 255 : 0;

	// Trigger and grip routing copied from the desktop VR layer's pad
	// assembly: the right trigger is the accelerator (Cross), the left is
	// brake/reverse (Square), and the grips are analogue shoulder buttons.
	// TriggerValue there is a 0..255 scale of the analogue value.
	const int16 leftTriggerValue =
		(int16)(clamp(in.leftTrigger,  0.0f, 1.0f)*255.0f);
	const int16 rightTriggerValue =
		(int16)(clamp(in.rightTrigger, 0.0f, 1.0f)*255.0f);
	state.Square = Max(state.Square, leftTriggerValue);
	state.Cross  = Max(state.Cross,  rightTriggerValue);
	state.LeftShoulder1  =
		(int16)(clamp(in.leftGrip,  0.0f, 1.0f)*255.0f);
	state.RightShoulder1 =
		(int16)(clamp(in.rightGrip, 0.0f, 1.0f)*255.0f);
	if(remoteMode && in.a){
		if(padMode == 3)
			state.RightShoulder1 = 255;
		else
			state.Circle = 255;
	}
	state.LeftShock  = in.leftStickClick  ? 255 : 0;
	state.RightShock = in.rightStickClick ? 255 : 0;

	// The frontend walks its menus on the d-pad, so mirror the left stick onto
	// it; without this the stick moves the player but cannot pick a menu item.
	const float deadzone = 0.5f;
	state.DPadLeft  = in.leftStickX < -deadzone ? 255 : 0;
	state.DPadRight = in.leftStickX >  deadzone ? 255 : 0;
	state.DPadUp    = in.leftStickY >  deadzone ? 255 : 0;
	state.DPadDown  = in.leftStickY < -deadzone ? 255 : 0;
}

// GetLocalTime_CP comes from src/skel/crossplatform.cpp, which is already in
// the build for every non-Windows target.

void
InitialiseLanguage(void)
{
	// The headset has no reliable per-title locale, and the shipped TEXT set is
	// American. Anything else can be selected from the in-game menu.
	CGame::nastyGame = true;
	CGame::frenchGame = false;
	CGame::germanGame = false;
	CGame::noProstitutes = false;
	FrontEndMenuManager.m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;
	TheText.Unload();
	TheText.Load();
}

RwBool
psInitialize(void)
{
	RsGlobal.ps = &PsGlobal;

	PsGlobal.fullScreen = TRUE;
	PsGlobal.lastMousePos.x = PsGlobal.lastMousePos.y = 0.0f;
	PsGlobal.mouseWheel = 0.0;
	PsGlobal.cursorIsInWindow = FALSE;
	PsGlobal.joy1id = -1;
	PsGlobal.joy2id = -1;

	const long pages = sysconf(_SC_PHYS_PAGES);
	const long pageSize = sysconf(_SC_PAGESIZE);
	size_t physical = pages > 0 && pageSize > 0 ?
		(size_t)pages * (size_t)pageSize : (size_t)2048 * 1024 * 1024;
	const size_t cap = (size_t)1536 * 1024 * 1024;
	_dwMemAvailPhys = physical < cap ? physical : cap;
	ALOG("physical memory %zu MB, reporting %zu MB to the streamer",
	     physical / (1024*1024), _dwMemAvailPhys / (1024*1024));

	CFileMgr::Initialise();
	InitialiseLanguage();
	return TRUE;
}

void
psTerminate(void)
{
}

// ---------------------------------------------------------------------------
// Game lifecycle driven by the OpenXR application layer
// ---------------------------------------------------------------------------

namespace androidgame {

bool
Initialise(const VulkanContext &context)
{
	RsGlobal.appName = "Vice City VR";
	RsGlobal.maximumWidth = (RwInt32)context.width;
	RsGlobal.maximumHeight = (RwInt32)context.height;
	RsGlobal.width = (RwInt32)context.width;
	RsGlobal.height = (RwInt32)context.height;
	RsGlobal.quit = FALSE;
	RsGlobal.maxFPS = 90;

	// The game addresses its data with relative paths throughout, right down
	// to statvfs("models/gta3.img") in CdStreamInit. Rather than rewrite every
	// call site, make the staged data directory the working directory; re3's
	// existing POSIX casepath/fcaseopen then absorbs the DATA\\GTA_VC.DAT
	// spelling against a case-sensitive filesystem.
	const char *dataRoot = platform::gameDataRoot();
	if(dataRoot == nil || dataRoot[0] == '\0' || chdir(dataRoot) != 0){
		ALOGE("cannot enter game data directory '%s'",
		      dataRoot != nil ? dataRoot : "(null)");
		return false;
	}
	ALOG("working directory: %s", dataRoot);

	if(RsEventHandler(rsINITIALIZE, nil) == rsEVENTERROR){
		ALOGE("rsINITIALIZE failed");
		return false;
	}

	// RsRwInitialize takes a raw void* and wraps it in RwEngineOpenParams
	// itself, and RwEngineOpen then does
	//   openParams = *(rw::EngineOpenParams*)initParams->displayID;
	// on every non-D3D platform. So the rsRWINITIALIZE parameter is the
	// backend struct directly -- wrapping it here as well would leave the
	// backend reading the struct one indirection off. It must outlive the call.
	static rw::EngineOpenParams backendParams;
	backendParams.instance = context.instance;
	backendParams.physicalDevice = context.physicalDevice;
	backendParams.device = context.device;
	backendParams.queue = context.queue;
	backendParams.queueFamilyIndex = context.queueFamilyIndex;
	backendParams.width = context.width;
	backendParams.height = context.height;
	backendParams.viewCount = context.viewCount;
	backendParams.colourFormat = context.colourFormat;

	if(RsEventHandler(rsRWINITIALIZE, &backendParams) == rsEVENTERROR){
		ALOGE("rsRWINITIALIZE failed");
		RsEventHandler(rsTERMINATE, nil);
		return false;
	}
	gRwInitialised = true;

	RwRect r;
	r.x = 0;
	r.y = 0;
	r.w = RsGlobal.maximumWidth;
	r.h = RsGlobal.maximumHeight;
	RsEventHandler(rsCAMERASIZE, &r);

	ALOG("game initialised at %ux%u, %u view(s)",
	     context.width, context.height, context.viewCount);
	return true;
}

void
Step(void)
{
	if(!gForegroundApp || !gRwInitialised)
		return;

	static RwUInt32 reportedState = 0xFFFFFFFF;
	if(gGameState != reportedState){
		ALOG("gGameState -> %u", gGameState);
		reportedState = gGameState;
	}

	// Keeps the stall watchdog honest: without this the checkpoint stops
	// moving whenever the game is somewhere that has none, and an idle-but-
	// healthy frontend looks identical to a hang.
	RE3_CHECKPOINT("game/Step running");

	// Starting a new game or loading a save asks for a restart rather than
	// continuing the current state machine. Mirrors the desktop skeleton's
	// handling of the same flag.
	if(FrontEndMenuManager.m_bWantToRestart){
		ALOG("restart requested (load=%d, firstTime=%d)",
		     (int)FrontEndMenuManager.m_bWantToLoad,
		     (int)FrontEndMenuManager.m_bFirstTime);

		CPad::ResetCheats();
		CPad::StopPadsShaking();
		DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);
		CTimer::Stop();

		if(FrontEndMenuManager.m_bWantToLoad){
			// The desktop frontend constructs a normal game world before it
			// enters the restart/load path.  The Android frame loop used to
			// service m_bWantToRestart first, while we were still in
			// GS_INIT_PLAYING_GAME, so ShutDownForRestart tried to clear pools
			// which had never been created (first the replay vehicle pool, then
			// the temporary-object pool).  Prime the same baseline world here
			// and only then run the ordinary restart/load sequence.
			if(gGameState != GS_PLAYING_GAME){
				ALOG("initialising baseline world before frontend save load (state=%u)",
				     gGameState);
				InitialiseGame();
				FrontEndMenuManager.m_bGameNotLoaded = false;
				gGameState = GS_PLAYING_GAME;
			}
			CGame::ShutDownForRestart();
			CGame::InitialiseWhenRestarting();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			LoadSplash(GetLevelSplashScreen(CGame::currLevel));
			FrontEndMenuManager.m_bWantToLoad = false;
		}else{
			if(gGameState == GS_PLAYING_GAME)
				CGame::ShutDown();
			CTimer::Stop();
			gGameState = FrontEndMenuManager.m_bFirstTime ?
				GS_INIT_FRONTEND : GS_INIT_PLAYING_GAME;
		}

		FrontEndMenuManager.m_bFirstTime = false;
		FrontEndMenuManager.m_bWantToRestart = false;
		return;
	}

	// Heartbeat: distinguishes "one call never returned" from "the frame loop
	// is spinning and calling Step over and over".
	static unsigned int steps = 0;
	static unsigned int reportedSteps = 0;
	if(++steps - reportedSteps >= 200){
		reportedSteps = steps;
		ALOG("Step heartbeat: %u calls, gGameState=%u", steps, gGameState);

		// Diagnostic for the reversed player model: if the ped's forward in
		// game data points away from the camera while the rendered model faces
		// it, the fault is in the renderer's bone path; if the data itself
		// tracks the camera, it is game logic. GetForward is the frame's
		// heading in world space, so this compares like with like.
		CPlayerPed *player = FindPlayerPed();
		if(player != nil && gGameState == GS_PLAYING_GAME){
			const CVector pedFwd = player->GetForward();
			const CVector pedPos = player->GetPosition();
			const CVector camFwd = TheCamera.GetForward();
			const CVector camPos = TheCamera.GetPosition();
			// Which side of the player the camera sits on decides whether
			// "camera forward equals ped forward" means back view or front
			// view. dotSide > 0: camera is IN FRONT of the ped.
			const CVector toCam = camPos - pedPos;
			const float dotSide = toCam.x*pedFwd.x + toCam.y*pedFwd.y;
			ALOG("[probe] ped fwd %.2f %.2f pos %.1f %.1f | cam fwd %.2f %.2f "
			     "pos %.1f %.1f %.1f side %.1f",
			     pedFwd.x, pedFwd.y, pedPos.x, pedPos.y,
			     camFwd.x, camFwd.y, camPos.x, camPos.y, camPos.z, dotSide);
		}
	}

	// Mirrors the desktop skeleton's gGameState machine, minus the startup
	// movies: there is no 2D window to play them into, and the VR build shows
	// its own world-locked theatre instead.
	switch(gGameState){
	case GS_START_UP:
		gGameState = GS_INIT_ONCE;
		break;

	case GS_INIT_ONCE:
		ALOG("GS_INIT_ONCE: loading screen");
		LoadingScreen(nil, nil, "loadsc0");
		ALOG("GS_INIT_ONCE: InitialiseOnceAfterRW");
		if(!CGame::InitialiseOnceAfterRW()){
			ALOGE("InitialiseOnceAfterRW failed");
			RsGlobal.quit = TRUE;
			break;
		}
		ALOG("GS_INIT_ONCE: done");
		gGameState = GS_INIT_FRONTEND;
		break;

	case GS_INIT_FRONTEND:
		LoadingScreen(nil, nil, "loadsc0");
		FrontEndMenuManager.m_bGameNotLoaded = true;
		FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
		gGameState = GS_FRONTEND;
		break;

	case GS_FRONTEND:
		RsEventHandler(rsFRONTENDIDLE, nil);
		if(!FrontEndMenuManager.m_bMenuActive ||
		   FrontEndMenuManager.m_bWantToLoad)
			gGameState = GS_INIT_PLAYING_GAME;
		break;

	case GS_INIT_PLAYING_GAME:
		InitialiseGame();
		FrontEndMenuManager.m_bGameNotLoaded = false;
		gGameState = GS_PLAYING_GAME;
		break;

	case GS_PLAYING_GAME:
		// No frame limiter: xrWaitFrame already paces the application to the
		// headset's refresh rate, and a second limiter on top of it only adds
		// judder.
		RsEventHandler(rsIDLE, (void*)TRUE);
		// Rendering ran on the eye fov; game logic keeps the original.
		::VrRestoreFov();
		break;

	default:
		break;
	}
}

bool
WantsToQuit(void)
{
	// m_bWantToRestart is not a request to quit. It means "tear the session
	// down and come back", which is what the frontend sets when a new game is
	// started or a save is loaded. Treating it as quit ran the full CGame
	// shutdown instead -- the log showed REMOVE frontend and CPedStats
	// shutting down the instant New Game was picked, ending in a double
	// pthread_mutex_destroy. Restarting is handled in Step.
	return RsGlobal.quit;
}

void
Shutdown(void)
{
	if(gRwInitialised){
		RsEventHandler(rsRWTERMINATE, nil);
		gRwInitialised = false;
	}
	RsEventHandler(rsTERMINATE, nil);
}

} // namespace androidgame
