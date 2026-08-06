#include "common.h"

#if defined(GTA_VR_WEAPONS) && defined(__ANDROID__)

#include "OculusVR.h"

#include "android.h"
#include "platform_android.h"
#include "Automobile.h"
#include "Bike.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "Game.h"
#include "main.h"
#include "ModelInfo.h"
#include "ModelIndices.h"
#include "Pad.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Vehicle.h"
#include "VehicleModelInfo.h"
#include "WeaponType.h"
#include "crossplatform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern CVehicle *FindPlayerVehicle(void);
extern const char *GetVrVehicleModelName(int model);
extern bool gVrFirstPersonActive;

namespace OculusVR {
namespace {

enum {
	VR_HAND_COUNT = 2,
	VEHICLE_CALIBRATION_VALUE_SCALE = 2,
	VR_BIKE_MODEL_COUNT = 6,
	VR_CAR_MODEL_COUNT = MI_LAST_VEHICLE-MI_FIRST_VEHICLE+1
};

enum eVrDrivingType
{
	VR_DRIVING_DEFAULT = 0,
	VR_DRIVING_IMMERSIVE,
	VR_DRIVING_MOTION,
	VR_DRIVING_TYPE_COUNT
};

const int gVrBikeModels[VR_BIKE_MODEL_COUNT] = {
	MI_ANGEL, MI_FREEWAY, MI_PCJ600,
	MI_FAGGIO, MI_PIZZABOY, MI_SANCHEZ
};

struct HandleCalibration
{
	int offsetX, offsetY, offsetZ;
	int rotationX, rotationY, rotationZ;
	bool valid;

	HandleCalibration() :
		offsetX(0), offsetY(0), offsetZ(0),
		rotationX(0), rotationY(0), rotationZ(0), valid(false)
	{}
};

struct VehicleViewCalibration
{
	int seatDistanceCm;
	bool valid;

	VehicleViewCalibration() : seatDistanceCm(0), valid(false) {}
};

struct BikeLeanCalibration
{
	int wheelieHeightCm;
	int standHeightCm;
	bool valid;

	BikeLeanCalibration() :
		wheelieHeightCm(20), standHeightCm(20), valid(false)
	{}
};

static bool gSettingsLoaded;
static int gDrivingType;
static int gMotionSteeringHand = 1;
static bool gHandleHighlightsEnabled = true;
static bool gBikeHorizonLocked = true;
static int gDrivingYOffsetCm = 15;
static bool gCalibrationPreview;

static HandleCalibration
	gBikeCalibration[VR_BIKE_MODEL_COUNT][VR_HAND_COUNT];
static HandleCalibration
	gCarCalibration[VR_CAR_MODEL_COUNT][VR_HAND_COUNT];
static VehicleViewCalibration
	gVehicleViewCalibration[VR_CAR_MODEL_COUNT];
static BikeLeanCalibration
	gBikeLeanCalibration[VR_BIKE_MODEL_COUNT];

static bool gBikeHandleGrabbed[VR_HAND_COUNT];
static bool gBikeHandleGripDown[VR_HAND_COUNT];
static float gBikeHandleDistance[VR_HAND_COUNT] = {
	1000.0f, 1000.0f
};
static float gImmersiveBikeSteering;
static float gImmersiveBikeThrottle;
static float gImmersiveBikeLean;
static bool gBikeThrottleReferenceValid;
static CVector gBikeThrottleReference;
static bool gBikeLeanReferenceValid[VR_HAND_COUNT];
static float gBikeLeanReferenceTrackingY[VR_HAND_COUNT];
static int gBikeLeanGestureState;

static bool gCarWheelGrabbed[VR_HAND_COUNT];
static bool gCarWheelGripDown[VR_HAND_COUNT];
static float gCarWheelDistance[VR_HAND_COUNT] = {
	1000.0f, 1000.0f
};
static float gCarWheelGrabReferenceAngle[VR_HAND_COUNT];
static float gCarWheelContinuousAngle[VR_HAND_COUNT];
static bool gCarWheelAngleValid[VR_HAND_COUNT];
static float gCarWheelTwoHandReferenceAngle;
static float gCarWheelTwoHandContinuousAngle;
static bool gCarWheelTwoHandAngleValid;
static float gImmersiveCarSteering;
static float gImmersiveCarPhysicalAngle;
static bool gImmersiveCarHornPressed;
static bool gCarHornContact[VR_HAND_COUNT];
static bool gCarHornArmed[VR_HAND_COUNT];
static float gCarHornPreviousDistance[VR_HAND_COUNT] = {
	1000.0f, 1000.0f
};

static bool gVrRadioButtonDown;
static bool gVrRadioChangeJustPressed;

static CVehicle *gMotionSteeringVehicle;
static float gMotionVehicleSteering;
static float gMotionVehiclePhysicalAngle;
static bool gMotionSteeringReferenceValid;
static float gMotionSteeringReferenceHeading;

static const char *const kSettingsPath = ".\\vr_settings.ini";

static void
SaveSetting(const char *name, int value)
{
	char text[32];
	sprintf(text, "%d", value);
	WritePrivateProfileStringA("VR", name, text, kSettingsPath);
}

static void
LoadDrivingSettings()
{
	if(gSettingsLoaded)
		return;
	char drivingType[16] = {};
	GetPrivateProfileStringA("VR", "DrivingType", "", drivingType,
		sizeof(drivingType), kSettingsPath);
	if(drivingType[0] != '\0')
		gDrivingType = atoi(drivingType);
	else
		gDrivingType =
			GetPrivateProfileIntA("VR", "ImmersiveDriving", 0,
				kSettingsPath) != 0 ?
				VR_DRIVING_IMMERSIVE : VR_DRIVING_DEFAULT;
	gDrivingType = clamp(gDrivingType, (int)VR_DRIVING_DEFAULT,
		(int)VR_DRIVING_TYPE_COUNT-1);
	gMotionSteeringHand = clamp((int)(int32)GetPrivateProfileIntA(
		"VR", "MotionSteeringHand", 1, kSettingsPath), 0,
		VR_HAND_COUNT-1);
	gHandleHighlightsEnabled = GetPrivateProfileIntA(
		"VR", "BikeHandleHighlights", 1, kSettingsPath) != 0;
	gBikeHorizonLocked = GetPrivateProfileIntA(
		"VR", "BikeLockHorizon", 1, kSettingsPath) != 0;
	gDrivingYOffsetCm = clamp((int)(int32)GetPrivateProfileIntA(
		"VR", "DrivingYOffsetCm", 15, kSettingsPath), -100, 150);
	gSettingsLoaded = true;
}

static int
GetBikeModelIndex(int model)
{
	for(int index = 0; index < VR_BIKE_MODEL_COUNT; index++)
		if(gVrBikeModels[index] == model)
			return index;
	return -1;
}

static int
GetCarModelIndex(int model)
{
	if(model < MI_FIRST_VEHICLE || model > MI_LAST_VEHICLE)
		return -1;
	CBaseModelInfo *base = CModelInfo::GetModelInfo(model);
	if(!base ||
	   ((CVehicleModelInfo*)base)->m_vehicleType != VEHICLE_TYPE_CAR)
		return -1;
	return model-MI_FIRST_VEHICLE;
}

static CBike *
GetActivePlayerBike()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle && vehicle->IsBike() ? (CBike*)vehicle : nil;
}

static CAutomobile *
GetActivePlayerCar()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle && vehicle->IsCar() &&
		!vehicle->IsRealHeli() && !vehicle->IsRealPlane() ?
			(CAutomobile*)vehicle : nil;
}

