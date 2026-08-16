#include "common.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "AutoPilot.h"
#include "CarCtrl.h"
#include "CutsceneMgr.h"
#include "PathFind.h"
#include "Ped.h"
#include "Physical.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Pools.h"
#include "Timer.h"
#include "Vehicle.h"
#include "World.h"
#include "PhysicsDirector.h"

namespace androidgame {
namespace {

enum {
	MAX_DIRECTOR_VEHICLES = 256,
	VISIBLE_RENDER_GRACE_MS = 250
};

struct DirectorVehicle
{
	CVehicle *vehicle;
	QuestVehiclePhysicsTier tier;
	QuestVehiclePhysicsTier previousTier;
	float distanceSq;
	float radius;
	uint32 contactHoldUntilMs;
	bool protectedVehicle;
	bool imminentCollision;
	bool sectorFootprintValid;
	int32 sectorXStart;
	int32 sectorXEnd;
	int32 sectorYStart;
	int32 sectorYEnd;
};

struct DirectorPresetConfig
{
	float budgetMs;
	float reducedDistance;
	float proxyDistance;
	int32 railCollisionDivisor;
	int32 proxyCollisionDivisor;
	int32 railAiDivisor;
	int32 proxyAiDivisor;
	int32 maxAdaptiveLevel;
};

const DirectorPresetConfig gPresetConfig[QUEST_PHYSICS_PRESET_COUNT] = {
	// The budget covers only director-managed work: classification, ordinary
	// STATUS_SIMPLE AI/collision scans and vehicle RemoveAndAdd. Full rigid-body
	// player/mission physics is deliberately not throttled by this number.
	{ 3.00f, 95.0f, 180.0f, 2, 3, 2, 3, 2 },
	{ 2.50f, 90.0f, 170.0f, 2, 4, 2, 4, 3 },
	{ 1.80f, 82.0f, 150.0f, 3, 5, 3, 5, 3 }
};

int32 gMode = QUEST_PHYSICS_DIRECTOR_MEASURE;
int32 gPreset = QUEST_PHYSICS_PRESET_BALANCED;
int32 gAdaptiveLevel;
bool gTelemetryEnabled;
bool gDetailedTimingEnabled;
DirectorVehicle gVehicles[MAX_DIRECTOR_VEHICLES];
int32 gVehicleCount;
const CVehicle *gNarrowPhaseVehicle;
QuestPhysicsDirectorSnapshot gSnapshot;
uint64 gManagedFrameNs;
bool gManagedFrameValid;
bool gManagedAverageValid;
int32 gOverBudgetFrames;
int32 gUnderBudgetFrames;

uint64
NowNanoseconds(void)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64)now.tv_sec*1000000000ULL+(uint64)now.tv_nsec;
}

const DirectorPresetConfig &
GetPresetConfig(void)
{
	return gPresetConfig[gPreset];
}

void
ResetAdaptiveHistory(void)
{
	gAdaptiveLevel = 0;
	gManagedFrameNs = 0;
	gManagedFrameValid = false;
	gManagedAverageValid = false;
	gOverBudgetFrames = 0;
	gUnderBudgetFrames = 0;
	gSnapshot.adaptiveLevel = 0;
	gSnapshot.managedFrameMs = 0.0f;
	gSnapshot.managedAverageMs = 0.0f;
}

void
FinalizeManagedFrame(void)
{
	const DirectorPresetConfig &config = GetPresetConfig();
	gSnapshot.preset = gPreset;
	gSnapshot.budgetMs = config.budgetMs;
	if(!gManagedFrameValid){
		gManagedFrameValid = true;
		gManagedFrameNs = 0;
		gSnapshot.adaptiveLevel = gAdaptiveLevel;
		return;
	}

	const float frameMs = (float)gManagedFrameNs/1000000.0f;
	gSnapshot.managedFrameMs = frameMs;
	if(!gManagedAverageValid){
		gSnapshot.managedAverageMs = frameMs;
		gManagedAverageValid = true;
	}else
		gSnapshot.managedAverageMs +=
			(frameMs-gSnapshot.managedAverageMs)*0.10f;
	gManagedFrameNs = 0;

	if(gMode != QUEST_PHYSICS_DIRECTOR_ADAPTIVE){
		gSnapshot.adaptiveLevel = gAdaptiveLevel;
		return;
	}

	// Escalate quickly under sustained pressure, relax much more slowly. This
	// hysteresis prevents a line of cars at a tier boundary from changing
	// cadence every few frames.
	if(gSnapshot.managedAverageMs > config.budgetMs*1.05f){
		gOverBudgetFrames++;
		gUnderBudgetFrames = 0;
	}else if(gSnapshot.managedAverageMs < config.budgetMs*0.70f){
		gUnderBudgetFrames++;
		gOverBudgetFrames = 0;
	}else{
		gOverBudgetFrames = 0;
		gUnderBudgetFrames = 0;
	}
	if(gOverBudgetFrames >= 12 &&
	   gAdaptiveLevel < config.maxAdaptiveLevel){
		gAdaptiveLevel++;
		gOverBudgetFrames = 0;
		gUnderBudgetFrames = 0;
		gSnapshot.adaptiveEscalations++;
	}else if(gUnderBudgetFrames >= 90 && gAdaptiveLevel > 0){
		gAdaptiveLevel--;
		gOverBudgetFrames = 0;
		gUnderBudgetFrames = 0;
		gSnapshot.adaptiveRelaxations++;
	}
	gSnapshot.adaptiveLevel = gAdaptiveLevel;
}

