#pragma once

class CPhysical;
class CVehicle;

namespace androidgame {

enum QuestPhysicsDirectorMode
{
	QUEST_PHYSICS_DIRECTOR_OFF = 0,
	QUEST_PHYSICS_DIRECTOR_MEASURE,
	QUEST_PHYSICS_DIRECTOR_SAFE,
	QUEST_PHYSICS_DIRECTOR_ADAPTIVE,
	QUEST_PHYSICS_DIRECTOR_MODE_COUNT
};

enum QuestPhysicsDirectorPreset
{
	QUEST_PHYSICS_PRESET_QUALITY = 0,
	QUEST_PHYSICS_PRESET_BALANCED,
	QUEST_PHYSICS_PRESET_PERFORMANCE,
	QUEST_PHYSICS_PRESET_COUNT
};

enum QuestVehiclePhysicsTier
{
	QUEST_VEHICLE_PHYSICS_FULL = 0,
	QUEST_VEHICLE_PHYSICS_REDUCED,
	QUEST_VEHICLE_PHYSICS_RAIL,
	QUEST_VEHICLE_PHYSICS_PROXY,
	QUEST_VEHICLE_PHYSICS_TIER_COUNT
};

// Tier counts and classifyMs describe the latest simulation frame. The uint64
// counters are monotonic so QuestProfiler can turn them into one-second rates
// without resetting live collision state.
struct QuestPhysicsDirectorSnapshot
{
	int32 mode;
	int32 tierCount[QUEST_VEHICLE_PHYSICS_TIER_COUNT];
	int32 trackedVehicles;
	int32 overflowVehicles;
	float classifyMs;
	int32 preset;
	int32 adaptiveLevel;
	float budgetMs;
	float managedFrameMs;
	float managedAverageMs;

	uint64 checkCollisionCalls;
	uint64 checkCollisionSimpleCalls;
	uint64 simpleChecksSkipped;
	uint64 railCollisionRuns;
	uint64 railCollisionSkips;
	uint64 proxyCollisionRuns;
	uint64 proxyCollisionSkips;
	uint64 railAiRuns;
	uint64 railAiSkips;
	uint64 proxyAiRuns;
	uint64 proxyAiSkips;
	uint64 safetyPromotions;
	uint64 imminentPromotions;
	uint64 contactPromotions;
	uint64 adaptiveEscalations;
	uint64 adaptiveRelaxations;
	uint64 managedSimpleCollisionNs;
	uint64 managedAiNs;
	uint64 removeAndAddSkipped;
	uint64 sectorNodesVisited;
	uint64 broadphaseCandidates;
	uint64 pairsAfterFiltering;
	uint64 processColModelsCalls;
	uint64 wheelLineTests;
	uint64 triangleTests;
	uint64 substeps;
	uint64 extraSubsteps;
	uint64 retryPasses;
	uint64 contactManifolds;
	uint64 removeAndAddCalls;
	uint64 removeAndAddNs;
};

void QuestPhysicsDirectorSetMode(int32 mode);
int32 QuestPhysicsDirectorGetMode(void);
const char *QuestPhysicsDirectorGetModeName(void);
void QuestPhysicsDirectorSetPreset(int32 preset);
int32 QuestPhysicsDirectorGetPreset(void);
const char *QuestPhysicsDirectorGetPresetName(void);
const char *QuestPhysicsDirectorGetStatusName(void);

// Called once at the start of CWorld::Process. Profiling can request the same
// classification/counters while the scheduler itself is OFF.
void QuestPhysicsDirectorBeginFrame(bool profilerEnabled);
QuestVehiclePhysicsTier QuestPhysicsDirectorGetTier(const CVehicle *vehicle);

// Shared fail-closed classifier for optional visual scheduling. It is
// independent of the Physics Director mode and never mutates the vehicle.
bool QuestPhysicsDirectorIsSafeAmbientCruise(CVehicle *vehicle);

// SAFE V1 only schedules the existing STATUS_SIMPLE collision scan. It never
// skips control, path following, transforms, sector membership updates or any
// full rigid-body vehicle processing.
bool QuestPhysicsDirectorShouldRunSimpleCollision(CVehicle *vehicle);

// ADAPTIVE V2 schedules only the policy update for distant, hidden, ordinary
// STATUS_SIMPLE cruise cars. UpdateCarOnRails, transforms and CWorld sector
// membership still run every frame, so there is no low-rate visual stepping.
bool QuestPhysicsDirectorShouldRunRailAi(CVehicle *vehicle);

// Timers cover only work controlled by the director. They are no-ops unless
// ADAPTIVE V2 is active or the profiler is recording.
uint64 QuestPhysicsDirectorBeginSimpleCollision(CVehicle *vehicle);
void QuestPhysicsDirectorEndSimpleCollision(CVehicle *vehicle,
	uint64 startedNs);
uint64 QuestPhysicsDirectorBeginRailAi(CVehicle *vehicle);
void QuestPhysicsDirectorEndRailAi(CVehicle *vehicle,
	uint64 startedNs);

bool QuestPhysicsDirectorTelemetryEnabled(void);
void QuestPhysicsDirectorRecordCheckCollision(const CPhysical *physical,
	bool simpleCar);
void QuestPhysicsDirectorRecordSectorNode(const CPhysical *physical);
void QuestPhysicsDirectorRecordBroadphaseCandidate(
	const CPhysical *physical);
void QuestPhysicsDirectorBeginVehicleNarrowPhase(const CVehicle *vehicle);
void QuestPhysicsDirectorEndVehicleNarrowPhase(const CVehicle *vehicle);
void QuestPhysicsDirectorRecordProcessColModelsWork(int32 wheelLines,
	int32 sphereCandidates, int32 sphereTargets, int32 boxTargets,
	int32 triangleTargets);
void QuestPhysicsDirectorRecordContactManifold(const CVehicle *vehicle,
	bool producedContact);
void QuestPhysicsDirectorRecordSubsteps(const CVehicle *vehicle,
	int32 substeps);
void QuestPhysicsDirectorRecordRetry(const CVehicle *vehicle);
uint64 QuestPhysicsDirectorBeginRemoveAndAdd(const CPhysical *physical);
bool QuestPhysicsDirectorShouldRunRemoveAndAdd(CPhysical *physical);
void QuestPhysicsDirectorEndRemoveAndAdd(const CPhysical *physical,
	uint64 startedNs);

QuestPhysicsDirectorSnapshot QuestPhysicsDirectorGetSnapshot(void);

} // namespace androidgame
