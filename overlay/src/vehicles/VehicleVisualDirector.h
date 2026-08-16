#pragma once

class CVehicle;

namespace androidgame {

enum QuestVehicleVisualBudgetMode
{
	QUEST_VEHICLE_VISUAL_STOCK = 0,
	QUEST_VEHICLE_VISUAL_BALANCED,
	QUEST_VEHICLE_VISUAL_PERFORMANCE,
	QUEST_VEHICLE_VISUAL_AGGRESSIVE,
	QUEST_VEHICLE_VISUAL_MODE_COUNT
};

// Monotonic submission counters. QuestProfiler samples deltas once per
// second, while the Traffic page can also display the live totals.
struct QuestVehicleVisualBudgetSnapshot
{
	int32 mode;
	uint64 highVehicleSubmissions;
	uint64 vloVehicleSubmissions;
	uint64 atomicsSkipped;
	uint64 occupantsSkipped;
};

void QuestVehicleVisualBudgetSetMode(int32 mode);
int32 QuestVehicleVisualBudgetGetMode(void);
const char *QuestVehicleVisualBudgetGetModeName(void);

// Begin is called after SetupVehicleVariables and before occupants. It is
// fail-closed: true is returned only for a distant, safe ambient Modern car
// with exactly one renderable `chassis_vlo` atomic in its live clump.
bool QuestVehicleVisualBudgetBeginVehicle(CVehicle *vehicle);
bool QuestVehicleVisualBudgetRenderPreparedVehicle(CVehicle *vehicle);
void QuestVehicleVisualBudgetRecordOccupantSkipped(void);
void QuestVehicleVisualBudgetEndVehicle(void);

QuestVehicleVisualBudgetSnapshot QuestVehicleVisualBudgetGetSnapshot(void);

} // namespace androidgame
