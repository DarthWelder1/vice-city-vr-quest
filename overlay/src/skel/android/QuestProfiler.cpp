#include "common.h"

#include <android/log.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "QuestProfiler.h"
#include "xr_vulkan_session.h"
#include "vulkan/rwvk.h"

namespace androidgame {
namespace {

bool gProfilerEnabled;
bool gAppFrameOpen;
bool gVkBeginOpen;
bool gStepOpen;
bool gVkEndOpen;
double gAppFrameStartedMs;
double gVkBeginStartedMs;
double gStepStartedMs;
double gStepEndedMs;
double gVkEndStartedMs;
double gLogWindowStartedMs;
int gLogWindowFrames;
float gLocalCpuAppMs;
float gWindowAppMaxMs;
float gWindowPreMaxMs;
float gWindowVkBeginMaxMs;
float gWindowStepMaxMs;
float gWindowPostMaxMs;
float gWindowVkEndMaxMs;
bool gPhaseOpen[QUEST_PROFILER_PHASE_COUNT];
double gPhaseStartedMs[QUEST_PROFILER_PHASE_COUNT];
float gFramePhaseMs[QUEST_PROFILER_PHASE_COUNT];
float gPhaseSmoothedMs[QUEST_PROFILER_PHASE_COUNT];
float gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_COUNT];
float gSimOtherSmoothedMs;
float gWindowSimOtherMaxMs;
char gLogPath[512];
FILE *gLogFile;
QuestProfilerSnapshot gSnapshot;

double
NowMilliseconds(void)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec*1000.0+(double)now.tv_nsec/1000000.0;
}

float
Smooth(float previous, float sample)
{
	if(sample < 0.0f || sample > 1000.0f)
		return previous;
	return previous > 0.0f ? previous*0.85f+sample*0.15f : sample;
}

void
Record(float sample, float *smoothed, float *windowMaximum)
{
	*smoothed = Smooth(*smoothed, sample);
	if(sample > *windowMaximum && sample < 1000.0f)
		*windowMaximum = sample;
}

void
ResetOpenPhases(void)
{
	for(int i = 0; i < QUEST_PROFILER_PHASE_COUNT; i++)
		gPhaseOpen[i] = false;
}

void
CloseOpenPhases(double now)
{
	for(int i = 0; i < QUEST_PROFILER_PHASE_COUNT; i++){
		if(!gPhaseOpen[i])
			continue;
		const float elapsed = (float)(now-gPhaseStartedMs[i]);
		if(elapsed >= 0.0f && elapsed < 1000.0f)
			gFramePhaseMs[i] += elapsed;
		gPhaseOpen[i] = false;
	}
}

void
UpdatePhaseSnapshot(void)
{
	gSnapshot.cpuGameMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_GAME];
	gSnapshot.cpuStreamingMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_STREAMING];
	gSnapshot.cpuAudioMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_AUDIO];
	gSnapshot.cpuWorldListMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_WORLD_LIST];
	gSnapshot.cpuPreRenderMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_PRE_RENDER];
	gSnapshot.cpuSceneSetupMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_SCENE_SETUP];
	gSnapshot.cpuWorldRenderMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_WORLD_RENDER];
	gSnapshot.cpuUiMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_UI];
	gSnapshot.cpuSimOtherMs = gSimOtherSmoothedMs;
}

void
UpdatePhaseMaximumSnapshot(void)
{
	gSnapshot.cpuGameMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_GAME];
	gSnapshot.cpuStreamingMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_STREAMING];
	gSnapshot.cpuAudioMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_AUDIO];
	gSnapshot.cpuWorldListMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_WORLD_LIST];
	gSnapshot.cpuPreRenderMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_PRE_RENDER];
	gSnapshot.cpuSceneSetupMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_SCENE_SETUP];
	gSnapshot.cpuWorldRenderMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_WORLD_RENDER];
	gSnapshot.cpuUiMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_UI];
	gSnapshot.cpuSimOtherMaxMs = gWindowSimOtherMaxMs;
}

} // namespace

