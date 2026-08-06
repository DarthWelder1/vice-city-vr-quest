#pragma once

#ifdef GTA_VR_OCULUS

class CMatrix;
class CVector;
class CControllerState;
class CEntity;
class CVehicle;

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
bool IsTrackedWeaponHeld(int hand);
int GetHeldWeaponSlot(int hand);
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
bool IsImmersiveDrivingActive();
bool IsImmersiveCarDrivingActive();
bool IsImmersiveBikeDrivingActive();
bool IsVrCarDrivingActive();
bool IsVrBikeDrivingActive();
bool IsVrRadioControlActive();
bool ConsumeVrRadioChange();
bool GetImmersiveCarSteering(CVehicle *car, float *steering);
bool GetImmersiveBikeSteering(CVehicle *bike, float *steering);
bool GetImmersiveBikeThrottle(CVehicle *bike, float *throttle);
bool GetImmersiveBikeLean(CVehicle *bike, float *lean);
bool GetImmersiveBikeHandleMatrix(int hand, CMatrix *matrix);
bool IsImmersiveBikeHandleGrabbed(int hand);
bool ShouldRenderImmersiveBikeHandleMarker(int hand);
bool GetImmersiveSteeringHandleMatrix(int hand, CMatrix *matrix);
bool IsImmersiveSteeringHandleGrabbed(int hand);
bool ShouldRenderImmersiveSteeringHandleMarker(int hand);
bool IsImmersiveBikeSidearm(int weaponType);
bool IsImmersiveVehicleSidearm(int weaponType);
bool ConsumePhysicalMeleeStrike(int hand, int *slot, int *weaponType,
	CVector *sweepStart, CVector *sweepEnd, float *speed = nil,
	CVector *rootStart = nil, CVector *rootEnd = nil);
void ResolvePhysicalMeleeStrike(int hand, bool contact);

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