const DirectorVehicle *
FindPrevious(const DirectorVehicle *previous, int32 previousCount,
	const CVehicle *vehicle)
{
	for(int32 i = 0; i < previousCount; i++)
		if(previous[i].vehicle == vehicle)
			return &previous[i];
	return nil;
}

bool
HasMissionOccupant(CVehicle *vehicle)
{
	if(vehicle->pDriver != nil &&
	   vehicle->pDriver->CharCreatedBy == MISSION_CHAR)
		return true;
	for(int32 i = 0; i < vehicle->m_nNumMaxPassengers; i++)
		if(vehicle->pPassengers[i] != nil &&
		   vehicle->pPassengers[i]->CharCreatedBy == MISSION_CHAR)
			return true;
	return false;
}

bool
IsSupportedCruiseStyle(CVehicle *vehicle)
{
	return vehicle->AutoPilot.m_nDrivingStyle ==
			DRIVINGSTYLE_STOP_FOR_CARS ||
		vehicle->AutoPilot.m_nDrivingStyle ==
			DRIVINGSTYLE_SLOW_DOWN_FOR_CARS ||
		vehicle->AutoPilot.m_nDrivingStyle ==
			DRIVINGSTYLE_STOP_FOR_CARS_IGNORE_LIGHTS;
}

bool
IsSafeAmbientCruise(CVehicle *vehicle)
{
	CPed *playerPed = FindPlayerPed();
	if(vehicle == nil || vehicle == FindPlayerVehicle() ||
	   vehicle->m_vehType != VEHICLE_TYPE_CAR ||
	   vehicle->GetStatus() != STATUS_SIMPLE ||
	   vehicle->VehicleCreatedBy != RANDOM_VEHICLE ||
	   vehicle->AutoPilot.m_nCarMission != MISSION_CRUISE ||
	   vehicle->AutoPilot.m_nTempAction != TEMPACT_NONE ||
	   vehicle->AutoPilot.m_bIgnorePathfinding ||
	   !IsSupportedCruiseStyle(vehicle))
		return false;
	if(vehicle->bIsLawEnforcer || vehicle->bCreatedAsPoliceVehicle ||
	   vehicle->bIsAmbulanceOnDuty || vehicle->bIsFireTruckOnDuty ||
	   vehicle->bIsLocked || vehicle->bExtendedRange ||
	   vehicle->bTakeLessDamage || vehicle->bHasBeenOwnedByPlayer ||
	   !vehicle->bCanBeDamaged || vehicle->bUsingSpecialColModel ||
	   vehicle->bIsCarParkVehicle || vehicle->bHasAlreadyBeenRecorded ||
	   vehicle->m_bombType != CARBOMB_NONE || vehicle->bPartOfConvoy ||
	   vehicle->bCreateRoadBlockPeds || vehicle->bIsBeingCarJacked ||
	   vehicle->m_numPedsUseItAsCover != 0 ||
	   vehicle->m_nNumGettingIn != 0 ||
	   vehicle->m_nGettingInFlags != 0 ||
	   vehicle->m_nGettingOutFlags != 0 || HasMissionOccupant(vehicle) ||
	   (playerPed != nil && playerPed->m_pPointGunAt == vehicle))
		return false;
	if(vehicle->bIsDamaged || vehicle->m_pCarFire != nil ||
	   vehicle->m_fHealth < 999.0f || vehicle->bHasHitWall ||
	   vehicle->bHasCollided || vehicle->bHasContacted ||
	   vehicle->m_nCollisionRecords != 0 || vehicle->bIsStuck ||
	   vehicle->bIsInWater || vehicle->bRestingOnPhysical ||
	   vehicle->m_pDamageEntity != nil || vehicle->m_fDamageImpulse > 0.0f)
		return false;
	if(vehicle->GetUp().z < 0.92f ||
	   Abs(vehicle->m_vecMoveSpeed.z) > 0.015f ||
	   vehicle->m_vecTurnSpeed.MagnitudeSqr() > 0.0025f)
		return false;
	const int32 numLinks = Min(ThePaths.m_numCarPathLinks,
		(int32)NUM_CARPATHLINKS);
	if(numLinks <= 0 ||
	   vehicle->AutoPilot.m_nCurrentPathNodeInfo >= (uint32)numLinks ||
	   vehicle->AutoPilot.m_nNextPathNodeInfo >= (uint32)numLinks)
		return false;
	return true;
}