void
QuestProfilerSetLogDirectory(const char *directory)
{
	gLogPath[0] = '\0';
	if(directory == nil || directory[0] == '\0')
		return;
	snprintf(gLogPath, sizeof(gLogPath), "%s/quest_perf.csv", directory);
}

void
QuestProfilerSetEnabled(bool enabled)
{
	if(gProfilerEnabled == enabled)
		return;
	gProfilerEnabled = enabled;
	if(gLogFile != nil){
		fclose(gLogFile);
		gLogFile = nil;
	}
	if(enabled && gLogPath[0] != '\0'){
		gLogFile = fopen(gLogPath, "w");
		if(gLogFile != nil){
			fputs("time_s,fps,cpu_app_ms,cpu_app_max_ms,"
			      "pre_ms,pre_max_ms,vk_begin_ms,vk_begin_max_ms,"
			      "step_ms,step_max_ms,post_ms,post_max_ms,"
			      "vk_end_ms,vk_end_max_ms,gap_ms,"
			      "gpu_app_ms,gpu_vk_ms,budget_ms,"
			      "game_ms,game_max_ms,streaming_ms,streaming_max_ms,"
			      "sim_other_ms,sim_other_max_ms,audio_ms,audio_max_ms,"
			      "world_list_ms,world_list_max_ms,"
			      "pre_render_ms,pre_render_max_ms,"
			      "scene_setup_ms,scene_setup_max_ms,"
			      "world_render_ms,world_render_max_ms,"
			      "ui_ms,ui_max_ms\n", gLogFile);
			fflush(gLogFile);
			__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
				"persistent log opened: %s", gLogPath);
		}else
			__android_log_print(ANDROID_LOG_ERROR, "QuestPerf",
				"cannot open persistent log: %s", gLogPath);
	}
	gAppFrameOpen = false;
	gVkBeginOpen = false;
	gStepOpen = false;
	gVkEndOpen = false;
	gSnapshot = {};
	gLocalCpuAppMs = 0.0f;
	gWindowAppMaxMs = 0.0f;
	gWindowPreMaxMs = 0.0f;
	gWindowVkBeginMaxMs = 0.0f;
	gWindowStepMaxMs = 0.0f;
	gWindowPostMaxMs = 0.0f;
	gWindowVkEndMaxMs = 0.0f;
	memset(gPhaseStartedMs, 0, sizeof(gPhaseStartedMs));
	memset(gFramePhaseMs, 0, sizeof(gFramePhaseMs));
	memset(gPhaseSmoothedMs, 0, sizeof(gPhaseSmoothedMs));
	memset(gWindowPhaseMaxMs, 0, sizeof(gWindowPhaseMaxMs));
	gSimOtherSmoothedMs = 0.0f;
	gWindowSimOtherMaxMs = 0.0f;
	ResetOpenPhases();
	gLogWindowStartedMs = NowMilliseconds();
	gLogWindowFrames = 0;
	rw::vulkan::setGpuFrameTimingEnabled(enabled ? 1 : 0);
	xrvk::setPerformanceMetricsEnabled(enabled);
}

bool
QuestProfilerIsEnabled(void)
{
	return gProfilerEnabled;
}

void
QuestProfilerBeginAppFrame(void)
{
	if(!gProfilerEnabled)
		return;
	gAppFrameStartedMs = NowMilliseconds();
	gAppFrameOpen = true;
	gVkBeginOpen = false;
	gStepOpen = false;
	gVkEndOpen = false;
	gStepEndedMs = 0.0;
	memset(gFramePhaseMs, 0, sizeof(gFramePhaseMs));
	ResetOpenPhases();
}

void
QuestProfilerCancelAppFrame(void)
{
	gAppFrameOpen = false;
	gVkBeginOpen = false;
	gStepOpen = false;
	gVkEndOpen = false;
	ResetOpenPhases();
}

void
QuestProfilerBeginVkBegin(void)
{
	if(!gProfilerEnabled || !gAppFrameOpen)
		return;
	const double now = NowMilliseconds();
	Record((float)(now-gAppFrameStartedMs), &gSnapshot.cpuPreMs,
		&gWindowPreMaxMs);
	gVkBeginStartedMs = now;
	gVkBeginOpen = true;
}