static bool
IsDrivingEnvironmentActive()
{
	LoadDrivingSettings();
	if(gDrivingType == VR_DRIVING_DEFAULT ||
	   !gVrFirstPersonActive ||
	   gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bGameNotLoaded ||
	   FrontEndMenuManager.m_bWantToRestart ||
	   FrontEndMenuManager.m_bWantToLoad ||
	   CGame::playingIntro ||
	   CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn)
		return false;
	CPlayerPed *player = FindPlayerPed();
	CVehicle *vehicle = FindPlayerVehicle();
	return player && !player->DyingOrDead() && vehicle &&
		vehicle->pDriver == player;
}

static bool
IsImmersiveEnvironmentActive()
{
	return gDrivingType == VR_DRIVING_IMMERSIVE &&
		IsDrivingEnvironmentActive();
}

static bool
IsMotionEnvironmentActive()
{
	return gDrivingType == VR_DRIVING_MOTION &&
		IsDrivingEnvironmentActive();
}

static bool
IsImmersiveBikeActive(CVehicle *expected = nil)
{
	if(!IsImmersiveEnvironmentActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	return bike && (!expected || expected == bike) &&
		GetBikeModelIndex(bike->GetModelIndex()) >= 0;
}

static bool
IsImmersiveCarActive(CVehicle *expected = nil)
{
	if(!IsImmersiveEnvironmentActive())
		return false;
	CAutomobile *car = GetActivePlayerCar();
	return car && (!expected || expected == car);
}

static bool
IsVrBikeActive(CVehicle *expected = nil)
{
	if(!IsDrivingEnvironmentActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	return bike && (!expected || expected == bike) &&
		GetBikeModelIndex(bike->GetModelIndex()) >= 0;
}

static bool
IsVrCarActive(CVehicle *expected = nil)
{
	if(!IsDrivingEnvironmentActive())
		return false;
	CAutomobile *car = GetActivePlayerCar();
	return car && (!expected || expected == car);
}

static bool
IsVrDrivingActive(CVehicle *expected = nil)
{
	return IsVrBikeActive(expected) || IsVrCarActive(expected);
}

static int
GetDefaultBikeHandleOffsetX(int bikeIndex, int hand)
{
	static const int halfWidthCm[VR_BIKE_MODEL_COUNT] = {
		36, 36, 32, 26, 26, 32
	};
	if(bikeIndex < 0 || bikeIndex >= VR_BIKE_MODEL_COUNT ||
	   hand < 0 || hand >= VR_HAND_COUNT)
		return 0;
	return halfWidthCm[bikeIndex]*
		VEHICLE_CALIBRATION_VALUE_SCALE*
		(hand == 0 ? -1 : 1);
}

struct BuiltInBikeHandleDefaults
{
	int model;
	int hand[VR_HAND_COUNT][6];
};

// Absolute v0.3.1 release baselines from the desktop calibration profile.
// Hand order is LEFT, RIGHT; fields are offset XYZ then rotation XYZ, all in
// the existing half-centimetre / half-degree Quest calibration scale.
static const BuiltInBikeHandleDefaults gBuiltInBikeHandleDefaults[] = {
	{ MI_ANGEL, {
		{ -61,-83,82, 137,0,0 },
		{ 61,-82,84, 138,0,0 } } },
	{ MI_FREEWAY, {
		{ -64,-82,85, 135,0,0 },
		{ 64,-82,85, 133,0,0 } } },
	{ MI_PCJ600, {
		{ -66,-27,81, -193,1,-15 },
		{ 66,-27,83, -192,0,0 } } },
	{ MI_FAGGIO, {
		{ -65,-35,84, 156,0,0 },
		{ 66,-33,84, 156,15,0 } } },
	{ MI_PIZZABOY, {
		{ -68,-34,83, -202,0,0 },
		{ 68,-34,83, 164,0,0 } } },
	{ MI_SANCHEZ, {
		{ -64,-23,46, -222,5,0 },
		{ 65,-24,46, 149,0,0 } } },
};

static bool
GetBuiltInBikeHandleCalibration(int model, int hand,
	HandleCalibration *calibration)
{
	if(!calibration || hand < 0 || hand >= VR_HAND_COUNT)
		return false;
	for(int index = 0;
	    index < (int)ARRAY_SIZE(gBuiltInBikeHandleDefaults); index++){
		const BuiltInBikeHandleDefaults &defaults =
			gBuiltInBikeHandleDefaults[index];
		if(defaults.model != model)
			continue;
		const int *value = defaults.hand[hand];
		calibration->offsetX = value[0];
		calibration->offsetY = value[1];
		calibration->offsetZ = value[2];
		calibration->rotationX = value[3];
		calibration->rotationY = value[4];
		calibration->rotationZ = value[5];
		calibration->valid = true;
		return true;
	}
	return false;
}

static HandleCalibration *
GetBikeCalibration(int model, int hand)
{
	const int bikeIndex = GetBikeModelIndex(model);
	if(bikeIndex < 0 || hand < 0 || hand >= VR_HAND_COUNT)
		return nil;
	HandleCalibration &calibration = gBikeCalibration[bikeIndex][hand];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, "BikeHandle_%d_%s", model,
		hand == 0 ? "Left" : "Right");
	HandleCalibration defaults;
	if(!GetBuiltInBikeHandleCalibration(model, hand, &defaults))
		defaults.offsetX = GetDefaultBikeHandleOffsetX(bikeIndex, hand);
	calibration.offsetX = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetX",
		defaults.offsetX, kSettingsPath),
		-300, 300);
	calibration.offsetY = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetY", defaults.offsetY, kSettingsPath), -300, 300);
	calibration.offsetZ = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetZ", defaults.offsetZ, kSettingsPath), -300, 300);
	calibration.rotationX = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationX", defaults.rotationX, kSettingsPath),
		-720, 720);
	calibration.rotationY = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationY", defaults.rotationY, kSettingsPath),
		-720, 720);
	calibration.rotationZ = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationZ", defaults.rotationZ, kSettingsPath),
		-720, 720);
	calibration.valid = true;
	return &calibration;
}

static HandleCalibration *
GetCarCalibration(int model, int hand)
{
	const int carIndex = GetCarModelIndex(model);
	if(carIndex < 0 || hand < 0 || hand >= VR_HAND_COUNT)
		return nil;
	HandleCalibration &calibration = gCarCalibration[carIndex][hand];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, "CarWheelV2_%d_%s", model,
		hand == 0 ? "Left" : "Right");
	const int defaultX = 18*VEHICLE_CALIBRATION_VALUE_SCALE*
		(hand == 0 ? -1 : 1);
	calibration.offsetX = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetX", defaultX, kSettingsPath), -300, 300);
	calibration.offsetY = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetY", 0, kSettingsPath), -300, 300);
	calibration.offsetZ = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetZ", 0, kSettingsPath), -300, 300);
	calibration.rotationX = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationX", 0, kSettingsPath), -720, 720);
	calibration.rotationY = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationY", 0, kSettingsPath), -720, 720);
	calibration.rotationZ = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationZ", 0, kSettingsPath), -720, 720);
	calibration.valid = true;
	return &calibration;
}

