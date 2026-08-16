#include "common.h"

#include <string.h>

#include "CutsceneMgr.h"
#include "ModelInfo.h"
#include "ModelSets.h"
#include "NodeName.h"
#include "PhysicsDirector.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Vehicle.h"
#include "VehicleModelInfo.h"
#include "VisibilityPlugins.h"
#include "VehicleVisualDirector.h"

namespace androidgame {
namespace {

int32 gMode = QUEST_VEHICLE_VISUAL_STOCK;
QuestVehicleVisualBudgetSnapshot gSnapshot;
CVehicle *gPreparedVehicle;
RpAtomic *gPreparedVlo;
int32 gPreparedAtomicCount;

struct VloScan
{
	RpAtomic *atomic;
	int32 matches;
	int32 atomics;
};

RpAtomic *
FindChassisVloCB(RpAtomic *atomic, void *data)
{
	VloScan *scan = (VloScan*)data;
	if(RpAtomicGetFlags(atomic) & rpATOMICRENDER)
		scan->atomics++;
	const char *name = GetFrameNodeName(RpAtomicGetFrame(atomic));
	if(name != nil && strcmp(name, "chassis_vlo") == 0){
		scan->matches++;
		if(scan->atomic == nil)
			scan->atomic = atomic;
	}
	return atomic;
}

float
GetFarDistance(void)
{
	if(gMode == QUEST_VEHICLE_VISUAL_BALANCED)
		return 80.0f;
	if(gMode == QUEST_VEHICLE_VISUAL_PERFORMANCE)
		return 65.0f;
	if(gMode == QUEST_VEHICLE_VISUAL_AGGRESSIVE)
		return 45.0f;
	return 1000000.0f;
}

void
RecordHighSubmission(void)
{
	gSnapshot.highVehicleSubmissions++;
}

} // namespace

void
QuestVehicleVisualBudgetSetMode(int32 mode)
{
	gMode = Min(Max(mode, (int32)QUEST_VEHICLE_VISUAL_STOCK),
		(int32)QUEST_VEHICLE_VISUAL_MODE_COUNT-1);
	gSnapshot.mode = gMode;
}

int32
QuestVehicleVisualBudgetGetMode(void)
{
	return gMode;
}

const char *
QuestVehicleVisualBudgetGetModeName(void)
{
	static const char *const names[QUEST_VEHICLE_VISUAL_MODE_COUNT] = {
		"STOCK - NO FORCED VLO", "BALANCED 80M TEST",
		"PERFORMANCE 65M TEST",
		"AGGRESSIVE 45M"
	};
	return names[gMode];
}

bool
QuestVehicleVisualBudgetBeginVehicle(CVehicle *vehicle)
{
	gPreparedVehicle = nil;
	gPreparedVlo = nil;
	gPreparedAtomicCount = 0;

	if(vehicle == nil || vehicle->m_rwObject == nil ||
	   vehicle->m_vehType != VEHICLE_TYPE_CAR ||
	   !ModelSets::IsCategoryModernActive(ModelSets::MODEL_CATEGORY_VEHICLES))
		return false;

	// STOCK is deliberately the default and stays on the exact original body
	// submission path. The counter is useful when comparing an opt-in capture.
	if(gMode == QUEST_VEHICLE_VISUAL_STOCK){
		RecordHighSubmission();
		return false;
	}

	const float farDistance = GetFarDistance();
	const float distanceSq = CVisibilityPlugins::GetDistanceSquaredFromCamera(
		RpClumpGetFrame(vehicle->GetClump()));
	if(FindPlayerPed() == nil || CCutsceneMgr::IsRunning() ||
	   CVehicle::bWheelsOnlyCheat ||
	   !QuestPhysicsDirectorIsSafeAmbientCruise(vehicle) ||
	   distanceSq < farDistance*farDistance){
		RecordHighSubmission();
		return false;
	}

	VloScan scan = {};
	RpClumpForAllAtomics(vehicle->GetClump(), FindChassisVloCB, &scan);
	// Do not guess from model names or fall back to another `_vlo` component.
	// Ambiguous, hidden or incomplete replacements remain fully stock.
	if(scan.matches != 1 || scan.atomic == nil ||
	   (RpAtomicGetFlags(scan.atomic) & rpATOMICRENDER) == 0){
		RecordHighSubmission();
		return false;
	}

	gPreparedVehicle = vehicle;
	gPreparedVlo = scan.atomic;
	gPreparedAtomicCount = scan.atomics;
	return true;
}

bool
QuestVehicleVisualBudgetRenderPreparedVehicle(CVehicle *vehicle)
{
	if(vehicle == nil || vehicle != gPreparedVehicle ||
	   gPreparedVlo == nil){
		RecordHighSubmission();
		return false;
	}

	CVehicleModelInfo *modelInfo = (CVehicleModelInfo*)
		CModelInfo::GetModelInfo(vehicle->GetModelIndex());
	modelInfo->SetVehicleColour(vehicle->m_currentColour1,
		vehicle->m_currentColour2);

	vehicle->bImBeingRendered = true;
	const int32 alpha = CVisibilityPlugins::GetClumpAlpha(
		vehicle->GetClump());
	if(alpha == 255)
		CVisibilityPlugins::RenderAtomicDefault(gPreparedVlo);
	else
		CVisibilityPlugins::RenderAlphaAtomic(gPreparedVlo, alpha);
	vehicle->bImBeingRendered = false;

	gSnapshot.vloVehicleSubmissions++;
	if(gPreparedAtomicCount > 1)
		gSnapshot.atomicsSkipped += gPreparedAtomicCount-1;
	return true;
}

void
QuestVehicleVisualBudgetRecordOccupantSkipped(void)
{
	gSnapshot.occupantsSkipped++;
}

void
QuestVehicleVisualBudgetEndVehicle(void)
{
	gPreparedVehicle = nil;
	gPreparedVlo = nil;
	gPreparedAtomicCount = 0;
}

QuestVehicleVisualBudgetSnapshot
QuestVehicleVisualBudgetGetSnapshot(void)
{
	gSnapshot.mode = gMode;
	return gSnapshot;
}

} // namespace androidgame