void
QuestProfilerEndVkBegin(void)
{
	if(!gProfilerEnabled || !gVkBeginOpen)
		return;
	Record((float)(NowMilliseconds()-gVkBeginStartedMs),
		&gSnapshot.cpuVkBeginMs, &gWindowVkBeginMaxMs);
	gVkBeginOpen = false;
}

void
QuestProfilerBeginStep(void)
{
	if(!gProfilerEnabled || !gAppFrameOpen)
		return;
	gStepStartedMs = NowMilliseconds();
	gStepOpen = true;
}

void
QuestProfilerEndStep(void)
{
	if(!gProfilerEnabled || !gStepOpen)
		return;
	const double now = NowMilliseconds();
	Record((float)(now-gStepStartedMs), &gSnapshot.cpuStepMs,
		&gWindowStepMaxMs);
	gStepEndedMs = now;
	gStepOpen = false;
}

void
QuestProfilerBeginVkEnd(void)
{
	if(!gProfilerEnabled || !gAppFrameOpen)
		return;
	const double now = NowMilliseconds();
	if(gStepEndedMs > 0.0)
		Record((float)(now-gStepEndedMs), &gSnapshot.cpuPostMs,
			&gWindowPostMaxMs);
	gVkEndStartedMs = now;
	gVkEndOpen = true;
}

void
QuestProfilerEndVkEnd(void)
{
	if(!gProfilerEnabled || !gVkEndOpen)
		return;
	Record((float)(NowMilliseconds()-gVkEndStartedMs),
		&gSnapshot.cpuVkEndMs, &gWindowVkEndMaxMs);
	gVkEndOpen = false;
}

void
QuestProfilerBeginPhase(QuestProfilerPhase phase)
{
	if(!gProfilerEnabled || !gAppFrameOpen ||
	   phase < 0 || phase >= QUEST_PROFILER_PHASE_COUNT ||
	   gPhaseOpen[phase])
		return;
	gPhaseStartedMs[phase] = NowMilliseconds();
	gPhaseOpen[phase] = true;
}

void
QuestProfilerEndPhase(QuestProfilerPhase phase)
{
	if(!gProfilerEnabled || !gAppFrameOpen ||
	   phase < 0 || phase >= QUEST_PROFILER_PHASE_COUNT ||
	   !gPhaseOpen[phase])
		return;
	const double now = NowMilliseconds();
	const float elapsed = (float)(now-gPhaseStartedMs[phase]);
	if(elapsed >= 0.0f && elapsed < 1000.0f)
		gFramePhaseMs[phase] += elapsed;
	gPhaseOpen[phase] = false;
}