static BikeLeanCalibration *
GetBikeLeanCalibration(int model)
{
	const int bikeIndex = GetBikeModelIndex(model);
	if(bikeIndex < 0)
		return nil;
	BikeLeanCalibration &calibration = gBikeLeanCalibration[bikeIndex];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, "BikeControl_%d", model);
	calibration.wheelieHeightCm = clamp((int)(int32)
		GetPrivateProfileIntA(section, "WheelieHeightCm", 20,
			kSettingsPath), 5, 100);
	calibration.standHeightCm = clamp((int)(int32)
		GetPrivateProfileIntA(section, "StandHeightCm", 20,
			kSettingsPath), 5, 100);
	calibration.valid = true;
	return &calibration;
}

static VehicleViewCalibration *
GetViewCalibration(int model)
{
	if(model < MI_FIRST_VEHICLE || model > MI_LAST_VEHICLE)
		return nil;
	VehicleViewCalibration &calibration =
		gVehicleViewCalibration[model-MI_FIRST_VEHICLE];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, "VehicleView_%d", model);
	const int defaultSeatDistanceCm = model == MI_SANCHEZ ? -23 : 0;
	calibration.seatDistanceCm = clamp((int)(int32)
		GetPrivateProfileIntA(section, "SeatDistanceCm",
			defaultSeatDistanceCm, kSettingsPath), -100, 100);
	calibration.valid = true;
	return &calibration;
}

static void
SaveCalibrationValue(const char *prefix, int model, int hand,
	const char *name, int value)
{
	if(!prefix || !name || hand < 0 || hand >= VR_HAND_COUNT)
		return;
	char section[64], text[32];
	sprintf(section, "%s_%d_%s", prefix, model,
		hand == 0 ? "Left" : "Right");
	sprintf(text, "%d", value);
	WritePrivateProfileStringA(section, name, text, kSettingsPath);
}

static void
SaveBikeLeanValue(int model, const char *name, int value)
{
	char section[64], text[32];
	sprintf(section, "BikeControl_%d", model);
	sprintf(text, "%d", value);
	WritePrivateProfileStringA(section, name, text, kSettingsPath);
}

static CVector
RotateAroundAxis(const CVector &value, CVector axis, float angle)
{
	if(axis.MagnitudeSqr() < 0.000001f || angle == 0.0f)
		return value;
	axis.Normalise();
	const float cosine = Cos(angle);
	const float sine = Sin(angle);
	return value*cosine + CrossProduct(axis, value)*sine +
		axis*(DotProduct(axis, value)*(1.0f-cosine));
}

static void
ApplyHandleRotation(CMatrix *matrix,
	const HandleCalibration &calibration)
{
	const float pitch = DEGTORAD((float)calibration.rotationX/
		VEHICLE_CALIBRATION_VALUE_SCALE);
	const float yaw = DEGTORAD((float)calibration.rotationY/
		VEHICLE_CALIBRATION_VALUE_SCALE);
	const float roll = DEGTORAD((float)calibration.rotationZ/
		VEHICLE_CALIBRATION_VALUE_SCALE);
	if(pitch != 0.0f){
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			matrix->GetRight(), pitch);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			matrix->GetRight(), pitch);
	}
	if(yaw != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			matrix->GetUp(), yaw);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			matrix->GetUp(), yaw);
	}
	if(roll != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			matrix->GetForward(), roll);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			matrix->GetForward(), roll);
	}
	matrix->GetRight().Normalise();
	matrix->GetForward().Normalise();
	matrix->GetUp().Normalise();
}

static bool
BuildBikeHandleMatrix(int hand, CMatrix *matrix, bool applySteering)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT ||
	   !IsImmersiveBikeActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	HandleCalibration *calibration =
		GetBikeCalibration(bike->GetModelIndex(), hand);
	if(!calibration)
		return false;
	CVector bikeRight = bike->GetRight();
	CVector bikeForward = bike->GetForward();
	CVector bikeUp = bike->GetUp();
	bikeRight.Normalise();
	bikeForward.Normalise();
	bikeUp.Normalise();
	CVector center =
		bike->GetPosition()+bikeForward*0.30f+bikeUp*0.82f;
	if(bike->m_aBikeNodes[BIKE_HANDLEBARS]){
		RwMatrix *handle =
			RwFrameGetLTM(bike->m_aBikeNodes[BIKE_HANDLEBARS]);
		if(handle)
			center = CVector(handle->pos);
	}
	matrix->SetUnity();
	matrix->GetRight() = bikeUp*(hand == 0 ? -1.0f : 1.0f);
	matrix->GetForward() = bikeForward;
	matrix->GetUp() =
		CrossProduct(matrix->GetRight(), matrix->GetForward());
	matrix->GetUp().Normalise();
	matrix->GetPosition() = center+
		bikeRight*((float)calibration->offsetX/200.0f)+
		bikeForward*((float)calibration->offsetY/200.0f)+
		bikeUp*((float)calibration->offsetZ/200.0f);
	ApplyHandleRotation(matrix, *calibration);
	if(applySteering && bike->m_fWheelAngle != 0.0f){
		const float angle = bike->m_fWheelAngle;
		matrix->GetPosition() = center+RotateAroundAxis(
			matrix->GetPosition()-center, bikeUp, angle);
		matrix->GetRight() = RotateAroundAxis(
			matrix->GetRight(), bikeUp, angle);
		matrix->GetForward() = RotateAroundAxis(
			matrix->GetForward(), bikeUp, angle);
		matrix->GetUp() = RotateAroundAxis(
			matrix->GetUp(), bikeUp, angle);
	}
	return true;
}

static bool
BuildCarWheelCenter(CAutomobile *car, CVector *center, CVector *axis)
{
	if(!car || !center || !axis)
		return false;
	CVehicleModelInfo *model = (CVehicleModelInfo*)
		CModelInfo::GetModelInfo(car->GetModelIndex());
	if(!model)
		return false;
	CVector local = model->GetFrontSeatPosn();
	local.x = -local.x;
	local.y += 0.33f;
	local.z += 0.30f;
	*center = car->GetPosition()+
		Multiply3x3(car->GetMatrix(), local);
	*axis = car->GetForward();
	if(axis->MagnitudeSqr() < 0.0001f)
		return false;
	axis->Normalise();
	return true;
}

static bool
BuildCarWheelMatrix(int hand, CMatrix *matrix, bool applySteering)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT ||
	   !IsVrCarActive())
		return false;
	CAutomobile *car = GetActivePlayerCar();
	HandleCalibration *calibration =
		GetCarCalibration(car->GetModelIndex(), hand);
	if(!calibration)
		return false;
	CVector center, axis;
	if(!BuildCarWheelCenter(car, &center, &axis))
		return false;
	CVector carRight = car->GetRight();
	CVector carForward = car->GetForward();
	CVector carUp = car->GetUp();
	carRight.Normalise();
	carForward.Normalise();
	carUp.Normalise();
	matrix->SetUnity();
	matrix->GetRight() = carUp*(hand == 0 ? -1.0f : 1.0f);
	matrix->GetForward() = carForward;
	matrix->GetUp() =
		CrossProduct(matrix->GetRight(), matrix->GetForward());
	matrix->GetUp().Normalise();
	matrix->GetPosition() = center+
		carRight*((float)calibration->offsetX/200.0f)+
		carForward*((float)calibration->offsetY/200.0f)+
		carUp*((float)calibration->offsetZ/200.0f);
	ApplyHandleRotation(matrix, *calibration);
	const float physicalAngle = IsImmersiveCarActive() ?
		gImmersiveCarPhysicalAngle : gMotionVehiclePhysicalAngle;
	if(applySteering && physicalAngle != 0.0f){
		const float angle = -physicalAngle;
		matrix->GetPosition() = center+RotateAroundAxis(
			matrix->GetPosition()-center, axis, angle);
		matrix->GetRight() = RotateAroundAxis(
			matrix->GetRight(), axis, angle);
		matrix->GetForward() = RotateAroundAxis(
			matrix->GetForward(), axis, angle);
		matrix->GetUp() = RotateAroundAxis(
			matrix->GetUp(), axis, angle);
	}
	return true;
}

