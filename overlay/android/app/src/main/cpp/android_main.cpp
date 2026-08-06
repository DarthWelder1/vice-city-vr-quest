#include "platform_android.h"
#include "xr_vulkan_session.h"

#include <android_native_app_glue.h>
#include <android/log.h>
#include <time.h>

#include <atomic>
#include <thread>

#ifndef MIAMIVR_BRINGUP
#include "rw.h"
#include "android.h"
#endif

namespace {

struct AppState
{
	std::atomic<bool> resumed{false};
	std::atomic<bool> destroyRequested{false};
	std::atomic<bool> stopRequested{false};
};

#ifndef MIAMIVR_BRINGUP
// Installed as the OpenXR layer's frame renderer once the game is up. Opens
// librw's multiview render pass against the swapchain image the runtime just
// handed over, steps the game inside it, and submits.
void
renderGameFrame(VkImage image, VkImageView view, const float viewProj[2][16],
                const float im2dWorld[16], float im2dDistance,
                const float headPos[3], float headYaw,
                const float headQuat[4], float eyeFovDeg)
{
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
	androidgame::SetPadInput(pad);

	// Debug overlay: the game side owns the chord and the pixels (its port of
	// the desktop panel); the session just carries them to a quad layer.
	androidgame::VrDebugUpdate(pad);
	{
		int width = 0, height = 0;
		xrvk::setDebugOverlay(androidgame::VrDebugPixels(&width, &height));
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

	rw::vulkan::setStereoViewProjection(viewProj[0], viewProj[1]);
	rw::vulkan::setIm2DTransform(im2dWorld, im2dDistance, headPos);
	rw::vulkan::setHeadPose(headPos, headYaw, headQuat);
	androidgame::SetEyeFovDeg(eyeFovDeg);
	// Game time follows the display clock, not the wall clock; see
	// getPredictedDisplayTimeNs.
	androidgame::SetFrameTimeNs(xrvk::getPredictedDisplayTimeNs());
	platform::setCheckpoint("vk/beginFrame");
	if(!rw::vulkan::beginFrame(image, view))
		return;
	platform::setCheckpoint("game/Step");
	androidgame::Step();
	platform::setCheckpoint("vk/endFrame");
	rw::vulkan::endFrame();
	platform::setCheckpoint("frame/done");
}
#endif

void
handleAppCmd(android_app *app, int32_t cmd)
{
	AppState *state = (AppState *)app->userData;
	switch(cmd){
	case APP_CMD_START:
	case APP_CMD_RESUME:
		state->resumed = true;
		break;
	case APP_CMD_PAUSE:
	case APP_CMD_STOP:
		state->resumed = false;
		break;
	case APP_CMD_DESTROY:
		state->destroyRequested = true;
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

	while(!state->stopRequested.load()){
		if(!sessionCreated){
			// Creation used to wait for APP_CMD_RESUME. It does not need to:
			// the session simply sits in IDLE until the runtime is ready, and
			// waiting meant that whenever Horizon OS chose not to resume the
			// activity the app came up completely inert, which is impossible
			// to tell apart from a hang. Try, and retry on failure.
			if(!xrvk::create(app)){
				static int attempts = 0;
				if(++attempts >= 50){
					LOGE("giving up on the OpenXR/Vulkan session");
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}
			sessionCreated = true;
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

		if(xrvk::shouldRender()){
			// Steps the game inside the frame, through the renderer callback.
			platform::setCheckpoint("xr/renderFrame");
			xrvk::renderFrame();
		}else{
#ifndef MIAMIVR_BRINGUP
			// No frame to draw into, but the game should still advance: a load
			// takes tens of seconds and there is no reason to stall it until
			// the headset is picked up. Every draw path checks for an open
			// frame and does nothing, so this is logic only.
			if(gameStarted)
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
	platform::setStorageRoot(app->activity->externalDataPath);

	LOGI("Vice City VR (Quest) starting");

	std::thread gameThread(gameThreadMain, app, &state);

	// This thread now does nothing but keep the looper serviced, so lifecycle
	// commands and input dispatch are always answered promptly no matter how
	// long the game blocks.
	while(!app->destroyRequested && !state.destroyRequested.load()){
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
}