bool
HasPossibleCollision(const CVector &positionA, const CVector &velocityA,
	float radiusA, const CVector &positionB, const CVector &velocityB,
	float radiusB)
{
	const CVector dp = positionB-positionA;
	if(Abs(dp.z) > 5.0f)
		return false;
	const float combined = radiusA+radiusB+1.5f;
	if(dp.x*dp.x+dp.y*dp.y < combined*combined)
		return true;
	const CVector dv = velocityB-velocityA;
	const float speedSq = dv.x*dv.x+dv.y*dv.y;
	if(speedSq < 0.000025f)
		return false;
	// Vice City move speed is displacement per original simulation tick.
	// A 100-tick horizon is roughly two seconds at the original 50 Hz.
	float t = -(dp.x*dv.x+dp.y*dv.y)/speedSq;
	t = clamp(t, 0.0f, 100.0f);
	const float closestX = dp.x+dv.x*t;
	const float closestY = dp.y+dv.y*t;
	return closestX*closestX+closestY*closestY < combined*combined;
}

DirectorVehicle *
FindEntry(const CVehicle *vehicle)
{
	for(int32 i = 0; i < gVehicleCount; i++)
		if(gVehicles[i].vehicle == vehicle)
			return &gVehicles[i];
	return nil;
}

bool
WasActuallySubmittedRecently(CVehicle *vehicle)
{
	// Quest deliberately builds a 360-degree safety list before the HMD view is
	// known, so the legacy bOffscreen flag is false for almost every streamed
	// car. The render submission hook is the authoritative per-view signal.
	return CCarCtrl::WasVrVehicleRenderedRecently(vehicle,
		VISIBLE_RENDER_GRACE_MS);
}

bool
PhysicalIsTrackedVehicle(const CPhysical *physical)
{
	return gTelemetryEnabled && physical != nil &&
		physical->GetType() == ENTITY_TYPE_VEHICLE;
}

void
PromoteEntryToFull(DirectorVehicle *entry, uint64 *counter)
{
	if(entry == nil)
		return;
	if(entry->tier != QUEST_VEHICLE_PHYSICS_FULL){
		if(counter != nil)
			(*counter)++;
		if(gSnapshot.tierCount[entry->tier] > 0)
			gSnapshot.tierCount[entry->tier]--;
		gSnapshot.tierCount[QUEST_VEHICLE_PHYSICS_FULL]++;
	}
	entry->tier = QUEST_VEHICLE_PHYSICS_FULL;
	entry->protectedVehicle = true;
}

DirectorVehicle *
FindSchedulableEntry(CVehicle *vehicle)
{
	DirectorVehicle *entry = FindEntry(vehicle);
	if(entry == nil)
		return nil;
	// Classification happens before vehicle control. Recheck here so aiming,
	// damage or scripted ownership changes in the same frame immediately cancel
	// a scheduled update instead of waiting for the next BeginFrame.
	if(WasActuallySubmittedRecently(vehicle) ||
	   !IsSafeAmbientCruise(vehicle)){
		PromoteEntryToFull(entry, &gSnapshot.safetyPromotions);
		return nil;
	}
	return entry;
}

bool
RunsOnThisFrame(const CVehicle *vehicle, int32 divisor, uint32 salt)
{
	if(divisor <= 1)
		return true;
	const uintptr_t identity = reinterpret_cast<uintptr_t>(vehicle) >> 4;
	return ((CTimer::GetFrameCounter()+(uint32)identity+salt) %
		divisor) == 0;
}

