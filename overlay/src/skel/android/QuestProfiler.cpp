#include "common.h"

#include <android/log.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "QuestProfiler.h"
#include "xr_vulkan_session.h"
#include "vulkan/rwvk.h"
#include "CarCtrl.h"
#include "Entity.h"
#include "Game.h"
#include "IniFile.h"
#include "ModelInfo.h"
#include "Occlusion.h"
#include "Particle.h"
#include "Physical.h"
#include "PhysicsDirector.h"
#include "VehicleVisualDirector.h"
#include "Pools.h"
#include "Population.h"
#include "Renderer.h"
#include "Streaming.h"

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
float gRenderOtherSmoothedMs;
float gWindowRenderOtherMaxMs;
float gWorldSimSmoothedMs;
float gWorldSimMaxMs;
float gEntitySimSmoothedMs[QUEST_WORLD_ENTITY_COUNT];
float gEntitySimMaxMs[QUEST_WORLD_ENTITY_COUNT];
float gWorldAnimationSmoothedMs;
float gWorldAnimationMaxMs;
float gWorldCollisionSmoothedMs;
float gWorldCollisionMaxMs;
float gWorldEntityPhaseSmoothedMs[QUEST_WORLD_ENTITY_COUNT]
	[QUEST_WORLD_SIM_PHASE_COUNT];
float gWorldEntityPhaseMaxMs[QUEST_WORLD_ENTITY_COUNT]
	[QUEST_WORLD_SIM_PHASE_COUNT];
float gFrameWorldSimMs;
uint64 gFrameRenderEntityNs[QUEST_RENDER_ENTITY_COUNT];
uint64 gFrameRenderFadingEntityNs[QUEST_RENDER_ENTITY_COUNT];
uint32 gFrameRenderEntityCalls[QUEST_RENDER_ENTITY_COUNT];
uint32 gFrameRenderFadingEntityCalls[QUEST_RENDER_ENTITY_COUNT];
uint64 gFrameVehicleRenderPhaseNs[QUEST_VEHICLE_RENDER_PHASE_COUNT];
uint32 gFrameVehicleOccupantsSubmitted;
uint32 gFrameVisibleBuildings;
uint32 gFrameVisibleRoads;
const CEntity *gFrameVisibleBuildingSlots[4096];
float gRenderEntitySmoothedMs[QUEST_RENDER_ENTITY_COUNT];
float gRenderEntityMaxMs[QUEST_RENDER_ENTITY_COUNT];
float gRenderFadingEntitySmoothedMs[QUEST_RENDER_ENTITY_COUNT];
float gRenderFadingEntityMaxMs[QUEST_RENDER_ENTITY_COUNT];
float gVehicleRenderPhaseSmoothedMs[QUEST_VEHICLE_RENDER_PHASE_COUNT];
float gVehicleRenderPhaseMaxMs[QUEST_VEHICLE_RENDER_PHASE_COUNT];
char gLogPath[512];
FILE *gLogFile;
QuestProfilerSnapshot gSnapshot;
int32 gPreviousSpawnTrace[16];
int32 gPreviousSpawnAttempts;
int32 gPreviousSpawnSuccess;
int32 gPreviousTrafficJobHits;
int32 gPreviousTrafficJobMisses;
int32 gPreviousTrafficJobDispatches;
int32 gPreviousTrafficJobSkips;
int32 gPreviousTrafficCruiseEligible;
int32 gPreviousTrafficCruiseHits;
int32 gPreviousTrafficCruiseMisses;
int32 gPreviousTrafficCruiseStale;
int32 gPreviousCollisionFlatScopes;
int32 gPreviousCollisionFlatBuilds;
int32 gPreviousCollisionFlatReuses;
int32 gPreviousCollisionFlatStale;
int32 gPreviousCollisionFlatOverflows;
int32 gPreviousCollisionFlatItems;
int32 gPreviousCollisionFlatSavedNodeVisits;
QuestPhysicsDirectorSnapshot gPreviousPhysicsDirector;
QuestVehicleVisualBudgetSnapshot gPreviousVehicleVisualBudget;

int32
CounterDelta(uint64 current, uint64 previous)
{
	const uint64 delta = current >= previous ? current-previous : 0;
	return (int32)Min(delta, (uint64)0x7FFFFFFF);
}

double
NowMilliseconds(void)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec*1000.0+(double)now.tv_nsec/1000000.0;
}

uint64
NowNanoseconds(void)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64)now.tv_sec*1000000000ULL+(uint64)now.tv_nsec;
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
	gSnapshot.cpuScriptMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_SCRIPT];
	gSnapshot.cpuPreWorldMiscMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_PRE_WORLD_MISC];
	gSnapshot.cpuWorldProcessMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_WORLD_PROCESS];
	gSnapshot.cpuPostWorldCameraFxMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_POST_WORLD_CAMERA_FX];
	gSnapshot.cpuPopulationMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_POPULATION];
	gSnapshot.cpuCarMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_CAR];
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
	gSnapshot.cpuRenderSkyMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_RENDER_SKY];
	gSnapshot.cpuRenderRoadsMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_RENDER_ROADS];
	gSnapshot.cpuRenderReflectionsMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_RENDER_REFLECTIONS];
	gSnapshot.cpuRenderWorldMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_RENDER_WORLD];
	gSnapshot.cpuRenderWaterMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_RENDER_WATER];
	gSnapshot.cpuRenderFadingMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_RENDER_FADING];
	gSnapshot.cpuRenderWeatherMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_RENDER_WEATHER];
	gSnapshot.cpuRenderEffectsMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_RENDER_EFFECTS];
	gSnapshot.cpuRenderOtherMs = gRenderOtherSmoothedMs;
	gSnapshot.cpuUiMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_UI];
	gSnapshot.cpuParticleUpdateMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_PARTICLE_UPDATE];
	gSnapshot.cpuParticleRenderMs =
		gPhaseSmoothedMs[QUEST_PROFILER_PHASE_PARTICLE_RENDER];
	gSnapshot.cpuSimOtherMs = gSimOtherSmoothedMs;
	gSnapshot.cpuWorldSimMs = gWorldSimSmoothedMs;
	gSnapshot.cpuPedSimMs =
		gEntitySimSmoothedMs[QUEST_WORLD_ENTITY_PED];
	gSnapshot.cpuVehicleSimMs =
		gEntitySimSmoothedMs[QUEST_WORLD_ENTITY_VEHICLE];
	gSnapshot.cpuObjectSimMs =
		gEntitySimSmoothedMs[QUEST_WORLD_ENTITY_OBJECT];
	gSnapshot.cpuWorldAnimationMs = gWorldAnimationSmoothedMs;
	gSnapshot.cpuWorldCollisionMs = gWorldCollisionSmoothedMs;
	memcpy(gSnapshot.cpuWorldEntityPhaseMs,
		gWorldEntityPhaseSmoothedMs,
		sizeof(gSnapshot.cpuWorldEntityPhaseMs));
	memcpy(gSnapshot.cpuRenderEntityMs,
		gRenderEntitySmoothedMs,
		sizeof(gSnapshot.cpuRenderEntityMs));
	memcpy(gSnapshot.cpuRenderFadingEntityMs,
		gRenderFadingEntitySmoothedMs,
		sizeof(gSnapshot.cpuRenderFadingEntityMs));
	memcpy(gSnapshot.cpuVehicleRenderPhaseMs,
		gVehicleRenderPhaseSmoothedMs,
		sizeof(gSnapshot.cpuVehicleRenderPhaseMs));
}

void
UpdatePhaseMaximumSnapshot(void)
{
	gSnapshot.cpuGameMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_GAME];
	gSnapshot.cpuStreamingMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_STREAMING];
	gSnapshot.cpuScriptMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_SCRIPT];
	gSnapshot.cpuPreWorldMiscMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_PRE_WORLD_MISC];
	gSnapshot.cpuWorldProcessMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_WORLD_PROCESS];
	gSnapshot.cpuPostWorldCameraFxMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_POST_WORLD_CAMERA_FX];
	gSnapshot.cpuPopulationMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_POPULATION];
	gSnapshot.cpuCarMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_CAR];
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
	gSnapshot.cpuRenderSkyMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_RENDER_SKY];
	gSnapshot.cpuRenderRoadsMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_RENDER_ROADS];
	gSnapshot.cpuRenderReflectionsMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_RENDER_REFLECTIONS];
	gSnapshot.cpuRenderWorldMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_RENDER_WORLD];
	gSnapshot.cpuRenderWaterMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_RENDER_WATER];
	gSnapshot.cpuRenderFadingMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_RENDER_FADING];
	gSnapshot.cpuRenderWeatherMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_RENDER_WEATHER];
	gSnapshot.cpuRenderEffectsMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_RENDER_EFFECTS];
	gSnapshot.cpuRenderOtherMaxMs = gWindowRenderOtherMaxMs;
	gSnapshot.cpuUiMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_UI];
	gSnapshot.cpuParticleUpdateMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_PARTICLE_UPDATE];
	gSnapshot.cpuParticleRenderMaxMs =
		gWindowPhaseMaxMs[QUEST_PROFILER_PHASE_PARTICLE_RENDER];
	gSnapshot.cpuSimOtherMaxMs = gWindowSimOtherMaxMs;
	gSnapshot.cpuWorldSimMaxMs = gWorldSimMaxMs;
	gSnapshot.cpuPedSimMaxMs =
		gEntitySimMaxMs[QUEST_WORLD_ENTITY_PED];
	gSnapshot.cpuVehicleSimMaxMs =
		gEntitySimMaxMs[QUEST_WORLD_ENTITY_VEHICLE];
	gSnapshot.cpuObjectSimMaxMs =
		gEntitySimMaxMs[QUEST_WORLD_ENTITY_OBJECT];
	gSnapshot.cpuWorldAnimationMaxMs = gWorldAnimationMaxMs;
	gSnapshot.cpuWorldCollisionMaxMs = gWorldCollisionMaxMs;
	memcpy(gSnapshot.cpuWorldEntityPhaseMaxMs,
		gWorldEntityPhaseMaxMs,
		sizeof(gSnapshot.cpuWorldEntityPhaseMaxMs));
	memcpy(gSnapshot.cpuRenderEntityMaxMs,
		gRenderEntityMaxMs,
		sizeof(gSnapshot.cpuRenderEntityMaxMs));
	memcpy(gSnapshot.cpuRenderFadingEntityMaxMs,
		gRenderFadingEntityMaxMs,
		sizeof(gSnapshot.cpuRenderFadingEntityMaxMs));
	memcpy(gSnapshot.cpuVehicleRenderPhaseMaxMs,
		gVehicleRenderPhaseMaxMs,
		sizeof(gSnapshot.cpuVehicleRenderPhaseMaxMs));
}