static float
WrapAngle(float angle)
{
	while(angle > PI) angle -= TWOPI;
	while(angle < -PI) angle += TWOPI;
	return angle;
}

static float
UnwrapAngle(float angle, float reference)
{
	while(angle-reference > PI) angle -= TWOPI;
	while(angle-reference < -PI) angle += TWOPI;
	return angle;
}

static float
PlanarAngle(const CVector &vector, const CVector &right,
	const CVector &up)
{
	return atan2f(DotProduct(vector, up), DotProduct(vector, right));
}

static void
ResetBikeInteraction()
{
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		gBikeHandleGrabbed[hand] = false;
		gBikeHandleGripDown[hand] = false;
		gBikeHandleDistance[hand] = 1000.0f;
		gBikeLeanReferenceValid[hand] = false;
		gBikeLeanReferenceTrackingY[hand] = 0.0f;
	}
	gImmersiveBikeSteering = 0.0f;
	gImmersiveBikeThrottle = 0.0f;
	gImmersiveBikeLean = 0.0f;
	gBikeLeanGestureState = 0;
	gBikeThrottleReferenceValid = false;
}

static void
ResetCarInteraction()
{
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		gCarWheelGrabbed[hand] = false;
		gCarWheelGripDown[hand] = false;
		gCarWheelDistance[hand] = 1000.0f;
		gCarWheelGrabReferenceAngle[hand] = 0.0f;
		gCarWheelContinuousAngle[hand] = 0.0f;
		gCarWheelAngleValid[hand] = false;
		gCarHornContact[hand] = false;
		gCarHornArmed[hand] = false;
		gCarHornPreviousDistance[hand] = 1000.0f;
	}
	gImmersiveCarSteering = 0.0f;
	gImmersiveCarPhysicalAngle = 0.0f;
	gCarWheelTwoHandReferenceAngle = 0.0f;
	gCarWheelTwoHandContinuousAngle = 0.0f;
	gCarWheelTwoHandAngleValid = false;
	gImmersiveCarHornPressed = false;
}

static void
ResetMotionInteraction()
{
	gMotionSteeringVehicle = nil;
	gMotionVehicleSteering = 0.0f;
	gMotionVehiclePhysicalAngle = 0.0f;
	gMotionSteeringReferenceValid = false;
	gMotionSteeringReferenceHeading = 0.0f;
}

static bool
GetRawHandPosition(int hand, CVector *position)
{
	CMatrix matrix;
	if(!position || !GetQuestRawTrackedHandMatrix(hand, &matrix))
		return false;
	*position = matrix.GetPosition();
	return true;
}

static void
UpdateCarHorn(CAutomobile *car, const CVector &center,
	uint32 blockedHands)
{
	if(!car){
		gImmersiveCarHornPressed = false;
		return;
	}
	bool pressed = false;
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			gCarWheelGrabbed[hand] ||
			IsQuestDrivingHandUnavailable(hand);
		CVector handPosition;
		if(unavailable ||
		   !GetRawHandPosition(hand, &handPosition)){
			gCarHornContact[hand] = false;
			gCarHornArmed[hand] = false;
			gCarHornPreviousDistance[hand] = 1000.0f;
			continue;
		}
		const float distance = (handPosition-center).Magnitude();
		const float previous = gCarHornPreviousDistance[hand];
		if(gCarHornContact[hand]){
			if(distance > 0.15f)
				gCarHornContact[hand] = false;
		}else{
			if(distance >= 0.12f && distance <= 0.26f &&
			   previous < 999.0f && distance < previous-0.002f)
				gCarHornArmed[hand] = true;
			if(distance > 0.26f)
				gCarHornArmed[hand] = false;
			if(gCarHornArmed[hand] && distance <= 0.105f &&
			   previous < 999.0f && distance < previous){
				gCarHornContact[hand] = true;
				gCarHornArmed[hand] = false;
			}
		}
		gCarHornPreviousDistance[hand] = distance;
		pressed = pressed || gCarHornContact[hand];
	}
	gImmersiveCarHornPressed = pressed;
}

static uint32
UpdateImmersiveCarInput(const float *grips, uint32 blockedHands)
{
	if(!grips || !IsImmersiveCarActive()){
		ResetCarInteraction();
		return 0;
	}
	CAutomobile *car = GetActivePlayerCar();
	if(!car){
		ResetCarInteraction();
		return 0;
	}
	RestrictQuestVehicleWeaponsToSidearms();
	CMatrix anchors[VR_HAND_COUNT];
	CVector positions[VR_HAND_COUNT];
	bool anchorValid[VR_HAND_COUNT] = {};
	bool poseValid[VR_HAND_COUNT] = {};
	bool justGrabbed[VR_HAND_COUNT] = {};
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		anchorValid[hand] =
			BuildCarWheelMatrix(hand, &anchors[hand], false);
		poseValid[hand] =
			GetRawHandPosition(hand, &positions[hand]);
		gCarWheelDistance[hand] =
			anchorValid[hand] && poseValid[hand] ?
				(positions[hand]-
				 anchors[hand].GetPosition()).Magnitude() :
				1000.0f;
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			!anchorValid[hand] || !poseValid[hand] ||
			IsQuestDrivingHandUnavailable(hand);
		if(gCarWheelGrabbed[hand] &&
		   (unavailable || grips[hand] <= 0.30f)){
			gCarWheelGrabbed[hand] = false;
			gCarWheelAngleValid[hand] = false;
		}
		if(!gCarWheelGrabbed[hand] && !unavailable &&
		   grips[hand] >= 0.65f && !gCarWheelGripDown[hand] &&
		   gCarWheelDistance[hand] <= 0.23f){
			gCarWheelGrabbed[hand] = true;
			justGrabbed[hand] = true;
		}
		if(grips[hand] <= 0.30f)
			gCarWheelGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gCarWheelGripDown[hand] = true;
	}

	const bool left = gCarWheelGrabbed[0] && anchorValid[0];
	const bool right = gCarWheelGrabbed[1] && anchorValid[1];
	CVector center, axis;
	if(!BuildCarWheelCenter(car, &center, &axis)){
		ResetCarInteraction();
		return 0;
	}
	if(anchorValid[0] && anchorValid[1])
		center = (anchors[0].GetPosition()+
			anchors[1].GetPosition())*0.5f;
	CVector carRight = car->GetRight();
	CVector carUp = car->GetUp();
	carRight.Normalise();
	carUp.Normalise();
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		if(!justGrabbed[hand])
			continue;
		const float angle = PlanarAngle(
			positions[hand]-center, carRight, carUp);
		gCarWheelGrabReferenceAngle[hand] =
			WrapAngle(angle-gImmersiveCarPhysicalAngle);
		gCarWheelContinuousAngle[hand] =
			gImmersiveCarPhysicalAngle;
		gCarWheelAngleValid[hand] = true;
	}

	float steeringAngle = 0.0f;
	if(left && right){
		const float chordAngle = PlanarAngle(
			positions[1]-positions[0], carRight, carUp);
		if(!gCarWheelTwoHandAngleValid ||
		   justGrabbed[0] || justGrabbed[1]){
			gCarWheelTwoHandReferenceAngle =
				WrapAngle(chordAngle-gImmersiveCarPhysicalAngle);
			gCarWheelTwoHandContinuousAngle =
				gImmersiveCarPhysicalAngle;
			gCarWheelTwoHandAngleValid = true;
		}
		steeringAngle = WrapAngle(
			chordAngle-gCarWheelTwoHandReferenceAngle);
		steeringAngle = UnwrapAngle(steeringAngle,
			gCarWheelTwoHandContinuousAngle);
		gCarWheelTwoHandContinuousAngle = steeringAngle;
		for(int hand = 0; hand < VR_HAND_COUNT; hand++){
			const float handAngle = PlanarAngle(
				positions[hand]-center, carRight, carUp);
			gCarWheelGrabReferenceAngle[hand] =
				WrapAngle(handAngle-steeringAngle);
			gCarWheelContinuousAngle[hand] = steeringAngle;
			gCarWheelAngleValid[hand] = true;
		}
	}else{
		gCarWheelTwoHandAngleValid = false;
		for(int hand = 0; hand < VR_HAND_COUNT; hand++){
			if(!gCarWheelGrabbed[hand])
				continue;
			float angle = WrapAngle(
				PlanarAngle(positions[hand]-center,
					carRight, carUp)-
				gCarWheelGrabReferenceAngle[hand]);
			if(gCarWheelAngleValid[hand])
				angle = UnwrapAngle(angle,
					gCarWheelContinuousAngle[hand]);
			gCarWheelContinuousAngle[hand] = angle;
			gCarWheelAngleValid[hand] = true;
			steeringAngle = angle;
			break;
		}
	}

	const float maxSteering = DEGTORAD(80.0f);
	gImmersiveCarPhysicalAngle =
		clamp(steeringAngle, -maxSteering, maxSteering);
	float steering =
		clamp(steeringAngle/maxSteering, -1.0f, 1.0f);
	const float deadZone = 0.03f;
	if(Abs(steering) <= deadZone)
		steering = 0.0f;
	else
		steering = (steering > 0.0f ? 1.0f : -1.0f)*
			(Abs(steering)-deadZone)/(1.0f-deadZone);
	gImmersiveCarSteering = steering;
	const uint32 captured =
		(left ? 1u : 0u) | (right ? 2u : 0u);
	UpdateCarHorn(car, center, blockedHands | captured);
	return captured;
}