int32
LimitCollisionDivisorByTravel(const DirectorVehicle *entry,
	int32 divisor)
{
	const float travelPerFrame =
		entry->vehicle->GetMoveSpeed().Magnitude2D()*CTimer::GetTimeStep();
	if(travelPerFrame <= 0.0001f)
		return divisor;
	// STATUS_SIMPLE cars remain on the road graph. Still cap the interval so a
	// fast car cannot travel more than roughly half a body radius between
	// generic world scans. This is intentionally much stricter than a pure
	// background proxy, while avoiding SAFE V1's near-always-run behaviour.
	const float maximumTravel =
		Min(1.25f, Max(0.65f, entry->radius*0.50f));
	const int32 travelDivisor = Max(1,
		(int32)(maximumTravel/travelPerFrame));
	return Min(divisor, travelDivisor);
}

void
GetSectorFootprint(CPhysical *physical, int32 &xStart, int32 &xEnd,
	int32 &yStart, int32 &yEnd)
{
	const CRect bounds = physical->GetBoundRect();
	xStart = CWorld::GetClampedSectorIndexX(bounds.left);
	xEnd = CWorld::GetClampedSectorIndexX(bounds.right);
	yStart = CWorld::GetClampedSectorIndexY(bounds.top);
	yEnd = CWorld::GetClampedSectorIndexY(bounds.bottom);
}

void
RecordTierSchedule(QuestVehiclePhysicsTier tier, bool collision,
	bool run)
{
	if(tier == QUEST_VEHICLE_PHYSICS_RAIL){
		uint64 &counter = collision ?
			(run ? gSnapshot.railCollisionRuns :
			 gSnapshot.railCollisionSkips) :
			(run ? gSnapshot.railAiRuns : gSnapshot.railAiSkips);
		counter++;
	}else if(tier == QUEST_VEHICLE_PHYSICS_PROXY){
		uint64 &counter = collision ?
			(run ? gSnapshot.proxyCollisionRuns :
			 gSnapshot.proxyCollisionSkips) :
			(run ? gSnapshot.proxyAiRuns : gSnapshot.proxyAiSkips);
		counter++;
	}
}

} // namespace

bool
QuestPhysicsDirectorIsSafeAmbientCruise(CVehicle *vehicle)
{
	return IsSafeAmbientCruise(vehicle);
}

void
QuestPhysicsDirectorSetMode(int32 mode)
{
	const int32 nextMode = Min(Max(mode,
		(int32)QUEST_PHYSICS_DIRECTOR_OFF),
		(int32)QUEST_PHYSICS_DIRECTOR_MODE_COUNT-1);
	if(nextMode != gMode)
		ResetAdaptiveHistory();
	gMode = nextMode;
	gSnapshot.mode = gMode;
}

int32
QuestPhysicsDirectorGetMode(void)
{
	return gMode;
}

const char *
QuestPhysicsDirectorGetModeName(void)
{
	static const char *const names[QUEST_PHYSICS_DIRECTOR_MODE_COUNT] = {
		"OFF / ORIGINAL", "MEASURE", "SAFE V1", "ADAPTIVE V2"
	};
	return names[gMode];
}

void
QuestPhysicsDirectorSetPreset(int32 preset)
{
	const int32 nextPreset = Min(Max(preset,
		(int32)QUEST_PHYSICS_PRESET_QUALITY),
		(int32)QUEST_PHYSICS_PRESET_COUNT-1);
	if(nextPreset != gPreset)
		ResetAdaptiveHistory();
	gPreset = nextPreset;
	gSnapshot.preset = gPreset;
	gSnapshot.budgetMs = GetPresetConfig().budgetMs;
}

int32
QuestPhysicsDirectorGetPreset(void)
{
	return gPreset;
}

const char *
QuestPhysicsDirectorGetPresetName(void)
{
	static const char *const names[QUEST_PHYSICS_PRESET_COUNT] = {
		"QUALITY 3.0MS", "BALANCED 2.5MS", "PERFORMANCE 1.8MS"
	};
	return names[gPreset];
}

const char *
QuestPhysicsDirectorGetStatusName(void)
{
	if(gMode == QUEST_PHYSICS_DIRECTOR_OFF)
		return "ORIGINAL PHYSICS";
	if(gMode == QUEST_PHYSICS_DIRECTOR_MEASURE)
		return "MEASURING ONLY";
	if(gMode == QUEST_PHYSICS_DIRECTOR_SAFE)
		return "SAFE FIXED CADENCE";
	static const char *const adaptiveStatus[] = {
		"V2 NOMINAL", "V2 PRESSURE 1", "V2 PRESSURE 2", "V2 MAX PRESSURE"
	};
	return adaptiveStatus[Min(Max(gAdaptiveLevel, 0), 3)];
}