void
SampleDiagnostics(void)
{
	gSnapshot.streamingRequested = CStreaming::ms_numModelsRequested;
	gSnapshot.streamingPriority = CStreaming::ms_numPriorityRequests;
	gSnapshot.streamingMemoryUsed = (uint64)CStreaming::ms_memoryUsed;
	gSnapshot.streamingMemoryAvailable =
		(uint64)CStreaming::ms_memoryAvailable;
	gSnapshot.streamingVehiclesLoaded = CStreaming::ms_numVehiclesLoaded;
	gSnapshot.streamingPedsLoaded = CStreaming::ms_numPedsLoaded;
	gSnapshot.pedTrafficPercent =
		(int32)(CIniFile::PedNumberMultiplier*100.0f+0.5f);
	gSnapshot.carTrafficPercent =
		(int32)(CIniFile::CarNumberMultiplier*100.0f+0.5f);
	gSnapshot.ambientPeds = (int32)CPopulation::ms_nTotalPeds;
	gSnapshot.targetAmbientPeds = CPopulation::VrTargetAmbientPeds;
	gSnapshot.ambientPedCap = CGame::IsInInterior() ?
		CPopulation::MaxNumberOfPedsInUseInterior :
		CPopulation::MaxNumberOfPedsInUse;

	CPedPool *pedPool = CPools::GetPedPool();
	CVehiclePool *vehiclePool = CPools::GetVehiclePool();
	CObjectPool *objectPool = CPools::GetObjectPool();
	CEntryInfoNodePool *entryInfoPool = CPools::GetEntryInfoNodePool();
	gSnapshot.pedPoolUsed = pedPool != nil ?
		pedPool->GetNoOfUsedSpaces() : 0;
	gSnapshot.pedPoolSize = pedPool != nil ? pedPool->GetSize() : 0;
	gSnapshot.vehiclePoolUsed = vehiclePool != nil ?
		vehiclePool->GetNoOfUsedSpaces() : 0;
	gSnapshot.vehiclePoolSize = vehiclePool != nil ?
		vehiclePool->GetSize() : 0;
	gSnapshot.objectPoolUsed = objectPool != nil ?
		objectPool->GetNoOfUsedSpaces() : 0;
	gSnapshot.objectPoolSize = objectPool != nil ? objectPool->GetSize() : 0;
	gSnapshot.entryInfoPoolUsed = entryInfoPool != nil ?
		entryInfoPool->GetNoOfUsedSpaces() : 0;
	gSnapshot.entryInfoPoolSize = entryInfoPool != nil ?
		entryInfoPool->GetSize() : 0;
	gSnapshot.particleActive = CParticle::GetTotalActiveCount();
	gSnapshot.heliDustActive = CParticle::GetActiveCount(PARTICLE_HELI_DUST);
	gSnapshot.visibleEntities = CRenderer::GetVisibleEntityCount();
	gSnapshot.invisibleEntities = CRenderer::GetInvisibleEntityCount();
	xrvk::RenderScaleStatus renderScale = {};
	if(xrvk::getRenderScaleStatus(&renderScale)){
		gSnapshot.renderScaleRequestedPercent =
			renderScale.requestedPercent;
		gSnapshot.renderScaleSelectedPresetPercent =
			renderScale.selectedPresetPercent;
		gSnapshot.renderScaleEffectivePercent =
			renderScale.effectivePercent;
		gSnapshot.renderScaleRecommendedWidth =
			(int32)renderScale.recommendedWidth;
		gSnapshot.renderScaleRecommendedHeight =
			(int32)renderScale.recommendedHeight;
		gSnapshot.renderScaleActualWidth =
			(int32)renderScale.actualWidth;
		gSnapshot.renderScaleActualHeight =
			(int32)renderScale.actualHeight;
		gSnapshot.renderScaleRuntimeMaxWidth =
			(int32)renderScale.runtimeMaxWidth;
		gSnapshot.renderScaleRuntimeMaxHeight =
			(int32)renderScale.runtimeMaxHeight;
		gSnapshot.renderScaleFallbackReason =
			renderScale.fallbackReason;
		gSnapshot.renderScalePreviousFallbackRequest =
			renderScale.previousFallbackRequestedPercent;
		gSnapshot.renderScalePreviousFallbackPercent =
			renderScale.previousFallbackPercent;
		gSnapshot.renderScalePreviousFallbackReason =
			renderScale.previousFallbackReason;
	}
	{
		uint32 mode = 0, sceneWidth = 0, sceneHeight = 0;
		uint32 outputWidth = 0, outputHeight = 0;
		if(rw::vulkan::getSgsrStatus(&mode, &sceneWidth, &sceneHeight,
		                                 &outputWidth, &outputHeight)){
			gSnapshot.sgsrMode = (int32)mode;
			gSnapshot.sgsrSceneWidth = (int32)sceneWidth;
			gSnapshot.sgsrSceneHeight = (int32)sceneHeight;
			gSnapshot.sgsrOutputWidth = (int32)outputWidth;
			gSnapshot.sgsrOutputHeight = (int32)outputHeight;
		}
	}

	gSnapshot.spawnTopDenyReason = -1;
	gSnapshot.spawnTopDenyCount = 0;
#if defined(GTA_VR_OCULUS) || defined(GTA_VR_WEAPONS)
	gSnapshot.trafficDesired = CCarCtrl::VrLocalDesired;
	gSnapshot.trafficServed = CCarCtrl::VrLocalServed;
	gSnapshot.trafficAmbient = CCarCtrl::NumVrEffectiveAmbient;
	gSnapshot.trafficPursuit = CCarCtrl::NumVrEffectivePursuit;
	gSnapshot.trafficProxies = CCarCtrl::NumVrActiveProxies;
	gSnapshot.trafficPending = CCarCtrl::NumVrPendingMaterializations;
	gSnapshot.trafficDebris = CCarCtrl::NumVrDebris;
	gSnapshot.trafficStalled = CCarCtrl::NumVrStalled;
	gSnapshot.trafficScavenged = CCarCtrl::NumVrScavengedCars;
	gSnapshot.trafficPoolReserveBlocks = CCarCtrl::NumVrPoolReserveBlocks;
	gSnapshot.trafficDirectorMs = CCarCtrl::VrDirectorLastUpdateMs;
	gSnapshot.trafficDirectorMaxMs = CCarCtrl::VrDirectorMaxUpdateMs;
#if defined(__ANDROID__) && defined(GTA_VR_WEAPONS)
	const int32 trafficJobHits = CCarCtrl::NumVrTrafficJobHits;
	const int32 trafficJobMisses = CCarCtrl::NumVrTrafficJobMisses;
	const int32 trafficJobDispatches = CCarCtrl::NumVrTrafficJobDispatches;
	const int32 trafficJobSkips = CCarCtrl::NumVrTrafficJobSkips;
	gSnapshot.trafficJobHitsPerSecond =
		trafficJobHits >= gPreviousTrafficJobHits ?
		trafficJobHits-gPreviousTrafficJobHits : 0;
	gSnapshot.trafficJobMissesPerSecond =
		trafficJobMisses >= gPreviousTrafficJobMisses ?
		trafficJobMisses-gPreviousTrafficJobMisses : 0;
	gPreviousTrafficJobHits = trafficJobHits;
	gPreviousTrafficJobMisses = trafficJobMisses;
	gSnapshot.trafficJobDispatchesPerSecond =
		trafficJobDispatches >= gPreviousTrafficJobDispatches ?
		trafficJobDispatches-gPreviousTrafficJobDispatches : 0;
	gSnapshot.trafficJobSkipsPerSecond =
		trafficJobSkips >= gPreviousTrafficJobSkips ?
		trafficJobSkips-gPreviousTrafficJobSkips : 0;
	gPreviousTrafficJobDispatches = trafficJobDispatches;
	gPreviousTrafficJobSkips = trafficJobSkips;
	gSnapshot.trafficJobMs = CCarCtrl::VrTrafficJobLastMs;
	gSnapshot.trafficJobMaxMs = CCarCtrl::VrTrafficJobMaxMs;
	gSnapshot.trafficJobBuildMs = CCarCtrl::VrTrafficJobBuildMs;
	gSnapshot.trafficJobBuildMaxMs = CCarCtrl::VrTrafficJobBuildMaxMs;
	const int32 trafficCruiseEligible = CCarCtrl::NumVrTrafficCruiseEligible;
	const int32 trafficCruiseHits = CCarCtrl::NumVrTrafficCruiseHits;
	const int32 trafficCruiseMisses = CCarCtrl::NumVrTrafficCruiseMisses;
	const int32 trafficCruiseStale = CCarCtrl::NumVrTrafficCruiseStale;
	gSnapshot.trafficCruiseEligiblePerSecond =
		trafficCruiseEligible >= gPreviousTrafficCruiseEligible ?
		trafficCruiseEligible-gPreviousTrafficCruiseEligible : 0;
	gSnapshot.trafficCruiseHitsPerSecond =
		trafficCruiseHits >= gPreviousTrafficCruiseHits ?
		trafficCruiseHits-gPreviousTrafficCruiseHits : 0;
	gSnapshot.trafficCruiseMissesPerSecond =
		trafficCruiseMisses >= gPreviousTrafficCruiseMisses ?
		trafficCruiseMisses-gPreviousTrafficCruiseMisses : 0;
	gSnapshot.trafficCruiseStalePerSecond =
		trafficCruiseStale >= gPreviousTrafficCruiseStale ?
		trafficCruiseStale-gPreviousTrafficCruiseStale : 0;
	gPreviousTrafficCruiseEligible = trafficCruiseEligible;
	gPreviousTrafficCruiseHits = trafficCruiseHits;
	gPreviousTrafficCruiseMisses = trafficCruiseMisses;
	gPreviousTrafficCruiseStale = trafficCruiseStale;
	const int32 collisionFlatScopes = CPhysical::NumVrCollisionFlatScopes;
	const int32 collisionFlatBuilds = CPhysical::NumVrCollisionFlatBuilds;
	const int32 collisionFlatReuses = CPhysical::NumVrCollisionFlatReuses;
	const int32 collisionFlatStale = CPhysical::NumVrCollisionFlatStale;
	const int32 collisionFlatOverflows = CPhysical::NumVrCollisionFlatOverflows;
	const int32 collisionFlatItems = CPhysical::NumVrCollisionFlatItems;
	const int32 collisionFlatSavedNodeVisits =
		CPhysical::NumVrCollisionFlatSavedNodeVisits;
	gSnapshot.collisionFlatScopesPerSecond =
		collisionFlatScopes >= gPreviousCollisionFlatScopes ?
		collisionFlatScopes-gPreviousCollisionFlatScopes : 0;
	gSnapshot.collisionFlatBuildsPerSecond =
		collisionFlatBuilds >= gPreviousCollisionFlatBuilds ?
		collisionFlatBuilds-gPreviousCollisionFlatBuilds : 0;
	gSnapshot.collisionFlatReusesPerSecond =
		collisionFlatReuses >= gPreviousCollisionFlatReuses ?
		collisionFlatReuses-gPreviousCollisionFlatReuses : 0;
	gSnapshot.collisionFlatStalePerSecond =
		collisionFlatStale >= gPreviousCollisionFlatStale ?
		collisionFlatStale-gPreviousCollisionFlatStale : 0;
	gSnapshot.collisionFlatOverflowsPerSecond =
		collisionFlatOverflows >= gPreviousCollisionFlatOverflows ?
		collisionFlatOverflows-gPreviousCollisionFlatOverflows : 0;
	gSnapshot.collisionFlatItemsPerSecond =
		collisionFlatItems >= gPreviousCollisionFlatItems ?
		collisionFlatItems-gPreviousCollisionFlatItems : 0;
	gSnapshot.collisionFlatSavedNodeVisitsPerSecond =
		collisionFlatSavedNodeVisits >= gPreviousCollisionFlatSavedNodeVisits ?
		collisionFlatSavedNodeVisits-gPreviousCollisionFlatSavedNodeVisits : 0;
	gPreviousCollisionFlatScopes = collisionFlatScopes;
	gPreviousCollisionFlatBuilds = collisionFlatBuilds;
	gPreviousCollisionFlatReuses = collisionFlatReuses;
	gPreviousCollisionFlatStale = collisionFlatStale;
	gPreviousCollisionFlatOverflows = collisionFlatOverflows;
	gPreviousCollisionFlatItems = collisionFlatItems;
	gPreviousCollisionFlatSavedNodeVisits = collisionFlatSavedNodeVisits;

	const QuestPhysicsDirectorSnapshot physics =
		QuestPhysicsDirectorGetSnapshot();
	gSnapshot.physicsDirectorMode = physics.mode;
	gSnapshot.physicsDirectorPreset = physics.preset;
	gSnapshot.physicsAdaptiveLevel = physics.adaptiveLevel;
	gSnapshot.physicsBudgetMs = physics.budgetMs;
	gSnapshot.physicsManagedFrameMs = physics.managedFrameMs;
	gSnapshot.physicsManagedAverageMs = physics.managedAverageMs;
	gSnapshot.physicsFull =
		physics.tierCount[QUEST_VEHICLE_PHYSICS_FULL];
	gSnapshot.physicsReduced =
		physics.tierCount[QUEST_VEHICLE_PHYSICS_REDUCED];
	gSnapshot.physicsRail =
		physics.tierCount[QUEST_VEHICLE_PHYSICS_RAIL];
	gSnapshot.physicsProxy =
		physics.tierCount[QUEST_VEHICLE_PHYSICS_PROXY];
	gSnapshot.physicsTracked = physics.trackedVehicles;
	gSnapshot.physicsOverflow = physics.overflowVehicles;
	gSnapshot.physicsClassifyMs = physics.classifyMs;
	gSnapshot.physicsCheckCollisionPerSecond = CounterDelta(
		physics.checkCollisionCalls,
		gPreviousPhysicsDirector.checkCollisionCalls);
	gSnapshot.physicsCheckSimplePerSecond = CounterDelta(
		physics.checkCollisionSimpleCalls,
		gPreviousPhysicsDirector.checkCollisionSimpleCalls);
	gSnapshot.physicsSimpleSkippedPerSecond = CounterDelta(
		physics.simpleChecksSkipped,
		gPreviousPhysicsDirector.simpleChecksSkipped);
	gSnapshot.physicsRailCollisionRunsPerSecond = CounterDelta(
		physics.railCollisionRuns,
		gPreviousPhysicsDirector.railCollisionRuns);
	gSnapshot.physicsRailCollisionSkipsPerSecond = CounterDelta(
		physics.railCollisionSkips,
		gPreviousPhysicsDirector.railCollisionSkips);
	gSnapshot.physicsProxyCollisionRunsPerSecond = CounterDelta(
		physics.proxyCollisionRuns,
		gPreviousPhysicsDirector.proxyCollisionRuns);
	gSnapshot.physicsProxyCollisionSkipsPerSecond = CounterDelta(
		physics.proxyCollisionSkips,
		gPreviousPhysicsDirector.proxyCollisionSkips);
	gSnapshot.physicsRailAiRunsPerSecond = CounterDelta(
		physics.railAiRuns, gPreviousPhysicsDirector.railAiRuns);
	gSnapshot.physicsRailAiSkipsPerSecond = CounterDelta(
		physics.railAiSkips, gPreviousPhysicsDirector.railAiSkips);
	gSnapshot.physicsProxyAiRunsPerSecond = CounterDelta(
		physics.proxyAiRuns, gPreviousPhysicsDirector.proxyAiRuns);
	gSnapshot.physicsProxyAiSkipsPerSecond = CounterDelta(
		physics.proxyAiSkips, gPreviousPhysicsDirector.proxyAiSkips);
	gSnapshot.physicsSafetyPromotionsPerSecond = CounterDelta(
		physics.safetyPromotions,
		gPreviousPhysicsDirector.safetyPromotions);
	gSnapshot.physicsImminentPromotionsPerSecond = CounterDelta(
		physics.imminentPromotions,
		gPreviousPhysicsDirector.imminentPromotions);
	gSnapshot.physicsContactPromotionsPerSecond = CounterDelta(
		physics.contactPromotions,
		gPreviousPhysicsDirector.contactPromotions);
	gSnapshot.physicsAdaptiveEscalationsPerSecond = CounterDelta(
		physics.adaptiveEscalations,
		gPreviousPhysicsDirector.adaptiveEscalations);
	gSnapshot.physicsAdaptiveRelaxationsPerSecond = CounterDelta(
		physics.adaptiveRelaxations,
		gPreviousPhysicsDirector.adaptiveRelaxations);
	const uint64 managedSimpleNs = physics.managedSimpleCollisionNs >=
		gPreviousPhysicsDirector.managedSimpleCollisionNs ?
		physics.managedSimpleCollisionNs-
			gPreviousPhysicsDirector.managedSimpleCollisionNs : 0;
	const uint64 managedAiNs = physics.managedAiNs >=
		gPreviousPhysicsDirector.managedAiNs ?
		physics.managedAiNs-gPreviousPhysicsDirector.managedAiNs : 0;
	gSnapshot.physicsManagedSimpleCollisionMs =
		(float)managedSimpleNs/1000000.0f;
	gSnapshot.physicsManagedAiMs = (float)managedAiNs/1000000.0f;
	gSnapshot.physicsRemoveAndAddSkippedPerSecond = CounterDelta(
		physics.removeAndAddSkipped,
		gPreviousPhysicsDirector.removeAndAddSkipped);
	gSnapshot.physicsSectorNodesPerSecond = CounterDelta(
		physics.sectorNodesVisited,
		gPreviousPhysicsDirector.sectorNodesVisited);
	gSnapshot.physicsBroadphaseCandidatesPerSecond = CounterDelta(
		physics.broadphaseCandidates,
		gPreviousPhysicsDirector.broadphaseCandidates);
	gSnapshot.physicsPairsAfterFilteringPerSecond = CounterDelta(
		physics.pairsAfterFiltering,
		gPreviousPhysicsDirector.pairsAfterFiltering);
	gSnapshot.physicsProcessColModelsPerSecond = CounterDelta(
		physics.processColModelsCalls,
		gPreviousPhysicsDirector.processColModelsCalls);
	gSnapshot.physicsWheelLineTestsPerSecond = CounterDelta(
		physics.wheelLineTests,
		gPreviousPhysicsDirector.wheelLineTests);
	gSnapshot.physicsTriangleTestsPerSecond = CounterDelta(
		physics.triangleTests,
		gPreviousPhysicsDirector.triangleTests);
	gSnapshot.physicsSubstepsPerSecond = CounterDelta(
		physics.substeps, gPreviousPhysicsDirector.substeps);
	gSnapshot.physicsExtraSubstepsPerSecond = CounterDelta(
		physics.extraSubsteps,
		gPreviousPhysicsDirector.extraSubsteps);
	gSnapshot.physicsRetryPassesPerSecond = CounterDelta(
		physics.retryPasses, gPreviousPhysicsDirector.retryPasses);
	gSnapshot.physicsContactManifoldsPerSecond = CounterDelta(
		physics.contactManifolds,
		gPreviousPhysicsDirector.contactManifolds);
	gSnapshot.physicsRemoveAndAddPerSecond = CounterDelta(
		physics.removeAndAddCalls,
		gPreviousPhysicsDirector.removeAndAddCalls);
	const uint64 removeAndAddNs = physics.removeAndAddNs >=
		gPreviousPhysicsDirector.removeAndAddNs ?
		physics.removeAndAddNs-gPreviousPhysicsDirector.removeAndAddNs : 0;
	gSnapshot.physicsRemoveAndAddMs =
		(float)removeAndAddNs/1000000.0f;
	gPreviousPhysicsDirector = physics;

	const QuestVehicleVisualBudgetSnapshot visual =
		QuestVehicleVisualBudgetGetSnapshot();
	gSnapshot.vehicleVisualBudgetMode = visual.mode;
	gSnapshot.vehicleVisualHighPerSecond = CounterDelta(
		visual.highVehicleSubmissions,
		gPreviousVehicleVisualBudget.highVehicleSubmissions);
	gSnapshot.vehicleVisualVloPerSecond = CounterDelta(
		visual.vloVehicleSubmissions,
		gPreviousVehicleVisualBudget.vloVehicleSubmissions);
	gSnapshot.vehicleVisualAtomicsSkippedPerSecond = CounterDelta(
		visual.atomicsSkipped,
		gPreviousVehicleVisualBudget.atomicsSkipped);
	gSnapshot.vehicleVisualOccupantsSkippedPerSecond = CounterDelta(
		visual.occupantsSkipped,
		gPreviousVehicleVisualBudget.occupantsSkipped);
	gPreviousVehicleVisualBudget = visual;
#endif

	const int32 attempts = CCarCtrl::NumVrSpawnAttempts;
	const int32 successes = CCarCtrl::NumVrSpawnSuccess;
	gSnapshot.spawnAttemptsPerSecond =
		attempts >= gPreviousSpawnAttempts ?
		attempts-gPreviousSpawnAttempts : 0;
	gSnapshot.spawnSuccessPerSecond =
		successes >= gPreviousSpawnSuccess ?
		successes-gPreviousSpawnSuccess : 0;
	gSnapshot.spawnDeniedPerSecond = Max(0,
		gSnapshot.spawnAttemptsPerSecond-
		gSnapshot.spawnSuccessPerSecond);
	gPreviousSpawnAttempts = attempts;
	gPreviousSpawnSuccess = successes;
	for(int i = 0; i < 16; i++){
		const int32 current = CCarCtrl::VrSpawnTrace[i];
		const int32 delta = current >= gPreviousSpawnTrace[i] ?
			current-gPreviousSpawnTrace[i] : 0;
		gSnapshot.spawnReasonPerSecond[i] = delta;
		gPreviousSpawnTrace[i] = current;
		if(i != 12 && delta > gSnapshot.spawnTopDenyCount){
			gSnapshot.spawnTopDenyCount = delta;
			gSnapshot.spawnTopDenyReason = i;
		}
	}
#endif
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
			      "script_ms,script_max_ms,pre_world_misc_ms,pre_world_misc_max_ms,"
			      "world_process_ms,world_process_max_ms,"
			      "post_world_camera_fx_ms,post_world_camera_fx_max_ms,"
			      "sim_other_ms,sim_other_max_ms,audio_ms,audio_max_ms,"
			      "world_list_ms,world_list_max_ms,"
			      "pre_render_ms,pre_render_max_ms,"
			      "scene_setup_ms,scene_setup_max_ms,"
			      "world_render_ms,world_render_max_ms,"
			      "ui_ms,ui_max_ms,population_ms,population_max_ms,"
			      "car_ms,car_max_ms,stream_req,stream_prio,"
			      "stream_mem_used,stream_mem_available,"
			      "stream_vehicles,stream_peds,ped_percent,car_percent,"
			      "ped_ambient,ped_target,ped_cap,ped_pool_used,ped_pool_size,"
			      "vehicle_pool_used,vehicle_pool_size,object_pool_used,"
			      "object_pool_size,visible_entities,invisible_entities,"
			      "visible_buildings,visible_roads,active_occluders,"
			      "occlusion_culling_mode,"
			      "traffic_desired,traffic_served,traffic_ambient,"
			      "traffic_pursuit,traffic_proxies,traffic_pending,"
			      "traffic_debris,traffic_stalled,traffic_scavenged,"
			      "traffic_pool_blocks,traffic_director_ms,"
			      "traffic_director_max_ms,spawn_attempt_s,spawn_ok_s,"
			      "spawn_deny_s,spawn_top_reason,spawn_top_count,"
			      "spawn_no_model_s,spawn_coors_s,spawn_dice_s,"
			      "spawn_colliding_s,spawn_closeness_s,spawn_lanes_s,"
			      "spawn_nowhere_s,spawn_ground_s,spawn_too_far_s,"
			      "spawn_in_view_s,spawn_approach_s,spawn_cops_blocked_s,"
			      "spawn_ok_trace_s,spawn_cell_seen_s,"
			      "spawn_pool_reserve_s,spawn_demand_s,"
			      "world_sim_ms,world_sim_max_ms,ped_sim_ms,ped_sim_max_ms,"
			      "vehicle_sim_ms,vehicle_sim_max_ms,object_sim_ms,object_sim_max_ms,"
			      "world_animation_ms,world_animation_max_ms,"
			      "world_collision_ms,world_collision_max_ms,"
			      "ped_control_calls,vehicle_control_calls,collision_calls,"
			      "entry_info_pool_used,entry_info_pool_size,"
			      "particle_update_ms,particle_update_max_ms,"
			      "particle_render_ms,particle_render_max_ms,"
			      "particle_active,heli_dust_active,"
			      "ped_animation_ms,ped_animation_max_ms,"
			      "ped_control_ms,ped_control_max_ms,"
			      "ped_postponed_control_ms,ped_postponed_control_max_ms,"
			      "ped_collision_ms,ped_collision_max_ms,"
			      "ped_shift_ms,ped_shift_max_ms,"
			      "ped_transform_ms,ped_transform_max_ms,"
			      "vehicle_animation_ms,vehicle_animation_max_ms,"
			      "vehicle_control_ms,vehicle_control_max_ms,"
			      "vehicle_postponed_control_ms,vehicle_postponed_control_max_ms,"
			      "vehicle_collision_ms,vehicle_collision_max_ms,"
			      "vehicle_shift_ms,vehicle_shift_max_ms,"
			      "vehicle_transform_ms,vehicle_transform_max_ms,"
			      "traffic_job_hit_s,traffic_job_miss_s,"
			      "traffic_job_ms,traffic_job_max_ms,"
			      "render_sky_ms,render_sky_max_ms,"
			      "render_roads_ms,render_roads_max_ms,"
			      "render_reflections_ms,render_reflections_max_ms,"
			      "render_world_ms,render_world_max_ms,"
			      "render_water_ms,render_water_max_ms,"
			      "render_fading_ms,render_fading_max_ms,"
			      "render_weather_ms,render_weather_max_ms,"
			      "render_effects_ms,render_effects_max_ms,"
			      "render_other_ms,render_other_max_ms,"
			      "traffic_job_dispatch_s,traffic_job_skip_s,"
			      "traffic_job_build_ms,traffic_job_build_max_ms,"
			      "render_entity_vehicle_ms,render_entity_vehicle_max_ms,"
			      "render_entity_vehicle_calls,"
			      "render_entity_ped_ms,render_entity_ped_max_ms,"
			      "render_entity_ped_calls,"
			      "render_entity_static_ms,render_entity_static_max_ms,"
			      "render_entity_static_calls,"
			      "render_fading_vehicle_ms,render_fading_vehicle_max_ms,"
			      "render_fading_vehicle_calls,"
			      "render_fading_ped_ms,render_fading_ped_max_ms,"
			      "render_fading_ped_calls,"
			      "render_fading_static_ms,render_fading_static_max_ms,"
			      "render_fading_static_calls,"
			      "render_vehicle_lighting_ms,render_vehicle_lighting_max_ms,"
			      "render_vehicle_occupants_ms,render_vehicle_occupants_max_ms,"
			      "render_vehicle_body_ms,render_vehicle_body_max_ms,"
			      "render_vehicle_alpha_ms,render_vehicle_alpha_max_ms,"
			      "render_vehicle_occupants_submitted,"
			      "cpu_perf_requested,cpu_perf_active,"
			      "cpu_perf_supported,cpu_boost_blocked,"
			      "gpu_perf_requested,gpu_perf_active,"
			      "traffic_job_cruise_eligible_s,"
			      "traffic_job_cruise_hit_s,"
			      "traffic_job_cruise_miss_s,"
			      "traffic_job_cruise_stale_s,"
			      "collision_flat_scope_s,"
			      "collision_flat_build_s,"
			      "collision_flat_reuse_s,"
			      "collision_flat_stale_s,"
			      "collision_flat_overflow_s,"
			      "collision_flat_items_s,"
			      "collision_flat_saved_nodes_s,"
			      "physics_director_mode,physics_full,physics_reduced,"
			      "physics_rail,physics_proxy,physics_tracked,physics_overflow,"
			      "physics_classify_ms,physics_check_collision_s,"
			      "physics_check_simple_s,physics_simple_skipped_s,"
			      "physics_sector_nodes_s,physics_broadphase_candidates_s,"
			      "physics_pairs_filtered_s,physics_colmodels_s,"
			      "physics_wheel_line_tests_s,physics_triangle_tests_s,"
			      "physics_substeps_s,physics_extra_substeps_s,"
			      "physics_retry_passes_s,physics_manifolds_s,"
			      "physics_remove_add_s,physics_remove_add_ms,"
			      "physics_preset,physics_adaptive_level,physics_budget_ms,"
			      "physics_managed_frame_ms,physics_managed_avg_ms,"
			      "physics_rail_collision_run_s,physics_rail_collision_skip_s,"
			      "physics_proxy_collision_run_s,physics_proxy_collision_skip_s,"
			      "physics_rail_ai_run_s,physics_rail_ai_skip_s,"
			      "physics_proxy_ai_run_s,physics_proxy_ai_skip_s,"
			      "physics_safety_promotions_s,physics_imminent_promotions_s,"
			      "physics_contact_promotions_s,physics_adaptive_up_s,"
			      "physics_adaptive_down_s,physics_managed_simple_ms,"
			      "physics_managed_ai_ms,physics_remove_add_skipped_s,"
			      "vehicle_visual_mode,vehicle_visual_vhi_s,"
			      "vehicle_visual_vlo_s,vehicle_visual_atomics_skipped_s,"
			      "vehicle_visual_occupants_skipped_s,"
			      "render_scale_requested,render_scale_selected,"
			      "render_scale_effective,render_width,render_height,"
			      "render_recommended_width,render_recommended_height,"
			      "render_max_width,render_max_height,"
			      "render_scale_fallback,render_scale_previous_request,"
			      "render_scale_previous_percent,"
			      "render_scale_previous_fallback,sgsr_mode,"
			      "sgsr_scene_width,sgsr_scene_height,sgsr_output_width,"
			      "sgsr_output_height\n", gLogFile);
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
	gSnapshot.spawnTopDenyReason = -1;
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
	gRenderOtherSmoothedMs = 0.0f;
	gWindowRenderOtherMaxMs = 0.0f;
	gWorldSimSmoothedMs = 0.0f;
	gWorldSimMaxMs = 0.0f;
	gFrameWorldSimMs = 0.0f;
	memset(gEntitySimSmoothedMs, 0, sizeof(gEntitySimSmoothedMs));
	memset(gEntitySimMaxMs, 0, sizeof(gEntitySimMaxMs));
	gWorldAnimationSmoothedMs = 0.0f;
	gWorldAnimationMaxMs = 0.0f;
	gWorldCollisionSmoothedMs = 0.0f;
	gWorldCollisionMaxMs = 0.0f;
	memset(gWorldEntityPhaseSmoothedMs, 0,
		sizeof(gWorldEntityPhaseSmoothedMs));
	memset(gWorldEntityPhaseMaxMs, 0,
		sizeof(gWorldEntityPhaseMaxMs));
	memset(gFrameRenderEntityNs, 0, sizeof(gFrameRenderEntityNs));
	memset(gFrameRenderFadingEntityNs, 0,
		sizeof(gFrameRenderFadingEntityNs));
	memset(gFrameRenderEntityCalls, 0,
		sizeof(gFrameRenderEntityCalls));
	memset(gFrameRenderFadingEntityCalls, 0,
		sizeof(gFrameRenderFadingEntityCalls));
	memset(gFrameVehicleRenderPhaseNs, 0,
		sizeof(gFrameVehicleRenderPhaseNs));
	gFrameVehicleOccupantsSubmitted = 0;
	memset(gRenderEntitySmoothedMs, 0,
		sizeof(gRenderEntitySmoothedMs));
	memset(gRenderEntityMaxMs, 0, sizeof(gRenderEntityMaxMs));
	memset(gRenderFadingEntitySmoothedMs, 0,
		sizeof(gRenderFadingEntitySmoothedMs));
	memset(gRenderFadingEntityMaxMs, 0,
		sizeof(gRenderFadingEntityMaxMs));
	memset(gVehicleRenderPhaseSmoothedMs, 0,
		sizeof(gVehicleRenderPhaseSmoothedMs));
	memset(gVehicleRenderPhaseMaxMs, 0,
		sizeof(gVehicleRenderPhaseMaxMs));
	memset(gPreviousSpawnTrace, 0, sizeof(gPreviousSpawnTrace));
	gPreviousSpawnAttempts = 0;
	gPreviousSpawnSuccess = 0;
	gPreviousTrafficJobHits = 0;
	gPreviousTrafficJobMisses = 0;
	gPreviousTrafficJobDispatches = 0;
	gPreviousTrafficJobSkips = 0;
	gPreviousTrafficCruiseEligible = 0;
	gPreviousTrafficCruiseHits = 0;
	gPreviousTrafficCruiseMisses = 0;
	gPreviousTrafficCruiseStale = 0;
	gPreviousCollisionFlatScopes = 0;
	gPreviousCollisionFlatBuilds = 0;
	gPreviousCollisionFlatReuses = 0;
	gPreviousCollisionFlatStale = 0;
	gPreviousCollisionFlatOverflows = 0;
	gPreviousCollisionFlatItems = 0;
	gPreviousCollisionFlatSavedNodeVisits = 0;
	gPreviousPhysicsDirector = {};
	gPreviousVehicleVisualBudget = {};
