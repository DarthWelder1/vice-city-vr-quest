#pragma once

#if defined(GTA_VR_OCULUS) || defined(GTA_VR_WEAPONS)

class CMatrix;
class CVector;
class CControllerState;
class CEntity;
class CVehicle;

// Single source of truth for the build name shown to players: the flat main
// menu, the VR About page and the APK version must never disagree again.
#define MIAMIVR_VERSION_TEXT "0.5.2 (0.5.3 RC1)"

namespace OculusVR
{
enum ePerfPhase
{
	PERF_PHASE_GAME,
	PERF_PHASE_AUDIO,
	PERF_PHASE_STREAMING,
	PERF_PHASE_WORLD_LIST,
	PERF_PHASE_PRE_RENDER,
	PERF_PHASE_SCENE_SETUP,
	PERF_PHASE_DESKTOP_RENDER,
	PERF_PHASE_UI,
	PERF_PHASE_CINEMA_SUBMIT,
	PERF_PHASE_LEFT_EYE,
	PERF_PHASE_RIGHT_EYE,
	PERF_PHASE_SUBMIT,
	PERF_PHASE_DESKTOP_PRESENT,
	PERF_PHASE_COUNT
};

enum ePerfVisibleType
{
	PERF_VISIBLE_BUILDING,
	PERF_VISIBLE_OBJECT,
	PERF_VISIBLE_PED,
	PERF_VISIBLE_VEHICLE
};

bool BeginStereoFrame(RwCamera *camera, const CMatrix &baseCamera);
bool GetEyeCamera(int eye, CMatrix *eyeCamera);
bool BeginEye(RwCamera *camera, int eye, CMatrix *eyeCamera, float *horizontalFov);
void GetTemporalJitterClip(float *x, float *y);
bool SubmitStereoFrame(RwCamera *camera);
bool SubmitCinemaFrame(RwCamera *camera, bool holdLastFrame = false);
void BeginNewGameCinemaHold();
bool IsNewGameCinemaHoldActive();
void EndNewGameCinemaHold();
void InitializeTemporalAAEarly();
void StartEarly();
bool SubmitStartupFrame(void *nativeWindow);
void CancelStereoFrame(RwCamera *camera);
void SetInactive();
void PrepareForGameShutdown();
int GetStereoScalePercent();
bool IsStereoReversed();
bool IsFirstPersonEnabled();
bool CanSkipDesktopGameplayRender();
bool UseFullStereoSinglePass();
bool AreTrackedHandsEnabled();
bool AreWeaponHolsterHighlightsEnabled();
bool ShouldUseTrackedHands();
bool IsTrackedHandReady(int hand);
bool IsTrackedWeaponLaserEnabled();
bool IsTrackedScopeActive();
bool IsTrackedScopeActiveForHand(int hand);
int GetTrackedScopeWeaponType();
bool ShouldUseTrackedWeapon(int hand);
bool IsTrackedWeaponTriggerPressed(int hand);
bool IsTrackedWeaponTriggerJustPressed(int hand);
bool IsTrackedWeaponTriggerJustReleased(int hand);
bool IsTrackedWeaponFireTriggerPressed(int hand, int weaponType);
bool IsTrackedWeaponFireTriggerJustPressed(int hand, int weaponType);
bool IsTrackedDetonatorActive(int hand);
bool IsTrackedDetonatorTriggerJustPressed(int hand);
bool IsTrackedRemoteGrenadeFireActive();
bool ShouldKeepTrackedRemoteCharges(CEntity *source);
void NotifyTrackedRemoteGrenadeThrown(int hand);
void NotifyTrackedDetonatorActivated(int hand);
bool IsManualReloadWeaponType(int weaponType);
void SetManualReloadWeaponState(int weaponHand, int slot, int weaponType, bool available);
bool ShouldUseManualReload();
bool ConsumeManualReloadRequest(int weaponHand, int slot, int weaponType);
bool GetManualReloadMagazineMatrix(int weaponHand, CMatrix *matrix,
	int *weaponType = nil, bool *held = nil);
void SetWeaponHolsterMask(uint32 mask);
bool ConsumeWeaponHolsterSelection(int hand, int *slot);
bool GetWeaponHolsterMatrix(int slot, CMatrix *matrix);
bool IsPhysicalGunType(int weaponType);
bool IsPhysicalMeleeType(int weaponType);
bool IsPhysicalThrowableType(int weaponType);
bool IsPhysicalWeaponType(int weaponType);
bool IsPhysicalWeaponInteractionActive();
// True while the world is rendered around the player's own head on foot.
bool IsQuestFirstPersonOnFoot();
bool IsTrackedWeaponHeld(int hand);
int GetHeldWeaponSlot(int hand);
bool IsRunWithoutLimitsEnabled();
void SetRunWithoutLimitsEnabled(bool enabled);
void SetTrackedWeaponRenderMatrix(int hand, int slot, int weaponType,
	const CMatrix *matrix,
	const CMatrix *contactMatrix = nil);
int GetDroppedWeaponSlot(int hand);
bool GetDroppedWeaponMatrix(int slot, CMatrix *matrix);
void GetTrackedWeaponOffset(float *offsetX, float *offsetY, float *offsetZ);
void GetTrackedWeaponRotation(float *rotationX, float *rotationY, float *rotationZ);
void GetTrackedWeaponOffsetForType(int hand, int weaponType, float *offsetX, float *offsetY, float *offsetZ);
void GetTrackedWeaponRotationForType(int hand, int weaponType, float *rotationX, float *rotationY, float *rotationZ);
bool ApplyTrackedWeaponTwoHandTransform(int primaryHand, int weaponType,
	CMatrix *matrix);
bool GetTrackedWeaponSupportAnchor(int primaryHand, int weaponType,
	CVector *position, bool *engaged = nil);
bool GetTrackedWeaponAim(int hand, int weaponType, CVector *source, CVector *direction);
bool GetTrackedThrowableLaunch(int hand, int weaponType, CVector *source,
	CVector *velocity);
void SetTrackedThrowablePreviewActive(int hand, bool active);
bool IsTrackedThrowablePreviewActive(int hand);
void BeginTrackedWeaponFire(int hand, int weaponType, const CVector &source,
	const CVector &direction);
void EndTrackedWeaponFire();
bool GetActiveTrackedWeaponAim(CVector *source, CVector *direction);
bool GetActiveTrackedThrowableLaunch(CVector *source, CVector *velocity);
void ReleaseTrackedWeaponAfterUse(int hand, int slot);
bool GetTrackedHandMatrix(int hand, CMatrix *handMatrix, float *grip = nil, float *trigger = nil);
bool GetTrackedHandAimRay(int hand, CVector *origin, CVector *direction);
#ifdef GTA_VR_WEAPONS
bool GetTrackedVisualHandMatrix(int hand, CMatrix *handMatrix,
	float *grip = nil, float *trigger = nil);
bool GetTrackedVisualHandAimRay(int hand, CVector *origin,
	CVector *direction);
#endif
bool IsImmersiveDrivingActive();
bool IsQuestBikeManualThrottle();
// Fraction of the physical lean the bike is drawn with, 1.0 outside VR riding.
float QuestBikeVisualLeanScale(CVehicle *vehicle);
int GetQuestBikeVisualLeanPercent();
bool IsQuestBikeViewFollowingTilt();
void ToggleQuestBikeViewFollowsTilt();
bool CanQuestBikeRiderBeThrown();
void ToggleQuestBikeRiderCanBeThrown();
void AdjustQuestBikeVisualLeanPercent(int direction);
void ToggleQuestBikeManualThrottle();
bool IsQuestCarDrivingDefault();
bool IsQuestBikeDrivingDefault();
// Neutral control centre of the vehicle being driven, for the dashboard HUD.
bool GetQuestVehicleHudAnchor(CMatrix *matrix);
bool IsImmersiveCarDrivingActive();
bool IsImmersiveBikeDrivingActive();
bool IsVrCarDrivingActive();
bool IsVrBikeDrivingActive();
bool IsVrRadioControlActive();
bool ConsumeVrRadioChange();
bool GetImmersiveCarSteering(CVehicle *car, float *steering);
bool GetImmersiveBikeSteering(CVehicle *bike, float *steering);
bool GetImmersiveBikeThrottle(CVehicle *bike, float *throttle);
bool GetQuestBikeVisualSteerAngle(CVehicle *bike, float *angle);
bool GetImmersiveBikeLean(CVehicle *bike, float *lean);
bool GetImmersiveBikeHandleMatrix(int hand, CMatrix *matrix);
bool IsImmersiveBikeHandleGrabbed(int hand);
bool ShouldRenderImmersiveBikeHandleMarker(int hand);
bool GetImmersiveSteeringHandleMatrix(int hand, CMatrix *matrix);
bool IsImmersiveSteeringHandleGrabbed(int hand);
bool ShouldRenderImmersiveSteeringHandleMarker(int hand);
#if defined(GTA_VR_WEAPONS) && defined(__ANDROID__)
void UpdateImmersiveCarModelSteeringWheel(CVehicle *vehicle);
#endif
bool IsImmersiveBikeSidearm(int weaponType);
bool IsImmersiveVehicleSidearm(int weaponType);
bool ConsumePhysicalMeleeStrike(int hand, int *slot, int *weaponType,
	CVector *sweepStart, CVector *sweepEnd, float *speed = nil,
	CVector *rootStart = nil, CVector *rootEnd = nil);
void ResolvePhysicalMeleeStrike(int hand, bool contact);

#ifdef GTA_VR_WEAPONS
// Small runtime configuration bridge used by the native Quest in-headset
// menu. Values are persisted in the same vr_settings.ini schema as desktop.
enum eQuestVehicleCalibrationItem
{
	QUEST_VEHICLE_CAL_HAND = 0,
	QUEST_VEHICLE_CAL_OFFSET_X,
	QUEST_VEHICLE_CAL_OFFSET_Y,
	QUEST_VEHICLE_CAL_OFFSET_Z,
	QUEST_VEHICLE_CAL_ROT_X,
	QUEST_VEHICLE_CAL_ROT_Y,
	QUEST_VEHICLE_CAL_ROT_Z,
	QUEST_VEHICLE_CAL_GLOBAL_CENTER_X,
	QUEST_VEHICLE_CAL_GLOBAL_CENTER_Y,
	QUEST_VEHICLE_CAL_GLOBAL_CENTER_Z,
	QUEST_VEHICLE_CAL_GLOBAL_RADIUS,
	QUEST_VEHICLE_CAL_MODEL_CENTER_X,
	QUEST_VEHICLE_CAL_MODEL_CENTER_Y,
	QUEST_VEHICLE_CAL_MODEL_CENTER_Z,
	QUEST_VEHICLE_CAL_MODEL_RADIUS,
	QUEST_VEHICLE_CAL_GLOBAL_PITCH,
	QUEST_VEHICLE_CAL_GLOBAL_YAW,
	QUEST_VEHICLE_CAL_GLOBAL_ROLL,
	QUEST_VEHICLE_CAL_MODEL_PITCH,
	QUEST_VEHICLE_CAL_MODEL_YAW,
	QUEST_VEHICLE_CAL_MODEL_ROLL,
	QUEST_VEHICLE_CAL_WHEELIE_HEIGHT,
	QUEST_VEHICLE_CAL_STAND_HEIGHT,
	QUEST_VEHICLE_CAL_ITEM_COUNT
};

// Called after the Android layer has supplied the current tracked poses.
// The returned bit mask identifies hands captured by a physical vehicle grip.
uint32 UpdateQuestDrivingInput(CControllerState *state, bool blocked);
void ResetQuestDrivingInteraction();
// Reprojects the already sampled OpenXR poses after the game has advanced
// physics and refreshed its first-person anchor. This updates render matrices
// only; input edges, hand velocity and interaction state remain single-update.
void RefreshQuestTrackedHandWorldPosesForRender();
bool GetQuestRawTrackedHandMatrix(int hand, CMatrix *matrix,
	float *grip = nil, float *trigger = nil);
bool GetQuestRawTrackedHandAimMatrix(int hand, CMatrix *matrix);
bool IsQuestDrivingHandUnavailable(int hand);
void RestrictQuestVehicleWeaponsToSidearms();

const char *GetQuestCarDrivingTypeName();
const char *GetQuestBikeDrivingTypeName();
void CycleQuestCarDrivingType(int direction);
void CycleQuestBikeDrivingType(int direction);
bool HasQuestVehicleSeatCalibrationTarget();
const char *GetQuestVehicleCategoryName();
int GetQuestVehicleGlobalSeatHeightCm();
int GetQuestVehicleGlobalSeatDistanceCm();
int GetQuestVehicleModelSeatHeightCm();
int GetQuestVehicleModelSeatDistanceCm();
void AdjustQuestVehicleGlobalSeatHeightCm(int direction);
void AdjustQuestVehicleGlobalSeatDistanceCm(int direction);
void AdjustQuestVehicleModelSeatHeightCm(int direction);
void AdjustQuestVehicleModelSeatDistanceCm(int direction);
bool HasQuestDefaultVehicleViewOffsetTarget();
const char *GetQuestDefaultVehicleViewOffsetName();
int GetQuestDefaultVehicleSeatHeightCm();
int GetQuestDefaultVehicleSeatDistanceCm();
void AdjustQuestDefaultVehicleSeatHeightCm(int direction);
void AdjustQuestDefaultVehicleSeatDistanceCm(int direction);
int GetQuestMotionSteeringHand();
void ToggleQuestMotionSteeringHand();
bool IsQuestImmersiveCarWheelVisible();
bool ShouldRenderQuestImmersiveCarWheel();
void ToggleQuestImmersiveCarWheelVisible();
const char *GetQuestVehicleModelWheelVisibilityName();
void ToggleQuestVehicleModelWheelVisibility();
bool AreQuestVehicleHandleHighlightsEnabled();
// How far a hand gripping the virtual wheel is pulled off the wheel plane
// towards the driver, millimetres; negative pushes it away. Only the
// immersive car wheel uses it; nothing else places a hand on a rim.
int GetQuestWheelHandPullBackMm();
void AdjustQuestWheelHandPullBackMm(int direction);
// Applied to the rendered hand after every other visual adjustment, so it
// cannot disturb the anchor the wheel maths and the grab test use.
void ApplyQuestWheelHandPullBack(int hand, CMatrix *matrix);
void ToggleQuestVehicleHandleHighlights();
bool IsQuestBikeHorizonLocked();
// Third-person vehicle view: the stock chase camera in stereo with default
// controls, for players who do not want to sit inside the vehicle.
bool IsQuestVehicleThirdPerson();
void ToggleQuestVehicleThirdPerson();
void ToggleQuestBikeHorizonLock();
const char *GetQuestActiveVehicleName();
bool IsQuestVehicleCalibrationAvailable();
bool IsQuestVehicleCalibrationBike();
int GetQuestVehicleCalibrationItemCount();
int GetQuestVehicleCalibrationItemForRow(int row);
const char *GetQuestVehicleCalibrationItemName(int item);
bool IsQuestVehicleCalibrationRotation(int item);
bool IsQuestVehicleCalibrationWholeCentimeters(int item);
int GetQuestVehicleCalibrationValue(int hand, int item);
void AdjustQuestVehicleCalibrationValue(int hand, int item, int direction);
void SetQuestVehicleCalibrationPreview(bool visible);
void ApplyQuestVehicleViewOffset(CMatrix *eyeCamera);
void ApplyQuestBikeHorizonLock(CMatrix *baseCamera);

int GetQuestWeaponSettingCount();
const char *GetQuestWeaponSettingName(int setting);
bool GetQuestWeaponSetting(int setting);
void ToggleQuestWeaponSetting(int setting);
int GetQuestCalibrationWeaponType(int hand);
int GetQuestCalibrationValue(int hand, int weaponType, int item);
void AdjustQuestCalibrationValue(int hand, int weaponType, int item,
	int direction);
int GetQuestHolsterPointCount();
const char *GetQuestHolsterPointName(int point);
int GetQuestHolsterPointSlot(int point);
void CycleQuestHolsterPointSlot(int point, int direction);
#endif

void PerfBeginFrame();
void PerfAbortFrame();
void PerfEndFrame(float playerX, float playerY, float playerZ);
void PerfBeginPhase(ePerfPhase phase);
void PerfEndPhase(ePerfPhase phase);
void PerfSetStreamingStats(int requestedModels, uint64 memoryUsed);
void PerfBeginStreamItem(int streamId, int streamType);
void PerfEndStreamItem();
void PerfCountVisibleEntity(ePerfVisibleType type);
void PerfCountEntityRender();

bool ApplyTouchInput(CControllerState *state);
void ToggleCheatMenu();
void Shutdown();
}

#endif