void
QuestPhysicsDirectorBeginFrame(bool profilerEnabled)
{
	FinalizeManagedFrame();
	// MEASURE is a zero-overhead baseline while the profiler is closed. SAFE
	// and ADAPTIVE must classify so their scheduler/contact state stays live.
	gTelemetryEnabled = profilerEnabled ||
		gMode == QUEST_PHYSICS_DIRECTOR_SAFE ||
		gMode == QUEST_PHYSICS_DIRECTOR_ADAPTIVE;
	gDetailedTimingEnabled = profilerEnabled ||
		gMode == QUEST_PHYSICS_DIRECTOR_ADAPTIVE;
	gSnapshot.mode = gMode;
	gSnapshot.preset = gPreset;
	gSnapshot.adaptiveLevel = gAdaptiveLevel;
	gSnapshot.budgetMs = GetPresetConfig().budgetMs;
	memset(gSnapshot.tierCount, 0, sizeof(gSnapshot.tierCount));
	gSnapshot.trackedVehicles = 0;
	gSnapshot.overflowVehicles = 0;
	gSnapshot.classifyMs = 0.0f;
	if(!gTelemetryEnabled){
		gVehicleCount = 0;
		return;
	}

	const uint64 startedNs = gDetailedTimingEnabled ? NowNanoseconds() : 0;
	DirectorVehicle previous[MAX_DIRECTOR_VEHICLES];
	const int32 previousCount = gVehicleCount;
	memcpy(previous, gVehicles,
		previousCount*sizeof(DirectorVehicle));
	gVehicleCount = 0;

	CVehiclePool *pool = CPools::GetVehiclePool();
	const int32 poolSize = pool != nil ? pool->GetSize() : 0;
	CPed *playerPed = FindPlayerPed();
	const uint32 nowMs = CTimer::GetTimeInMilliseconds();
	const CVector playerPosition = playerPed != nil ?
		playerPed->GetPosition() : CVector(0.0f, 0.0f, 0.0f);
	for(int32 i = 0; i < poolSize; i++){
		CVehicle *vehicle = pool->GetSlot(i);
		if(vehicle == nil)
			continue;
		if(gVehicleCount >= MAX_DIRECTOR_VEHICLES){
			gSnapshot.overflowVehicles++;
			gSnapshot.tierCount[QUEST_VEHICLE_PHYSICS_FULL]++;
			continue;
		}
		DirectorVehicle &entry = gVehicles[gVehicleCount++];
		entry.vehicle = vehicle;
		const DirectorVehicle *old = FindPrevious(previous,
			previousCount, vehicle);
		entry.tier = old != nil ? old->tier :
			QUEST_VEHICLE_PHYSICS_FULL;
		entry.previousTier = entry.tier;
		entry.distanceSq = playerPed != nil ?
			(vehicle->GetPosition()-playerPosition).MagnitudeSqr() : 0.0f;
		entry.radius = Max(0.5f, vehicle->GetBoundRadius());
		entry.contactHoldUntilMs = old != nil ?
			old->contactHoldUntilMs : 0;
		if(entry.contactHoldUntilMs != 0 &&
		   (int32)(entry.contactHoldUntilMs-nowMs) <= 0)
			entry.contactHoldUntilMs = 0;
		entry.protectedVehicle = !IsSafeAmbientCruise(vehicle) ||
			entry.contactHoldUntilMs != 0;
		entry.imminentCollision = false;
		GetSectorFootprint(vehicle, entry.sectorXStart, entry.sectorXEnd,
			entry.sectorYStart, entry.sectorYEnd);
		entry.sectorFootprintValid = old != nil &&
			vehicle->m_entryInfoList.first != nil;
	}

	// Promote both members before either is allowed onto a reduced schedule.
	// This is an O(n^2) pass over a small fixed vehicle pool (normally 20-40),
	// not a replacement collision broadphase.
	for(int32 i = 0; i < gVehicleCount; i++){
		DirectorVehicle &a = gVehicles[i];
		if(!a.protectedVehicle && playerPed != nil && HasPossibleCollision(
		   a.vehicle->GetPosition(), a.vehicle->GetMoveSpeed(), a.radius,
		   playerPosition, playerPed->GetMoveSpeed(), 1.0f))
			a.imminentCollision = true;
		for(int32 j = i+1; j < gVehicleCount; j++){
			DirectorVehicle &b = gVehicles[j];
			// Protected vehicles are Full regardless, but an approaching ambient
			// car must also be promoted. Do not make that safety check depend on
			// pool ordering.
			if(a.protectedVehicle && b.protectedVehicle)
				continue;
			const CVector delta =
				b.vehicle->GetPosition()-a.vehicle->GetPosition();
			if(delta.MagnitudeSqr() > 80.0f*80.0f)
				continue;
			// Dense queues deserve full cadence even when both cars currently have
			// almost identical velocity. Relative-velocity prediction alone cannot
			// see a same-speed overlap developing when the leading AI changes state
			// on a skipped frame. This guard trades a little of the excellent 300%
			// CPU saving for visibly stable close traffic.
			const float queueGuard = a.radius+b.radius+8.0f;
			if(Abs(delta.z) < 5.0f &&
			   delta.x*delta.x+delta.y*delta.y < queueGuard*queueGuard){
				a.imminentCollision = true;
				b.imminentCollision = true;
			}else if(HasPossibleCollision(a.vehicle->GetPosition(),
			   a.vehicle->GetMoveSpeed(), a.radius,
			   b.vehicle->GetPosition(), b.vehicle->GetMoveSpeed(),
			   b.radius)){
				a.imminentCollision = true;
				b.imminentCollision = true;
			}
		}
	}

	for(int32 i = 0; i < gVehicleCount; i++){
		DirectorVehicle &entry = gVehicles[i];
		const bool wasFull =
			entry.previousTier == QUEST_VEHICLE_PHYSICS_FULL;
		const DirectorPresetConfig &config = GetPresetConfig();
		const float reducedDistance = Max(66.0f,
			config.reducedDistance-gAdaptiveLevel*8.0f);
		const float proxyDistance = Max(reducedDistance+35.0f,
			config.proxyDistance-gAdaptiveLevel*15.0f);
		if(playerPed == nil || entry.protectedVehicle ||
		   entry.imminentCollision || entry.distanceSq <= 45.0f*45.0f ||
		   WasActuallySubmittedRecently(entry.vehicle) ||
		   wasFull && entry.distanceSq <= 65.0f*65.0f)
			entry.tier = QUEST_VEHICLE_PHYSICS_FULL;
		else if(entry.distanceSq <= reducedDistance*reducedDistance)
			entry.tier = QUEST_VEHICLE_PHYSICS_REDUCED;
		else if(entry.distanceSq <= proxyDistance*proxyDistance)
			entry.tier = QUEST_VEHICLE_PHYSICS_RAIL;
		else
			entry.tier = QUEST_VEHICLE_PHYSICS_PROXY;
		if(entry.tier == QUEST_VEHICLE_PHYSICS_FULL &&
		   entry.previousTier != QUEST_VEHICLE_PHYSICS_FULL){
			if(entry.imminentCollision)
				gSnapshot.imminentPromotions++;
			else if(entry.protectedVehicle ||
			        WasActuallySubmittedRecently(entry.vehicle))
				gSnapshot.safetyPromotions++;
		}
		gSnapshot.tierCount[entry.tier]++;
	}
	gSnapshot.trackedVehicles = gVehicleCount;
	if(startedNs != 0){
		const uint64 classifyNs = NowNanoseconds()-startedNs;
		gSnapshot.classifyMs = (float)classifyNs/1000000.0f;
		gManagedFrameNs += classifyNs;
	}
}