#if defined(GTA_VR_OCULUS) || defined(GTA_VR_WEAPONS)
	memcpy(gPreviousSpawnTrace, CCarCtrl::VrSpawnTrace,
		sizeof(gPreviousSpawnTrace));
	gPreviousSpawnAttempts = CCarCtrl::NumVrSpawnAttempts;
	gPreviousSpawnSuccess = CCarCtrl::NumVrSpawnSuccess;
#if defined(__ANDROID__) && defined(GTA_VR_WEAPONS)
	gPreviousTrafficJobHits = CCarCtrl::NumVrTrafficJobHits;
	gPreviousTrafficJobMisses = CCarCtrl::NumVrTrafficJobMisses;
	gPreviousTrafficJobDispatches = CCarCtrl::NumVrTrafficJobDispatches;
	gPreviousTrafficJobSkips = CCarCtrl::NumVrTrafficJobSkips;
	gPreviousTrafficCruiseEligible = CCarCtrl::NumVrTrafficCruiseEligible;
	gPreviousTrafficCruiseHits = CCarCtrl::NumVrTrafficCruiseHits;
	gPreviousTrafficCruiseMisses = CCarCtrl::NumVrTrafficCruiseMisses;
	gPreviousTrafficCruiseStale = CCarCtrl::NumVrTrafficCruiseStale;
	gPreviousCollisionFlatScopes = CPhysical::NumVrCollisionFlatScopes;
	gPreviousCollisionFlatBuilds = CPhysical::NumVrCollisionFlatBuilds;
	gPreviousCollisionFlatReuses = CPhysical::NumVrCollisionFlatReuses;
	gPreviousCollisionFlatStale = CPhysical::NumVrCollisionFlatStale;
	gPreviousCollisionFlatOverflows = CPhysical::NumVrCollisionFlatOverflows;
	gPreviousCollisionFlatItems = CPhysical::NumVrCollisionFlatItems;
	gPreviousCollisionFlatSavedNodeVisits =
		CPhysical::NumVrCollisionFlatSavedNodeVisits;
	gPreviousPhysicsDirector = QuestPhysicsDirectorGetSnapshot();
	gPreviousVehicleVisualBudget = QuestVehicleVisualBudgetGetSnapshot();