void
QuestProfilerEndAppFrame(void)
{
	if(!gProfilerEnabled || !gAppFrameOpen)
		return;
	if(gStepOpen)
		QuestProfilerEndStep();
	if(gVkEndOpen)
		QuestProfilerEndVkEnd();
	const double now = NowMilliseconds();
	CloseOpenPhases(now);
	for(int i = 0; i < QUEST_PROFILER_PHASE_COUNT; i++)
		Record(gFramePhaseMs[i], &gPhaseSmoothedMs[i],
			&gWindowPhaseMaxMs[i]);
	const float simOtherRaw = Max(0.0f,
		gFramePhaseMs[QUEST_PROFILER_PHASE_GAME]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_STREAMING]);
	Record(simOtherRaw, &gSimOtherSmoothedMs,
		&gWindowSimOtherMaxMs);
	UpdatePhaseSnapshot();
	const float localCpuAppRaw =
		(float)(now-gAppFrameStartedMs);
	Record(localCpuAppRaw, &gLocalCpuAppMs, &gWindowAppMaxMs);

	xrvk::PerformanceMetrics runtime = {};
	xrvk::getPerformanceMetrics(&runtime);
	gSnapshot.frameBudgetMs =
		runtime.displayRefreshRateHz > 1.0f ?
			1000.0f/runtime.displayRefreshRateHz : 1000.0f/72.0f;
	gSnapshot.cpuAppRuntime = runtime.appCpuFrameValid;
	gSnapshot.cpuAppMs = runtime.appCpuFrameValid ?
		Smooth(gSnapshot.cpuAppMs, runtime.appCpuFrameMs) :
		gLocalCpuAppMs;
	gSnapshot.cpuUnaccountedMs = Max(0.0f,
		gLocalCpuAppMs-
		(gSnapshot.cpuPreMs+gSnapshot.cpuVkBeginMs+
		 gSnapshot.cpuStepMs+gSnapshot.cpuPostMs+
		 gSnapshot.cpuVkEndMs));

	float vulkanGpuMs = 0.0f;
	gSnapshot.gpuVulkanValid =
		rw::vulkan::getGpuFrameTimeMs(&vulkanGpuMs) != 0;
	if(gSnapshot.gpuVulkanValid)
		gSnapshot.gpuVulkanMs =
			Smooth(gSnapshot.gpuVulkanMs, vulkanGpuMs);

	gSnapshot.gpuFrameRuntime = runtime.appGpuFrameValid;
	if(runtime.appGpuFrameValid){
		gSnapshot.gpuFrameMs =
			Smooth(gSnapshot.gpuFrameMs, runtime.appGpuFrameMs);
		gSnapshot.gpuFrameValid = true;
	}else{
		gSnapshot.gpuFrameMs = gSnapshot.gpuVulkanMs;
		gSnapshot.gpuFrameValid = gSnapshot.gpuVulkanValid;
	}

	gLogWindowFrames++;
	if(now-gLogWindowStartedMs >= 1000.0){
		const float fps = gLogWindowFrames*1000.0f/
			(float)(now-gLogWindowStartedMs);
		gSnapshot.cpuAppMaxMs = gWindowAppMaxMs;
		gSnapshot.cpuPreMaxMs = gWindowPreMaxMs;
		gSnapshot.cpuVkBeginMaxMs = gWindowVkBeginMaxMs;
		gSnapshot.cpuStepMaxMs = gWindowStepMaxMs;
		gSnapshot.cpuPostMaxMs = gWindowPostMaxMs;
		gSnapshot.cpuVkEndMaxMs = gWindowVkEndMaxMs;
		UpdatePhaseMaximumSnapshot();
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"QuestPerf CPU_APP=%.2fms MAX=%.2fms(%s) "
			"PRE=%.2f/%.2f VK_BEGIN=%.2f/%.2f "
			"STEP=%.2f/%.2f POST=%.2f/%.2f "
			"VK_END=%.2f/%.2f GAP=%.2f "
			"GPU=%.2fms(%s) GPU_VK=%.2fms FPS=%.1f BUDGET=%.2fms",
			gSnapshot.cpuAppMs,
			gSnapshot.cpuAppMaxMs,
			gSnapshot.cpuAppRuntime ? "META" : "LOCAL",
			gSnapshot.cpuPreMs, gSnapshot.cpuPreMaxMs,
			gSnapshot.cpuVkBeginMs, gSnapshot.cpuVkBeginMaxMs,
			gSnapshot.cpuStepMs,
			gSnapshot.cpuStepMaxMs,
			gSnapshot.cpuPostMs, gSnapshot.cpuPostMaxMs,
			gSnapshot.cpuVkEndMs, gSnapshot.cpuVkEndMaxMs,
			gSnapshot.cpuUnaccountedMs,
			gSnapshot.gpuFrameValid ? gSnapshot.gpuFrameMs : -1.0f,
			gSnapshot.gpuFrameRuntime ? "META" :
				(gSnapshot.gpuVulkanValid ? "VK" : "NA"),
			gSnapshot.gpuVulkanValid ?
				gSnapshot.gpuVulkanMs : -1.0f,
			fps, gSnapshot.frameBudgetMs);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"QuestPerfPhases GAME=%.2f/%.2f "
			"STREAM=%.2f/%.2f SIM_OTHER=%.2f/%.2f "
			"AUDIO=%.2f/%.2f LIST=%.2f/%.2f "
			"PRE_RENDER=%.2f/%.2f SETUP=%.2f/%.2f "
			"WORLD_RENDER=%.2f/%.2f UI=%.2f/%.2f",
			gSnapshot.cpuGameMs, gSnapshot.cpuGameMaxMs,
			gSnapshot.cpuStreamingMs,
			gSnapshot.cpuStreamingMaxMs,
			gSnapshot.cpuSimOtherMs,
			gSnapshot.cpuSimOtherMaxMs,
			gSnapshot.cpuAudioMs, gSnapshot.cpuAudioMaxMs,
			gSnapshot.cpuWorldListMs,
			gSnapshot.cpuWorldListMaxMs,
			gSnapshot.cpuPreRenderMs,
			gSnapshot.cpuPreRenderMaxMs,
			gSnapshot.cpuSceneSetupMs,
			gSnapshot.cpuSceneSetupMaxMs,
			gSnapshot.cpuWorldRenderMs,
			gSnapshot.cpuWorldRenderMaxMs,
			gSnapshot.cpuUiMs, gSnapshot.cpuUiMaxMs);
		if(gLogFile != nil){
			fprintf(gLogFile,
				"%.3f,%.2f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f\n",
				now/1000.0, fps,
				gSnapshot.cpuAppMs, gSnapshot.cpuAppMaxMs,
				gSnapshot.cpuPreMs, gSnapshot.cpuPreMaxMs,
				gSnapshot.cpuVkBeginMs,
				gSnapshot.cpuVkBeginMaxMs,
				gSnapshot.cpuStepMs, gSnapshot.cpuStepMaxMs,
				gSnapshot.cpuPostMs, gSnapshot.cpuPostMaxMs,
				gSnapshot.cpuVkEndMs, gSnapshot.cpuVkEndMaxMs,
				gSnapshot.cpuUnaccountedMs,
				gSnapshot.gpuFrameValid ?
					gSnapshot.gpuFrameMs : -1.0f,
				gSnapshot.gpuVulkanValid ?
					gSnapshot.gpuVulkanMs : -1.0f,
				gSnapshot.frameBudgetMs,
				gSnapshot.cpuGameMs,
				gSnapshot.cpuGameMaxMs,
				gSnapshot.cpuStreamingMs,
				gSnapshot.cpuStreamingMaxMs,
				gSnapshot.cpuSimOtherMs,
				gSnapshot.cpuSimOtherMaxMs,
				gSnapshot.cpuAudioMs,
				gSnapshot.cpuAudioMaxMs,
				gSnapshot.cpuWorldListMs,
				gSnapshot.cpuWorldListMaxMs,
				gSnapshot.cpuPreRenderMs,
				gSnapshot.cpuPreRenderMaxMs,
				gSnapshot.cpuSceneSetupMs,
				gSnapshot.cpuSceneSetupMaxMs,
				gSnapshot.cpuWorldRenderMs,
				gSnapshot.cpuWorldRenderMaxMs,
				gSnapshot.cpuUiMs,
				gSnapshot.cpuUiMaxMs);
			fflush(gLogFile);
		}
		gLogWindowStartedMs = now;
		gLogWindowFrames = 0;
		gWindowAppMaxMs = 0.0f;
		gWindowPreMaxMs = 0.0f;
		gWindowVkBeginMaxMs = 0.0f;
		gWindowStepMaxMs = 0.0f;
		gWindowPostMaxMs = 0.0f;
		gWindowVkEndMaxMs = 0.0f;
		memset(gWindowPhaseMaxMs, 0,
			sizeof(gWindowPhaseMaxMs));
		gWindowSimOtherMaxMs = 0.0f;
	}
	gAppFrameOpen = false;
	gVkBeginOpen = false;
	gVkEndOpen = false;
	ResetOpenPhases();
}

QuestProfilerSnapshot
QuestProfilerGetSnapshot(void)
{
	return gSnapshot;
}

} // namespace androidgame