QuestVehiclePhysicsTier
QuestPhysicsDirectorGetTier(const CVehicle *vehicle)
{
	DirectorVehicle *entry = FindEntry(vehicle);
	if(entry != nil && (WasActuallySubmittedRecently(
	   const_cast<CVehicle *>(vehicle)) ||
	   !IsSafeAmbientCruise(const_cast<CVehicle *>(vehicle))))
		PromoteEntryToFull(entry, &gSnapshot.safetyPromotions);
	return entry != nil ? entry->tier : QUEST_VEHICLE_PHYSICS_FULL;
}

bool
QuestPhysicsDirectorShouldRunSimpleCollision(CVehicle *vehicle)
{
	if((gMode != QUEST_PHYSICS_DIRECTOR_SAFE &&
	    gMode != QUEST_PHYSICS_DIRECTOR_ADAPTIVE) ||
	   CCutsceneMgr::IsRunning())
		return true;
	DirectorVehicle *entry = FindSchedulableEntry(vehicle);
	if(entry == nil || entry->imminentCollision ||
	   entry->tier < QUEST_VEHICLE_PHYSICS_RAIL)
		return true;

	int32 divisor;
	if(gMode == QUEST_PHYSICS_DIRECTOR_SAFE)
		divisor = entry->tier == QUEST_VEHICLE_PHYSICS_PROXY ? 3 : 2;
	else{
		const DirectorPresetConfig &config = GetPresetConfig();
		divisor = entry->tier == QUEST_VEHICLE_PHYSICS_PROXY ?
			config.proxyCollisionDivisor+gAdaptiveLevel*2 :
			config.railCollisionDivisor+gAdaptiveLevel;
	}
	divisor = LimitCollisionDivisorByTravel(entry, divisor);
	const bool run = RunsOnThisFrame(vehicle, divisor, 0x31U);
	RecordTierSchedule(entry->tier, true, run);
	if(!run)
		gSnapshot.simpleChecksSkipped++;
	return run;
}