static bool
GetMotionHeading(const androidgame::PadInput &input, float *heading)
{
	const int hand = clamp(gMotionSteeringHand, 0, VR_HAND_COUNT-1);
	const androidgame::PadInput::Pose &pose = input.aimPose[hand];
	if(!heading || !pose.valid)
		return false;
	const float x = pose.orientation[0];
	const float y = pose.orientation[1];
	const float z = pose.orientation[2];
	const float w = pose.orientation[3];
	const float forwardX = -2.0f*(x*z+w*y);
	const float forwardZ = -(1.0f-2.0f*(x*x+y*y));
	if(forwardX*forwardX+forwardZ*forwardZ < 0.01f)
		return false;
	*heading = atan2f(forwardX, -forwardZ);
	return true;
}

static void
UpdateMotionInput(const androidgame::PadInput &input, bool blocked)
{
	if(blocked || !IsMotionEnvironmentActive()){
		ResetMotionInteraction();
		return;
	}
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle ||
	   (!IsVrCarActive(vehicle) && !IsVrBikeActive(vehicle))){
		ResetMotionInteraction();
		return;
	}
	RestrictQuestVehicleWeaponsToSidearms();
	if(gMotionSteeringVehicle != vehicle){
		gMotionSteeringVehicle = vehicle;
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		gMotionSteeringReferenceValid = false;
		gMotionSteeringReferenceHeading = 0.0f;
	}
	float heading;
	if(!GetMotionHeading(input, &heading)){
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		return;
	}
	if(!gMotionSteeringReferenceValid){
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		if(input.rightTrigger < 0.15f)
			return;
		gMotionSteeringReferenceHeading = heading;
		gMotionSteeringReferenceValid = true;
	}
	const float maxAngle = DEGTORAD(90.0f);
	const float midAngle = DEGTORAD(30.0f);
	const float fineScale = 0.5f;
	float angle = clamp(-WrapAngle(
		heading-gMotionSteeringReferenceHeading),
		-maxAngle, maxAngle);
	if(Abs(angle) < DEGTORAD(3.0f))
		angle = 0.0f;
	gMotionVehiclePhysicalAngle = angle;
	const float absolute = Abs(angle);
	float steering;
	if(absolute <= midAngle)
		steering = angle/midAngle*fineScale;
	else{
		const float sign = angle >= 0.0f ? 1.0f : -1.0f;
		steering = sign*(fineScale+
			(absolute-midAngle)/(maxAngle-midAngle)*
			(1.0f-fineScale));
	}
	gMotionVehicleSteering = Abs(steering) < 0.01f ?
		0.0f : clamp(steering, -1.0f, 1.0f);
}

static bool
GetThrottleOrientation(CVector *orientation, const CVector &axis)
{
	CMatrix hand;
	if(!orientation ||
	   !GetQuestRawTrackedHandAimMatrix(1, &hand))
		return false;
	*orientation = hand.GetForward();
	*orientation -= axis*DotProduct(*orientation, axis);
	if(orientation->MagnitudeSqr() < 0.01f){
		*orientation = hand.GetUp();
		*orientation -= axis*DotProduct(*orientation, axis);
	}
	if(orientation->MagnitudeSqr() < 0.01f)
		return false;
	orientation->Normalise();
	return true;
}

static void
UpdateBikeThrottle(CBike *bike, bool rightGrabbed, float trigger)
{
	if(!bike || !rightGrabbed || trigger < 0.45f){
		gImmersiveBikeThrottle = 0.0f;
		gBikeThrottleReferenceValid = false;
		return;
	}
	CVector axis = bike->GetRight();
	CVector bikeUp = bike->GetUp();
	axis = RotateAroundAxis(axis, bikeUp, bike->m_fWheelAngle);
	axis.Normalise();
	CVector current;
	if(!GetThrottleOrientation(&current, axis)){
		gImmersiveBikeThrottle = 0.0f;
		gBikeThrottleReferenceValid = false;
		return;
	}
	if(!gBikeThrottleReferenceValid){
		gBikeThrottleReference = current;
		gBikeThrottleReferenceValid = true;
		gImmersiveBikeThrottle = 0.0f;
		return;
	}
	const float twist = atan2f(DotProduct(axis,
		CrossProduct(gBikeThrottleReference, current)),
		clamp(DotProduct(gBikeThrottleReference, current),
			-1.0f, 1.0f));
	const float throttleAngle =
		Max(twist-DEGTORAD(2.0f), 0.0f);
	gImmersiveBikeThrottle =
		clamp(throttleAngle/DEGTORAD(43.0f), 0.0f, 1.0f);
}