#endif
#endif
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

uint64
QuestProfilerNowNanoseconds(void)
{
	return NowNanoseconds();
}

void
QuestProfilerSubmitWorldSimSample(const QuestWorldSimSample &sample)
{
	if(!gProfilerEnabled)
		return;

	const float nsToMs = 1.0f/1000000.0f;
	const float worldMs = (float)sample.worldTotalNs*nsToMs;
	gFrameWorldSimMs += worldMs;
	Record(worldMs, &gWorldSimSmoothedMs, &gWorldSimMaxMs);

	float animationMs = 0.0f;
	float collisionMs = 0.0f;
	for(int i = 0; i < QUEST_WORLD_ENTITY_COUNT; i++){
		const uint64 phaseNs[QUEST_WORLD_SIM_PHASE_COUNT] = {
			sample.animationNs[i],
			sample.controlNs[i],
			sample.postponedControlNs[i],
			sample.collisionNs[i],
			sample.shiftNs[i],
			sample.transformNs[i]
		};
		for(int phase = 0; phase < QUEST_WORLD_SIM_PHASE_COUNT; phase++)
			Record((float)phaseNs[phase]*nsToMs,
				&gWorldEntityPhaseSmoothedMs[i][phase],
				&gWorldEntityPhaseMaxMs[i][phase]);
		animationMs += (float)sample.animationNs[i]*nsToMs;
		collisionMs += (float)sample.collisionNs[i]*nsToMs;
		float entityMs = (float)(
			sample.animationNs[i]+sample.controlNs[i]+
			sample.postponedControlNs[i]+sample.collisionNs[i]+
			sample.shiftNs[i]+sample.transformNs[i])*nsToMs;
		if(i == QUEST_WORLD_ENTITY_PED)
			entityMs += (float)sample.pedAttachmentNs*nsToMs;
		Record(entityMs, &gEntitySimSmoothedMs[i],
			&gEntitySimMaxMs[i]);
	}
	Record(animationMs, &gWorldAnimationSmoothedMs,
		&gWorldAnimationMaxMs);
	Record(collisionMs, &gWorldCollisionSmoothedMs,
		&gWorldCollisionMaxMs);

	gSnapshot.pedControlCalls =
		(int32)(sample.controlCalls[QUEST_WORLD_ENTITY_PED]+
			sample.postponedControlCalls[QUEST_WORLD_ENTITY_PED]);
	gSnapshot.vehicleControlCalls =
		(int32)(sample.controlCalls[QUEST_WORLD_ENTITY_VEHICLE]+
			sample.postponedControlCalls[QUEST_WORLD_ENTITY_VEHICLE]);
	gSnapshot.collisionCalls = 0;
	for(int i = 0; i < QUEST_WORLD_ENTITY_COUNT; i++)
		gSnapshot.collisionCalls += (int32)sample.collisionCalls[i];
}

