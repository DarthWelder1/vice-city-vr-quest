#include "platform_android.h"
#include "xr_vulkan_session.h"

#include <android_native_app_glue.h>
#include <android/log.h>
#include <math.h>
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

// Installed as the OpenXR layer's frame renderer once the game is up. Opens
// librw's multiview render pass against the swapchain image the runtime just
// handed over, steps the game inside it, and submits.
void
renderGameFrame(VkImage image, VkImageView view, const float viewProj[2][16],
                const float im2dWorld[16], float im2dDistance,
                const float headPos[3], float headYaw,
                const float headQuat[4], float eyeFovDeg)
{
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

	// Frame cadence probe: the vehicle "pushes" forward instead of gliding,
	// which smells like uneven frame pacing. Reports the achieved rate and
	// the worst gap so the theory gets numbers.
	{
		static timespec last, windowStart;
		static int frames;
		static double worstMs;
		timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
		if(last.tv_sec != 0){
			const double ms = (now.tv_sec - last.tv_sec)*1000.0 +
			                  (now.tv_nsec - last.tv_nsec)/1e6;
			if(ms > worstMs) worstMs = ms;
			if(++frames >= 360){
				const double windowS =
					(now.tv_sec - windowStart.tv_sec) +
					(now.tv_nsec - windowStart.tv_nsec)/1e9;
				__android_log_print(ANDROID_LOG_INFO, "MiamiVR",
					"[probe] cadence: %.1f fps, worst gap %.1f ms",
					frames/windowS, worstMs);
				frames = 0; worstMs = 0.0; windowStart = now;
			}
		}else
			windowStart = now;
		last = now;
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
	}else{
		rw::vulkan::setStereoViewProjection(viewProj[0], viewProj[1]);
		rw::vulkan::setIm2DTransform(im2dWorld, im2dDistance, headPos);
		rw::vulkan::setHeadPose(headPos, headYaw, headQuat);
	}
	androidgame::SetEyeFovDeg(eyeFovDeg);
	// Game time follows the display clock, not the wall clock; see
	// getPredictedDisplayTimeNs.
	androidgame::SetFrameTimeNs(xrvk::getPredictedDisplayTimeNs());
	platform::setCheckpoint("vk/beginFrame");
	androidgame::QuestProfilerBeginVkBegin();
	if(!rw::vulkan::beginFrame(image, view)){
		androidgame::QuestProfilerCancelAppFrame();
		return;
	}
	androidgame::QuestProfilerEndVkBegin();
	platform::setCheckpoint("game/Step");
	androidgame::QuestProfilerBeginStep();
	androidgame::Step();
	androidgame::QuestProfilerEndStep();

	// Step owns both simulation and rendering, so a cutscene/menu transition
	// can occur inside it. Keep a frame flat on either side of that boundary;
	// the following frame will already have the correct mono matrices.
	const bool theaterAfter = androidgame::VrShouldUseTheaterMode();
	const bool theaterFrame = theaterBefore || theaterAfter;
	xrvk::setTheaterMode(theaterFrame);

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
	// A scene boundary can briefly render after the old world has been removed
	// but before the next cutscene camera is ready. Mask that invalid frame
	// instead of showing the clear-colour sky and half rebuilt geometry.
	const bool worldTransitionFrame =
		gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!FrontEndMenuManager.m_bMenuActive &&
		!CCutsceneMgr::IsRunning() &&
		!TheCamera.m_WideScreenOn &&
		(CGame::playingIntro || CCutsceneMgr::IsCutsceneProcessing() ||
		 FindPlayerPed() == nil);
	const bool gameplayCutsceneBoundary =
		gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bMenuActive &&
		theaterBefore != theaterAfter;
	const bool maskTransition =
		worldTransitionFrame || gameplayCutsceneBoundary;
	const uint32 colourMode = maskTransition ? 2u :
		(androidgame::VrViceCityColorEnabled() &&
		 (!theaterFrame || cutsceneFrame) &&
		 TheCamera.m_BlurType != MOTION_BLUR_NONE ? 1u : 0u);
	rw::vulkan::setPostFx(colourMode,
		(uint32)TheCamera.m_BlurRed,
		(uint32)TheCamera.m_BlurGreen,
		(uint32)TheCamera.m_BlurBlue,
		1.0f);
	rw::vulkan::setFxaaEnabled(androidgame::VrFxaaEnabled());
	static uint32 lastColourMode = ~0u;
	if(colourMode != lastColourMode){
		__android_log_print(ANDROID_LOG_INFO, "MiamiVR",
			"Vice City colour filter %s (type=%d rgb=%d,%d,%d)",
			colourMode == 2 ? "TRANSITION MASK" :
				(colourMode ? "ON" : "OFF"), TheCamera.m_BlurType,
			TheCamera.m_BlurRed, TheCamera.m_BlurGreen,
			TheCamera.m_BlurBlue);
		lastColourMode = colourMode;
	}

	platform::setCheckpoint("vk/endFrame");
	androidgame::QuestProfilerBeginVkEnd();
	rw::vulkan::endFrame();
	androidgame::QuestProfilerEndVkEnd();
	androidgame::QuestProfilerEndAppFrame();
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
			context.viewCount = graphics.viewCount;
			context.colourFormat = graphics.colourFormat;

			if(!androidgame::Initialise(context)){
				LOGE("game initialisation failed");
				break;
			}
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