static void
UpdateBikeLean(CBike *bike, const androidgame::PadInput &input,
	bool left, bool right)
{
	if(!bike || (!left && !right)){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		for(int hand = 0; hand < VR_HAND_COUNT; hand++)
			gBikeLeanReferenceValid[hand] = false;
		return;
	}
	float heightTotal = 0.0f;
	int heightHands = 0;
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		const bool grabbed = hand == 0 ? left : right;
		if(!grabbed || !input.gripPose[hand].valid){
			gBikeLeanReferenceValid[hand] = false;
			continue;
		}
		const float currentY =
			input.gripPose[hand].position[1];
		if(!gBikeLeanReferenceValid[hand]){
			gBikeLeanReferenceTrackingY[hand] = currentY;
			gBikeLeanReferenceValid[hand] = true;
			continue;
		}
		heightTotal +=
			currentY-gBikeLeanReferenceTrackingY[hand];
		heightHands++;
	}
	if(heightHands == 0){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		return;
	}
	BikeLeanCalibration *calibration =
		GetBikeLeanCalibration(bike->GetModelIndex());
	if(!calibration){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		return;
	}
	const float height = heightTotal/(float)heightHands;
	const float wheelie =
		(float)calibration->wheelieHeightCm/100.0f;
	const float stand =
		(float)calibration->standHeightCm/100.0f;
	if(gBikeLeanGestureState == 0){
		if(height >= wheelie)
			gBikeLeanGestureState = -1;
		else if(height <= -stand)
			gBikeLeanGestureState = 1;
	}else if(gBikeLeanGestureState < 0){
		if(height <= Max(wheelie*0.50f, 0.025f))
			gBikeLeanGestureState = 0;
	}else if(height >= -Max(stand*0.50f, 0.025f))
		gBikeLeanGestureState = 0;
	gImmersiveBikeLean = (float)gBikeLeanGestureState;
}

static uint32
UpdateImmersiveBikeInput(const float *grips,
	const androidgame::PadInput &input, uint32 blockedHands)
{
	if(!grips || !IsImmersiveBikeActive()){
		ResetBikeInteraction();
		return 0;
	}
	CBike *bike = GetActivePlayerBike();
	if(!bike){
		ResetBikeInteraction();
		return 0;
	}
	RestrictQuestVehicleWeaponsToSidearms();
	CMatrix anchors[VR_HAND_COUNT];
	CVector positions[VR_HAND_COUNT];
	bool anchorValid[VR_HAND_COUNT] = {};
	bool poseValid[VR_HAND_COUNT] = {};
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		anchorValid[hand] =
			BuildBikeHandleMatrix(hand, &anchors[hand], false);
		poseValid[hand] =
			GetRawHandPosition(hand, &positions[hand]);
		gBikeHandleDistance[hand] =
			anchorValid[hand] && poseValid[hand] ?
				(positions[hand]-
				 anchors[hand].GetPosition()).Magnitude() :
				1000.0f;
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			!anchorValid[hand] || !poseValid[hand] ||
			IsQuestDrivingHandUnavailable(hand);
		if(gBikeHandleGrabbed[hand] &&
		   (unavailable || grips[hand] <= 0.30f)){
			gBikeHandleGrabbed[hand] = false;
			gBikeLeanReferenceValid[hand] = false;
		}
		if(!gBikeHandleGrabbed[hand] && !unavailable &&
		   grips[hand] >= 0.65f && !gBikeHandleGripDown[hand] &&
		   gBikeHandleDistance[hand] <= 0.17f){
			gBikeHandleGrabbed[hand] = true;
			gBikeLeanReferenceValid[hand] = false;
		}
		if(grips[hand] <= 0.30f)
			gBikeHandleGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gBikeHandleGripDown[hand] = true;
	}
	const bool left =
		gBikeHandleGrabbed[0] && anchorValid[0];
	const bool right =
		gBikeHandleGrabbed[1] && anchorValid[1];
	float steeringAngle = 0.0f;
	CVector bikeRight = bike->GetRight();
	CVector bikeForward = bike->GetForward();
	bikeRight.Normalise();
	bikeForward.Normalise();
	if(left && right){
		const CVector neutral =
			anchors[1].GetPosition()-anchors[0].GetPosition();
		const CVector actual = positions[1]-positions[0];
		steeringAngle = WrapAngle(
			PlanarAngle(actual, bikeRight, bikeForward)-
			PlanarAngle(neutral, bikeRight, bikeForward));
	}else if(left || right){
		const int hand = left ? 0 : 1;
		const CVector center =
			(anchors[0].GetPosition()+
			 anchors[1].GetPosition())*0.5f;
		const CVector neutral =
			anchors[hand].GetPosition()-center;
		const CVector actual = positions[hand]-center;
		steeringAngle = WrapAngle(
			PlanarAngle(actual, bikeRight, bikeForward)-
			PlanarAngle(neutral, bikeRight, bikeForward));
	}
	const float maxSteering = DEGTORAD(35.0f);
	float steering =
		clamp(steeringAngle/maxSteering, -1.0f, 1.0f);
	const float deadZone = 0.035f;
	if(Abs(steering) <= deadZone)
		steering = 0.0f;
	else
		steering = (steering > 0.0f ? 1.0f : -1.0f)*
			(Abs(steering)-deadZone)/(1.0f-deadZone);
	gImmersiveBikeSteering = steering;
	UpdateBikeLean(bike, input, left, right);
	UpdateBikeThrottle(bike, right, input.rightTrigger);
	return (left ? 1u : 0u) | (right ? 2u : 0u);
}

static void
MapHornToPad(CControllerState *state)
{
	if(!state || !gImmersiveCarHornPressed)
		return;
	switch(CPad::GetPad(0)->GetMode()){
	case 1:
		state->LeftShoulder1 =
			Max(state->LeftShoulder1, (int16)255);
		break;
	case 2:
		state->RightShoulder1 =
			Max(state->RightShoulder1, (int16)255);
		break;
	default:
		state->LeftShock =
			Max(state->LeftShock, (int16)255);
		break;
	}
}

static void
MapDefaultDriveBy(CControllerState *state,
	const androidgame::PadInput &input, bool blocked)
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(!state || blocked || !vehicle ||
	   gDrivingType != VR_DRIVING_DEFAULT)
		return;
	const bool left = input.b &&
		input.leftGrip >= 0.55f && input.rightGrip < 0.55f;
	const bool right = input.b &&
		input.rightGrip >= 0.55f && input.leftGrip < 0.55f;
	const bool forward = input.b &&
		input.leftGrip < 0.55f && input.rightGrip < 0.55f &&
		vehicle->IsBike();
	if(left){
		// The grip selects the drive-by side; it must not also reach the
		// current pad mode as handbrake, horn or exit-vehicle input.
		state->LeftShoulder1 = 0;
		state->LeftShoulder2 =
			Max(state->LeftShoulder2, (int16)255);
	}
	if(right){
		state->RightShoulder1 = 0;
		state->RightShoulder2 =
			Max(state->RightShoulder2, (int16)255);
	}
	if(left || right || forward){
		if(CPad::GetPad(0)->GetMode() == 3)
			state->RightShoulder1 =
				Max(state->RightShoulder1, (int16)255);
		else
			state->Circle =
				Max(state->Circle, (int16)255);
	}
}

} // namespace