void
QuestProfilerSubmitRenderEntitySample(const QuestRenderEntitySample &sample)
{
	if(!gProfilerEnabled || !gAppFrameOpen ||
	   sample.entityClass < 0 ||
	   sample.entityClass >= QUEST_RENDER_ENTITY_COUNT)
		return;

	const int entityClass = (int)sample.entityClass;
	gFrameRenderEntityNs[entityClass] += sample.totalNs;
	gFrameRenderEntityCalls[entityClass]++;
	if(sample.fadingPath){
		gFrameRenderFadingEntityNs[entityClass] += sample.totalNs;
		gFrameRenderFadingEntityCalls[entityClass]++;
	}
	if(sample.entityClass == QUEST_RENDER_ENTITY_VEHICLE){
		for(int phase = 0; phase < QUEST_VEHICLE_RENDER_PHASE_COUNT;
		    phase++)
			gFrameVehicleRenderPhaseNs[phase] +=
				sample.vehiclePhaseNs[phase];
		gFrameVehicleOccupantsSubmitted +=
			sample.vehicleOccupantsSubmitted;
	}
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
	gFrameWorldSimMs = 0.0f;
	memset(gFramePhaseMs, 0, sizeof(gFramePhaseMs));
	memset(gFrameRenderEntityNs, 0, sizeof(gFrameRenderEntityNs));
	memset(gFrameRenderFadingEntityNs, 0,
		sizeof(gFrameRenderFadingEntityNs));
	memset(gFrameRenderEntityCalls, 0,
		sizeof(gFrameRenderEntityCalls));
	memset(gFrameRenderFadingEntityCalls, 0,
		sizeof(gFrameRenderFadingEntityCalls));
	memset(gFrameVehicleRenderPhaseNs, 0,
		sizeof(gFrameVehicleRenderPhaseNs));
	gFrameVehicleOccupantsSubmitted = 0;
	gFrameVisibleBuildings = 0;
	gFrameVisibleRoads = 0;
	memset(gFrameVisibleBuildingSlots, 0,
		sizeof(gFrameVisibleBuildingSlots));
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
	gSnapshot.visibleBuildings = (int32)gFrameVisibleBuildings;
	gSnapshot.visibleRoads = (int32)gFrameVisibleRoads;
	gSnapshot.activeOccluders = CRenderer::GetVrOcclusionCullingMode() >=
		VR_OCCLUSION_CULLING_AUTHORED ?
		COcclusion::NumActiveOccluders : 0;
	gSnapshot.occlusionCullingMode =
		CRenderer::GetVrOcclusionCullingMode();
	CloseOpenPhases(now);
	for(int i = 0; i < QUEST_PROFILER_PHASE_COUNT; i++)
		Record(gFramePhaseMs[i], &gPhaseSmoothedMs[i],
			&gWindowPhaseMaxMs[i]);
	const float simOtherRaw = Max(0.0f,
		gFramePhaseMs[QUEST_PROFILER_PHASE_GAME]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_STREAMING]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_SCRIPT]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_PRE_WORLD_MISC]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_WORLD_PROCESS]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_POST_WORLD_CAMERA_FX]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_POPULATION]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_CAR]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_PARTICLE_UPDATE]);
	Record(simOtherRaw, &gSimOtherSmoothedMs,
		&gWindowSimOtherMaxMs);
	const float renderOtherRaw = Max(0.0f,
		gFramePhaseMs[QUEST_PROFILER_PHASE_WORLD_RENDER]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_RENDER_SKY]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_RENDER_ROADS]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_RENDER_REFLECTIONS]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_RENDER_WORLD]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_RENDER_WATER]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_RENDER_FADING]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_RENDER_WEATHER]-
		gFramePhaseMs[QUEST_PROFILER_PHASE_RENDER_EFFECTS]);
	Record(renderOtherRaw, &gRenderOtherSmoothedMs,
		&gWindowRenderOtherMaxMs);
	const float nsToMs = 1.0f/1000000.0f;
	for(int entityClass = 0;
	    entityClass < QUEST_RENDER_ENTITY_COUNT;
	    entityClass++){
		Record((float)gFrameRenderEntityNs[entityClass]*nsToMs,
			&gRenderEntitySmoothedMs[entityClass],
			&gRenderEntityMaxMs[entityClass]);
		Record((float)gFrameRenderFadingEntityNs[entityClass]*nsToMs,
			&gRenderFadingEntitySmoothedMs[entityClass],
			&gRenderFadingEntityMaxMs[entityClass]);
		gSnapshot.renderEntityCalls[entityClass] =
			(int32)gFrameRenderEntityCalls[entityClass];
		gSnapshot.renderFadingEntityCalls[entityClass] =
			(int32)gFrameRenderFadingEntityCalls[entityClass];
	}
	for(int phase = 0; phase < QUEST_VEHICLE_RENDER_PHASE_COUNT;
	    phase++)
		Record((float)gFrameVehicleRenderPhaseNs[phase]*nsToMs,
			&gVehicleRenderPhaseSmoothedMs[phase],
			&gVehicleRenderPhaseMaxMs[phase]);
	gSnapshot.vehicleOccupantsSubmitted =
		(int32)gFrameVehicleOccupantsSubmitted;
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
		gSnapshot.appFps = fps;
		SampleDiagnostics();
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
			"Traffic want=%.1f served=%.1f amb=%d pursuit=%d proxy=%d "
			"pending=%d debris=%d stalled=%d pool=%d/%d ped=%d/%d "
			"spawn=%d ok=%d deny=%d top=%d:%d",
			gSnapshot.trafficDesired, gSnapshot.trafficServed,
			gSnapshot.trafficAmbient, gSnapshot.trafficPursuit,
			gSnapshot.trafficProxies, gSnapshot.trafficPending,
			gSnapshot.trafficDebris, gSnapshot.trafficStalled,
			gSnapshot.vehiclePoolUsed, gSnapshot.vehiclePoolSize,
			gSnapshot.pedPoolUsed, gSnapshot.pedPoolSize,
			gSnapshot.spawnAttemptsPerSecond,
			gSnapshot.spawnSuccessPerSecond,
			gSnapshot.spawnDeniedPerSecond,
			gSnapshot.spawnTopDenyReason,
			gSnapshot.spawnTopDenyCount);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"TrafficJob dispatch=%d skip=%d hit=%d miss=%d "
			"worker=%.3f/%.3fms build=%.3f/%.3fms",
			gSnapshot.trafficJobDispatchesPerSecond,
			gSnapshot.trafficJobSkipsPerSecond,
			gSnapshot.trafficJobHitsPerSecond,
			gSnapshot.trafficJobMissesPerSecond,
			gSnapshot.trafficJobMs,
			gSnapshot.trafficJobMaxMs,
			gSnapshot.trafficJobBuildMs,
			gSnapshot.trafficJobBuildMaxMs);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"TrafficCruise eligible=%d hit=%d miss=%d stale=%d",
			gSnapshot.trafficCruiseEligiblePerSecond,
			gSnapshot.trafficCruiseHitsPerSecond,
			gSnapshot.trafficCruiseMissesPerSecond,
			gSnapshot.trafficCruiseStalePerSecond);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"CollisionFlat scope=%d build=%d reuse=%d stale=%d "
			"overflow=%d items=%d saved=%d",
			gSnapshot.collisionFlatScopesPerSecond,
			gSnapshot.collisionFlatBuildsPerSecond,
			gSnapshot.collisionFlatReusesPerSecond,
			gSnapshot.collisionFlatStalePerSecond,
			gSnapshot.collisionFlatOverflowsPerSecond,
			gSnapshot.collisionFlatItemsPerSecond,
			gSnapshot.collisionFlatSavedNodeVisitsPerSecond);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"PhysicsDirector mode=%d tiers=%d/%d/%d/%d "
			"check=%d simple=%d skip=%d nodes=%d cand=%d pair=%d "
			"col=%d wheel=%d tri=%d step=%d+%d retry=%d "
			"manifold=%d remove=%d/%.3fms classify=%.3fms",
			gSnapshot.physicsDirectorMode,
			gSnapshot.physicsFull, gSnapshot.physicsReduced,
			gSnapshot.physicsRail, gSnapshot.physicsProxy,
			gSnapshot.physicsCheckCollisionPerSecond,
			gSnapshot.physicsCheckSimplePerSecond,
			gSnapshot.physicsSimpleSkippedPerSecond,
			gSnapshot.physicsSectorNodesPerSecond,
			gSnapshot.physicsBroadphaseCandidatesPerSecond,
			gSnapshot.physicsPairsAfterFilteringPerSecond,
			gSnapshot.physicsProcessColModelsPerSecond,
			gSnapshot.physicsWheelLineTestsPerSecond,
			gSnapshot.physicsTriangleTestsPerSecond,
			gSnapshot.physicsSubstepsPerSecond,
			gSnapshot.physicsExtraSubstepsPerSecond,
			gSnapshot.physicsRetryPassesPerSecond,
			gSnapshot.physicsContactManifoldsPerSecond,
			gSnapshot.physicsRemoveAndAddPerSecond,
			gSnapshot.physicsRemoveAndAddMs,
			gSnapshot.physicsClassifyMs);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"PhysicsV2 preset=%d level=%d budget=%.2fms managed=%.3f/%.3fms "
			"rail col=%d/%d ai=%d/%d proxy col=%d/%d ai=%d/%d "
			"promote=%d/%d/%d adapt=%d/%d work=%.3f/%.3fms remove_skip=%d",
			gSnapshot.physicsDirectorPreset,
			gSnapshot.physicsAdaptiveLevel,
			gSnapshot.physicsBudgetMs,
			gSnapshot.physicsManagedFrameMs,
			gSnapshot.physicsManagedAverageMs,
			gSnapshot.physicsRailCollisionRunsPerSecond,
			gSnapshot.physicsRailCollisionSkipsPerSecond,
			gSnapshot.physicsRailAiRunsPerSecond,
			gSnapshot.physicsRailAiSkipsPerSecond,
			gSnapshot.physicsProxyCollisionRunsPerSecond,
			gSnapshot.physicsProxyCollisionSkipsPerSecond,
			gSnapshot.physicsProxyAiRunsPerSecond,
			gSnapshot.physicsProxyAiSkipsPerSecond,
			gSnapshot.physicsSafetyPromotionsPerSecond,
			gSnapshot.physicsImminentPromotionsPerSecond,
			gSnapshot.physicsContactPromotionsPerSecond,
			gSnapshot.physicsAdaptiveEscalationsPerSecond,
			gSnapshot.physicsAdaptiveRelaxationsPerSecond,
			gSnapshot.physicsManagedSimpleCollisionMs,
			gSnapshot.physicsManagedAiMs,
			gSnapshot.physicsRemoveAndAddSkippedPerSecond);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"VehicleVisual mode=%d VHI=%d VLO=%d SKIPPED=%d OCC_SKIP=%d",
			gSnapshot.vehicleVisualBudgetMode,
			gSnapshot.vehicleVisualHighPerSecond,
			gSnapshot.vehicleVisualVloPerSecond,
			gSnapshot.vehicleVisualAtomicsSkippedPerSecond,
			gSnapshot.vehicleVisualOccupantsSkippedPerSecond);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"OpenXR performance supported=%d CPU=%d/%d GPU=%d/%d",
			xrvk::isPerformanceModeSupported() ? 1 : 0,
			xrvk::getPerformanceMode(),
			xrvk::getActivePerformanceMode(),
			xrvk::getGpuPerformanceMode(),
			xrvk::getActiveGpuPerformanceMode());
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"RenderScale request=%d selected=%d effective=%.2f%% "
			"actual=%dx%d recommended=%dx%d max=%dx%d fallback=%s "
			"previous=%d->%d/%s",
			gSnapshot.renderScaleRequestedPercent,
			gSnapshot.renderScaleSelectedPresetPercent,
			gSnapshot.renderScaleEffectivePercent,
			gSnapshot.renderScaleActualWidth,
			gSnapshot.renderScaleActualHeight,
			gSnapshot.renderScaleRecommendedWidth,
			gSnapshot.renderScaleRecommendedHeight,
			gSnapshot.renderScaleRuntimeMaxWidth,
			gSnapshot.renderScaleRuntimeMaxHeight,
			xrvk::getRenderScaleFallbackReasonName(
				gSnapshot.renderScaleFallbackReason),
			gSnapshot.renderScalePreviousFallbackRequest,
			gSnapshot.renderScalePreviousFallbackPercent,
			xrvk::getRenderScaleFallbackReasonName(
				gSnapshot.renderScalePreviousFallbackReason));
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
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"QuestGameSplit SCRIPT=%.2f/%.2f PRE_MISC=%.2f/%.2f "
			"WORLD=%.2f/%.2f POST_FX=%.2f/%.2f RESIDUAL=%.2f/%.2f",
			gSnapshot.cpuScriptMs, gSnapshot.cpuScriptMaxMs,
			gSnapshot.cpuPreWorldMiscMs,
			gSnapshot.cpuPreWorldMiscMaxMs,
			gSnapshot.cpuWorldProcessMs,
			gSnapshot.cpuWorldProcessMaxMs,
			gSnapshot.cpuPostWorldCameraFxMs,
			gSnapshot.cpuPostWorldCameraFxMaxMs,
			gSnapshot.cpuSimOtherMs,
			gSnapshot.cpuSimOtherMaxMs);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"QuestWorldSim TOTAL=%.2f/%.2f PED=%.2f/%.2f "
			"VEH=%.2f/%.2f OBJ=%.2f/%.2f ANI=%.2f/%.2f "
			"COL=%.2f/%.2f CALLS=%d/%d/%d",
			gSnapshot.cpuWorldSimMs, gSnapshot.cpuWorldSimMaxMs,
			gSnapshot.cpuPedSimMs, gSnapshot.cpuPedSimMaxMs,
			gSnapshot.cpuVehicleSimMs, gSnapshot.cpuVehicleSimMaxMs,
			gSnapshot.cpuObjectSimMs, gSnapshot.cpuObjectSimMaxMs,
			gSnapshot.cpuWorldAnimationMs,
			gSnapshot.cpuWorldAnimationMaxMs,
			gSnapshot.cpuWorldCollisionMs,
			gSnapshot.cpuWorldCollisionMaxMs,
			gSnapshot.pedControlCalls,
			gSnapshot.vehicleControlCalls,
			gSnapshot.collisionCalls);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"QuestRender SKY=%.2f/%.2f ROAD=%.2f/%.2f "
			"REFL=%.2f/%.2f WORLD=%.2f/%.2f WATER=%.2f/%.2f "
			"FADE=%.2f/%.2f WEATHER=%.2f/%.2f FX=%.2f/%.2f "
			"OTHER=%.2f/%.2f",
			gSnapshot.cpuRenderSkyMs, gSnapshot.cpuRenderSkyMaxMs,
			gSnapshot.cpuRenderRoadsMs, gSnapshot.cpuRenderRoadsMaxMs,
			gSnapshot.cpuRenderReflectionsMs,
			gSnapshot.cpuRenderReflectionsMaxMs,
			gSnapshot.cpuRenderWorldMs, gSnapshot.cpuRenderWorldMaxMs,
			gSnapshot.cpuRenderWaterMs, gSnapshot.cpuRenderWaterMaxMs,
			gSnapshot.cpuRenderFadingMs, gSnapshot.cpuRenderFadingMaxMs,
			gSnapshot.cpuRenderWeatherMs, gSnapshot.cpuRenderWeatherMaxMs,
			gSnapshot.cpuRenderEffectsMs, gSnapshot.cpuRenderEffectsMaxMs,
			gSnapshot.cpuRenderOtherMs, gSnapshot.cpuRenderOtherMaxMs);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"QuestRenderEntity V=%.2f/%d P=%.2f/%d S=%.2f/%d "
			"FV=%.2f/%d FP=%.2f/%d FS=%.2f/%d",
			gSnapshot.cpuRenderEntityMs[QUEST_RENDER_ENTITY_VEHICLE],
			gSnapshot.renderEntityCalls[QUEST_RENDER_ENTITY_VEHICLE],
			gSnapshot.cpuRenderEntityMs[QUEST_RENDER_ENTITY_PED],
			gSnapshot.renderEntityCalls[QUEST_RENDER_ENTITY_PED],
			gSnapshot.cpuRenderEntityMs[QUEST_RENDER_ENTITY_STATIC],
			gSnapshot.renderEntityCalls[QUEST_RENDER_ENTITY_STATIC],
			gSnapshot.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_VEHICLE],
			gSnapshot.renderFadingEntityCalls[QUEST_RENDER_ENTITY_VEHICLE],
			gSnapshot.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_PED],
			gSnapshot.renderFadingEntityCalls[QUEST_RENDER_ENTITY_PED],
			gSnapshot.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_STATIC],
			gSnapshot.renderFadingEntityCalls[QUEST_RENDER_ENTITY_STATIC]);
		__android_log_print(ANDROID_LOG_INFO, "QuestPerf",
			"QuestRenderVehicle LIGHT=%.2f OCC=%.2f BODY=%.2f "
			"ALPHA=%.2f OCC_N=%d",
			gSnapshot.cpuVehicleRenderPhaseMs
				[QUEST_VEHICLE_RENDER_PHASE_LIGHTING],
			gSnapshot.cpuVehicleRenderPhaseMs
				[QUEST_VEHICLE_RENDER_PHASE_OCCUPANTS],
			gSnapshot.cpuVehicleRenderPhaseMs
				[QUEST_VEHICLE_RENDER_PHASE_BODY_SUBMIT],
			gSnapshot.cpuVehicleRenderPhaseMs
				[QUEST_VEHICLE_RENDER_PHASE_ALPHA_ATOMICS],
			gSnapshot.vehicleOccupantsSubmitted);
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
				"%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,",
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
				gSnapshot.cpuScriptMs,
				gSnapshot.cpuScriptMaxMs,
				gSnapshot.cpuPreWorldMiscMs,
				gSnapshot.cpuPreWorldMiscMaxMs,
				gSnapshot.cpuWorldProcessMs,
				gSnapshot.cpuWorldProcessMaxMs,
				gSnapshot.cpuPostWorldCameraFxMs,
				gSnapshot.cpuPostWorldCameraFxMaxMs,
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
			fprintf(gLogFile,
				"%.3f,%.3f,%.3f,%.3f,%d,%d,%llu,%llu,%d,%d,"
				"%d,%d,%d,%.3f,%d,"
				"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%d,%d,%d,%d,"
				"%d,%d,%d,%d,%.3f,%.3f,%d,%d,%d,%d,%d,"
				"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
				"%d,%d,",
				gSnapshot.cpuPopulationMs,
				gSnapshot.cpuPopulationMaxMs,
				gSnapshot.cpuCarMs, gSnapshot.cpuCarMaxMs,
				gSnapshot.streamingRequested,
				gSnapshot.streamingPriority,
				(unsigned long long)gSnapshot.streamingMemoryUsed,
				(unsigned long long)gSnapshot.streamingMemoryAvailable,
				gSnapshot.streamingVehiclesLoaded,
				gSnapshot.streamingPedsLoaded,
				gSnapshot.pedTrafficPercent,
				gSnapshot.carTrafficPercent,
				gSnapshot.ambientPeds,
				gSnapshot.targetAmbientPeds,
				gSnapshot.ambientPedCap,
				gSnapshot.pedPoolUsed, gSnapshot.pedPoolSize,
				gSnapshot.vehiclePoolUsed, gSnapshot.vehiclePoolSize,
				gSnapshot.objectPoolUsed, gSnapshot.objectPoolSize,
				gSnapshot.visibleEntities, gSnapshot.invisibleEntities,
				gSnapshot.visibleBuildings, gSnapshot.visibleRoads,
				gSnapshot.activeOccluders,
				gSnapshot.occlusionCullingMode,
				gSnapshot.trafficDesired, gSnapshot.trafficServed,
				gSnapshot.trafficAmbient, gSnapshot.trafficPursuit,
				gSnapshot.trafficProxies, gSnapshot.trafficPending,
				gSnapshot.trafficDebris, gSnapshot.trafficStalled,
				gSnapshot.trafficScavenged,
				gSnapshot.trafficPoolReserveBlocks,
				gSnapshot.trafficDirectorMs,
				gSnapshot.trafficDirectorMaxMs,
				gSnapshot.spawnAttemptsPerSecond,
				gSnapshot.spawnSuccessPerSecond,
				gSnapshot.spawnDeniedPerSecond,
				gSnapshot.spawnTopDenyReason,
				gSnapshot.spawnTopDenyCount,
				gSnapshot.spawnReasonPerSecond[0],
				gSnapshot.spawnReasonPerSecond[1],
				gSnapshot.spawnReasonPerSecond[2],
				gSnapshot.spawnReasonPerSecond[3],
				gSnapshot.spawnReasonPerSecond[4],
				gSnapshot.spawnReasonPerSecond[5],
				gSnapshot.spawnReasonPerSecond[6],
				gSnapshot.spawnReasonPerSecond[7],
				gSnapshot.spawnReasonPerSecond[8],
				gSnapshot.spawnReasonPerSecond[9],
				gSnapshot.spawnReasonPerSecond[10],
				gSnapshot.spawnReasonPerSecond[11],
				gSnapshot.spawnReasonPerSecond[12],
				gSnapshot.spawnReasonPerSecond[13],
				gSnapshot.spawnReasonPerSecond[14],
				gSnapshot.spawnReasonPerSecond[15]);
			fprintf(gLogFile,
				"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,"
				"%.3f,%.3f,%.3f,%.3f,%d,%d,",
				gSnapshot.cpuWorldSimMs,
				gSnapshot.cpuWorldSimMaxMs,
				gSnapshot.cpuPedSimMs,
				gSnapshot.cpuPedSimMaxMs,
				gSnapshot.cpuVehicleSimMs,
				gSnapshot.cpuVehicleSimMaxMs,
				gSnapshot.cpuObjectSimMs,
				gSnapshot.cpuObjectSimMaxMs,
				gSnapshot.cpuWorldAnimationMs,
				gSnapshot.cpuWorldAnimationMaxMs,
				gSnapshot.cpuWorldCollisionMs,
				gSnapshot.cpuWorldCollisionMaxMs,
				gSnapshot.pedControlCalls,
				gSnapshot.vehicleControlCalls,
				gSnapshot.collisionCalls,
				gSnapshot.entryInfoPoolUsed,
				gSnapshot.entryInfoPoolSize,
				gSnapshot.cpuParticleUpdateMs,
				gSnapshot.cpuParticleUpdateMaxMs,
				gSnapshot.cpuParticleRenderMs,
				gSnapshot.cpuParticleRenderMaxMs,
				gSnapshot.particleActive,
				gSnapshot.heliDustActive);
			fprintf(gLogFile,
				"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,",
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_ANIMATION],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_ANIMATION],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_CONTROL],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_CONTROL],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_POSTPONED_CONTROL],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_POSTPONED_CONTROL],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_COLLISION],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_COLLISION],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_SHIFT],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_SHIFT],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_TRANSFORM],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_PED][QUEST_WORLD_SIM_PHASE_TRANSFORM],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_ANIMATION],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_ANIMATION],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_CONTROL],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_CONTROL],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_POSTPONED_CONTROL],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_POSTPONED_CONTROL],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_COLLISION],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_COLLISION],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_SHIFT],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_SHIFT],
				gSnapshot.cpuWorldEntityPhaseMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_TRANSFORM],
				gSnapshot.cpuWorldEntityPhaseMaxMs
					[QUEST_WORLD_ENTITY_VEHICLE][QUEST_WORLD_SIM_PHASE_TRANSFORM]);
			fprintf(gLogFile, "%d,%d,%.3f,%.3f,",
				gSnapshot.trafficJobHitsPerSecond,
				gSnapshot.trafficJobMissesPerSecond,
				gSnapshot.trafficJobMs,
				gSnapshot.trafficJobMaxMs);
			fprintf(gLogFile,
				"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
				"%.3f,%.3f,",
				gSnapshot.cpuRenderSkyMs,
				gSnapshot.cpuRenderSkyMaxMs,
				gSnapshot.cpuRenderRoadsMs,
				gSnapshot.cpuRenderRoadsMaxMs,
				gSnapshot.cpuRenderReflectionsMs,
				gSnapshot.cpuRenderReflectionsMaxMs,
				gSnapshot.cpuRenderWorldMs,
				gSnapshot.cpuRenderWorldMaxMs,
				gSnapshot.cpuRenderWaterMs,
				gSnapshot.cpuRenderWaterMaxMs,
				gSnapshot.cpuRenderFadingMs,
				gSnapshot.cpuRenderFadingMaxMs,
				gSnapshot.cpuRenderWeatherMs,
				gSnapshot.cpuRenderWeatherMaxMs,
				gSnapshot.cpuRenderEffectsMs,
				gSnapshot.cpuRenderEffectsMaxMs,
				gSnapshot.cpuRenderOtherMs,
				gSnapshot.cpuRenderOtherMaxMs);
			fprintf(gLogFile, "%d,%d,%.3f,%.3f,",
				gSnapshot.trafficJobDispatchesPerSecond,
				gSnapshot.trafficJobSkipsPerSecond,
				gSnapshot.trafficJobBuildMs,
				gSnapshot.trafficJobBuildMaxMs);
			fprintf(gLogFile,
				"%.3f,%.3f,%d,%.3f,%.3f,%d,%.3f,%.3f,%d,"
				"%.3f,%.3f,%d,%.3f,%.3f,%d,%.3f,%.3f,%d,"
				"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
				"%d,%d,%d,%d,%d,%d,%d,",
				gSnapshot.cpuRenderEntityMs[QUEST_RENDER_ENTITY_VEHICLE],
				gSnapshot.cpuRenderEntityMaxMs[QUEST_RENDER_ENTITY_VEHICLE],
				gSnapshot.renderEntityCalls[QUEST_RENDER_ENTITY_VEHICLE],
				gSnapshot.cpuRenderEntityMs[QUEST_RENDER_ENTITY_PED],
				gSnapshot.cpuRenderEntityMaxMs[QUEST_RENDER_ENTITY_PED],
				gSnapshot.renderEntityCalls[QUEST_RENDER_ENTITY_PED],
				gSnapshot.cpuRenderEntityMs[QUEST_RENDER_ENTITY_STATIC],
				gSnapshot.cpuRenderEntityMaxMs[QUEST_RENDER_ENTITY_STATIC],
				gSnapshot.renderEntityCalls[QUEST_RENDER_ENTITY_STATIC],
				gSnapshot.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_VEHICLE],
				gSnapshot.cpuRenderFadingEntityMaxMs[QUEST_RENDER_ENTITY_VEHICLE],
				gSnapshot.renderFadingEntityCalls[QUEST_RENDER_ENTITY_VEHICLE],
				gSnapshot.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_PED],
				gSnapshot.cpuRenderFadingEntityMaxMs[QUEST_RENDER_ENTITY_PED],
				gSnapshot.renderFadingEntityCalls[QUEST_RENDER_ENTITY_PED],
				gSnapshot.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_STATIC],
				gSnapshot.cpuRenderFadingEntityMaxMs[QUEST_RENDER_ENTITY_STATIC],
				gSnapshot.renderFadingEntityCalls[QUEST_RENDER_ENTITY_STATIC],
				gSnapshot.cpuVehicleRenderPhaseMs
					[QUEST_VEHICLE_RENDER_PHASE_LIGHTING],
				gSnapshot.cpuVehicleRenderPhaseMaxMs
					[QUEST_VEHICLE_RENDER_PHASE_LIGHTING],
				gSnapshot.cpuVehicleRenderPhaseMs
					[QUEST_VEHICLE_RENDER_PHASE_OCCUPANTS],
				gSnapshot.cpuVehicleRenderPhaseMaxMs
					[QUEST_VEHICLE_RENDER_PHASE_OCCUPANTS],
				gSnapshot.cpuVehicleRenderPhaseMs
					[QUEST_VEHICLE_RENDER_PHASE_BODY_SUBMIT],
				gSnapshot.cpuVehicleRenderPhaseMaxMs
					[QUEST_VEHICLE_RENDER_PHASE_BODY_SUBMIT],
				gSnapshot.cpuVehicleRenderPhaseMs
					[QUEST_VEHICLE_RENDER_PHASE_ALPHA_ATOMICS],
				gSnapshot.cpuVehicleRenderPhaseMaxMs
					[QUEST_VEHICLE_RENDER_PHASE_ALPHA_ATOMICS],
				gSnapshot.vehicleOccupantsSubmitted,
				xrvk::getPerformanceMode(),
				xrvk::getActivePerformanceMode(),
				xrvk::isPerformanceModeSupported() ? 1 : 0,
				xrvk::isPerformanceBoostBlocked() ? 1 : 0,
				xrvk::getGpuPerformanceMode(),
				xrvk::getActiveGpuPerformanceMode());
			fprintf(gLogFile, "%d,%d,%d,%d,",
				gSnapshot.trafficCruiseEligiblePerSecond,
				gSnapshot.trafficCruiseHitsPerSecond,
				gSnapshot.trafficCruiseMissesPerSecond,
				gSnapshot.trafficCruiseStalePerSecond);
			fprintf(gLogFile, "%d,%d,%d,%d,%d,%d,%d,",
				gSnapshot.collisionFlatScopesPerSecond,
				gSnapshot.collisionFlatBuildsPerSecond,
				gSnapshot.collisionFlatReusesPerSecond,
				gSnapshot.collisionFlatStalePerSecond,
				gSnapshot.collisionFlatOverflowsPerSecond,
				gSnapshot.collisionFlatItemsPerSecond,
				gSnapshot.collisionFlatSavedNodeVisitsPerSecond);
			fprintf(gLogFile,
				"%d,%d,%d,%d,%d,%d,%d,%.3f,"
				"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,",
				gSnapshot.physicsDirectorMode,
				gSnapshot.physicsFull,
				gSnapshot.physicsReduced,
				gSnapshot.physicsRail,
				gSnapshot.physicsProxy,
				gSnapshot.physicsTracked,
				gSnapshot.physicsOverflow,
				gSnapshot.physicsClassifyMs,
				gSnapshot.physicsCheckCollisionPerSecond,
				gSnapshot.physicsCheckSimplePerSecond,
				gSnapshot.physicsSimpleSkippedPerSecond,
				gSnapshot.physicsSectorNodesPerSecond,
				gSnapshot.physicsBroadphaseCandidatesPerSecond,
				gSnapshot.physicsPairsAfterFilteringPerSecond,
				gSnapshot.physicsProcessColModelsPerSecond,
				gSnapshot.physicsWheelLineTestsPerSecond,
				gSnapshot.physicsTriangleTestsPerSecond,
				gSnapshot.physicsSubstepsPerSecond,
				gSnapshot.physicsExtraSubstepsPerSecond,
				gSnapshot.physicsRetryPassesPerSecond,
				gSnapshot.physicsContactManifoldsPerSecond,
				gSnapshot.physicsRemoveAndAddPerSecond,
				gSnapshot.physicsRemoveAndAddMs);
			fprintf(gLogFile,
				"%d,%d,%.3f,%.3f,%.3f,"
				"%d,%d,%d,%d,%d,%d,%d,%d,"
				"%d,%d,%d,%d,%d,%.3f,%.3f,%d,",
				gSnapshot.physicsDirectorPreset,
				gSnapshot.physicsAdaptiveLevel,
				gSnapshot.physicsBudgetMs,
				gSnapshot.physicsManagedFrameMs,
				gSnapshot.physicsManagedAverageMs,
				gSnapshot.physicsRailCollisionRunsPerSecond,
				gSnapshot.physicsRailCollisionSkipsPerSecond,
				gSnapshot.physicsProxyCollisionRunsPerSecond,
				gSnapshot.physicsProxyCollisionSkipsPerSecond,
				gSnapshot.physicsRailAiRunsPerSecond,
				gSnapshot.physicsRailAiSkipsPerSecond,
				gSnapshot.physicsProxyAiRunsPerSecond,
				gSnapshot.physicsProxyAiSkipsPerSecond,
				gSnapshot.physicsSafetyPromotionsPerSecond,
				gSnapshot.physicsImminentPromotionsPerSecond,
				gSnapshot.physicsContactPromotionsPerSecond,
				gSnapshot.physicsAdaptiveEscalationsPerSecond,
				gSnapshot.physicsAdaptiveRelaxationsPerSecond,
				gSnapshot.physicsManagedSimpleCollisionMs,
				gSnapshot.physicsManagedAiMs,
				gSnapshot.physicsRemoveAndAddSkippedPerSecond);
			fprintf(gLogFile, "%d,%d,%d,%d,%d,",
				gSnapshot.vehicleVisualBudgetMode,
				gSnapshot.vehicleVisualHighPerSecond,
				gSnapshot.vehicleVisualVloPerSecond,
				gSnapshot.vehicleVisualAtomicsSkippedPerSecond,
				gSnapshot.vehicleVisualOccupantsSkippedPerSecond);
			fprintf(gLogFile,
				"%d,%d,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,",
				gSnapshot.renderScaleRequestedPercent,
				gSnapshot.renderScaleSelectedPresetPercent,
				gSnapshot.renderScaleEffectivePercent,
				gSnapshot.renderScaleActualWidth,
				gSnapshot.renderScaleActualHeight,
				gSnapshot.renderScaleRecommendedWidth,
				gSnapshot.renderScaleRecommendedHeight,
				gSnapshot.renderScaleRuntimeMaxWidth,
				gSnapshot.renderScaleRuntimeMaxHeight,
				gSnapshot.renderScaleFallbackReason,
				gSnapshot.renderScalePreviousFallbackRequest,
				gSnapshot.renderScalePreviousFallbackPercent,
				gSnapshot.renderScalePreviousFallbackReason);
			fprintf(gLogFile, "%d,%d,%d,%d,%d\n",
				gSnapshot.sgsrMode,
				gSnapshot.sgsrSceneWidth,
				gSnapshot.sgsrSceneHeight,
				gSnapshot.sgsrOutputWidth,
				gSnapshot.sgsrOutputHeight);
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
		gWindowRenderOtherMaxMs = 0.0f;
		gWorldSimMaxMs = 0.0f;
		memset(gEntitySimMaxMs, 0, sizeof(gEntitySimMaxMs));
		gWorldAnimationMaxMs = 0.0f;
		gWorldCollisionMaxMs = 0.0f;
		memset(gWorldEntityPhaseMaxMs, 0,
			sizeof(gWorldEntityPhaseMaxMs));
		memset(gRenderEntityMaxMs, 0, sizeof(gRenderEntityMaxMs));
		memset(gRenderFadingEntityMaxMs, 0,
			sizeof(gRenderFadingEntityMaxMs));
		memset(gVehicleRenderPhaseMaxMs, 0,
			sizeof(gVehicleRenderPhaseMaxMs));
	}
	gAppFrameOpen = false;
	gVkBeginOpen = false;
	gVkEndOpen = false;
	ResetOpenPhases();
}