bool
QuestPhysicsDirectorShouldRunRailAi(CVehicle *vehicle)
{
	if(gMode != QUEST_PHYSICS_DIRECTOR_ADAPTIVE ||
	   CCutsceneMgr::IsRunning())
		return true;
	DirectorVehicle *entry = FindSchedulableEntry(vehicle);
	if(entry == nil || entry->imminentCollision ||
	   entry->tier < QUEST_VEHICLE_PHYSICS_RAIL)
		return true;

	const DirectorPresetConfig &config = GetPresetConfig();
	const int32 divisor = entry->tier == QUEST_VEHICLE_PHYSICS_PROXY ?
		config.proxyAiDivisor+gAdaptiveLevel*2 :
		config.railAiDivisor+gAdaptiveLevel;
	const bool run = RunsOnThisFrame(vehicle, divisor, 0x79U);
	RecordTierSchedule(entry->tier, false, run);
	return run;
}

uint64
QuestPhysicsDirectorBeginSimpleCollision(CVehicle *vehicle)
{
	return gDetailedTimingEnabled && vehicle != nil ?
		NowNanoseconds() : 0;
}

void
QuestPhysicsDirectorEndSimpleCollision(CVehicle *vehicle,
	uint64 startedNs)
{
	if(startedNs == 0 || vehicle == nil)
		return;
	const uint64 elapsed = NowNanoseconds()-startedNs;
	gSnapshot.managedSimpleCollisionNs += elapsed;
	gManagedFrameNs += elapsed;
}

uint64
QuestPhysicsDirectorBeginRailAi(CVehicle *vehicle)
{
	return gDetailedTimingEnabled && vehicle != nil ?
		NowNanoseconds() : 0;
}

void
QuestPhysicsDirectorEndRailAi(CVehicle *vehicle, uint64 startedNs)
{
	if(startedNs == 0 || vehicle == nil)
		return;
	const uint64 elapsed = NowNanoseconds()-startedNs;
	gSnapshot.managedAiNs += elapsed;
	gManagedFrameNs += elapsed;
}

bool
QuestPhysicsDirectorTelemetryEnabled(void)
{
	return gTelemetryEnabled;
}

void
QuestPhysicsDirectorRecordCheckCollision(const CPhysical *physical,
	bool simpleCar)
{
	if(!PhysicalIsTrackedVehicle(physical))
		return;
	if(simpleCar)
		gSnapshot.checkCollisionSimpleCalls++;
	else
		gSnapshot.checkCollisionCalls++;
}

void
QuestPhysicsDirectorRecordSectorNode(const CPhysical *physical)
{
	if(PhysicalIsTrackedVehicle(physical))
		gSnapshot.sectorNodesVisited++;
}

void
QuestPhysicsDirectorRecordBroadphaseCandidate(const CPhysical *physical)
{
	if(PhysicalIsTrackedVehicle(physical))
		gSnapshot.broadphaseCandidates++;
}

void
QuestPhysicsDirectorBeginVehicleNarrowPhase(const CVehicle *vehicle)
{
	if(!gTelemetryEnabled || vehicle == nil)
		return;
	gNarrowPhaseVehicle = vehicle;
	gSnapshot.pairsAfterFiltering++;
	gSnapshot.processColModelsCalls++;
}

void
QuestPhysicsDirectorEndVehicleNarrowPhase(const CVehicle *vehicle)
{
	if(gNarrowPhaseVehicle == vehicle)
		gNarrowPhaseVehicle = nil;
}