uint32
UpdateQuestDrivingInput(CControllerState *state, bool blocked)
{
	if(!state)
		return 0;
	LoadDrivingSettings();
	const androidgame::PadInput &input = androidgame::GetPadInput();
	CVehicle *vehicle = FindPlayerVehicle();

	const bool radioPressed =
		!blocked && vehicle && input.x;
	gVrRadioChangeJustPressed =
		radioPressed && !gVrRadioButtonDown;
	gVrRadioButtonDown = radioPressed;
	if(vehicle && !blocked){
		// X is a dedicated radio edge in VR. Preserve L2 brake/reverse, but
		// remove CapturePad's face-X contribution to Square. CapturePad already
		// cleared every game input while the VR menu owns the controllers, so
		// never reconstruct L2 from the raw state in that case.
		state->Square = (int16)(clamp(
			input.leftTrigger, 0.0f, 1.0f)*255.0f);
	}
	MapDefaultDriveBy(state, input, blocked);

	const float grips[VR_HAND_COUNT] = {
		input.leftGrip, input.rightGrip
	};
	const uint32 blockedHands = blocked ? 3u : 0u;
	uint32 captured = UpdateImmersiveBikeInput(
		grips, input, blockedHands);
	captured |= UpdateImmersiveCarInput(grips, blockedHands);
	UpdateMotionInput(input, blocked);

	if(IsVrCarActive()){
		// Car grips belong to the virtual wheel/horn system, even while the
		// hands are approaching it. Never leak them into classic radio actions.
		state->LeftShoulder1 = 0;
		state->RightShoulder1 = 0;
	}else{
		if(captured & 1u)
			state->LeftShoulder1 = 0;
		if(captured & 2u)
			state->RightShoulder1 = 0;
	}
	MapHornToPad(state);
	return captured;
}

void
ResetQuestDrivingInteraction()
{
	ResetBikeInteraction();
	ResetCarInteraction();
	ResetMotionInteraction();
	gVrRadioButtonDown = false;
	gVrRadioChangeJustPressed = false;
}

bool IsImmersiveDrivingActive() { return IsVrDrivingActive(); }
bool IsImmersiveCarDrivingActive() { return IsImmersiveCarActive(); }
bool IsImmersiveBikeDrivingActive() { return IsImmersiveBikeActive(); }
bool IsVrCarDrivingActive() { return IsVrCarActive(); }
bool IsVrBikeDrivingActive() { return IsVrBikeActive(); }

bool
IsVrRadioControlActive()
{
	// VrHands controls only whether the hand meshes are drawn. Controller
	// tracking and vehicle input stay active when a player hides those meshes.
	return gVrFirstPersonActive && FindPlayerVehicle() != nil;
}

bool
ConsumeVrRadioChange()
{
	if(!IsVrRadioControlActive()){
		gVrRadioChangeJustPressed = false;
		return false;
	}
	const bool pressed = gVrRadioChangeJustPressed;
	gVrRadioChangeJustPressed = false;
	return pressed;
}

bool
GetImmersiveCarSteering(CVehicle *car, float *steering)
{
	if(!steering)
		return false;
	if(IsImmersiveCarActive(car)){
		*steering = gImmersiveCarSteering;
		return true;
	}
	if(IsMotionEnvironmentActive() && IsVrCarActive(car)){
		*steering = gMotionVehicleSteering;
		return true;
	}
	return false;
}

bool
GetImmersiveBikeSteering(CVehicle *bike, float *steering)
{
	if(!steering)
		return false;
	if(IsImmersiveBikeActive(bike)){
		*steering = gImmersiveBikeSteering;
		return true;
	}
	if(IsMotionEnvironmentActive() && IsVrBikeActive(bike)){
		*steering = gMotionVehicleSteering;
		return true;
	}
	return false;
}

bool
GetImmersiveBikeThrottle(CVehicle *bike, float *throttle)
{
	if(!throttle || !IsImmersiveBikeActive(bike))
		return false;
	*throttle = gImmersiveBikeThrottle;
	return true;
}

bool
GetImmersiveBikeLean(CVehicle *bike, float *lean)
{
	if(!lean || !IsImmersiveBikeActive(bike))
		return false;
	*lean = gImmersiveBikeLean;
	return true;
}

bool
GetImmersiveBikeHandleMatrix(int hand, CMatrix *matrix)
{
	return BuildBikeHandleMatrix(hand, matrix, true);
}

bool
IsImmersiveBikeHandleGrabbed(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		IsImmersiveBikeActive() && gBikeHandleGrabbed[hand];
}

bool
ShouldRenderImmersiveBikeHandleMarker(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		IsImmersiveBikeActive() &&
		(gCalibrationPreview ||
		 (gHandleHighlightsEnabled &&
		  (gBikeHandleGrabbed[hand] ||
		   gBikeHandleDistance[hand] <= 0.23f)));
}

bool
GetImmersiveSteeringHandleMatrix(int hand, CMatrix *matrix)
{
	if(IsVrCarActive())
		return BuildCarWheelMatrix(hand, matrix, true);
	return BuildBikeHandleMatrix(hand, matrix, true);
}

bool
IsImmersiveSteeringHandleGrabbed(int hand)
{
	if(hand < 0 || hand >= VR_HAND_COUNT)
		return false;
	if(IsImmersiveCarActive())
		return gCarWheelGrabbed[hand];
	return IsImmersiveBikeHandleGrabbed(hand);
}

bool
ShouldRenderImmersiveSteeringHandleMarker(int hand)
{
	if(hand < 0 || hand >= VR_HAND_COUNT)
		return false;
	if(IsImmersiveCarActive())
		return gCalibrationPreview || gHandleHighlightsEnabled;
	return ShouldRenderImmersiveBikeHandleMarker(hand);
}

bool
IsImmersiveBikeSidearm(int weaponType)
{
	return IsImmersiveVehicleSidearm(weaponType);
}

bool
IsImmersiveVehicleSidearm(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_PYTHON:
	case WEAPONTYPE_TEC9:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SILENCED_INGRAM:
		return true;
	default:
		return false;
	}
}

int
GetQuestDrivingType()
{
	LoadDrivingSettings();
	return gDrivingType;
}

const char *
GetQuestDrivingTypeName()
{
	LoadDrivingSettings();
	switch(gDrivingType){
	case VR_DRIVING_IMMERSIVE: return "IMMERSIVE";
	case VR_DRIVING_MOTION: return "MOTION";
	default: return "DEFAULT";
	}
}

void
CycleQuestDrivingType(int direction)
{
	LoadDrivingSettings();
	gDrivingType =
		(gDrivingType+VR_DRIVING_TYPE_COUNT+
		 (direction < 0 ? -1 : 1)) % VR_DRIVING_TYPE_COUNT;
	SaveSetting("DrivingType", gDrivingType);
	ResetQuestDrivingInteraction();
}

int
GetQuestDrivingYOffsetCm()
{
	LoadDrivingSettings();
	return gDrivingYOffsetCm;
}

void
AdjustQuestDrivingYOffsetCm(int direction)
{
	LoadDrivingSettings();
	gDrivingYOffsetCm = clamp(
		gDrivingYOffsetCm+(direction < 0 ? -5 : 5), -100, 150);
	SaveSetting("DrivingYOffsetCm", gDrivingYOffsetCm);
}

bool
HasQuestVehicleSeatCalibrationTarget()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle && GetViewCalibration(
		vehicle->GetModelIndex()) != nil;
}

int
GetQuestVehicleSeatDistanceCm()
{
	CVehicle *vehicle = FindPlayerVehicle();
	VehicleViewCalibration *calibration = vehicle ?
		GetViewCalibration(vehicle->GetModelIndex()) : nil;
	return calibration ? calibration->seatDistanceCm : 0;
}

void
AdjustQuestVehicleSeatDistanceCm(int direction)
{
	CVehicle *vehicle = FindPlayerVehicle();
	VehicleViewCalibration *calibration = vehicle ?
		GetViewCalibration(vehicle->GetModelIndex()) : nil;
	if(!calibration)
		return;
	calibration->seatDistanceCm = clamp(
		calibration->seatDistanceCm+
		(direction < 0 ? -1 : 1), -100, 100);
	char section[64], value[32];
	sprintf(section, "VehicleView_%d", vehicle->GetModelIndex());
	sprintf(value, "%d", calibration->seatDistanceCm);
	WritePrivateProfileStringA(section, "SeatDistanceCm",
		value, kSettingsPath);
}

