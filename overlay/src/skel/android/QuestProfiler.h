#pragma once

namespace androidgame {

enum QuestProfilerPhase
{
	QUEST_PROFILER_PHASE_GAME,
	QUEST_PROFILER_PHASE_STREAMING,
	QUEST_PROFILER_PHASE_AUDIO,
	QUEST_PROFILER_PHASE_WORLD_LIST,
	QUEST_PROFILER_PHASE_PRE_RENDER,
	QUEST_PROFILER_PHASE_SCENE_SETUP,
	QUEST_PROFILER_PHASE_WORLD_RENDER,
	QUEST_PROFILER_PHASE_UI,
	QUEST_PROFILER_PHASE_COUNT
};

struct QuestProfilerSnapshot
{
	float cpuAppMs;
	float cpuPreMs;
	float cpuVkBeginMs;
	float cpuStepMs;
	float cpuPostMs;
	float cpuVkEndMs;
	float cpuUnaccountedMs;
	float cpuAppMaxMs;
	float cpuPreMaxMs;
	float cpuVkBeginMaxMs;
	float cpuStepMaxMs;
	float cpuPostMaxMs;
	float cpuVkEndMaxMs;
	float gpuFrameMs;
	float gpuVulkanMs;
	float frameBudgetMs;
	bool cpuAppRuntime;
	bool gpuFrameRuntime;
	bool gpuFrameValid;
	bool gpuVulkanValid;

	// GAME is the complete CGame::Process interval and therefore contains
	// STREAMING. SIM_OTHER is derived from GAME - STREAMING so phase totals can
	// be compared without counting streaming twice.
	float cpuGameMs;
	float cpuGameMaxMs;
	float cpuStreamingMs;
	float cpuStreamingMaxMs;
	float cpuSimOtherMs;
	float cpuSimOtherMaxMs;
	float cpuAudioMs;
	float cpuAudioMaxMs;
	float cpuWorldListMs;
	float cpuWorldListMaxMs;
	float cpuPreRenderMs;
	float cpuPreRenderMaxMs;
	float cpuSceneSetupMs;
	float cpuSceneSetupMaxMs;
	float cpuWorldRenderMs;
	float cpuWorldRenderMaxMs;
	float cpuUiMs;
	float cpuUiMaxMs;
};

// The profiler is deliberately opt-in. Vulkan timestamp commands are only
// recorded while enabled, keeping the normal release path untouched.
void QuestProfilerSetLogDirectory(const char *directory);
void QuestProfilerSetEnabled(bool enabled);
bool QuestProfilerIsEnabled(void);

// App frame brackets the native renderer callback, excluding xrWaitFrame
// pacing. Step brackets Vice City's simulation + render command recording.
void QuestProfilerBeginAppFrame(void);
void QuestProfilerCancelAppFrame(void);
void QuestProfilerBeginVkBegin(void);
void QuestProfilerEndVkBegin(void);
void QuestProfilerBeginStep(void);
void QuestProfilerEndStep(void);
void QuestProfilerBeginVkEnd(void);
void QuestProfilerEndVkEnd(void);
void QuestProfilerEndAppFrame(void);

// Named sub-phases are independent timers. This permits STREAMING to be
// measured while the containing GAME timer remains open.
void QuestProfilerBeginPhase(QuestProfilerPhase phase);
void QuestProfilerEndPhase(QuestProfilerPhase phase);

QuestProfilerSnapshot QuestProfilerGetSnapshot(void);

} // namespace androidgame