void
QuestPhysicsDirectorRecordProcessColModelsWork(int32 wheelLines,
	int32 sphereCandidates, int32 sphereTargets, int32 boxTargets,
	int32 triangleTargets)
{
	if(!gTelemetryEnabled || gNarrowPhaseVehicle == nil)
		return;
	const uint64 targets = (uint64)Max(0, sphereTargets)+
		(uint64)Max(0, boxTargets)+(uint64)Max(0, triangleTargets);
	gSnapshot.wheelLineTests += (uint64)Max(0, wheelLines)*targets;
	gSnapshot.triangleTests +=
		(uint64)(Max(0, sphereCandidates)+Max(0, wheelLines))*
		(uint64)Max(0, triangleTargets);
}

void
QuestPhysicsDirectorRecordContactManifold(const CVehicle *vehicle,
	bool producedContact)
{
	if(!gTelemetryEnabled || vehicle == nil || !producedContact)
		return;
	gSnapshot.contactManifolds++;
	DirectorVehicle *entry = FindEntry(vehicle);
	if(entry != nil){
		// Body contacts retain Full for three seconds after the last hit so
		// cars cannot oscillate at a tier boundary while a pileup settles.
		if(entry->tier != QUEST_VEHICLE_PHYSICS_FULL)
			gSnapshot.contactPromotions++;
		entry->contactHoldUntilMs = CTimer::GetTimeInMilliseconds()+3000;
		PromoteEntryToFull(entry, nil);
	}
}

void
QuestPhysicsDirectorRecordSubsteps(const CVehicle *vehicle, int32 substeps)
{
	if(!gTelemetryEnabled || vehicle == nil)
		return;
	const int32 count = Max(1, substeps);
	gSnapshot.substeps += count;
	gSnapshot.extraSubsteps += Max(0, count-1);
}

void
QuestPhysicsDirectorRecordRetry(const CVehicle *vehicle)
{
	if(gTelemetryEnabled && vehicle != nil)
		gSnapshot.retryPasses++;
}

bool
QuestPhysicsDirectorShouldRunRemoveAndAdd(CPhysical *physical)
{
	if(gMode != QUEST_PHYSICS_DIRECTOR_ADAPTIVE || physical == nil ||
	   physical->GetType() != ENTITY_TYPE_VEHICLE ||
	   CCutsceneMgr::IsRunning())
		return true;
	CVehicle *vehicle = (CVehicle *)physical;
	DirectorVehicle *entry = FindSchedulableEntry(vehicle);
	if(entry == nil || entry->imminentCollision ||
	   entry->tier < QUEST_VEHICLE_PHYSICS_RAIL)
		return true;

	int32 xStart, xEnd, yStart, yEnd;
	GetSectorFootprint(physical, xStart, xEnd, yStart, yEnd);
	if(!entry->sectorFootprintValid ||
	   physical->m_entryInfoList.first == nil){
		entry->sectorFootprintValid = true;
		entry->sectorXStart = xStart;
		entry->sectorXEnd = xEnd;
		entry->sectorYStart = yStart;
		entry->sectorYEnd = yEnd;
		return true;
	}

	const bool unchanged = xStart == entry->sectorXStart &&
		xEnd == entry->sectorXEnd && yStart == entry->sectorYStart &&
		yEnd == entry->sectorYEnd;
	entry->sectorXStart = xStart;
	entry->sectorXEnd = xEnd;
	entry->sectorYStart = yStart;
	entry->sectorYEnd = yEnd;
	if(unchanged)
		gSnapshot.removeAndAddSkipped++;
	return !unchanged;
}

uint64
QuestPhysicsDirectorBeginRemoveAndAdd(const CPhysical *physical)
{
	if(!PhysicalIsTrackedVehicle(physical))
		return 0;
	gSnapshot.removeAndAddCalls++;
	return gDetailedTimingEnabled ? NowNanoseconds() : 0;
}

void
QuestPhysicsDirectorEndRemoveAndAdd(const CPhysical *physical,
	uint64 startedNs)
{
	if(startedNs != 0 && PhysicalIsTrackedVehicle(physical)){
		const uint64 elapsed = NowNanoseconds()-startedNs;
		gSnapshot.removeAndAddNs += elapsed;
		gManagedFrameNs += elapsed;
	}
}

QuestPhysicsDirectorSnapshot
QuestPhysicsDirectorGetSnapshot(void)
{
	return gSnapshot;
}

} // namespace androidgame