void
QuestProfilerCountVisibleBuilding(const CEntity *entity)
{
	if(!gProfilerEnabled || !gAppFrameOpen || entity == nil)
		return;
	const uintptr_t value = reinterpret_cast<uintptr_t>(entity);
	uint32 slot = (uint32)((value >> 4) ^ (value >> 17)) &
		(ARRAY_SIZE(gFrameVisibleBuildingSlots)-1);
	for(uint32 probe = 0;
	    probe < ARRAY_SIZE(gFrameVisibleBuildingSlots); probe++){
		const CEntity *existing = gFrameVisibleBuildingSlots[slot];
		if(existing == entity)
			return;
		if(existing == nil){
			gFrameVisibleBuildingSlots[slot] = entity;
			CBaseModelInfo *modelInfo =
				CModelInfo::GetModelInfo(entity->GetModelIndex());
			if(modelInfo != nil && modelInfo->IsBuilding() &&
			   ((CSimpleModelInfo*)modelInfo)->m_wetRoadReflection)
				gFrameVisibleRoads++;
			else
				gFrameVisibleBuildings++;
			return;
		}
		slot = (slot+1) & (ARRAY_SIZE(gFrameVisibleBuildingSlots)-1);
	}
}

QuestProfilerSnapshot
QuestProfilerGetSnapshot(void)
{
	return gSnapshot;
}

} // namespace androidgame
