#include "platform_android.h"
#include "xr_vulkan_session.h"

#include <android_native_app_glue.h>
#include <android/log.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include <atomic>
#include <thread>
#include <unistd.h>

#ifndef MIAMIVR_BRINGUP
#include "common.h"
#include "android.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "Game.h"
#include "PlayerInfo.h"
#include "QuestProfiler.h"
#include "crossplatform.h"
#endif

namespace {

struct AppState
{
	std::atomic<bool> resumed{false};
	std::atomic<bool> destroyRequested{false};
	std::atomic<bool> stopRequested{false};
	std::atomic<bool> gameThreadFinished{false};
};

#ifndef MIAMIVR_BRINGUP
// A high-resolution OpenXR colour swapchain can succeed before librw allocates
// its two full-resolution scene/depth frame contexts. If that later Vulkan
// allocation fails, this process cannot safely resize the live swapchain and
// restart partially initialised RenderWare. Persist a conservative value so
// the user's next explicit launch is recoverable instead of boot-looping at
// the same oversized request.
void
autoResetRenderScaleAfterGameInitialiseFailure(void)
{
	const char *dataRoot = platform::gameDataRoot();
	if(dataRoot == nullptr || dataRoot[0] == '\0'){
		LOGE("render-scale recovery unavailable: game data root is empty");
		return;
	}
	char settingsPath[512];
	const int pathLength = snprintf(settingsPath, sizeof(settingsPath),
		"%s/vr_settings.ini", dataRoot);
	if(pathLength < 0 || pathLength >= (int)sizeof(settingsPath)){
		LOGE("render-scale recovery unavailable: settings path is too long");
		return;
	}

	const int persistedScale = (int)GetPrivateProfileIntA(
		"VR", "RenderScalePercent", 125, settingsPath);
	if(persistedScale <= 100)
		return;

	char failedRequest[16];
	snprintf(failedRequest, sizeof(failedRequest), "%d", persistedScale);
	char failureReason[16];
	snprintf(failureReason, sizeof(failureReason), "%d",
		xrvk::RENDER_SCALE_FALLBACK_GAME_RENDERER_ALLOCATION);
	const bool historyWritten =
		WritePrivateProfileStringA("VR", "RenderScaleLastFallbackRequest",
			failedRequest, settingsPath) &&
		WritePrivateProfileStringA("VR", "RenderScaleLastFallbackPercent",
			"100", settingsPath) &&
		WritePrivateProfileStringA("VR", "RenderScaleLastFallbackReason",
			failureReason, settingsPath);
	if(WritePrivateProfileStringA(
	     "VR", "RenderScalePercent", "100", settingsPath)){
		LOGE("game initialisation failed with persisted render scale %d%%; "
		     "auto-reset to 100%% for next launch (%s, history %s)",
		     persistedScale, settingsPath,
		     historyWritten ? "saved" : "FAILED");
	}else{
		LOGE("game initialisation failed with persisted render scale %d%%; "
		     "AUTO-RESET TO 100%% FAILED (%s)",
		     persistedScale, settingsPath);
	}
}

// The headset swapchain is portrait per eye, but theatre content is shown on
// a 16:9 physical quad. Build a conventional mono camera for that physical
// aspect so the picture does not inherit head pose, eye separation or lens
// asymmetry. Stretching the portrait storage onto the matching 16:9 quad is
// intentional: projection and display aspect cancel, leaving normal desktop
// proportions without another full-size render target.
void
makeTheaterMatrices(float viewProj[16], float im2dWorld[16],
                    float *distanceOut)
{
	for(int i = 0; i < 16; i++){
		viewProj[i] = 0.0f;
		im2dWorld[i] = 0.0f;
	}

	const float aspect = 16.0f/9.0f;
	const float nearZ = 0.05f;
	const float farZ = 1000.0f;
	const float tanHalfY = tanf(60.0f*0.5f*3.1415926535f/180.0f);
	viewProj[0] = 1.0f/(tanHalfY*aspect);
	// Vulkan's framebuffer Y axis points down.
	viewProj[5] = -1.0f/tanHalfY;
	viewProj[10] = -farZ/(farZ-nearZ);
	viewProj[11] = -1.0f;
	viewProj[14] = -(farZ*nearZ)/(farZ-nearZ);

	const float distance = 2.0f;
	const float planeHeight = 2.0f*distance*tanHalfY;
	const float planeWidth = planeHeight*aspect;
	const float screenWidth = RsGlobal.width > 0 ? (float)RsGlobal.width : 1.0f;
	const float screenHeight = RsGlobal.height > 0 ? (float)RsGlobal.height : 1.0f;
	im2dWorld[0] = planeWidth/screenWidth;
	im2dWorld[5] = -planeHeight/screenHeight;
	im2dWorld[10] = 1.0f;
	im2dWorld[12] = -planeWidth*0.5f;
	im2dWorld[13] = planeHeight*0.5f;
	im2dWorld[14] = -distance;
	im2dWorld[15] = 1.0f;
	*distanceOut = distance;
}

struct MenuTransitionTraceState
{
	bool initialized;
	uint64 frame;
	int theaterBefore;
	int theaterAfter;
	int gameState;
	int gameNotLoaded;
	int menuActive;
	int wantRestart;
	int wantLoad;
	int playingIntro;
	int cutsceneProcessing;
	int cutsceneRunning;
	int widescreen;
	int playerPresent;
	int currentScreen;
	uint32 colourMode;
};

MenuTransitionTraceState gMenuTransitionTrace = {};

int64_t
monotonicNowNs(void)
{
	timespec now = {};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec*1000000000LL + now.tv_nsec;
}

// The startup artifact is only visible for a fraction of a second, so log the
// first few seconds and every later state transition to a small persistent
// file.  This is intentionally outside logcat: the user can reproduce once,
// quit, and we can pull the exact transition without keeping adb attached.
void
traceMenuTransition(bool theaterBefore, bool theaterAfter,
	bool theaterSubmitted, uint32 colourMode,
	int64_t frameStartNs, int64_t beginStartNs, int64_t beginEndNs,
	int64_t stepEndNs, int64_t endFrameEndNs)
{
	MenuTransitionTraceState current = {};
	current.initialized = true;
	current.frame = gMenuTransitionTrace.frame;
	current.theaterBefore = theaterBefore;
	current.theaterAfter = theaterAfter;
	current.gameState = (int)gGameState;
	current.gameNotLoaded = FrontEndMenuManager.m_bGameNotLoaded;
	current.menuActive = FrontEndMenuManager.m_bMenuActive;
	current.wantRestart = FrontEndMenuManager.m_bWantToRestart;
	current.wantLoad = FrontEndMenuManager.m_bWantToLoad;
	current.playingIntro = CGame::playingIntro;
	current.cutsceneProcessing = CCutsceneMgr::IsCutsceneProcessing();
	current.cutsceneRunning = CCutsceneMgr::IsRunning();
	current.widescreen = TheCamera.m_WideScreenOn;
	current.playerPresent = FindPlayerPed() != nil;
	current.currentScreen = FrontEndMenuManager.m_nCurrScreen;
	current.colourMode = colourMode;

	const MenuTransitionTraceState &previous = gMenuTransitionTrace;
	const bool changed = !previous.initialized ||
		current.theaterBefore != previous.theaterBefore ||
		current.theaterAfter != previous.theaterAfter ||
		current.gameState != previous.gameState ||
		current.gameNotLoaded != previous.gameNotLoaded ||
		current.menuActive != previous.menuActive ||
		current.wantRestart != previous.wantRestart ||
		current.wantLoad != previous.wantLoad ||
		current.playingIntro != previous.playingIntro ||
		current.cutsceneProcessing != previous.cutsceneProcessing ||
		current.cutsceneRunning != previous.cutsceneRunning ||
		current.widescreen != previous.widescreen ||
		current.playerPresent != previous.playerPresent ||
		current.currentScreen != previous.currentScreen ||
		current.colourMode != previous.colourMode;
	const bool earlyFrame = current.frame < 288; // Four seconds at 72 Hz.

	if(earlyFrame || changed){
		static FILE *traceFile = nullptr;
		if(traceFile == nullptr){
			const char *root = platform::storageRoot();
			if(root != nullptr && root[0] != '\0'){
				char path[512];
				const int pathLength = snprintf(path, sizeof(path),
					"%s/menu_transition.log", root);
				if(pathLength > 0 && pathLength < (int)sizeof(path))
					traceFile = fopen(path, "w");
			}
			if(traceFile != nullptr){
				xrvk::RenderScaleStatus scale = {};
				uint32 temporalMode = 0, sceneWidth = 0, sceneHeight = 0;
				uint32 outputWidth = 0, outputHeight = 0;
				const bool scaleValid = xrvk::getRenderScaleStatus(&scale);
				const bool temporalValid = rw::vulkan::getSgsrStatus(
					&temporalMode, &sceneWidth, &sceneHeight,
					&outputWidth, &outputHeight);
				fprintf(traceFile,
					"# scale valid=%d request=%d selected=%d effective=%.2f "
					"base=%ux%u actual=%ux%u fallback=%d\n",
					scaleValid, scale.requestedPercent,
					scale.selectedPresetPercent, scale.effectivePercent,
					scale.recommendedWidth, scale.recommendedHeight,
					scale.actualWidth, scale.actualHeight,
					scale.fallbackReason);
				fprintf(traceFile,
					"# temporal valid=%d mode=%u scene=%ux%u output=%ux%u\n",
					temporalValid, temporalMode, sceneWidth, sceneHeight,
					outputWidth, outputHeight);
				fprintf(traceFile,
					"frame,time_ns,theater_before,theater_after,theater_submitted,"
					"game_state,game_not_loaded,menu_active,want_restart,want_load,"
					"playing_intro,cutscene_processing,cutscene_running,widescreen,"
					"player_present,screen,colour_mode,cpu_setup_ms,cpu_begin_ms,"
					"cpu_step_ms,cpu_end_ms,cpu_total_ms\n");
			}
		}
		if(traceFile != nullptr){
			fprintf(traceFile,
				"%llu,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,"
				"%.3f,%.3f,%.3f,%.3f,%.3f\n",
				(unsigned long long)current.frame,
				(long long)xrvk::getPredictedDisplayTimeNs(),
				current.theaterBefore, current.theaterAfter,
				theaterSubmitted, current.gameState,
				current.gameNotLoaded, current.menuActive,
				current.wantRestart, current.wantLoad,
				current.playingIntro, current.cutsceneProcessing,
				current.cutsceneRunning, current.widescreen,
				current.playerPresent, current.currentScreen,
				current.colourMode,
				(double)(beginStartNs-frameStartNs)/1000000.0,
				(double)(beginEndNs-beginStartNs)/1000000.0,
				(double)(stepEndNs-beginEndNs)/1000000.0,
				(double)(endFrameEndNs-stepEndNs)/1000000.0,
				(double)(endFrameEndNs-frameStartNs)/1000000.0);
			fflush(traceFile);
		}
	}

	current.frame++;
	gMenuTransitionTrace = current;
}

// Installed as the OpenXR layer's frame renderer once the game is up. Opens
// librw's multiview render pass against the swapchain image the runtime just
// handed over, steps the game inside it, and submits.
void
renderGameFrame(VkImage image, VkImageView view, const float viewProj[2][16],
                const float im2dWorld[16], float im2dDistance,
                const float headPos[3], float headYaw,
                const float headQuat[4], float eyeFovDeg,
                const float eyePos[2][3], const float eyeQuat[2][4])
{
	const int64_t frameStartNs = monotonicNowNs();
	androidgame::QuestProfilerBeginAppFrame();
	xrvk::ControllerInput controllers;
	xrvk::getInput(&controllers);
	androidgame::PadInput pad;
	pad.leftStickX = controllers.leftStickX;
	pad.leftStickY = controllers.leftStickY;
	pad.rightStickX = controllers.rightStickX;
	pad.rightStickY = controllers.rightStickY;
	pad.leftTrigger = controllers.leftTrigger;
	pad.rightTrigger = controllers.rightTrigger;
	pad.leftGrip = controllers.leftGrip;
	pad.rightGrip = controllers.rightGrip;
	pad.a = controllers.a;
	pad.b = controllers.b;
	pad.x = controllers.x;
	pad.y = controllers.y;
	pad.menu = controllers.menu;
	pad.leftStickClick = controllers.leftStickClick;
	pad.rightStickClick = controllers.rightStickClick;
	for(int hand = 0; hand < 2; hand++){
		for(int axis = 0; axis < 3; axis++){
			pad.gripPose[hand].position[axis] =
				controllers.gripPose[hand].position[axis];
			pad.aimPose[hand].position[axis] =
				controllers.aimPose[hand].position[axis];
		}
		for(int axis = 0; axis < 4; axis++){
			pad.gripPose[hand].orientation[axis] =
				controllers.gripPose[hand].orientation[axis];
			pad.aimPose[hand].orientation[axis] =
				controllers.aimPose[hand].orientation[axis];
		}
		pad.gripPose[hand].valid = controllers.gripPose[hand].valid;
		pad.aimPose[hand].valid = controllers.aimPose[hand].valid;
	}
	androidgame::SetPadInput(pad);

	// Debug overlay: the game side owns the chord and the pixels (its port of
	// the desktop panel); the session just carries them to a quad layer.
	androidgame::VrDebugUpdate(pad);
	{
		int width = 0, height = 0;
		const unsigned char *pixels =
			androidgame::VrDebugPixels(&width, &height);
		xrvk::setDebugOverlay(pixels, width, height);
	}

	const bool theaterBefore = androidgame::VrShouldUseTheaterMode();
	xrvk::setTheaterMode(theaterBefore);
	if(theaterBefore){
		float theaterViewProj[16], theaterIm2D[16], theaterDistance;
		makeTheaterMatrices(theaterViewProj, theaterIm2D, &theaterDistance);
		const float origin[3] = { 0.0f, 0.0f, 0.0f };
		const float identityQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		rw::vulkan::setStereoViewProjection(
			theaterViewProj, theaterViewProj);
		rw::vulkan::setIm2DTransform(
			theaterIm2D, theaterDistance, origin);
		rw::vulkan::setHeadPose(origin, 0.0f, identityQuat);
		rw::vulkan::clearFirstPersonEyePoses();
	}else{
		rw::vulkan::setStereoViewProjection(viewProj[0], viewProj[1]);
		rw::vulkan::setSgsrHorizontalFovDegrees(eyeFovDeg);
		rw::vulkan::setIm2DTransform(im2dWorld, im2dDistance, headPos);
		rw::vulkan::setHeadPose(headPos, headYaw, headQuat);
		rw::vulkan::setFirstPersonEyePoses(eyePos, eyeQuat);
	}
	androidgame::SetEyeFovDeg(eyeFovDeg);
	// Game time follows the display clock, not the wall clock; see
	// getPredictedDisplayTimeNs.
	androidgame::SetFrameTimeNs(xrvk::getPredictedDisplayTimeNs());
	platform::setCheckpoint("vk/beginFrame");
	androidgame::QuestProfilerBeginVkBegin();
	const int64_t beginStartNs = monotonicNowNs();
	if(!rw::vulkan::beginFrame(image, view)){
		androidgame::QuestProfilerCancelAppFrame();
		return;
	}
	const int64_t beginEndNs = monotonicNowNs();
	androidgame::QuestProfilerEndVkBegin();
	platform::setCheckpoint("game/Step");
	androidgame::QuestProfilerBeginStep();
	androidgame::Step();
	const int64_t stepEndNs = monotonicNowNs();
	androidgame::QuestProfilerEndStep();

	// Step owns both simulation and rendering, so a cutscene/menu transition
	// can occur inside it. Submit this frame with the same mode and matrices it
	// started with; switching the compositor layer to theaterAfter here used to
	// present an immersive-rendered image as a flat quad for one frame.
	const bool theaterAfter = androidgame::VrShouldUseTheaterMode();
	const bool theaterFrame = theaterBefore;

	// A cutscene can end inside Step.  That transition frame was prepared
	// above with the theater identity head pose, so first-person gameplay may
	// have latched yaw=0 before the real headset pose is restored.  Drop that
	// provisional anchor now; the next immersive frame will relatch against
	// the actual HMD yaw instead of leaving movement/view rotated by the
	// player's physical facing at the end of the cutscene.
	if(theaterBefore && !theaterAfter)
		rw::vulkan::setFirstPersonAnchor(nullptr, 0.0f, 0, 0);

	// Match the desktop VR renderer: Vice City's colour filter is a final-image
	// operation, after the world and HUD have been assembled. The Vulkan
	// backend applies it to both multiview layers in its resolve subpass.
	// EXTENDED_COLOURFILTER intentionally remains disabled for the old
	// camera-raster implementation on Vulkan. This final-image implementation
	// is independent of it and reproduces the default POSTFX_NORMAL path
	// directly, using the blur colour populated by Vice City's timecycle.
	const bool cutsceneFrame =
		CGame::playingIntro ||
		CCutsceneMgr::IsCutsceneProcessing() ||
		CCutsceneMgr::IsRunning() ||
		TheCamera.m_WideScreenOn;
	const uint32 colourMode =
		(androidgame::VrViceCityColorEnabled() &&
		 (!theaterFrame || cutsceneFrame) &&
		 TheCamera.m_BlurType != MOTION_BLUR_NONE ? 1u : 0u);
	rw::vulkan::setPostFx(colourMode,
		(uint32)TheCamera.m_BlurRed,
		(uint32)TheCamera.m_BlurGreen,
		(uint32)TheCamera.m_BlurBlue,
		1.0f);
	rw::vulkan::setSpatialAaMode((RwUInt32)androidgame::VrSpatialAaMode());
	static uint32 lastColourMode = ~0u;
	if(colourMode != lastColourMode){
		__android_log_print(ANDROID_LOG_INFO, "MiamiVR",
			"Vice City postfx %s (type=%d rgb=%d,%d,%d)",
			colourMode == 2u ? "STARTUP BLACK" :
				(colourMode == 1u ? "COLOUR ON" : "COLOUR OFF"),
			TheCamera.m_BlurType,
			TheCamera.m_BlurRed, TheCamera.m_BlurGreen,
			TheCamera.m_BlurBlue);
		lastColourMode = colourMode;
	}
	platform::setCheckpoint("vk/endFrame");
	androidgame::QuestProfilerBeginVkEnd();
	rw::vulkan::endFrame();
	const int64_t endFrameEndNs = monotonicNowNs();
	androidgame::QuestProfilerEndVkEnd();
	androidgame::QuestProfilerEndAppFrame();
	traceMenuTransition(theaterBefore, theaterAfter, theaterFrame, colourMode,
		frameStartNs, beginStartNs, beginEndNs, stepEndNs, endFrameEndNs);
	platform::setCheckpoint("frame/done");
}
#endif

void
handleAppCmd(android_app *app, int32_t cmd)
{
	AppState *state = (AppState *)app->userData;
	switch(cmd){
	case APP_CMD_RESUME:
		state->resumed = true;
		LOGI("activity lifecycle: RESUME");
		break;
	case APP_CMD_PAUSE:
	case APP_CMD_STOP:
		state->resumed = false;
		LOGI("activity lifecycle: %s",
		     cmd == APP_CMD_PAUSE ? "PAUSE" : "STOP");
		break;
	case APP_CMD_DESTROY:
		state->destroyRequested = true;
		LOGI("activity lifecycle: DESTROY");
		break;
	default:
		break;
	}
}

// The OpenXR session, the game and all rendering live here, off the thread that
// android_main runs on.
//
// This is not an optimisation. android_main's thread is what services the
// native-activity looper: lifecycle commands and the input queue. Loading a
// save or starting a new game keeps CGame inside a single Step() for tens of
// seconds, and while that ran on this thread the looper went unserviced, the UI
// thread blocked on a synchronous command, and Horizon OS raised an ANR --
// whose dialog then sat in the headset blocking further launches.
void
gameThreadMain(android_app *app, AppState *state)
{
	// The OpenXR loader and the runtime both call into Java from whichever
	// thread uses them, so this one has to be known to the VM.
	JNIEnv *env = nullptr;
	app->activity->vm->AttachCurrentThread(&env, nullptr);

	bool sessionCreated = false;
	bool gameStarted = false;
	int createAttempts = 0;

	while(!state->stopRequested.load()){
		if(!sessionCreated){
			// Creation used to wait for APP_CMD_RESUME. It does not need to:
			// the session simply sits in IDLE until the runtime is ready, and
			// waiting meant that whenever Horizon OS chose not to resume the
			// activity the app came up completely inert, which is impossible
			// to tell apart from a hang. Try, and retry on failure.
			if(!xrvk::create(app)){
				// create() can fail after allocating only part of the OpenXR
				// or Vulkan state. A retry must start from an empty State;
				// otherwise the second attempt reuses stale handles.
				xrvk::destroy();
				if(++createAttempts >= 50){
					LOGE("giving up on the OpenXR/Vulkan session");
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}
			sessionCreated = true;
			createAttempts = 0;
		}

		if(!xrvk::pollEvents())
			break;

#ifndef MIAMIVR_BRINGUP
		// Engine setup runs as soon as the session exists rather than waiting
		// for the first frame: rsINITIALIZE and rsRWINITIALIZE only adopt the
		// Vulkan device and build pipelines, they draw nothing.
		if(!gameStarted){
			xrvk::GraphicsContext graphics;
			if(!xrvk::getContext(&graphics)){
				LOGE("no Vulkan context to start the game with");
				break;
			}
			androidgame::VulkanContext context;
			context.instance = graphics.instance;
			context.physicalDevice = graphics.physicalDevice;
			context.device = graphics.device;
			context.queue = graphics.queue;
			context.queueFamilyIndex = graphics.queueFamilyIndex;
			context.width = graphics.width;
			context.height = graphics.height;
			context.renderScaleEffectivePercent =
				graphics.renderScaleEffectivePercent;
			context.viewCount = graphics.viewCount;
			context.colourFormat = graphics.colourFormat;

			bool renderTargetStartupFailure = false;
			if(!androidgame::Initialise(
			     context, &renderTargetStartupFailure)){
				if(renderTargetStartupFailure)
					autoResetRenderScaleAfterGameInitialiseFailure();
				LOGE("game initialisation failed");
				break;
			}
			// Finish the expensive one-time game/frontend setup before an
			// OpenXR frame can be begun.  Running this from Step held the first
			// submitted frame open for hundreds of milliseconds and produced a
			// repeatable compositor/reprojection flash in the main menu.
			if(!androidgame::PrepareFrontendBeforeFrames()){
				LOGE("pre-frame frontend preparation failed");
				androidgame::Shutdown();
				break;
			}
			xrvk::confirmRenderScaleRendererReady();
			xrvk::setFrameRenderer(renderGameFrame);
			gameStarted = true;
		}
		if(gameStarted && androidgame::WantsToQuit())
			break;
#endif

		// The OpenXR runtime owns its own lifecycle, but Android can pause the
		// NativeActivity a little before the STOPPING event reaches us. Do not
		// enter another xrWaitFrame or advance the game during that gap. Event
		// polling above remains active so STOPPING/READY are still consumed and
		// the same session can resume normally from the system UI.
		const bool resumed = state->resumed.load();
		if(resumed && xrvk::shouldRender()){
			// Steps the game inside the frame, through the renderer callback.
			platform::setCheckpoint("xr/renderFrame");
			xrvk::renderFrame();
		}else{
#ifndef MIAMIVR_BRINGUP
			// No frame to draw into, but the game should still advance: a load
			// takes tens of seconds and there is no reason to stall it until
			// the headset is picked up. Every draw path checks for an open
			// frame and does nothing, so this is logic only.
			if(resumed && gameStarted)
				androidgame::Step();
#endif
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

#ifndef MIAMIVR_BRINGUP
	if(gameStarted){
		xrvk::setFrameRenderer(nullptr);
		androidgame::Shutdown();
	}
#endif
	if(sessionCreated)
		xrvk::destroy();

	app->activity->vm->DetachCurrentThread();
	LOGI("game thread finished");
	state->gameThreadFinished.store(true);
	// android_main normally sleeps indefinitely in ALooper_pollOnce. Wake it
	// when OpenXR asks the game thread to exit; otherwise the Activity and
	// process stay alive without a renderer and the next launch resumes that
	// dead instance.
	ALooper_wake(app->looper);
}

} // namespace

void
android_main(android_app *app)
{
	AppState state;
	app->userData = &state;
	app->onAppCmd = handleAppCmd;

	// App-specific external storage. Needs no runtime permission and survives
	// app updates, which makes it the right home for a multi-gigabyte game
	// data set the user pushes over adb once.
	platform::redirectStdioToLog();
	platform::startStallWatchdog();
	platform::setNativeActivity(app->activity);
	platform::setStorageRoot(app->activity->externalDataPath);
#ifndef MIAMIVR_BRINGUP
	androidgame::QuestProfilerSetLogDirectory(
		app->activity->externalDataPath);
#endif

	LOGI("Vice City VR (Quest) starting");

	std::thread gameThread(gameThreadMain, app, &state);

	// This thread now does nothing but keep the looper serviced, so lifecycle
	// commands and input dispatch are always answered promptly no matter how
	// long the game blocks.
	while(!app->destroyRequested && !state.destroyRequested.load() &&
	      !state.gameThreadFinished.load()){
		int events = 0;
		android_poll_source *source = nullptr;
		if(ALooper_pollOnce(-1, nullptr, &events, (void **)&source) < 0)
			continue;
		if(source != nullptr)
			source->process(app, source);
	}

	state.stopRequested = true;
	gameThread.join();

	LOGI("Vice City VR (Quest) exiting");
	ANativeActivity_finish(app->activity);

	// This reverse-engineered game has process-lifetime singletons throughout
	// its audio, streaming and RenderWare state. Horizon may retain a process
	// after the only NativeActivity finishes, then invoke android_main again
	// inside that stale address space. End the application process after a
	// fully joined/cleaned game thread so every explicit relaunch starts from
	// the same state as the first launch.
	_exit(0);
}