int
GetQuestMotionSteeringHand()
{
	LoadDrivingSettings();
	return gMotionSteeringHand;
}

void
ToggleQuestMotionSteeringHand()
{
	LoadDrivingSettings();
	gMotionSteeringHand = 1-gMotionSteeringHand;
	SaveSetting("MotionSteeringHand", gMotionSteeringHand);
	ResetMotionInteraction();
}

bool
AreQuestVehicleHandleHighlightsEnabled()
{
	LoadDrivingSettings();
	return gHandleHighlightsEnabled;
}

void
ToggleQuestVehicleHandleHighlights()
{
	LoadDrivingSettings();
	gHandleHighlightsEnabled = !gHandleHighlightsEnabled;
	SaveSetting("BikeHandleHighlights",
		gHandleHighlightsEnabled);
}

bool
IsQuestBikeHorizonLocked()
{
	LoadDrivingSettings();
	return gBikeHorizonLocked;
}

void
ToggleQuestBikeHorizonLock()
{
	LoadDrivingSettings();
	gBikeHorizonLocked = !gBikeHorizonLocked;
	SaveSetting("BikeLockHorizon", gBikeHorizonLocked);
}

const char *
GetQuestActiveVehicleName()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle ?
		GetVrVehicleModelName(vehicle->GetModelIndex()) :
		"NO VEHICLE";
}

bool IsQuestVehicleCalibrationAvailable()
{
	return IsImmersiveBikeActive() || IsImmersiveCarActive();
}

bool IsQuestVehicleCalibrationBike()
{
	return IsImmersiveBikeActive();
}

int
GetQuestVehicleCalibrationItemCount()
{
	// Editable rows only. The menu owns its trailing Back row.
	return IsImmersiveCarActive() ? 7 : 9;
}

int
GetQuestVehicleCalibrationValue(int hand, int item)
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle || hand < 0 || hand >= VR_HAND_COUNT)
		return 0;
	const bool bike = vehicle->IsBike();
	HandleCalibration *calibration = bike ?
		GetBikeCalibration(vehicle->GetModelIndex(), hand) :
		GetCarCalibration(vehicle->GetModelIndex(), hand);
	BikeLeanCalibration *lean = bike ?
		GetBikeLeanCalibration(vehicle->GetModelIndex()) : nil;
	if(item == QUEST_VEHICLE_CAL_HAND)
		return hand;
	if(!calibration)
		return 0;
	switch(item){
	case QUEST_VEHICLE_CAL_OFFSET_X: return calibration->offsetX;
	case QUEST_VEHICLE_CAL_OFFSET_Y: return calibration->offsetY;
	case QUEST_VEHICLE_CAL_OFFSET_Z: return calibration->offsetZ;
	case QUEST_VEHICLE_CAL_ROT_X: return calibration->rotationX;
	case QUEST_VEHICLE_CAL_ROT_Y: return calibration->rotationY;
	case QUEST_VEHICLE_CAL_ROT_Z: return calibration->rotationZ;
	case QUEST_VEHICLE_CAL_WHEELIE_HEIGHT:
		return lean ? lean->wheelieHeightCm : 0;
	case QUEST_VEHICLE_CAL_STAND_HEIGHT:
		return lean ? lean->standHeightCm : 0;
	default:
		return 0;
	}
}

void
AdjustQuestVehicleCalibrationValue(int hand, int item, int direction)
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle || hand < 0 || hand >= VR_HAND_COUNT ||
	   direction == 0)
		return;
	const bool bike = vehicle->IsBike();
	const int model = vehicle->GetModelIndex();
	HandleCalibration *calibration = bike ?
		GetBikeCalibration(model, hand) :
		GetCarCalibration(model, hand);
	BikeLeanCalibration *lean = bike ?
		GetBikeLeanCalibration(model) : nil;
	if(!calibration)
		return;
	const int step = direction < 0 ? -1 : 1;
	const char *key = nil;
	int *value = nil;
	int minimum = -300;
	int maximum = 300;
	switch(item){
	case QUEST_VEHICLE_CAL_OFFSET_X:
		key = "OffsetX"; value = &calibration->offsetX; break;
	case QUEST_VEHICLE_CAL_OFFSET_Y:
		key = "OffsetY"; value = &calibration->offsetY; break;
	case QUEST_VEHICLE_CAL_OFFSET_Z:
		key = "OffsetZ"; value = &calibration->offsetZ; break;
	case QUEST_VEHICLE_CAL_ROT_X:
		key = "RotationX"; value = &calibration->rotationX;
		minimum = -720; maximum = 720; break;
	case QUEST_VEHICLE_CAL_ROT_Y:
		key = "RotationY"; value = &calibration->rotationY;
		minimum = -720; maximum = 720; break;
	case QUEST_VEHICLE_CAL_ROT_Z:
		key = "RotationZ"; value = &calibration->rotationZ;
		minimum = -720; maximum = 720; break;
	case QUEST_VEHICLE_CAL_WHEELIE_HEIGHT:
		if(lean){
			lean->wheelieHeightCm = clamp(
				lean->wheelieHeightCm+step, 5, 100);
			SaveBikeLeanValue(model, "WheelieHeightCm",
				lean->wheelieHeightCm);
		}
		return;
	case QUEST_VEHICLE_CAL_STAND_HEIGHT:
		if(lean){
			lean->standHeightCm = clamp(
				lean->standHeightCm+step, 5, 100);
			SaveBikeLeanValue(model, "StandHeightCm",
				lean->standHeightCm);
		}
		return;
	default:
		return;
	}
	*value = clamp(*value+step, minimum, maximum);
	SaveCalibrationValue(bike ? "BikeHandle" : "CarWheelV2",
		model, hand, key, *value);
}

void
SetQuestVehicleCalibrationPreview(bool visible)
{
	gCalibrationPreview = visible;
}

void
ApplyQuestVehicleViewOffset(CMatrix *eyeCamera)
{
	if(!eyeCamera)
		return;
	LoadDrivingSettings();
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle)
		return;
	eyeCamera->GetPosition().z +=
		(float)gDrivingYOffsetCm/100.0f;
	VehicleViewCalibration *calibration =
		GetViewCalibration(vehicle->GetModelIndex());
	if(calibration)
		eyeCamera->GetPosition() += vehicle->GetForward()*
			((float)calibration->seatDistanceCm/100.0f);
}

void
ApplyQuestBikeHorizonLock(CMatrix *baseCamera)
{
	if(!baseCamera)
		return;
	LoadDrivingSettings();
	CVehicle *vehicle = FindPlayerVehicle();
	if(!gBikeHorizonLocked || !vehicle || !vehicle->IsBike() ||
	   gGameState != GS_PLAYING_GAME ||
	   CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn)
		return;
	CVector forward = baseCamera->GetForward();
	if(forward.MagnitudeSqr() <= 0.0001f)
		return;
	forward.Normalise();
	const CVector worldUp(0.0f, 0.0f, 1.0f);
	CVector left = CrossProduct(worldUp, forward);
	if(left.MagnitudeSqr() <= 0.0001f)
		return;
	left.Normalise();
	CVector up = CrossProduct(forward, left);
	up.Normalise();
	baseCamera->GetRight() = left;
	baseCamera->GetUp() = up;
	baseCamera->GetForward() = forward;
}

} // namespace OculusVR

#endif
