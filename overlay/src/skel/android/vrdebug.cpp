// Port of the desktop VR debug overlay (src/vr/OpenXRVR.cpp): the software
// glyph font, DrawDebugText, the FPS line and the grips+A toggle chord are
// carried over verbatim. Only the delivery differs -- the pixels reach the
// compositor through the Android session's quad layer instead of a D3D12/GL
// swapchain upload.

#include "common.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "android.h"
#include "platform_android.h"
#include "OculusVR.h"
#include "QuestProfiler.h"
#include "PhysicsDirector.h"
#include "VehicleVisualDirector.h"
#include "xr_vulkan_session.h"
#include "WeaponType.h"
#include "IniFile.h"
#include "Population.h"
#include "ModelSets.h"
#include "CarCtrl.h"
#include "vulkan/rwvk.h"
#include "Pools.h"
#include "Game.h"
#include "Frontend.h"
#include "Renderer.h"
#include "Shadows.h"
#include "ParticleObject.h"
#include "CutsceneMgr.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Timer.h"
#include "crossplatform.h"
#include <android/log.h>

extern int GetVrCheatCount(void);
extern const char *GetVrCheatName(int index);
extern bool ActivateVrCheat(int index);
extern bool CycleVrCheatSelection(int index, int direction);
extern bool GetVrCheatToggleState(int index, bool *enabled);
extern int GetVrMissionCategoryCount(void);
extern const char *GetVrMissionCategoryName(int category);
extern int GetVrMissionCount(int category);
extern const char *GetVrMissionName(int category, int item);
extern bool ActivateVrMission(int category, int item);
extern int GetVrWeaponTypeForSlot(int slot);
extern const char *GetVrWeaponName(int weaponType);

enum {
	VR_DEBUG_WIDTH = 512,
	VR_DEBUG_HEIGHT = 437,
	VR_FPS_HEIGHT = 128,
	VR_MENU_WIDTH = 1024,
	VR_MENU_HEIGHT = 768,
	VR_CHEAT_ITEMS_PER_PAGE = 14,
};

// --- verbatim from OpenXRVR.cpp -------------------------------------------
struct DebugGlyph { char character; uint8 rows[7]; };
static const DebugGlyph gDebugGlyphs[] = {
	{' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, {'%',{0x11,0x12,0x02,0x04,0x08,0x09,0x11}},
	{'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
	{'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
	{'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
	{'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, {'<',{0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
	{'>',{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
	{'[',{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
	{']',{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
	{':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}}, {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
	{'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}, {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
	{'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}}, {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
	{'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}}, {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
	{'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}, {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
	{'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}}, {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
	{'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
	{'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
	{'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'I',{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}}, {'J',{0x01,0x01,0x01,0x01,0x11,0x11,0x0E}},
	{'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
	{'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x19,0x15,0x13,0x13,0x11}},
	{'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
	{'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
	{'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}, {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
	{'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
	{'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
	{'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}}
};

static bool gDebugVisible;
static uint8 gDebugPixels[VR_DEBUG_WIDTH*VR_DEBUG_HEIGHT*4];
static uint8 gVrMenuPixels[VR_MENU_WIDTH*VR_MENU_HEIGHT*4];
static int gDebugFps;
static double gDebugPreviousFrameMs;
static float gDebugSmoothedFrameMs;
static bool gTouchProfilerShortcutDown;
static bool gTouchRecenterShortcutDown;
static bool gVrMenuVisible;
static bool gVrMenuShortcutDown;
static bool gVrCheatShortcutDown;
static bool gVrMenuSelectDown;
static bool gVrMenuBackDown;
static int gVrMenuNavigateDirection;
static double gVrMenuNavigateRepeatAt;
static bool gVrMenuIncreaseDown;
static bool gVrMenuDecreaseDown;
static bool gVrCheatCycleDown;
static double gVrMenuIncreaseRepeatAt;
static double gVrMenuDecreaseRepeatAt;
static double gVrMenuIncreaseHoldStart;
static double gVrMenuDecreaseHoldStart;
static bool gVrSettingsLoaded;
static bool gViceCityColorEnabled = true;
// Stable single-frame FXAA. The stronger 3x3 experiment produced no visible
// headset improvement over this path, so do not expose duplicate choices.
static int gSpatialAaMode = 1;
static bool gQuestQuickTestStart;
static int gQuestRenderScalePercent = 125;
static int gQuestSgsrMode = rw::vulkan::SGSR_OFF;
static int gQuestMsaaSamples = 1;
static int gOcclusionCullingMode = VR_OCCLUSION_CULLING_SAFE;
static bool gGameplayHudEnabled = true;
static int gHudWidthPercent = 100;
static int gHudScalePercent = 130;
static int gHudOffsetXCm;
static int gHudOffsetYCm;
static int gQuestMovementOrientation = 1;
// 0.4.1 PC parity: walking head bob is off by default; the anchor holds a
// stable eye height while the legs animate.
static int gQuestHeadBobbing = 0;
// 72 is the safe default; the Quest 3 display also offers 80/90/120 and the
// game holds 90 with headroom after the traffic director work.
static int gQuestRefreshRateHz = 72;
static int gQuestTurnMode;
static int gQuestTurnSensitivityPercent = 100;
static int gQuestHeadSteeringSensitivityPercent = 50;
static int gQuestSnapTurnAngleDegrees = 30;
static int gVrMenuPage;
static int gVrMenuSelection;
static int gVrGraphicsSelection;
static int gVrWeaponsSelection;
static int gVrHudSelection;
static int gVrTrafficSelection;
static int gVrModelAssetsSelection;
static int gVrCheatSelection;
static int gVrVehicleSelection;
static int gVrVehicleCalibrationSelection;
static int gVrVehicleCalibrationHand;
static int gVrLocomotionSelection;
static int gVrCalibrationSelection;
static int gVrCalibrationHand = 1;
static int gVrHolsterSelection;
static bool gVrWelcomePending;
static bool gVrWelcomeFirstRun;
static bool gVrAboutDismissArmed;
static bool gVrAboutReleaseGate;
static bool gVrWelcomeBaselineValid;
static CVector gVrWelcomeBaseline;
static int gVrCheatStatusFrames;
static char gVrCheatStatus[48];
static int gVrMissionCategory = -1;
static int gVrMissionCategorySelection;
static int gVrMissionSelection;
static int gTrafficPedPercent = 135;
static int gTrafficCarPercent = 135;
static int gQuestCpuPerformanceMode;
static int gQuestCpuPerformanceSavedMode;
static int gQuestGpuPerformanceMode = 1; // SUSTAINED; enum is declared below.

enum {
	VR_MENU_PAGE_SETTINGS,
	VR_MENU_PAGE_GRAPHICS,
	VR_MENU_PAGE_WEAPONS,
	VR_MENU_PAGE_HUD,
	VR_MENU_PAGE_TRAFFIC,
	VR_MENU_PAGE_MODEL_ASSETS,
	VR_MENU_PAGE_VEHICLE,
	VR_MENU_PAGE_VEHICLE_CALIBRATION,
	VR_MENU_PAGE_LOCOMOTION,
	VR_MENU_PAGE_CALIBRATION,
	VR_MENU_PAGE_HOLSTERS,
	VR_MENU_PAGE_CHEATS,
	VR_MENU_PAGE_MISSIONS,
	VR_MENU_PAGE_ABOUT,
};

enum eVrTrafficMenuItem {
	VR_TRAFFIC_PEDESTRIANS = 0,
	VR_TRAFFIC_VEHICLES,
	VR_TRAFFIC_PHYSICS_DIRECTOR,
	VR_TRAFFIC_PHYSICS_PRESET,
	VR_TRAFFIC_VISUAL_BUDGET,
	VR_TRAFFIC_DEFAULTS,
	VR_TRAFFIC_BACK,
	VR_TRAFFIC_ITEM_COUNT
};

enum eVrHudMenuItem {
	VR_HUD_ENABLED = 0,
	VR_HUD_HORIZONTAL_SCALE,
	VR_HUD_SCALE,
	VR_HUD_OFFSET_X,
	VR_HUD_OFFSET_Y,
	VR_HUD_BACK,
	VR_HUD_ITEM_COUNT
};

// Native Quest exposes only settings backed by its Vulkan/OpenXR runtime.
// Desktop-only D3D12 features are deliberately omitted so every visible row
// is actionable on the headset.
enum eVrMainMenuItem {
	VR_MAIN_GRAPHICS = 0,
	VR_MAIN_TRAFFIC_SETTINGS,
	VR_MAIN_HUD,
	VR_MAIN_MODEL_ASSETS,
	VR_MAIN_VEHICLE_SETTINGS,
	VR_MAIN_LOCOMOTION_SETTINGS,
	VR_MAIN_WEAPONS,
	VR_MAIN_HOLSTERS,
	VR_MAIN_CHEATS,
	VR_MAIN_ABOUT,
	VR_MAIN_ITEM_COUNT
};

enum eVrGraphicsMenuItem {
	VR_GRAPHICS_RENDER_SCALE = 0,
	VR_GRAPHICS_SGSR,
	VR_GRAPHICS_MSAA,
	VR_GRAPHICS_FXAA,
	VR_GRAPHICS_COLOR,
	VR_GRAPHICS_PROFILER,
	VR_GRAPHICS_CPU_PERFORMANCE,
	VR_GRAPHICS_GPU_PERFORMANCE,
	VR_GRAPHICS_SHADOWS,
	VR_GRAPHICS_OCCLUSION,
	VR_GRAPHICS_FOUNTAIN,
	VR_GRAPHICS_QUICK_START,
	VR_GRAPHICS_BACK,
	VR_GRAPHICS_ITEM_COUNT
};

enum eVrWeaponsMenuItem {
	VR_WEAPONS_HANDS = 0,
	VR_WEAPONS_LASER,
	VR_WEAPONS_HOLSTER_HIGHLIGHTS,
	VR_WEAPONS_MANUAL_RELOAD,
	VR_WEAPONS_SCOPE_AIM,
	VR_WEAPONS_GRIP_LOCK,
	VR_WEAPONS_CALIBRATION,
	VR_WEAPONS_BACK,
	VR_WEAPONS_ITEM_COUNT
};

enum eVrModelAssetsMenuItem {
	VR_MODEL_ASSETS_PRESET = 0,
	VR_MODEL_ASSETS_WORLD,
	VR_MODEL_ASSETS_VEGETATION,
	VR_MODEL_ASSETS_VEHICLES,
	VR_MODEL_ASSETS_PEDS,
	VR_MODEL_ASSETS_WEAPONS,
	VR_MODEL_ASSETS_BACK,
	VR_MODEL_ASSETS_ITEM_COUNT
};

enum eQuestCpuPerformanceMode {
	QUEST_CPU_PERFORMANCE_AUTO = 0,
	QUEST_CPU_PERFORMANCE_SUSTAINED,
	QUEST_CPU_PERFORMANCE_BOOST,
	QUEST_CPU_PERFORMANCE_COUNT
};

enum eVrVehicleMenuItem {
	VR_VEHICLE_CAR_DRIVING_TYPE = 0,
	VR_VEHICLE_BIKE_DRIVING_TYPE,
	VR_VEHICLE_DEFAULT_SEAT_HEIGHT,
	VR_VEHICLE_DEFAULT_SEAT_FORWARD,
	VR_VEHICLE_GLOBAL_SEAT_HEIGHT,
	VR_VEHICLE_GLOBAL_SEAT_FORWARD,
	VR_VEHICLE_MODEL_SEAT_HEIGHT,
	VR_VEHICLE_MODEL_SEAT_FORWARD,
	VR_VEHICLE_MOTION_HAND,
	VR_VEHICLE_WHEEL_VISIBLE,
	VR_VEHICLE_MODEL_WHEEL_VISIBLE,
	VR_VEHICLE_HANDLE_HIGHLIGHTS,
	VR_VEHICLE_BIKE_LOCK_HORIZON,
	VR_VEHICLE_CALIBRATION,
	VR_VEHICLE_BACK,
	VR_VEHICLE_ITEM_COUNT
};

enum eVrLocomotionMenuItem {
	VR_LOCOMOTION_MOVEMENT_ORIENTATION = 0,
	VR_LOCOMOTION_TURN_MODE,
	VR_LOCOMOTION_TURN_SENSITIVITY,
	VR_LOCOMOTION_SNAP_ANGLE,
	VR_LOCOMOTION_HEAD_BOBBING,
	VR_LOCOMOTION_REFRESH_RATE,
	VR_LOCOMOTION_BACK,
	VR_LOCOMOTION_ITEM_COUNT
};

enum eQuestMovementOrientation {
	QUEST_MOVEMENT_ORIENTATION_BODY = 0,
	QUEST_MOVEMENT_ORIENTATION_HEAD,
	QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL,
	QUEST_MOVEMENT_ORIENTATION_COUNT
};

enum eQuestTurnMode {
	QUEST_TURN_SMOOTH = 0,
	QUEST_TURN_SNAP,
	QUEST_TURN_MODE_COUNT
};

static const char *
QuestCpuPerformanceModeName(int mode)
{
	static const char *const names[QUEST_CPU_PERFORMANCE_COUNT] = {
		"AUTO", "SUSTAINED", "BOOST (TEST)"
	};
	return mode >= 0 && mode < QUEST_CPU_PERFORMANCE_COUNT ?
		names[mode] : "UNKNOWN";
}

static const uint8 *
FindDebugGlyph(char character)
{
	if(character >= 'a' && character <= 'z')
		character = character-'a'+'A';
	for(uint32 i = 0; i < ARRAY_SIZE(gDebugGlyphs); i++)
		if(gDebugGlyphs[i].character == character)
			return gDebugGlyphs[i].rows;
	return gDebugGlyphs[0].rows;
}

static void
PutDebugPixel(int x, int y, uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	if(x < 0 || y < 0 || x >= VR_DEBUG_WIDTH || y >= VR_DEBUG_HEIGHT) return;
	// Vulkan images are top-down, same as the desktop's D3D12 branch.
	const int offset = (y*VR_DEBUG_WIDTH+x)*4;
	gDebugPixels[offset+0] = red; gDebugPixels[offset+1] = green;
	gDebugPixels[offset+2] = blue; gDebugPixels[offset+3] = alpha;
}

static void
DrawDebugText(const char *value, int centreX, int y, int scale,
              uint8 red, uint8 green, uint8 blue)
{
	const int advance = scale*6;
	int x = centreX-(int)strlen(value)*advance/2;
	for(const char *ch = value; *ch; ch++, x += advance){
		const uint8 *rows = FindDebugGlyph(*ch);
		for(int row = 0; row < 7; row++) for(int column = 0; column < 5; column++)
			if(rows[row]&(1<<(4-column)))
				for(int py = 0; py < scale; py++) for(int px = 0; px < scale; px++)
					PutDebugPixel(x+column*scale+px, y+row*scale+py, red, green, blue, 255);
	}
}

static void
PutVrMenuPixel(int x, int y, uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	if(x < 0 || y < 0 || x >= VR_MENU_WIDTH || y >= VR_MENU_HEIGHT)
		return;
	const int offset = (y*VR_MENU_WIDTH+x)*4;
	gVrMenuPixels[offset+0] = red;
	gVrMenuPixels[offset+1] = green;
	gVrMenuPixels[offset+2] = blue;
	gVrMenuPixels[offset+3] = alpha;
}

static void
FillVrMenuRect(int left, int top, int right, int bottom,
               uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	left = Max(0, left);
	top = Max(0, top);
	right = Min(VR_MENU_WIDTH, right);
	bottom = Min(VR_MENU_HEIGHT, bottom);
	for(int y = top; y < bottom; y++)
		for(int x = left; x < right; x++)
			PutVrMenuPixel(x, y, red, green, blue, alpha);
}

static void
DrawVrMenuText(const char *value, int centreX, int y, int scale,
               uint8 red, uint8 green, uint8 blue)
{
	const int advance = scale*6;
	int x = centreX-(int)strlen(value)*advance/2;
	for(const char *ch = value; *ch; ch++, x += advance){
		const uint8 *rows = FindDebugGlyph(*ch);
		for(int row = 0; row < 7; row++)
			for(int column = 0; column < 5; column++)
				if(rows[row]&(1<<(4-column)))
					for(int py = 0; py < scale; py++)
						for(int px = 0; px < scale; px++)
							PutVrMenuPixel(
								x+column*scale+px,
								y+row*scale+py,
								red, green, blue, 255);
	}
}
// --------------------------------------------------------------------------

namespace androidgame {

static void SaveVrInteger(const char *key, int value);

static void
ApplyTrafficSettings(void)
{
	CIniFile::PedNumberMultiplier = gTrafficPedPercent/100.0f;
	CIniFile::CarNumberMultiplier = gTrafficCarPercent/100.0f;
	CPopulation::MaxNumberOfPedsInUse =
		(int32)(25.0f*CIniFile::PedNumberMultiplier);
	CPopulation::MaxNumberOfPedsInUseInterior =
		(int32)(40.0f*CIniFile::PedNumberMultiplier);
	CCarCtrl::MaxNumberOfCarsInUse =
		(int32)(12.0f*CIniFile::CarNumberMultiplier);
}

static void
LoadVrSettings(void)
{
	if(gVrSettingsLoaded)
		return;
	gViceCityColorEnabled =
		GetPrivateProfileIntA("VR", "ViceCityColor", 1,
			".\\vr_settings.ini") != 0;
	gSpatialAaMode = GetPrivateProfileIntA("VR", "AntiAliasing", 1,
		".\\vr_settings.ini") != 0;
	gQuestQuickTestStart =
		GetPrivateProfileIntA("VR", "QuickTestStart", 0,
			".\\vr_settings.ini") != 0;
	{
		static const int scales[] = { 100, 125, 150, 175 };
		const int savedScale = GetPrivateProfileIntA("VR", "RenderScalePercent",
			125, ".\\vr_settings.ini");
		int best = 0;
		for(int i = 1; i < (int)ARRAY_SIZE(scales); i++)
			if(Abs(scales[i]-savedScale) < Abs(scales[best]-savedScale))
				best = i;
	gQuestRenderScalePercent = scales[best];
	}
	// Temporal AA is intentionally unavailable until its stereo instability is
	// solved. Preserve an exact native fallback instead of letting an old INI
	// silently reactivate an eye-straining mode.
	gQuestSgsrMode = rw::vulkan::SGSR_OFF;
	SaveVrInteger("Sgsr2Mode", gQuestSgsrMode);
	gQuestMsaaSamples = 1;
	SaveVrInteger("MsaaSamples", gQuestMsaaSamples);
	CParticleObject::SetVrFountainQuality(Min(Max(GetPrivateProfileIntA("VR",
		"FountainQuality", VR_FOUNTAIN_OPTIMIZED, ".\\vr_settings.ini"),
		(int)VR_FOUNTAIN_OFF), (int)VR_FOUNTAIN_QUALITY_COUNT-1));
	{
		// Migrate the old boolean without rewriting it. OFF remains the exact
		// original full-360 render path; SAFE is the former enabled behaviour.
		const int legacyOcclusion = GetPrivateProfileIntA("VR",
			"OcclusionCulling", 1, ".\\vr_settings.ini") != 0 ?
			VR_OCCLUSION_CULLING_SAFE : VR_OCCLUSION_CULLING_OFF;
		// V2 adds a correctness-first STEREO SAFE mode before the two authored
		// experimental modes. Do not reinterpret the old value 2 (AGGRESSIVE) as
		// a new experimental mode after an update; migrate everyone to SAFE/OFF.
		gOcclusionCullingMode = Min(Max(GetPrivateProfileIntA("VR",
			"OcclusionCullingModeV2", legacyOcclusion, ".\\vr_settings.ini"),
			(int)VR_OCCLUSION_CULLING_OFF),
			(int)VR_OCCLUSION_CULLING_MODE_COUNT-1);
		CRenderer::SetVrOcclusionCullingMode(gOcclusionCullingMode);
	}
	CShadows::SetRenderEnabled(GetPrivateProfileIntA("VR", "ShadowsEnabled",
		1, ".\\vr_settings.ini") != 0);
	gGameplayHudEnabled =
		GetPrivateProfileIntA("VR", "GameplayHud", 1,
			".\\vr_settings.ini") != 0;
	gHudWidthPercent = Min(Max(GetPrivateProfileIntA("VR",
		"HudWidthPercent", 100, ".\\vr_settings.ini"), 50), 200);
	gHudScalePercent = Min(Max(GetPrivateProfileIntA("VR",
		"HudScalePercent", 130, ".\\vr_settings.ini"), 50), 200);
	// Win32-style profile reads return an unsigned value. Cast back to signed
	// before clamping so a saved negative offset cannot become +100.
	gHudOffsetXCm = Min(Max((int)(int32)GetPrivateProfileIntA("VR",
		"HudOffsetXCm", 0, ".\\vr_settings.ini"), -100), 100);
	gHudOffsetYCm = Min(Max((int)(int32)GetPrivateProfileIntA("VR",
		"HudOffsetYCm", 0, ".\\vr_settings.ini"), -100), 100);
	gQuestMovementOrientation = Min(Max(GetPrivateProfileIntA("VR",
		"MovementOrientation", QUEST_MOVEMENT_ORIENTATION_HEAD,
		".\\vr_settings.ini"),
		(int)QUEST_MOVEMENT_ORIENTATION_BODY),
		(int)QUEST_MOVEMENT_ORIENTATION_COUNT-1);
	gQuestHeadBobbing = GetPrivateProfileIntA("VR", "HeadBobbing", 0,
		".\\vr_settings.ini") != 0;
	gQuestRefreshRateHz = Min(Max(GetPrivateProfileIntA("VR",
		"RefreshRate", 72, ".\\vr_settings.ini"), 72), 120);
	androidgame::SetPreferredRefreshRate(gQuestRefreshRateHz);
	const int persistedCpuPerformanceMode = GetPrivateProfileIntA("VR",
		"CpuPerformanceMode", QUEST_CPU_PERFORMANCE_SUSTAINED,
		".\\vr_settings.ini");
	// Boost is deliberately session-only. Old test builds may have persisted
	// value 2; treat that as Auto rather than re-entering Boost on startup.
	gQuestCpuPerformanceSavedMode =
		persistedCpuPerformanceMode >= QUEST_CPU_PERFORMANCE_AUTO &&
		persistedCpuPerformanceMode <= QUEST_CPU_PERFORMANCE_SUSTAINED ?
		persistedCpuPerformanceMode : QUEST_CPU_PERFORMANCE_SUSTAINED;
	gQuestCpuPerformanceMode = gQuestCpuPerformanceSavedMode;
	xrvk::setPerformanceMode(gQuestCpuPerformanceMode);
	gQuestGpuPerformanceMode = Min(Max(GetPrivateProfileIntA("VR",
		"GpuPerformanceMode", QUEST_CPU_PERFORMANCE_SUSTAINED,
		".\\vr_settings.ini"), (int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_BOOST);
	xrvk::setGpuPerformanceMode(gQuestGpuPerformanceMode);
	gQuestTurnMode = Min(Max(GetPrivateProfileIntA("VR", "TurnMode",
		QUEST_TURN_SMOOTH, ".\\vr_settings.ini"),
		(int)QUEST_TURN_SMOOTH), (int)QUEST_TURN_MODE_COUNT-1);
	gQuestTurnSensitivityPercent = Min(Max(GetPrivateProfileIntA("VR",
		"TurnSensitivityPercent", 100, ".\\vr_settings.ini"), 25), 300);
	gQuestHeadSteeringSensitivityPercent = Min(Max(
		GetPrivateProfileIntA("VR", "HeadSteeringSensitivityPercent",
			50, ".\\vr_settings.ini"), 25), 300);
	gQuestSnapTurnAngleDegrees = Min(Max(GetPrivateProfileIntA("VR",
		"SnapTurnAngleDegrees", 30, ".\\vr_settings.ini"), 15), 90);
	const int defaultPedPercent = Min(Max(
		(int)(CIniFile::PedNumberMultiplier*100.0f+0.5f), 50), 300);
	const int defaultCarPercent = Min(Max(
		(int)(CIniFile::CarNumberMultiplier*100.0f+0.5f), 50), 300);
	gTrafficPedPercent = Min(Max(GetPrivateProfileIntA("VR",
		"PedTrafficPercent", defaultPedPercent,
		".\\vr_settings.ini"), 50), 300);
	gTrafficCarPercent = Min(Max(GetPrivateProfileIntA("VR",
		"CarTrafficPercent", defaultCarPercent,
		".\\vr_settings.ini"), 50), 300);
	QuestPhysicsDirectorSetMode(Min(Max(GetPrivateProfileIntA("VR",
		"PhysicsDirectorMode", QUEST_PHYSICS_DIRECTOR_ADAPTIVE,
		".\\vr_settings.ini"),
		(int)QUEST_PHYSICS_DIRECTOR_OFF),
		(int)QUEST_PHYSICS_DIRECTOR_MODE_COUNT-1));
	QuestPhysicsDirectorSetPreset(Min(Max(GetPrivateProfileIntA("VR",
		"PhysicsDirectorPreset", QUEST_PHYSICS_PRESET_QUALITY,
		".\\vr_settings.ini"),
		(int)QUEST_PHYSICS_PRESET_QUALITY),
		(int)QUEST_PHYSICS_PRESET_COUNT-1));
	QuestVehicleVisualBudgetSetMode(Min(Max(GetPrivateProfileIntA("VR",
		"VehicleVisualBudgetMode", QUEST_VEHICLE_VISUAL_STOCK,
		".\\vr_settings.ini"),
		(int)QUEST_VEHICLE_VISUAL_STOCK),
		(int)QUEST_VEHICLE_VISUAL_MODE_COUNT-1));
	gVrWelcomePending = GetPrivateProfileIntA("VR", "WelcomeShown", 0,
		".\\vr_settings.ini") == 0;
	gVrWelcomeFirstRun = gVrWelcomePending;
	ApplyTrafficSettings();
	gVrSettingsLoaded = true;
}

static void
SaveViceCityColor(void)
{
	WritePrivateProfileStringA("VR", "ViceCityColor",
		gViceCityColorEnabled ? "1" : "0", ".\\vr_settings.ini");
}

static void
SaveFxaa(void)
{
	char value[8];
	snprintf(value, sizeof(value), "%d", gSpatialAaMode);
	WritePrivateProfileStringA("VR", "AntiAliasing", value,
	                         ".\\vr_settings.ini");
}

static void
SaveGameplayHud(void)
{
	WritePrivateProfileStringA("VR", "GameplayHud",
		gGameplayHudEnabled ? "1" : "0", ".\\vr_settings.ini");
}

static void
SaveVrInteger(const char *key, int value)
{
	char text[16];
	snprintf(text, sizeof(text), "%d", value);
	WritePrivateProfileStringA("VR", key, text, ".\\vr_settings.ini");
}

static void
CopyMenuText(char *destination, int capacity, const char *source)
{
	if(capacity <= 0)
		return;
	if(!source)
		source = "UNKNOWN";
	const int maxCharacters = capacity-1;
	int count = (int)strlen(source);
	if(count <= maxCharacters){
		memcpy(destination, source, count+1);
		return;
	}
	if(maxCharacters <= 3){
		destination[0] = '\0';
		return;
	}
	memcpy(destination, source, maxCharacters-3);
	memcpy(destination+maxCharacters-3, "...", 4);
}

static double
MonotonicMilliseconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec*1000.0+ts.tv_nsec/1e6;
}

static bool
MenuRepeatPulse(bool down, bool &wasDown, double &repeatAt,
	double &holdStart, double now,
                bool allowRepeat)
{
	if(!down){
		wasDown = false;
		repeatAt = 0.0;
		holdStart = 0.0;
		return false;
	}
	if(!wasDown){
		wasDown = true;
		holdStart = now;
		repeatAt = now+420.0;
		return true;
	}
	if(!allowRepeat)
		return false;
	if(now >= repeatAt){
		repeatAt = now+85.0;
		return true;
	}
	return false;
}

static int
MenuRepeatMagnitude(bool down, double holdStart, double now)
{
	if(!down || holdStart <= 0.0)
		return 1;
	const double held = now-holdStart;
	return held >= 3500.0 ? 10 : (held >= 1500.0 ? 3 : 1);
}

// Navigation needs two things at once: a deliberate tap must always move one
// row, while a held stick must eventually scroll long lists such as CHEATS.
// Separate engage/release thresholds prevent Touch-stick noise around a single
// threshold from manufacturing extra taps.
static int
MenuNavigationPulse(float axis, double now)
{
	const float engage = 0.68f;
	const float release = 0.34f;
	int requested = axis >= engage ? -1 : (axis <= -engage ? 1 : 0);

	if(gVrMenuNavigateDirection == 0){
		if(requested == 0)
			return 0;
		gVrMenuNavigateDirection = requested;
		gVrMenuNavigateRepeatAt = now+430.0;
		return requested;
	}

	if(Abs(axis) <= release){
		gVrMenuNavigateDirection = 0;
		gVrMenuNavigateRepeatAt = 0.0;
		return 0;
	}

	// A deliberate reversal is a new navigation edge even if the stick crossed
	// the centre too quickly for a rendered frame to observe the release band.
	if(requested != 0 && requested != gVrMenuNavigateDirection){
		gVrMenuNavigateDirection = requested;
		gVrMenuNavigateRepeatAt = now+430.0;
		return requested;
	}

	if(now >= gVrMenuNavigateRepeatAt){
		gVrMenuNavigateRepeatAt = now+110.0;
		return gVrMenuNavigateDirection;
	}
	return 0;
}

static void
ResetMenuNavigationRepeat(void)
{
	gVrMenuNavigateDirection = 0;
	gVrMenuNavigateRepeatAt = 0.0;
}

static int
QuestWeaponSettingForWeaponItem(int item)
{
	switch(item){
	case VR_WEAPONS_HANDS: return 0;
	case VR_WEAPONS_LASER: return 1;
	case VR_WEAPONS_HOLSTER_HIGHLIGHTS: return 2;
	case VR_WEAPONS_MANUAL_RELOAD: return 3;
	case VR_WEAPONS_SCOPE_AIM: return 4;
	case VR_WEAPONS_GRIP_LOCK: return 5;
	default: return -1;
	}
}

static int *
CurrentMenuSelection(void)
{
	switch(gVrMenuPage){
	case VR_MENU_PAGE_GRAPHICS: return &gVrGraphicsSelection;
	case VR_MENU_PAGE_WEAPONS: return &gVrWeaponsSelection;
	case VR_MENU_PAGE_HUD: return &gVrHudSelection;
	case VR_MENU_PAGE_TRAFFIC: return &gVrTrafficSelection;
	case VR_MENU_PAGE_MODEL_ASSETS: return &gVrModelAssetsSelection;
	case VR_MENU_PAGE_VEHICLE: return &gVrVehicleSelection;
	case VR_MENU_PAGE_VEHICLE_CALIBRATION:
		return &gVrVehicleCalibrationSelection;
	case VR_MENU_PAGE_LOCOMOTION: return &gVrLocomotionSelection;
	case VR_MENU_PAGE_CALIBRATION: return &gVrCalibrationSelection;
	case VR_MENU_PAGE_HOLSTERS: return &gVrHolsterSelection;
	case VR_MENU_PAGE_CHEATS: return &gVrCheatSelection;
	case VR_MENU_PAGE_MISSIONS:
		return gVrMissionCategory < 0 ?
			&gVrMissionCategorySelection : &gVrMissionSelection;
	default: return &gVrMenuSelection;
	}
}

static int
CurrentMenuItemCount(void)
{
	switch(gVrMenuPage){
	case VR_MENU_PAGE_SETTINGS: return VR_MAIN_ITEM_COUNT;
	case VR_MENU_PAGE_GRAPHICS: return VR_GRAPHICS_ITEM_COUNT;
	case VR_MENU_PAGE_WEAPONS: return VR_WEAPONS_ITEM_COUNT;
	case VR_MENU_PAGE_HUD: return VR_HUD_ITEM_COUNT;
	case VR_MENU_PAGE_TRAFFIC: return VR_TRAFFIC_ITEM_COUNT;
	case VR_MENU_PAGE_MODEL_ASSETS: return VR_MODEL_ASSETS_ITEM_COUNT;
	case VR_MENU_PAGE_VEHICLE: return VR_VEHICLE_ITEM_COUNT;
	case VR_MENU_PAGE_VEHICLE_CALIBRATION:
		return Max(1,
			OculusVR::GetQuestVehicleCalibrationItemCount()+1);
	case VR_MENU_PAGE_LOCOMOTION: return VR_LOCOMOTION_ITEM_COUNT;
	case VR_MENU_PAGE_CALIBRATION: return 21;
	case VR_MENU_PAGE_HOLSTERS:
		return OculusVR::GetQuestHolsterPointCount()+1;
	case VR_MENU_PAGE_CHEATS: return Max(1, GetVrCheatCount()+1);
	case VR_MENU_PAGE_MISSIONS:
		return Max(1, gVrMissionCategory < 0 ?
			GetVrMissionCategoryCount() :
			GetVrMissionCount(gVrMissionCategory));
	default: return 1;
	}
}

static bool
CurrentMenuValueRepeats(void)
{
	if(gVrMenuPage == VR_MENU_PAGE_CALIBRATION)
		return gVrCalibrationSelection >= 1 &&
			gVrCalibrationSelection <= 18;
	if(gVrMenuPage == VR_MENU_PAGE_HUD)
		return gVrHudSelection >= VR_HUD_HORIZONTAL_SCALE &&
			gVrHudSelection <= VR_HUD_OFFSET_Y;
	if(gVrMenuPage == VR_MENU_PAGE_TRAFFIC)
		return gVrTrafficSelection == VR_TRAFFIC_PEDESTRIANS ||
			gVrTrafficSelection == VR_TRAFFIC_VEHICLES;
	if(gVrMenuPage == VR_MENU_PAGE_VEHICLE)
		return gVrVehicleSelection >= VR_VEHICLE_DEFAULT_SEAT_HEIGHT &&
			gVrVehicleSelection <= VR_VEHICLE_MODEL_SEAT_FORWARD;
	if(gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION){
		const int row = gVrVehicleCalibrationSelection;
		const int item = OculusVR::GetQuestVehicleCalibrationItemForRow(row);
		return row < OculusVR::GetQuestVehicleCalibrationItemCount() &&
			item != OculusVR::QUEST_VEHICLE_CAL_HAND;
	}
	if(gVrMenuPage == VR_MENU_PAGE_LOCOMOTION)
		return gVrLocomotionSelection ==
			VR_LOCOMOTION_TURN_SENSITIVITY;
	return false;
}

static void
ReturnFromCurrentMenuPage(void)
{
	if(gVrMenuPage == VR_MENU_PAGE_SETTINGS){
		gVrMenuVisible = false;
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_MISSIONS){
		if(gVrMissionCategory >= 0)
			gVrMissionCategory = -1;
		else
			gVrMenuPage = VR_MENU_PAGE_CHEATS;
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION){
		OculusVR::SetQuestVehicleCalibrationPreview(false);
		gVrMenuPage = VR_MENU_PAGE_VEHICLE;
		return;
	}
	gVrMenuPage = VR_MENU_PAGE_SETTINGS;
	gVrCheatStatusFrames = 0;
}

static bool
AnyVrAboutButtonDown(const PadInput &in)
{
	return in.a || in.b || in.x || in.y || in.menu ||
		in.leftStickClick || in.rightStickClick ||
		in.leftTrigger >= 0.55f || in.rightTrigger >= 0.55f ||
		in.leftGrip >= 0.65f || in.rightGrip >= 0.65f;
}

static bool
DidPlayerTakeWelcomeStep(const PadInput &in)
{
	CPlayerPed *player = FindPlayerPed();
	if(!player || gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bMenuActive || CCutsceneMgr::IsRunning() ||
	   FindPlayerVehicle()){
		gVrWelcomeBaselineValid = false;
		return false;
	}
	const CVector position = player->GetPosition();
	if(!gVrWelcomeBaselineValid){
		gVrWelcomeBaseline = position;
		gVrWelcomeBaselineValid = true;
		return false;
	}
	const bool movementInput = Abs(in.leftStickX) >= 0.20f ||
		Abs(in.leftStickY) >= 0.20f;
	const CVector moved = position-gVrWelcomeBaseline;
	return movementInput && moved.x*moved.x+moved.y*moved.y >= 0.0025f;
}

// Runs once per rendered frame from the pad path. All menu state changes are
// edge/repeat driven so a tap remains one exact step while a held trigger can
// traverse large calibration ranges without hundreds of clicks.
void
VrDebugUpdate(const PadInput &in)
{
	LoadVrSettings();
	gQuestCpuPerformanceMode = Min(Max(xrvk::getPerformanceMode(),
		(int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_COUNT-1);
	// The VR menu is available before CGame::Initialise loads gta3.ini. Keep
	// the persisted headset values authoritative after that later load too.
	ApplyTrafficSettings();
	const double now = MonotonicMilliseconds();
	if(gVrWelcomePending && DidPlayerTakeWelcomeStep(in)){
		gVrWelcomePending = false;
		gVrMenuVisible = true;
		gVrMenuPage = VR_MENU_PAGE_ABOUT;
		gVrWelcomeFirstRun = true;
		gVrAboutDismissArmed = false;
		gVrAboutReleaseGate = false;
	}
	if(gVrMenuVisible && gVrMenuPage == VR_MENU_PAGE_ABOUT){
		const bool button = AnyVrAboutButtonDown(in);
		if(!button)
			gVrAboutDismissArmed = true;
		else if(gVrAboutDismissArmed){
			gVrMenuVisible = false;
			gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			gVrAboutDismissArmed = false;
			gVrAboutReleaseGate = true;
			if(gVrWelcomeFirstRun){
				WritePrivateProfileStringA("VR", "WelcomeShown", "1",
					".\\vr_settings.ini");
				gVrWelcomeFirstRun = false;
			}
		}
		return;
	}
	if(gVrAboutReleaseGate){
		if(!AnyVrAboutButtonDown(in))
			gVrAboutReleaseGate = false;
		return;
	}
	const bool bothGrips =
		in.leftGrip >= 0.75f && in.rightGrip >= 0.75f;
	// As on desktop, service shortcuts other than the settings panel must not
	// steal B/A while the player is holding a physical gun or detonator.
	const bool modifier = bothGrips &&
		OculusVR::GetHeldWeaponSlot(0) < 0 &&
		OculusVR::GetHeldWeaponSlot(1) < 0 &&
		!OculusVR::IsTrackedDetonatorActive(0) &&
		!OculusVR::IsTrackedDetonatorActive(1);
	// Match the desktop service chords.  Menu remains the primary shortcut,
	// while both triggers + X is the fallback for controllers which do not
	// expose a usable Menu action to the application.
	const bool alternateMenuShortcut = bothGrips &&
		in.leftTrigger >= 0.75f && in.rightTrigger >= 0.75f &&
		(in.x || in.leftStickClick);
	const bool menuShortcut =
		(bothGrips && in.menu) || alternateMenuShortcut;
	if(menuShortcut && !gVrMenuShortcutDown){
		if(gVrMenuVisible &&
		   gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION)
			OculusVR::SetQuestVehicleCalibrationPreview(false);
		gVrMenuVisible = !gVrMenuVisible;
		gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		gVrMenuSelection = 0;
		gVrMenuSelectDown = false;
		gVrMenuBackDown = in.b || in.leftStickClick;
		ResetMenuNavigationRepeat();
		// The fallback chord holds both triggers.  Seed their latches so
		// opening the panel cannot also change its first value.
		gVrMenuIncreaseDown = in.rightTrigger >= 0.55f;
		gVrMenuDecreaseDown = in.leftTrigger >= 0.55f;
		gVrMenuIncreaseRepeatAt = 0.0;
		gVrMenuDecreaseRepeatAt = 0.0;
	}
	gVrMenuShortcutDown = menuShortcut;

	// Desktop exposes CHEATS directly on both grips + B.  Keep the in-menu
	// entry too, but make the service chord behave identically and give it
	// priority over B's normal "back" action for this frame.
	const bool cheatShortcut = modifier && in.b;
	if(cheatShortcut && !gVrCheatShortcutDown){
		const bool closingCheats =
			gVrMenuVisible && gVrMenuPage == VR_MENU_PAGE_CHEATS;
		if(gVrMenuVisible &&
		   gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION)
			OculusVR::SetQuestVehicleCalibrationPreview(false);
		gVrMenuVisible = !closingCheats;
		gVrMenuPage = VR_MENU_PAGE_CHEATS;
		gVrCheatSelection = 0;
		gVrCheatStatusFrames = 0;
		gVrMenuSelectDown = false;
		gVrMenuBackDown = in.b || in.leftStickClick;
		ResetMenuNavigationRepeat();
		gVrMenuIncreaseDown = false;
		gVrMenuDecreaseDown = false;
		gVrMenuIncreaseRepeatAt = 0.0;
		gVrMenuDecreaseRepeatAt = 0.0;
	}
	gVrCheatShortcutDown = cheatShortcut;

	// The desktop chord rebuilds its gameplay reference space around the
	// current head pose. Quest already supplies head-relative translation, so
	// dropping the renderer anchor for one update is the equivalent operation:
	// VrUpdateFirstPersonAnchor will immediately re-latch the player's current
	// physical yaw against Tommy/vehicle heading. Release any physical wheel
	// grabs too, since their previous centre belongs to the old basis.
	const bool recenterShortcut = modifier &&
		!gVrMenuVisible && !VrShouldUseTheaterMode() &&
		in.leftStickClick && in.rightStickClick;
	if(recenterShortcut && !gTouchRecenterShortcutDown){
		rw::vulkan::setFirstPersonAnchor(nil, 0.0f, 0, 0);
		OculusVR::ResetQuestDrivingInteraction();
	}
	gTouchRecenterShortcutDown = recenterShortcut;

	// Performance profiling is separate from the lightweight debug/FPS
	// overlay. BOTH GRIPS + Y enables the runtime META counters and Vulkan
	// timestamp pair together, then exposes both values for comparison.
	const bool profilerShortcut =
		modifier && !gVrMenuVisible && in.y;
	if(profilerShortcut && !gTouchProfilerShortcutDown)
		QuestProfilerSetEnabled(!QuestProfilerIsEnabled());
	gTouchProfilerShortcutDown = profilerShortcut;

	if(gVrMenuVisible){
		if(gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION &&
		   !OculusVR::IsQuestVehicleCalibrationAvailable()){
			OculusVR::SetQuestVehicleCalibrationPreview(false);
			gVrMenuPage = VR_MENU_PAGE_VEHICLE;
		}
		const int itemCount = CurrentMenuItemCount();
		const int navigationPulse = MenuNavigationPulse(in.leftStickY, now);
		if(navigationPulse != 0){
			int *selection = CurrentMenuSelection();
			*selection = (*selection+navigationPulse+itemCount)%itemCount;
		}
		const bool cheatCycle = gVrMenuPage == VR_MENU_PAGE_CHEATS &&
			(Abs(in.leftStickX) >= 0.65f);
		if(cheatCycle && !gVrCheatCycleDown && gVrCheatSelection > 0){
			const int direction = in.leftStickX > 0.0f ? 1 : -1;
			if(CycleVrCheatSelection(gVrCheatSelection-1, direction)){
				snprintf(gVrCheatStatus, sizeof(gVrCheatStatus),
					"MODEL SELECTED");
				gVrCheatStatusFrames = 60;
			}
		}
		gVrCheatCycleDown = cheatCycle;

		const bool select = in.a || in.rightStickClick;
		const bool selectPulse = select && !gVrMenuSelectDown;
		gVrMenuSelectDown = select;
		const bool repeatValue = CurrentMenuValueRepeats();
		const bool increasePulse = MenuRepeatPulse(
			in.rightTrigger >= 0.55f, gVrMenuIncreaseDown,
			gVrMenuIncreaseRepeatAt, gVrMenuIncreaseHoldStart,
			now, repeatValue);
		const bool decreasePulse = MenuRepeatPulse(
			in.leftTrigger >= 0.55f, gVrMenuDecreaseDown,
			gVrMenuDecreaseRepeatAt, gVrMenuDecreaseHoldStart,
			now, repeatValue);
		const int repeatMagnitude = decreasePulse ?
			MenuRepeatMagnitude(in.leftTrigger >= 0.55f,
				gVrMenuDecreaseHoldStart, now) :
			MenuRepeatMagnitude(in.rightTrigger >= 0.55f,
				gVrMenuIncreaseHoldStart, now);
		const bool positivePulse = selectPulse || increasePulse;

		if(gVrMenuPage == VR_MENU_PAGE_SETTINGS &&
		   (positivePulse || decreasePulse)){
			if(gVrMenuSelection == VR_MAIN_GRAPHICS){
				gVrMenuPage = VR_MENU_PAGE_GRAPHICS;
				gVrGraphicsSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_HUD){
				gVrMenuPage = VR_MENU_PAGE_HUD;
				gVrHudSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_TRAFFIC_SETTINGS){
				gVrMenuPage = VR_MENU_PAGE_TRAFFIC;
				gVrTrafficSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_MODEL_ASSETS){
				gVrMenuPage = VR_MENU_PAGE_MODEL_ASSETS;
				gVrModelAssetsSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_VEHICLE_SETTINGS){
				gVrMenuPage = VR_MENU_PAGE_VEHICLE;
				gVrVehicleSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_LOCOMOTION_SETTINGS){
				gVrMenuPage = VR_MENU_PAGE_LOCOMOTION;
				gVrLocomotionSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_WEAPONS){
				gVrMenuPage = VR_MENU_PAGE_WEAPONS;
				gVrWeaponsSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_HOLSTERS){
				gVrMenuPage = VR_MENU_PAGE_HOLSTERS;
				gVrHolsterSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_CHEATS){
				gVrMenuPage = VR_MENU_PAGE_CHEATS;
				gVrCheatStatusFrames = 0;
			}else if(gVrMenuSelection == VR_MAIN_ABOUT){
				gVrMenuPage = VR_MENU_PAGE_ABOUT;
				gVrWelcomeFirstRun = false;
				gVrWelcomePending = false;
				gVrAboutDismissArmed = false;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_GRAPHICS &&
		         (positivePulse || decreasePulse)){
			if(gVrGraphicsSelection == VR_GRAPHICS_RENDER_SCALE){
				static const int scales[] = { 100, 125, 150, 175 };
				int index = 0;
				for(int i = 0; i < (int)ARRAY_SIZE(scales); i++)
					if(scales[i] == gQuestRenderScalePercent)
						index = i;
				const int direction = decreasePulse ? -1 : 1;
				index = (index+direction+(int)ARRAY_SIZE(scales)) %
					(int)ARRAY_SIZE(scales);
				gQuestRenderScalePercent = scales[index];
				SaveVrInteger("RenderScalePercent", gQuestRenderScalePercent);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_SGSR){
				// Reserved for a future stereo-stable temporal AA path.
			}else if(gVrGraphicsSelection == VR_GRAPHICS_MSAA){
				gQuestMsaaSamples = 1;
				SaveVrInteger("MsaaSamples", gQuestMsaaSamples);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_FXAA){
				gSpatialAaMode = !gSpatialAaMode;
				SaveFxaa();
			}else if(gVrGraphicsSelection == VR_GRAPHICS_COLOR){
				gViceCityColorEnabled = !gViceCityColorEnabled;
				SaveViceCityColor();
			}else if(gVrGraphicsSelection == VR_GRAPHICS_PROFILER){
				QuestProfilerSetEnabled(!QuestProfilerIsEnabled());
			}else if(gVrGraphicsSelection == VR_GRAPHICS_CPU_PERFORMANCE){
				if(xrvk::isPerformanceModeSupported()){
					const int direction = decreasePulse ? -1 : 1;
					gQuestCpuPerformanceMode =
						(gQuestCpuPerformanceMode+direction+
						 QUEST_CPU_PERFORMANCE_COUNT)%
						QUEST_CPU_PERFORMANCE_COUNT;
					if(gQuestCpuPerformanceMode <=
					   QUEST_CPU_PERFORMANCE_SUSTAINED){
						gQuestCpuPerformanceSavedMode =
							gQuestCpuPerformanceMode;
						SaveVrInteger("CpuPerformanceMode",
							gQuestCpuPerformanceSavedMode);
					}
					xrvk::setPerformanceMode(gQuestCpuPerformanceMode);
				}
			}else if(gVrGraphicsSelection == VR_GRAPHICS_GPU_PERFORMANCE){
				if(xrvk::isPerformanceModeSupported()){
					const int direction = decreasePulse ? -1 : 1;
					gQuestGpuPerformanceMode =
						(gQuestGpuPerformanceMode+direction+
						 QUEST_CPU_PERFORMANCE_COUNT)%
						QUEST_CPU_PERFORMANCE_COUNT;
					SaveVrInteger("GpuPerformanceMode",
					              gQuestGpuPerformanceMode);
					xrvk::setGpuPerformanceMode(gQuestGpuPerformanceMode);
				}
			}else if(gVrGraphicsSelection == VR_GRAPHICS_SHADOWS){
				CShadows::SetRenderEnabled(!CShadows::IsRenderEnabled());
				SaveVrInteger("ShadowsEnabled",
					CShadows::IsRenderEnabled() ? 1 : 0);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_OCCLUSION){
				const int direction = decreasePulse ? -1 : 1;
				gOcclusionCullingMode =
					(gOcclusionCullingMode+direction+
					 VR_OCCLUSION_CULLING_MODE_COUNT)%
					VR_OCCLUSION_CULLING_MODE_COUNT;
				CRenderer::SetVrOcclusionCullingMode(gOcclusionCullingMode);
				SaveVrInteger("OcclusionCullingModeV2", gOcclusionCullingMode);
				// Keep older builds able to honour the explicit OFF fallback.
				SaveVrInteger("OcclusionCulling",
					gOcclusionCullingMode != VR_OCCLUSION_CULLING_OFF ? 1 : 0);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_FOUNTAIN){
				const int direction = decreasePulse ? -1 : 1;
				const int quality =
					(CParticleObject::GetVrFountainQuality()+direction+
					 VR_FOUNTAIN_QUALITY_COUNT)%VR_FOUNTAIN_QUALITY_COUNT;
				CParticleObject::SetVrFountainQuality(quality);
				SaveVrInteger("FountainQuality", quality);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_QUICK_START){
				gQuestQuickTestStart = !gQuestQuickTestStart;
				SaveVrInteger("QuickTestStart",
					gQuestQuickTestStart ? 1 : 0);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_BACK){
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_WEAPONS &&
		         (positivePulse || decreasePulse)){
			const int weaponSetting =
				QuestWeaponSettingForWeaponItem(gVrWeaponsSelection);
			if(weaponSetting >= 0 && weaponSetting <
			   OculusVR::GetQuestWeaponSettingCount())
				OculusVR::ToggleQuestWeaponSetting(weaponSetting);
			else if(gVrWeaponsSelection == VR_WEAPONS_CALIBRATION){
				gVrMenuPage = VR_MENU_PAGE_CALIBRATION;
				gVrCalibrationSelection = 0;
			}else if(gVrWeaponsSelection == VR_WEAPONS_BACK){
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_MODEL_ASSETS &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			if(gVrModelAssetsSelection == VR_MODEL_ASSETS_PRESET){
				ModelSets::CycleRequested(direction);
				const ModelSets::eModelSet preset = ModelSets::GetRequested();
				for(int category = 0; category < ModelSets::MODEL_CATEGORY_COUNT;
				    category++){
					const ModelSets::eModelSet requested =
						preset == ModelSets::MODEL_SET_MODERN &&
						(category == ModelSets::MODEL_CATEGORY_WORLD ||
						 category == ModelSets::MODEL_CATEGORY_WEAPONS) ?
						ModelSets::MODEL_SET_MODERN : ModelSets::MODEL_SET_CLASSIC;
					ModelSets::SetRequestedForCategory(
						(ModelSets::eModelCategory)category, requested);
				}
			}else if(gVrModelAssetsSelection >= VR_MODEL_ASSETS_WORLD &&
			         gVrModelAssetsSelection <= VR_MODEL_ASSETS_WEAPONS){
				ModelSets::CycleRequestedCategory(
					(ModelSets::eModelCategory)(gVrModelAssetsSelection-1),
					direction);
			}else if(gVrModelAssetsSelection == VR_MODEL_ASSETS_BACK &&
			         positivePulse)
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		}else if(gVrMenuPage == VR_MENU_PAGE_TRAFFIC &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			switch(gVrTrafficSelection){
			case VR_TRAFFIC_PEDESTRIANS:
				gTrafficPedPercent = Min(Max(
					gTrafficPedPercent+direction*5, 50), 300);
				SaveVrInteger("PedTrafficPercent",
					gTrafficPedPercent);
				ApplyTrafficSettings();
				break;
			case VR_TRAFFIC_VEHICLES:
				gTrafficCarPercent = Min(Max(
					gTrafficCarPercent+direction*5, 50), 300);
				SaveVrInteger("CarTrafficPercent",
					gTrafficCarPercent);
				ApplyTrafficSettings();
				break;
			case VR_TRAFFIC_PHYSICS_DIRECTOR: {
				const int mode =
					(QuestPhysicsDirectorGetMode()+direction+
					 QUEST_PHYSICS_DIRECTOR_MODE_COUNT)%
					QUEST_PHYSICS_DIRECTOR_MODE_COUNT;
				QuestPhysicsDirectorSetMode(mode);
				SaveVrInteger("PhysicsDirectorMode", mode);
				break;
			}
			case VR_TRAFFIC_PHYSICS_PRESET: {
				const int preset =
					(QuestPhysicsDirectorGetPreset()+direction+
					 QUEST_PHYSICS_PRESET_COUNT)%
					QUEST_PHYSICS_PRESET_COUNT;
				QuestPhysicsDirectorSetPreset(preset);
				SaveVrInteger("PhysicsDirectorPreset", preset);
				break;
			}
			case VR_TRAFFIC_VISUAL_BUDGET: {
				const int mode =
					(QuestVehicleVisualBudgetGetMode()+direction+
					 QUEST_VEHICLE_VISUAL_MODE_COUNT)%
					QUEST_VEHICLE_VISUAL_MODE_COUNT;
				QuestVehicleVisualBudgetSetMode(mode);
				SaveVrInteger("VehicleVisualBudgetMode", mode);
				break;
			}
			case VR_TRAFFIC_DEFAULTS:
				gTrafficPedPercent = 135;
				gTrafficCarPercent = 135;
				QuestPhysicsDirectorSetMode(
					QUEST_PHYSICS_DIRECTOR_ADAPTIVE);
				QuestPhysicsDirectorSetPreset(
					QUEST_PHYSICS_PRESET_QUALITY);
				QuestVehicleVisualBudgetSetMode(
					QUEST_VEHICLE_VISUAL_STOCK);
				SaveVrInteger("PedTrafficPercent",
					gTrafficPedPercent);
				SaveVrInteger("CarTrafficPercent",
					gTrafficCarPercent);
				SaveVrInteger("PhysicsDirectorMode",
					QUEST_PHYSICS_DIRECTOR_ADAPTIVE);
				SaveVrInteger("PhysicsDirectorPreset",
					QUEST_PHYSICS_PRESET_QUALITY);
				SaveVrInteger("VehicleVisualBudgetMode",
					QUEST_VEHICLE_VISUAL_STOCK);
				ApplyTrafficSettings();
				break;
			case VR_TRAFFIC_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_HUD &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			switch(gVrHudSelection){
			case VR_HUD_ENABLED:
				gGameplayHudEnabled = !gGameplayHudEnabled;
				SaveGameplayHud();
				break;
			case VR_HUD_HORIZONTAL_SCALE:
				gHudWidthPercent = Min(Max(
					gHudWidthPercent+direction*5, 50), 200);
				SaveVrInteger("HudWidthPercent", gHudWidthPercent);
				break;
			case VR_HUD_SCALE:
				gHudScalePercent = Min(Max(
					gHudScalePercent+direction*5, 50), 200);
				SaveVrInteger("HudScalePercent", gHudScalePercent);
				break;
			case VR_HUD_OFFSET_X:
				gHudOffsetXCm = Min(Max(
					gHudOffsetXCm+direction, -100), 100);
				SaveVrInteger("HudOffsetXCm", gHudOffsetXCm);
				break;
			case VR_HUD_OFFSET_Y:
				gHudOffsetYCm = Min(Max(
					gHudOffsetYCm+direction, -100), 100);
				SaveVrInteger("HudOffsetYCm", gHudOffsetYCm);
				break;
			case VR_HUD_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_VEHICLE &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			switch(gVrVehicleSelection){
			case VR_VEHICLE_CAR_DRIVING_TYPE:
				OculusVR::CycleQuestCarDrivingType(direction);
				break;
			case VR_VEHICLE_BIKE_DRIVING_TYPE:
				OculusVR::CycleQuestBikeDrivingType(direction);
				break;
			case VR_VEHICLE_DEFAULT_SEAT_HEIGHT:
				OculusVR::AdjustQuestDefaultVehicleSeatHeightCm(
					direction*repeatMagnitude);
				break;
			case VR_VEHICLE_DEFAULT_SEAT_FORWARD:
				OculusVR::AdjustQuestDefaultVehicleSeatDistanceCm(
					direction*repeatMagnitude);
				break;
			case VR_VEHICLE_GLOBAL_SEAT_HEIGHT:
				if(!OculusVR::HasQuestDefaultVehicleViewOffsetTarget())
					OculusVR::AdjustQuestVehicleGlobalSeatHeightCm(
						direction*repeatMagnitude);
				break;
			case VR_VEHICLE_GLOBAL_SEAT_FORWARD:
				if(!OculusVR::HasQuestDefaultVehicleViewOffsetTarget())
					OculusVR::AdjustQuestVehicleGlobalSeatDistanceCm(
						direction*repeatMagnitude);
				break;
			case VR_VEHICLE_MODEL_SEAT_HEIGHT:
				if(!OculusVR::HasQuestDefaultVehicleViewOffsetTarget())
					OculusVR::AdjustQuestVehicleModelSeatHeightCm(
						direction*repeatMagnitude);
				break;
			case VR_VEHICLE_MODEL_SEAT_FORWARD:
				if(!OculusVR::HasQuestDefaultVehicleViewOffsetTarget())
					OculusVR::AdjustQuestVehicleModelSeatDistanceCm(
						direction*repeatMagnitude);
				break;
			case VR_VEHICLE_MOTION_HAND:
				OculusVR::ToggleQuestMotionSteeringHand();
				break;
			case VR_VEHICLE_WHEEL_VISIBLE:
				OculusVR::ToggleQuestImmersiveCarWheelVisible();
				break;
			case VR_VEHICLE_MODEL_WHEEL_VISIBLE:
				OculusVR::ToggleQuestVehicleModelWheelVisibility();
				break;
			case VR_VEHICLE_HANDLE_HIGHLIGHTS:
				OculusVR::ToggleQuestVehicleHandleHighlights();
				break;
			case VR_VEHICLE_BIKE_LOCK_HORIZON:
				OculusVR::ToggleQuestBikeHorizonLock();
				break;
			case VR_VEHICLE_CALIBRATION:
				if(OculusVR::IsQuestVehicleCalibrationAvailable()){
					gVrMenuPage =
						VR_MENU_PAGE_VEHICLE_CALIBRATION;
					gVrVehicleCalibrationSelection = 0;
					gVrVehicleCalibrationHand = 0;
					OculusVR::SetQuestVehicleCalibrationPreview(
						true);
				}
				break;
			case VR_VEHICLE_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage ==
		         VR_MENU_PAGE_VEHICLE_CALIBRATION &&
		         (positivePulse || decreasePulse)){
			const int valueCount =
				OculusVR::GetQuestVehicleCalibrationItemCount();
			const int direction = decreasePulse ? -1 : 1;
			const int item = OculusVR::GetQuestVehicleCalibrationItemForRow(
				gVrVehicleCalibrationSelection);
			if(item == OculusVR::QUEST_VEHICLE_CAL_HAND)
				gVrVehicleCalibrationHand =
					1-gVrVehicleCalibrationHand;
			else if(gVrVehicleCalibrationSelection < valueCount)
				OculusVR::AdjustQuestVehicleCalibrationValue(
					gVrVehicleCalibrationHand,
					item,
					direction*repeatMagnitude);
			else{
				OculusVR::SetQuestVehicleCalibrationPreview(
					false);
				gVrMenuPage = VR_MENU_PAGE_VEHICLE;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_LOCOMOTION &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			switch(gVrLocomotionSelection){
			case VR_LOCOMOTION_MOVEMENT_ORIENTATION:
				gQuestMovementOrientation =
					(gQuestMovementOrientation+
					 QUEST_MOVEMENT_ORIENTATION_COUNT+direction) %
					QUEST_MOVEMENT_ORIENTATION_COUNT;
				SaveVrInteger("MovementOrientation",
					gQuestMovementOrientation);
				break;
			case VR_LOCOMOTION_TURN_MODE:
				gQuestTurnMode =
					(gQuestTurnMode+QUEST_TURN_MODE_COUNT+
					 direction) % QUEST_TURN_MODE_COUNT;
				SaveVrInteger("TurnMode", gQuestTurnMode);
				break;
			case VR_LOCOMOTION_TURN_SENSITIVITY:
				if(gQuestMovementOrientation ==
				   QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL){
					gQuestHeadSteeringSensitivityPercent = Min(Max(
						gQuestHeadSteeringSensitivityPercent+
						 direction*5, 25), 300);
					SaveVrInteger("HeadSteeringSensitivityPercent",
						gQuestHeadSteeringSensitivityPercent);
				}else{
					gQuestTurnSensitivityPercent = Min(Max(
						gQuestTurnSensitivityPercent+
						 direction*5, 25), 300);
					SaveVrInteger("TurnSensitivityPercent",
						gQuestTurnSensitivityPercent);
				}
				break;
			case VR_LOCOMOTION_HEAD_BOBBING:
				gQuestHeadBobbing = !gQuestHeadBobbing;
				SaveVrInteger("HeadBobbing", gQuestHeadBobbing);
				break;
			case VR_LOCOMOTION_REFRESH_RATE: {
				static const int rates[] = { 72, 80, 90, 120 };
				const int count =
					(int)(sizeof(rates)/sizeof(rates[0]));
				int index = 0;
				for(int i = 0; i < count; i++)
					if(rates[i] == gQuestRefreshRateHz)
						index = i;
				index = (index + direction + count) % count;
				gQuestRefreshRateHz = rates[index];
				SaveVrInteger("RefreshRate", gQuestRefreshRateHz);
				androidgame::SetPreferredRefreshRate(
					gQuestRefreshRateHz);
				break;
			}
			case VR_LOCOMOTION_SNAP_ANGLE:
				gQuestSnapTurnAngleDegrees += direction*15;
				if(gQuestSnapTurnAngleDegrees < 15)
					gQuestSnapTurnAngleDegrees = 90;
				if(gQuestSnapTurnAngleDegrees > 90)
					gQuestSnapTurnAngleDegrees = 15;
				SaveVrInteger("SnapTurnAngleDegrees",
					gQuestSnapTurnAngleDegrees);
				break;
			case VR_LOCOMOTION_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_CALIBRATION){
			if(gVrCalibrationSelection == 0 &&
			   (positivePulse || decreasePulse))
				gVrCalibrationHand = 1-gVrCalibrationHand;
			else if(gVrCalibrationSelection >= 1 &&
			        gVrCalibrationSelection <= 19 &&
			        (positivePulse || decreasePulse)){
				const int weaponType =
					OculusVR::GetQuestCalibrationWeaponType(
						gVrCalibrationHand);
				OculusVR::AdjustQuestCalibrationValue(
					gVrCalibrationHand, weaponType,
					gVrCalibrationSelection-1,
					(decreasePulse ? -1 : 1)*repeatMagnitude);
			}else if(gVrCalibrationSelection == 20 &&
			         positivePulse)
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		}else if(gVrMenuPage == VR_MENU_PAGE_HOLSTERS){
			const int points = OculusVR::GetQuestHolsterPointCount();
			if(gVrHolsterSelection < points &&
			   (positivePulse || decreasePulse))
				OculusVR::CycleQuestHolsterPointSlot(
					gVrHolsterSelection,
					decreasePulse ? -1 : 1);
			else if(gVrHolsterSelection == points && positivePulse)
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		}else if(gVrMenuPage == VR_MENU_PAGE_CHEATS){
			if((increasePulse || decreasePulse) && !selectPulse &&
			   gVrCheatSelection > 0)
				CycleVrCheatSelection(gVrCheatSelection-1,
					decreasePulse ? -1 : 1);
			else if(selectPulse){
				if(gVrCheatSelection == 0){
					gVrMenuPage = VR_MENU_PAGE_MISSIONS;
					gVrMissionCategory = -1;
					gVrMissionCategorySelection = 0;
					gVrMissionSelection = 0;
				}else{
					const bool activated = ActivateVrCheat(
						gVrCheatSelection-1);
					snprintf(gVrCheatStatus, sizeof(gVrCheatStatus),
						"%s", activated ? "CHEAT ACTIVATED" :
						"UNAVAILABLE RIGHT NOW");
					gVrCheatStatusFrames = 120;
				}
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_MISSIONS && selectPulse){
			if(gVrMissionCategory < 0){
				gVrMissionCategory = gVrMissionCategorySelection;
				gVrMissionSelection = 0;
			}else if(ActivateVrMission(gVrMissionCategory,
			          gVrMissionSelection)){
				gVrMenuVisible = false;
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			}else{
				snprintf(gVrCheatStatus, sizeof(gVrCheatStatus),
					"MISSION NOT SAFE RIGHT NOW");
				gVrCheatStatusFrames = 120;
			}
		}

		const bool back = in.b || in.leftStickClick;
		if(back && !gVrMenuBackDown)
			ReturnFromCurrentMenuPage();
		gVrMenuBackDown = back;
		if(gVrCheatStatusFrames > 0)
			gVrCheatStatusFrames--;
	}else{
		gVrMenuIncreaseDown = false;
		gVrMenuDecreaseDown = false;
		gVrCheatCycleDown = false;
	}

	if(gDebugPreviousFrameMs > 0.0){
		const float frameMs = (float)(now-gDebugPreviousFrameMs);
		gDebugSmoothedFrameMs = gDebugSmoothedFrameMs > 0.0f ?
			gDebugSmoothedFrameMs*0.9f+frameMs*0.1f : frameMs;
		gDebugFps = gDebugSmoothedFrameMs > 0.0f ?
			(int)(1000.0f/gDebugSmoothedFrameMs+0.5f) : 0;
	}
	gDebugPreviousFrameMs = now;
}

static void
BeginFullVrMenuPage(const char *heading, const char *subtitle = nil)
{
	FillVrMenuRect(0, 0, VR_MENU_WIDTH, VR_MENU_HEIGHT,
		5, 10, 20, 238);
	FillVrMenuRect(28, 28, VR_MENU_WIDTH-28, VR_MENU_HEIGHT-28,
		10, 22, 38, 245);
	DrawVrMenuText("VICE CITY VR", VR_MENU_WIDTH/2, 55, 7,
		255, 120, 205);
	DrawVrMenuText(heading, VR_MENU_WIDTH/2, 112, 4,
		100, 225, 255);
	if(subtitle && subtitle[0] != '\0')
		DrawVrMenuText(subtitle, VR_MENU_WIDTH/2, 146, 2,
			170, 190, 210);
}

static void
DrawFullVrMenuRow(const char *text, int y, int scale, bool selected,
                  bool available = true, bool warning = false,
                  bool positive = false)
{
	if(selected)
		FillVrMenuRect(85, y-5, VR_MENU_WIDTH-85,
			y+scale*7+4,
			warning ? 125 : (positive ? 20 : (available ? 25 : 55)),
			warning ? 35 : (positive ? 105 : (available ? 95 : 62)),
			warning ? 30 : (positive ? 60 : (available ? 135 : 72)), 245);
	DrawVrMenuText(text, VR_MENU_WIDTH/2, y, scale,
		warning ? 255 : (positive ? (selected ? 210 : 105) :
			(selected ? 255 : (available ? 205 : 120))),
		warning ? (selected ? 225 : 105) :
			(positive ? 245 : (selected ? (available ? 245 : 180) :
			 (available ? 215 : 135))),
		warning ? (selected ? 120 : 95) :
			(positive ? 145 : (selected ? (available ? 110 : 130) :
			 (available ? 225 : 145))));
}

static void
QuestMainCategoryColour(int item, uint8 *red, uint8 *green, uint8 *blue)
{
	static const uint8 colours[VR_MAIN_ITEM_COUNT][3] = {
		{100,225,255}, {125,255,145}, {255,205,90},
		{190,145,255}, {255,155,90}, {90,220,210},
		{255,120,205}, {240,180,105}, {255,105,105},
		{170,190,210}
	};
	*red = colours[item][0]; *green = colours[item][1];
	*blue = colours[item][2];
}

static void
DrawQuestSettingsPage(void)
{
	gQuestCpuPerformanceMode = Min(Max(xrvk::getPerformanceMode(),
		(int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_COUNT-1);
	BeginFullVrMenuPage("SETTINGS");
	char rows[VR_MAIN_ITEM_COUNT][112];
	strcpy(rows[VR_MAIN_GRAPHICS], "GRAPHICS  < OPEN >");
	strcpy(rows[VR_MAIN_TRAFFIC_SETTINGS],
		"TRAFFIC  < OPEN >");
	strcpy(rows[VR_MAIN_HUD], "HUD  < OPEN >");
	strcpy(rows[VR_MAIN_MODEL_ASSETS],
		"MODEL ASSETS  < OPEN >");
	strcpy(rows[VR_MAIN_VEHICLE_SETTINGS],
		"VEHICLE  < OPEN >");
	strcpy(rows[VR_MAIN_LOCOMOTION_SETTINGS],
		"LOCOMOTION  < OPEN >");
	strcpy(rows[VR_MAIN_WEAPONS], "WEAPONS  < OPEN >");
	strcpy(rows[VR_MAIN_HOLSTERS], "HOLSTER LOADOUT  < OPEN >");
	strcpy(rows[VR_MAIN_CHEATS], "CHEAT MENU  < OPEN >");
	strcpy(rows[VR_MAIN_ABOUT], "ABOUT VICE CITY VR  < OPEN >");

	for(int item = 0; item < VR_MAIN_ITEM_COUNT; item++){
		DrawFullVrMenuRow(rows[item], 160+item*50, 3,
			item == gVrMenuSelection, true);
		uint8 red, green, blue;
		QuestMainCategoryColour(item, &red, &green, &blue);
		if(item != gVrMenuSelection)
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2,
				160+item*50, 3, red, green, blue);
	}
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B CLOSE",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestGraphicsPage(void)
{
	BeginFullVrMenuPage("GRAPHICS", "QUEST RENDERING AND PERFORMANCE");
	xrvk::RenderScaleStatus scaleStatus = {};
	const bool scaleStatusValid = xrvk::getRenderScaleStatus(&scaleStatus);
	char rows[VR_GRAPHICS_ITEM_COUNT][112];
	if(scaleStatusValid)
		snprintf(rows[VR_GRAPHICS_RENDER_SCALE], sizeof(rows[0]),
			"OUTPUT REQUEST < %d%% >  ACTIVE %.0f%% - RESTART",
			gQuestRenderScalePercent, scaleStatus.effectivePercent);
	else
		snprintf(rows[VR_GRAPHICS_RENDER_SCALE], sizeof(rows[0]),
			"OUTPUT REQUEST  < %d%% - RESTART >",
			gQuestRenderScalePercent);
	snprintf(rows[VR_GRAPHICS_SGSR], sizeof(rows[0]),
		"TEMPORAL AA < DISABLED - UNSTABLE >");
	snprintf(rows[VR_GRAPHICS_MSAA], sizeof(rows[0]),
		"STEREO MSAA  < DISABLED - NO VISIBLE GAIN >");
	snprintf(rows[VR_GRAPHICS_FXAA], sizeof(rows[0]),
		"SPATIAL AA  < %s >", gSpatialAaMode ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_COLOR], sizeof(rows[0]),
		"COLOR FILTER  < %s >", gViceCityColorEnabled ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_PROFILER], sizeof(rows[0]),
		"PERFORMANCE PROFILER  < %s >",
		QuestProfilerIsEnabled() ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_CPU_PERFORMANCE], sizeof(rows[0]),
		"CPU PERFORMANCE < %s > ACTIVE %s",
		QuestCpuPerformanceModeName(gQuestCpuPerformanceMode),
		xrvk::getActivePerformanceMode() >= 0 ?
			QuestCpuPerformanceModeName(xrvk::getActivePerformanceMode()) :
			"PENDING");
	snprintf(rows[VR_GRAPHICS_GPU_PERFORMANCE], sizeof(rows[0]),
		"GPU PERFORMANCE < %s > ACTIVE %s",
		QuestCpuPerformanceModeName(gQuestGpuPerformanceMode),
		xrvk::getActiveGpuPerformanceMode() >= 0 ?
			QuestCpuPerformanceModeName(xrvk::getActiveGpuPerformanceMode()) :
			"PENDING");
	snprintf(rows[VR_GRAPHICS_SHADOWS], sizeof(rows[0]),
		"WORLD SHADOWS  < %s >",
		CShadows::IsRenderEnabled() ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_OCCLUSION], sizeof(rows[0]),
		"OCCLUSION CULLING  < %s >",
		CRenderer::GetVrOcclusionCullingModeName());
	snprintf(rows[VR_GRAPHICS_FOUNTAIN], sizeof(rows[0]),
		"FOUNTAIN PARTICLES  < %s >",
		CParticleObject::GetVrFountainQualityName());
	snprintf(rows[VR_GRAPHICS_QUICK_START], sizeof(rows[0]),
		"QUICK TEST START  < %s - NEXT LAUNCH >",
		gQuestQuickTestStart ? "ON" : "OFF");
	strcpy(rows[VR_GRAPHICS_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_GRAPHICS_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 142+item*33, 3,
			item == gVrGraphicsSelection);
	if(scaleStatusValid){
		char activeScale[192];
		snprintf(activeScale, sizeof(activeScale),
			"OUTPUT ACTIVE %.0f%%  %u X %u  BASE %u X %u%s%s",
			scaleStatus.effectivePercent,
			scaleStatus.actualWidth, scaleStatus.actualHeight,
			scaleStatus.recommendedWidth, scaleStatus.recommendedHeight,
			scaleStatus.fallbackReason !=
				xrvk::RENDER_SCALE_FALLBACK_NONE ? "  FALLBACK: " : "",
			scaleStatus.fallbackReason !=
				xrvk::RENDER_SCALE_FALLBACK_NONE ?
				xrvk::getRenderScaleFallbackReasonName(
					scaleStatus.fallbackReason) : "");
		const bool fallback = scaleStatus.fallbackReason !=
			xrvk::RENDER_SCALE_FALLBACK_NONE;
		DrawVrMenuText(activeScale, VR_MENU_WIDTH/2, 608, 2,
			fallback ? 255 : 120, fallback ? 95 : 220,
			fallback ? 85 : 255);
		DrawVrMenuText("NATIVE SCENE  SPATIAL AA SINGLE-FRAME",
			VR_MENU_WIDTH/2, 630, 2, 170, 190, 210);
		if(scaleStatus.previousFallbackReason !=
		   xrvk::RENDER_SCALE_FALLBACK_NONE){
			char recoveredScale[192];
			snprintf(recoveredScale, sizeof(recoveredScale),
				"LAST FALLBACK %d%% TO %d%%: %s",
				scaleStatus.previousFallbackRequestedPercent,
				scaleStatus.previousFallbackPercent,
				xrvk::getRenderScaleFallbackReasonName(
					scaleStatus.previousFallbackReason));
			DrawVrMenuText(recoveredScale, VR_MENU_WIDTH/2, 652, 2,
				255, 105, 85);
		}
	}
	if(gOcclusionCullingMode >= VR_OCCLUSION_CULLING_AUTHORED)
		DrawVrMenuText("AUTHORED CULLING IS EXPERIMENTAL: USE STEREO SAFE IF EYES DISAGREE",
			VR_MENU_WIDTH/2, 690, 2, 255, 105, 95);
	else if(gQuestRenderScalePercent >= 150)
		DrawVrMenuText("150/175% IS EXPERIMENTAL: HIGH GPU AND MEMORY LOAD",
			VR_MENU_WIDTH/2, 690, 2, 255, 120, 95);
	else if(gQuestRenderScalePercent > 100)
		DrawVrMenuText("HIGHER SCALE SHARPENS THE IMAGE BUT INCREASES GPU LOAD",
			VR_MENU_WIDTH/2, 690, 2, 255, 175, 95);
}

static void
DrawQuestWeaponsPage(void)
{
	BeginFullVrMenuPage("WEAPONS", "PHYSICAL HANDS, GRIPS AND CALIBRATION");
	char rows[VR_WEAPONS_ITEM_COUNT][112];
	for(int item = VR_WEAPONS_HANDS; item <= VR_WEAPONS_GRIP_LOCK; item++){
		const int setting = QuestWeaponSettingForWeaponItem(item);
		snprintf(rows[item], sizeof(rows[item]), "%s  < %s >",
			OculusVR::GetQuestWeaponSettingName(setting),
			OculusVR::GetQuestWeaponSetting(setting) ? "ON" : "OFF");
	}
	strcpy(rows[VR_WEAPONS_CALIBRATION], "WEAPON CALIBRATION  < OPEN >");
	strcpy(rows[VR_WEAPONS_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_WEAPONS_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 170+item*55, 3,
			item == gVrWeaponsSelection);
}

static void
DrawQuestTrafficPage(void)
{
	BeginFullVrMenuPage("TRAFFIC SETTINGS",
		"LIVE DENSITY - SAFE ENTITIES CONVERGE WITHOUT HARD DELETION");
	char rows[VR_TRAFFIC_ITEM_COUNT][112];
	snprintf(rows[VR_TRAFFIC_PEDESTRIANS], sizeof(rows[0]),
		"PEDESTRIANS  < %d%% >", gTrafficPedPercent);
	snprintf(rows[VR_TRAFFIC_VEHICLES], sizeof(rows[0]),
		"VEHICLES  < %d%% >", gTrafficCarPercent);
	snprintf(rows[VR_TRAFFIC_PHYSICS_DIRECTOR], sizeof(rows[0]),
		"PHYSICS DIRECTOR  < %s >",
		QuestPhysicsDirectorGetModeName());
	snprintf(rows[VR_TRAFFIC_PHYSICS_PRESET], sizeof(rows[0]),
		"PHYSICS PRESET  < %s >",
		QuestPhysicsDirectorGetPresetName());
	snprintf(rows[VR_TRAFFIC_VISUAL_BUDGET], sizeof(rows[0]),
		"MODERN CAR VISUALS  < %s >",
		QuestVehicleVisualBudgetGetModeName());
	strcpy(rows[VR_TRAFFIC_DEFAULTS],
		"RESTORE DEFAULTS  < 135% / MEASURE / BALANCED / STOCK >");
	strcpy(rows[VR_TRAFFIC_BACK], "BACK TO SETTINGS");
	const bool modernVehicles = ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_MODERN;
	for(int item = 0; item < VR_TRAFFIC_ITEM_COUNT; item++){
		const bool gpuWarning = modernVehicles &&
			(item == VR_TRAFFIC_VEHICLES ||
			 item == VR_TRAFFIC_VISUAL_BUDGET);
		DrawFullVrMenuRow(rows[item], 166+item*40, 2,
			item == gVrTrafficSelection, true, gpuWarning);
	}

	char status[112];
	const QuestPhysicsDirectorSnapshot physics =
		QuestPhysicsDirectorGetSnapshot();
	if(QuestPhysicsDirectorGetMode() == QUEST_PHYSICS_DIRECTOR_MEASURE &&
	   !QuestProfilerIsEnabled())
		strcpy(status, "ENABLE PROFILER FOR PHYSICS TIERS");
	else
		snprintf(status, sizeof(status),
			"%s  F/R/R/P %d/%d/%d/%d  %.2f/%.2fMS",
			QuestPhysicsDirectorGetStatusName(),
			physics.tierCount[QUEST_VEHICLE_PHYSICS_FULL],
			physics.tierCount[QUEST_VEHICLE_PHYSICS_REDUCED],
			physics.tierCount[QUEST_VEHICLE_PHYSICS_RAIL],
			physics.tierCount[QUEST_VEHICLE_PHYSICS_PROXY],
			physics.managedAverageMs, physics.budgetMs);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 455, 2,
		125, 210, 255);
	const QuestVehicleVisualBudgetSnapshot visual =
		QuestVehicleVisualBudgetGetSnapshot();
	if(visual.mode != QUEST_VEHICLE_VISUAL_STOCK)
		DrawVrMenuText("FORCED VLO CAN POP IN VR - STOCK IS RECOMMENDED",
			VR_MENU_WIDTH/2, 680, 2, 255, 105, 95);
	snprintf(status, sizeof(status),
		"VIS VHI/VLO %llu / %llu   SKIP/OCC %llu / %llu",
		(unsigned long long)visual.highVehicleSubmissions,
		(unsigned long long)visual.vloVehicleSubmissions,
		(unsigned long long)visual.atomicsSkipped,
		(unsigned long long)visual.occupantsSkipped);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 485, 2,
		255, 190, 115);
	snprintf(status, sizeof(status),
		"WALKERS %u / %.1f   CAP %d",
		CPopulation::ms_nTotalPeds, CPopulation::VrTargetAmbientPeds,
		CGame::IsInInterior() ?
			CPopulation::MaxNumberOfPedsInUseInterior :
			CPopulation::MaxNumberOfPedsInUse);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 520, 3,
		125, 255, 145);
	CPedPool *pedPool = CPools::GetPedPool();
	snprintf(status, sizeof(status), "PED POOL %d / %d",
		pedPool != nil ? pedPool->GetNoOfUsedSpaces() : 0,
		pedPool != nil ? pedPool->GetSize() : 0);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 560, 3,
		125, 255, 145);
	snprintf(status, sizeof(status),
		"CARS %d + %d PROXY   LOCAL %.1f / %.1f",
		CCarCtrl::NumVrEffectiveAmbient,
		CCarCtrl::NumVrActiveProxies,
		CCarCtrl::VrLocalServed, CCarCtrl::VrLocalDesired);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 600, 3,
		125, 255, 145);
	DrawVrMenuText("RANGES 50-300%   STEP 5%",
		VR_MENU_WIDTH/2, 645, 2, 255, 180, 225);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestModelAssetsPage(void)
{
	BeginFullVrMenuPage("MODEL ASSETS",
		"MIX CLASSIC AND MODERN CONTENT - FULL APP RESTART REQUIRED");
	char rows[VR_MODEL_ASSETS_ITEM_COUNT][112];
	snprintf(rows[VR_MODEL_ASSETS_PRESET], sizeof(rows[0]),
		"RECOMMENDED PRESET  < %s >",
		ModelSets::GetSourceName(ModelSets::GetRequested()));
	for(int category = 0; category < ModelSets::MODEL_CATEGORY_COUNT;
	    category++){
		const bool available = ModelSets::IsCategoryAvailable(
			(ModelSets::eModelCategory)category);
		snprintf(rows[category+1], sizeof(rows[0]), "%s  < %s >%s%s",
			ModelSets::GetCategoryName((ModelSets::eModelCategory)category),
			ModelSets::GetSourceName(ModelSets::GetRequestedForCategory(
				(ModelSets::eModelCategory)category)),
			available ? "" : "  UNAVAILABLE",
			ModelSets::IsCategoryRestartRequired(
				(ModelSets::eModelCategory)category) ? "  RESTART" : "");
	}
	strcpy(rows[VR_MODEL_ASSETS_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_MODEL_ASSETS_ITEM_COUNT; item++){
		const bool available = item == VR_MODEL_ASSETS_PRESET ||
			item == VR_MODEL_ASSETS_BACK ||
			ModelSets::IsCategoryAvailable(
				(ModelSets::eModelCategory)(item-1));
		const bool modernVehicleWarning =
			item == VR_MODEL_ASSETS_VEHICLES &&
			ModelSets::GetRequestedForCategory(
				ModelSets::MODEL_CATEGORY_VEHICLES) ==
				ModelSets::MODEL_SET_MODERN;
		DrawFullVrMenuRow(rows[item], 210+item*42, 2,
			item == gVrModelAssetsSelection, available,
			modernVehicleWarning);
	}
	if(ModelSets::GetRequestedForCategory(
	   ModelSets::MODEL_CATEGORY_VEHICLES) ==
	   ModelSets::MODEL_SET_MODERN){
		DrawVrMenuText("WARNING: MODERN VEHICLES ARE VERY GPU HEAVY",
			VR_MENU_WIDTH/2, 630, 2, 255, 95, 85);
		DrawVrMenuText("USE CLASSIC VEHICLES OR REDUCE 300% TRAFFIC IF FPS DROPS",
			VR_MENU_WIDTH/2, 660, 2, 255, 155, 105);
	}else
		DrawVrMenuText("CLASSIC VEGETATION IS DEFAULT FOR QUEST PERFORMANCE",
			VR_MENU_WIDTH/2, 650, 2, 245, 205, 90);
}

static void
DrawQuestHudPage(void)
{
	BeginFullVrMenuPage("HUD SETTINGS",
		"HEAD-LOCKED GAMEPLAY INTERFACE");
	char rows[VR_HUD_ITEM_COUNT][112];
	snprintf(rows[VR_HUD_ENABLED], sizeof(rows[0]),
		"GAMEPLAY HUD  < %s >", gGameplayHudEnabled ? "ON" : "OFF");
	snprintf(rows[VR_HUD_HORIZONTAL_SCALE], sizeof(rows[0]),
		"HORIZONTAL WIDTH  < %d%% >", gHudWidthPercent);
	snprintf(rows[VR_HUD_SCALE], sizeof(rows[0]),
		"UNIFORM SCALE  < %d%% >", gHudScalePercent);
	snprintf(rows[VR_HUD_OFFSET_X], sizeof(rows[0]),
		"HORIZONTAL OFFSET  < %+d CM >", gHudOffsetXCm);
	snprintf(rows[VR_HUD_OFFSET_Y], sizeof(rows[0]),
		"VERTICAL OFFSET  < %+d CM >", gHudOffsetYCm);
	strcpy(rows[VR_HUD_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_HUD_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 190+item*68, 3,
			item == gVrHudSelection);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestVehiclePage(void)
{
	BeginFullVrMenuPage("VEHICLE SETTINGS",
		"DRIVING CONTROLS AND PER-VEHICLE CALIBRATION");
	char rows[VR_VEHICLE_ITEM_COUNT][112];
	snprintf(rows[VR_VEHICLE_CAR_DRIVING_TYPE], sizeof(rows[0]),
		"CAR DRIVING TYPE  < %s >", OculusVR::GetQuestCarDrivingTypeName());
	snprintf(rows[VR_VEHICLE_BIKE_DRIVING_TYPE], sizeof(rows[0]),
		"BIKE DRIVING TYPE  < %s >", OculusVR::GetQuestBikeDrivingTypeName());
	if(OculusVR::HasQuestDefaultVehicleViewOffsetTarget()){
		snprintf(rows[VR_VEHICLE_DEFAULT_SEAT_HEIGHT], sizeof(rows[0]),
			"DEFAULT %s HEIGHT  < %+d CM >",
			OculusVR::GetQuestDefaultVehicleViewOffsetName(),
			OculusVR::GetQuestDefaultVehicleSeatHeightCm());
		snprintf(rows[VR_VEHICLE_DEFAULT_SEAT_FORWARD], sizeof(rows[0]),
			"DEFAULT %s FORWARD  < %+d CM >",
			OculusVR::GetQuestDefaultVehicleViewOffsetName(),
			OculusVR::GetQuestDefaultVehicleSeatDistanceCm());
	}else{
		strcpy(rows[VR_VEHICLE_DEFAULT_SEAT_HEIGHT],
			"DEFAULT VIEW HEIGHT  < ENTER DEFAULT CAR / BIKE >");
		strcpy(rows[VR_VEHICLE_DEFAULT_SEAT_FORWARD],
			"DEFAULT VIEW FORWARD  < ENTER DEFAULT CAR / BIKE >");
	}
	if(OculusVR::HasQuestVehicleSeatCalibrationTarget() &&
	   !OculusVR::HasQuestDefaultVehicleViewOffsetTarget()){
		snprintf(rows[VR_VEHICLE_GLOBAL_SEAT_HEIGHT], sizeof(rows[0]),
			"%s GLOBAL HEIGHT  < %+d CM >", OculusVR::GetQuestVehicleCategoryName(),
			OculusVR::GetQuestVehicleGlobalSeatHeightCm());
		snprintf(rows[VR_VEHICLE_GLOBAL_SEAT_FORWARD], sizeof(rows[0]),
			"%s GLOBAL FORWARD  < %+d CM >", OculusVR::GetQuestVehicleCategoryName(),
			OculusVR::GetQuestVehicleGlobalSeatDistanceCm());
		snprintf(rows[VR_VEHICLE_MODEL_SEAT_HEIGHT], sizeof(rows[0]),
			"MODEL HEIGHT  < %+d CM >", OculusVR::GetQuestVehicleModelSeatHeightCm());
		snprintf(rows[VR_VEHICLE_MODEL_SEAT_FORWARD], sizeof(rows[0]),
			"MODEL FORWARD  < %+d CM >", OculusVR::GetQuestVehicleModelSeatDistanceCm());
	}else if(OculusVR::HasQuestDefaultVehicleViewOffsetTarget()){
		strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_HEIGHT],
			"GLOBAL HEIGHT  < IMMERSIVE / MOTION ONLY >");
		strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_FORWARD],
			"GLOBAL FORWARD  < IMMERSIVE / MOTION ONLY >");
		strcpy(rows[VR_VEHICLE_MODEL_SEAT_HEIGHT],
			"MODEL HEIGHT  < IMMERSIVE / MOTION ONLY >");
		strcpy(rows[VR_VEHICLE_MODEL_SEAT_FORWARD],
			"MODEL FORWARD  < IMMERSIVE / MOTION ONLY >");
	}else{
		strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_HEIGHT], "GLOBAL HEIGHT  < ENTER VEHICLE >");
		strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_FORWARD], "GLOBAL FORWARD  < ENTER VEHICLE >");
		strcpy(rows[VR_VEHICLE_MODEL_SEAT_HEIGHT], "MODEL HEIGHT  < ENTER VEHICLE >");
		strcpy(rows[VR_VEHICLE_MODEL_SEAT_FORWARD], "MODEL FORWARD  < ENTER VEHICLE >");
	}
	snprintf(rows[VR_VEHICLE_MOTION_HAND], sizeof(rows[0]),
		"MOTION STEERING HAND  < %s >",
		OculusVR::GetQuestMotionSteeringHand() == 0 ?
			"LEFT" : "RIGHT");
	snprintf(rows[VR_VEHICLE_WHEEL_VISIBLE], sizeof(rows[0]),
		"ALL CARS VIRTUAL WHEEL  < %s >",
		OculusVR::IsQuestImmersiveCarWheelVisible() ? "VISIBLE" : "HIDDEN");
	snprintf(rows[VR_VEHICLE_MODEL_WHEEL_VISIBLE], sizeof(rows[0]),
		"THIS MODEL VIRTUAL WHEEL  < %s >",
		OculusVR::GetQuestVehicleModelWheelVisibilityName());
	snprintf(rows[VR_VEHICLE_HANDLE_HIGHLIGHTS], sizeof(rows[0]),
		"VEHICLE GRIP HIGHLIGHTS  < %s >",
		OculusVR::AreQuestVehicleHandleHighlightsEnabled() ?
			"ON" : "OFF");
	snprintf(rows[VR_VEHICLE_BIKE_LOCK_HORIZON], sizeof(rows[0]),
		"BIKE LOCK HORIZON  < %s >",
		OculusVR::IsQuestBikeHorizonLocked() ? "ON" : "OFF");
	snprintf(rows[VR_VEHICLE_CALIBRATION], sizeof(rows[0]),
		"CONTROL CALIBRATION  < %s >",
		OculusVR::IsQuestVehicleCalibrationAvailable() ?
			"OPEN" : "ENTER VEHICLE");
	strcpy(rows[VR_VEHICLE_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_VEHICLE_ITEM_COUNT; item++){
		bool available = true;
		if(item >= VR_VEHICLE_DEFAULT_SEAT_HEIGHT &&
		   item <= VR_VEHICLE_DEFAULT_SEAT_FORWARD)
			available = OculusVR::HasQuestDefaultVehicleViewOffsetTarget();
		else if(item >= VR_VEHICLE_GLOBAL_SEAT_HEIGHT &&
		        item <= VR_VEHICLE_MODEL_SEAT_FORWARD)
			available = OculusVR::HasQuestVehicleSeatCalibrationTarget() &&
				!OculusVR::HasQuestDefaultVehicleViewOffsetTarget();
		DrawFullVrMenuRow(rows[item], 166+item*34, 2,
			item == gVrVehicleSelection, available);
	}
	DrawVrMenuText("VALUES ARE SAVED IN VR SETTINGS",
		VR_MENU_WIDTH/2, 675, 2, 125, 255, 145);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestVehicleCalibrationPage(void)
{
	char heading[128];
	snprintf(heading, sizeof(heading), "VEHICLE CONTROLS - %s",
		OculusVR::GetQuestActiveVehicleName());
	BeginFullVrMenuPage(heading,
		"VALUES ARE SAVED FOR THIS VEHICLE MODEL");
	const int valueCount =
		OculusVR::GetQuestVehicleCalibrationItemCount();
	for(int rowIndex = 0; rowIndex <= valueCount; rowIndex++){
		char row[112];
		const int item = OculusVR::GetQuestVehicleCalibrationItemForRow(rowIndex);
		const char *label = OculusVR::GetQuestVehicleCalibrationItemName(item);
		if(item == OculusVR::QUEST_VEHICLE_CAL_HAND)
			snprintf(row, sizeof(row), "%s  < %s >", label,
				gVrVehicleCalibrationHand == 0 ?
					"LEFT" : "RIGHT");
		else if(rowIndex < valueCount){
			const int value =
				OculusVR::GetQuestVehicleCalibrationValue(
					gVrVehicleCalibrationHand, item);
			if(OculusVR::IsQuestVehicleCalibrationRotation(item))
				snprintf(row, sizeof(row),
					"%s  < %+.1f DEG >", label,
					(float)value/2.0f);
			else if(OculusVR::IsQuestVehicleCalibrationWholeCentimeters(item))
				snprintf(row, sizeof(row), "%s  < %d CM >",
					label, value);
			else
				snprintf(row, sizeof(row),
					"%s  < %+.1f CM >", label,
					(float)value/2.0f);
		}else
			strcpy(row, "BACK TO VEHICLE SETTINGS");
		DrawFullVrMenuRow(row, 166+rowIndex*29, 2,
			rowIndex == gVrVehicleCalibrationSelection,
			true);
	}
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestLocomotionPage(void)
{
	BeginFullVrMenuPage("LOCOMOTION",
		"MOVEMENT AND COMFORT TURNING");
	char rows[VR_LOCOMOTION_ITEM_COUNT][112];
	snprintf(rows[VR_LOCOMOTION_MOVEMENT_ORIENTATION], sizeof(rows[0]),
		"MOVEMENT DIRECTION  < %s >",
		gQuestMovementOrientation ==
			QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL ?
				"HEAD TURN EXP" :
		gQuestMovementOrientation == QUEST_MOVEMENT_ORIENTATION_HEAD ?
			"HEAD" : "BODY");
	snprintf(rows[VR_LOCOMOTION_TURN_MODE], sizeof(rows[0]),
		"TURNING  < %s >",
		gQuestTurnMode == QUEST_TURN_SNAP ? "SNAP" : "SMOOTH");
	if(gQuestMovementOrientation ==
	   QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL)
		snprintf(rows[VR_LOCOMOTION_TURN_SENSITIVITY], sizeof(rows[0]),
			"HEAD TURN SENSITIVITY  < %d%% >",
			gQuestHeadSteeringSensitivityPercent);
	else
		snprintf(rows[VR_LOCOMOTION_TURN_SENSITIVITY], sizeof(rows[0]),
			"SMOOTH TURN SENSITIVITY  < %d%% >",
			gQuestTurnSensitivityPercent);
	snprintf(rows[VR_LOCOMOTION_SNAP_ANGLE], sizeof(rows[0]),
		"SNAP TURN ANGLE  < %d DEG >", gQuestSnapTurnAngleDegrees);
	snprintf(rows[VR_LOCOMOTION_HEAD_BOBBING], sizeof(rows[0]),
		"WALKING HEAD BOB  < %s >", gQuestHeadBobbing ? "ON" : "OFF");
	snprintf(rows[VR_LOCOMOTION_REFRESH_RATE], sizeof(rows[0]),
		"REFRESH RATE  < %d HZ >", gQuestRefreshRateHz);
	strcpy(rows[VR_LOCOMOTION_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_LOCOMOTION_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 190+item*68, 3,
			item == gVrLocomotionSelection);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestWeaponCalibrationPage(void)
{
	const int weaponType =
		OculusVR::GetQuestCalibrationWeaponType(gVrCalibrationHand);
	char weaponName[48], heading[128];
	CopyMenuText(weaponName, ARRAY_SIZE(weaponName),
		GetVrWeaponName(weaponType));
	snprintf(heading, sizeof(heading),
		"WEAPON CALIBRATION - %s - %s HAND", weaponName,
		gVrCalibrationHand == 0 ? "LEFT" : "RIGHT");
	BeginFullVrMenuPage(heading,
		"VALUES ARE SAVED PER WEAPON AND PER HAND");
	static const char *labels[19] = {
		"AIM X OFFSET", "AIM Y OFFSET", "AIM Z OFFSET",
		"AIM LOCAL ROT X", "AIM LOCAL ROT Y", "AIM LOCAL ROT Z",
		"WEAPON X OFFSET", "WEAPON Y OFFSET", "WEAPON Z OFFSET",
		"WEAPON LOCAL ROT X", "WEAPON LOCAL ROT Y",
		"WEAPON LOCAL ROT Z", "SUPPORT GRIP X",
		"SUPPORT GRIP Y", "SUPPORT GRIP Z",
		"SUPPORT GRIP ROT X", "SUPPORT GRIP ROT Y",
		"SUPPORT GRIP ROT Z", "SUPPORT GRIP STYLE"
	};
	for(int item = 0; item < 21; item++){
		char row[112];
		if(item == 0)
			snprintf(row, sizeof(row), "EDIT HAND  < %s >",
				gVrCalibrationHand == 0 ? "LEFT" : "RIGHT");
		else if(item <= 19){
			const int value =
				OculusVR::GetQuestCalibrationValue(
					gVrCalibrationHand, weaponType, item-1);
			if(item == 19)
				snprintf(row, sizeof(row), "%s  < %s >",
					labels[item-1], value == 1 ?
						"FROM BELOW" : "MAGAZINE");
			else
				snprintf(row, sizeof(row), "%s  < %+.1f %s >",
					labels[item-1], (float)value/2.0f,
					((item >= 4 && item <= 6) ||
					 (item >= 10 && item <= 12) ||
					 (item >= 16 && item <= 18)) ?
						"DEG" : "CM");
		}else
			strcpy(row, "BACK TO SETTINGS");
		DrawFullVrMenuRow(row, 154+item*25, 2,
			item == gVrCalibrationSelection);
	}
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestHolsterPage(void)
{
	BeginFullVrMenuPage("HOLSTER LOADOUT",
		"SEVEN BODY POINTS - CENTER THROWABLE SLOT IS FIXED");
	const int points = OculusVR::GetQuestHolsterPointCount();
	for(int item = 0; item <= points; item++){
		char row[128];
		if(item < points){
			const int slot =
				OculusVR::GetQuestHolsterPointSlot(item);
			const char *slotName = slot < 0 ? "EMPTY" :
				GetVrWeaponName(GetVrWeaponTypeForSlot(slot));
			snprintf(row, sizeof(row), "%s  %c %s %c",
				OculusVR::GetQuestHolsterPointName(item),
				item == 4 ? '[' : '<', slotName,
				item == 4 ? ']' : '>');
		}else
			strcpy(row, "BACK TO SETTINGS");
		DrawFullVrMenuRow(row, 180+item*55, 3,
			item == gVrHolsterSelection);
	}
	DrawVrMenuText(
		"CLEAR THE OLD POINT BEFORE MOVING AN ASSIGNED SLOT",
		VR_MENU_WIDTH/2, 650, 2, 255, 180, 225);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestCheatPage(void)
{
	BeginFullVrMenuPage("CHEATS");
	const int count = GetVrCheatCount()+1;
	const int first = count > 0 ?
		(gVrCheatSelection/VR_CHEAT_ITEMS_PER_PAGE)*
			VR_CHEAT_ITEMS_PER_PAGE : 0;
	for(int row = 0;
	    row < VR_CHEAT_ITEMS_PER_PAGE && first+row < count;
	    row++){
		const int item = first+row;
		const char *name = item == 0 ?
			"MISSION SELECTOR  < OPEN >" : GetVrCheatName(item-1);
		bool toggleEnabled = false;
		const bool isToggle = item > 0 &&
			GetVrCheatToggleState(item-1, &toggleEnabled);
		DrawFullVrMenuRow(name, 158+row*37, 2,
			item == gVrCheatSelection, true,
			isToggle && !toggleEnabled, isToggle && toggleEnabled);
	}
	char page[48];
	const int pageCount = Max(1,
		(count+VR_CHEAT_ITEMS_PER_PAGE-1)/VR_CHEAT_ITEMS_PER_PAGE);
	snprintf(page, sizeof(page), "PAGE %d OF %d",
		first/VR_CHEAT_ITEMS_PER_PAGE+1, pageCount);
	DrawVrMenuText(page, VR_MENU_WIDTH/2, 680, 2,
		120, 220, 255);
	DrawVrMenuText(
		gVrCheatStatusFrames > 0 ? gVrCheatStatus :
			"STICK UP/DOWN SELECT   LEFT/RIGHT MODEL   A ACTIVATE   B BACK",
		VR_MENU_WIDTH/2, 718, 2,
		gVrCheatStatusFrames > 0 ? 255 : 170,
		gVrCheatStatusFrames > 0 ? 220 : 190,
		gVrCheatStatusFrames > 0 ? 80 : 210);
}

static void
DrawQuestMissionPage(void)
{
	BeginFullVrMenuPage("CHEATS / MISSIONS",
		"TEST TOOL - STARTS IN THE CURRENT SAVE STATE");
	if(gVrMissionCategory < 0){
		const int count = GetVrMissionCategoryCount();
		for(int item = 0; item < count; item++){
			char row[112];
			snprintf(row, sizeof(row), "%s  < OPEN >",
				GetVrMissionCategoryName(item));
			DrawFullVrMenuRow(row, 235+item*90, 3,
				item == gVrMissionCategorySelection);
		}
		DrawVrMenuText("MISSION NUMBERS COME FROM THE ACTIVE MAIN.SCM TABLE",
			VR_MENU_WIDTH/2, 610, 2, 255, 180, 225);
		DrawVrMenuText(gVrCheatStatusFrames > 0 ? gVrCheatStatus :
			"UP/DOWN SELECT   A OPEN   B BACK TO CHEATS",
			VR_MENU_WIDTH/2, 718, 2,
			gVrCheatStatusFrames > 0 ? 255 : 170,
			gVrCheatStatusFrames > 0 ? 220 : 190,
			gVrCheatStatusFrames > 0 ? 80 : 210);
		return;
	}

	DrawVrMenuText(GetVrMissionCategoryName(gVrMissionCategory),
		VR_MENU_WIDTH/2, 132, 2, 255, 180, 225);
	const int count = GetVrMissionCount(gVrMissionCategory);
	const int first = (gVrMissionSelection/VR_CHEAT_ITEMS_PER_PAGE)*
		VR_CHEAT_ITEMS_PER_PAGE;
	for(int row = 0; row < VR_CHEAT_ITEMS_PER_PAGE && first+row < count;
	    row++){
		const int item = first+row;
		DrawFullVrMenuRow(GetVrMissionName(gVrMissionCategory, item),
			154+row*34, 2, item == gVrMissionSelection);
	}
	char page[48];
	const int pageCount = Max(1,
		(count+VR_CHEAT_ITEMS_PER_PAGE-1)/VR_CHEAT_ITEMS_PER_PAGE);
	snprintf(page, sizeof(page), "PAGE %d OF %d",
		first/VR_CHEAT_ITEMS_PER_PAGE+1, pageCount);
	DrawVrMenuText(page, VR_MENU_WIDTH/2, 650, 2, 120, 220, 255);
	DrawVrMenuText(gVrCheatStatusFrames > 0 ? gVrCheatStatus :
		"A STARTS THE MISSION   B RETURNS TO CATEGORIES",
		VR_MENU_WIDTH/2, 718, 2,
		gVrCheatStatusFrames > 0 ? 255 : 170,
		gVrCheatStatusFrames > 0 ? 220 : 190,
		gVrCheatStatusFrames > 0 ? 80 : 210);
}

static void
DrawQuestAboutPage(void)
{
	BeginFullVrMenuPage(gVrWelcomeFirstRun ?
		"WELCOME TO VICE CITY VR" : "ABOUT VICE CITY VR",
		"VERSION 0.5.0 ALPHA - NOT FOR SALE");
	static const char *lines[] = {
		"OPEN THE VR MENU: HOLD BOTH GRIPS + MENU",
		"CHEATS: HOLD BOTH GRIPS + B",
		"CHOOSE IMMERSIVE DRIVING IN VEHICLE SETTINGS",
		"CALIBRATE WEAPONS IF A MODEL OR GRIP IS MISALIGNED",
		"MODEL ASSETS CAN MIX CLASSIC AND MODERN CONTENT",
		"CLASSIC VEGETATION IS RECOMMENDED FOR QUEST PERFORMANCE",
		"THE FULL CAMPAIGN IS PLAYABLE, BUT THE MOD IS STILL IN DEVELOPMENT",
		"DISCUSSION: FLAT2VR DISCORD",
		"discord.com/channels/747967102895390741/1529621098751197365",
		"PRESS ANY BUTTON TO CLOSE"
	};
	for(int index = 0; index < (int)ARRAY_SIZE(lines); index++)
		DrawVrMenuText(lines[index], VR_MENU_WIDTH/2, 220+index*40,
			2, index == (int)ARRAY_SIZE(lines)-1 ? 255 : 215,
			index == (int)ARRAY_SIZE(lines)-1 ? 205 : 225,
			index == (int)ARRAY_SIZE(lines)-1 ? 80 : 235);
}

static const char *
SpawnDenyReasonName(int reason)
{
	static const char *const names[16] = {
		"MODEL", "COORS", "DICE", "COLLIDE", "CLOSE", "LANES",
		"NOWHERE", "GROUND", "FAR", "VIEW", "APPROACH", "COPS",
		"OK", "CELL", "POOL", "DEMAND"
	};
	return reason >= 0 && reason < 16 ? names[reason] : "NONE";
}

// Fills the same 1024x768 menu surface as desktop. Compact diagnostics use
// 512x128 for the FPS line and 512x437 for the full profiler.
const unsigned char *
VrDebugPixels(int *width, int *height)
{
	const bool profilerVisible = QuestProfilerIsEnabled();
	*width = gVrMenuVisible ? VR_MENU_WIDTH : VR_DEBUG_WIDTH;
	*height = gVrMenuVisible ? VR_MENU_HEIGHT :
		(profilerVisible ? VR_DEBUG_HEIGHT : VR_FPS_HEIGHT);
	if(!gDebugVisible && !gVrMenuVisible && !profilerVisible)
		return nil;

	if(gVrMenuVisible){
		switch(gVrMenuPage){
		case VR_MENU_PAGE_GRAPHICS:
			DrawQuestGraphicsPage();
			break;
		case VR_MENU_PAGE_WEAPONS:
			DrawQuestWeaponsPage();
			break;
		case VR_MENU_PAGE_HUD:
			DrawQuestHudPage();
			break;
		case VR_MENU_PAGE_TRAFFIC:
			DrawQuestTrafficPage();
			break;
		case VR_MENU_PAGE_MODEL_ASSETS:
			DrawQuestModelAssetsPage();
			break;
		case VR_MENU_PAGE_VEHICLE:
			DrawQuestVehiclePage();
			break;
		case VR_MENU_PAGE_VEHICLE_CALIBRATION:
			DrawQuestVehicleCalibrationPage();
			break;
		case VR_MENU_PAGE_LOCOMOTION:
			DrawQuestLocomotionPage();
			break;
		case VR_MENU_PAGE_CALIBRATION:
			DrawQuestWeaponCalibrationPage();
			break;
		case VR_MENU_PAGE_HOLSTERS:
			DrawQuestHolsterPage();
			break;
		case VR_MENU_PAGE_CHEATS:
			DrawQuestCheatPage();
			break;
		case VR_MENU_PAGE_MISSIONS:
			DrawQuestMissionPage();
			break;
		case VR_MENU_PAGE_ABOUT:
			DrawQuestAboutPage();
			break;
		default:
			DrawQuestSettingsPage();
			break;
		}
		return gVrMenuPixels;
	}

	for(int pixel = 0; pixel < VR_DEBUG_WIDTH*(*height); pixel++){
		gDebugPixels[pixel*4+0] = 5; gDebugPixels[pixel*4+1] = 4;
		gDebugPixels[pixel*4+2] = 12; gDebugPixels[pixel*4+3] = 225;
	}

	char value[128];
	if(!profilerVisible){
		sprintf(value, "OPENXR FPS:%d", gDebugFps);
		DrawDebugText(value, VR_DEBUG_WIDTH/2, 10, 3,
			255, 230, 64);
		return gDebugPixels;
	}

	const QuestProfilerSnapshot perf = QuestProfilerGetSnapshot();
	sprintf(value, "FPS:%.1f FRAME:%.2f BUDGET:%.2f",
		perf.appFps > 0.0f ? perf.appFps : (float)gDebugFps,
		perf.cpuAppMs, perf.frameBudgetMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 1, 2,
		perf.cpuAppMs > perf.frameBudgetMs ? 255 : 100,
		perf.cpuAppMs > perf.frameBudgetMs ? 90 : 230, 120);
	sprintf(value, "CPU APP:%.2f MAX:%.2f STEP:%.2f",
		perf.cpuAppMs, perf.cpuAppMaxMs, perf.cpuStepMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 16, 2,
		perf.cpuAppMs > perf.frameBudgetMs ? 255 : 100,
		perf.cpuAppMs > perf.frameBudgetMs ? 90 : 230, 120);
	sprintf(value, "GAME:%.2f/%.2f RES:%.2f/%.2f STR:%.2f",
		perf.cpuGameMs, perf.cpuGameMaxMs,
		perf.cpuSimOtherMs, perf.cpuSimOtherMaxMs,
		perf.cpuStreamingMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 31, 2,
		perf.cpuGameMs > perf.frameBudgetMs ? 255 : 100,
		perf.cpuGameMs > perf.frameBudgetMs ? 90 : 230, 120);
	sprintf(value, "SCR:%.2f PRE:%.2f WRLD:%.2f POST:%.2f",
		perf.cpuScriptMs, perf.cpuPreWorldMiscMs,
		perf.cpuWorldProcessMs, perf.cpuPostWorldCameraFxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 46, 2,
		120, 210, 255);
	sprintf(value, "PED A/C/P/COL/S/X:%.2f/%.2f/%.2f/%.2f/%.2f/%.2f",
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_ANIMATION],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_CONTROL],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_POSTPONED_CONTROL],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_COLLISION],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_SHIFT],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_TRANSFORM]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 61, 2,
		120, 210, 255);
	sprintf(value, "VEH A/C/P/COL/S/X:%.2f/%.2f/%.2f/%.2f/%.2f/%.2f",
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_ANIMATION],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_CONTROL],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_POSTPONED_CONTROL],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_COLLISION],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_SHIFT],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_TRANSFORM]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 76, 2,
		120, 210, 255);
	sprintf(value, "MGR P:%.2f C:%.2f RENDER:%.2f UI:%.2f",
		perf.cpuPopulationMs, perf.cpuCarMs,
		perf.cpuWorldRenderMs, perf.cpuUiMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 91, 2,
		120, 210, 255);
	const float limitingGpuMs = perf.gpuFrameRuntime && perf.gpuVulkanValid ?
		Max(perf.gpuFrameMs, perf.gpuVulkanMs) : perf.gpuFrameMs;
	if(perf.gpuFrameRuntime && perf.gpuVulkanValid)
		sprintf(value, "GPU META:%.2f VK:%.2f LIMIT:%.2f",
			perf.gpuFrameMs, perf.gpuVulkanMs, limitingGpuMs);
	else if(perf.gpuFrameValid)
		sprintf(value, "GPU:%.2f [%s]", perf.gpuFrameMs,
			perf.gpuFrameRuntime ? "META" : "VK");
	else
		sprintf(value, "GPU:N/A VK:N/A");
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 106, 2,
		perf.gpuFrameValid && limitingGpuMs > perf.frameBudgetMs ? 255 : 100,
		perf.gpuFrameValid && limitingGpuMs > perf.frameBudgetMs ? 90 : 210, 255);

	// The local APP bracket includes beginFrame's previous-GPU fence wait.
	// Do not call that a CPU bottleneck when META is unavailable; CPU STEP is
	// the wait-free game/recording workload in that fallback case.
	const float cpuMs = perf.cpuAppRuntime ?
		perf.cpuAppMs : perf.cpuStepMs;
	const char *bottleneck = "UNKNOWN";
	if(perf.gpuFrameValid){
		if(cpuMs < perf.frameBudgetMs*0.8f &&
		   limitingGpuMs < perf.frameBudgetMs*0.8f)
			bottleneck = "HEADROOM";
		else if(cpuMs > limitingGpuMs+0.5f)
			bottleneck = "CPU";
		else if(limitingGpuMs > cpuMs+0.5f)
			bottleneck = "GPU";
		else
			bottleneck = "BALANCED";
	}
	sprintf(value, "PART U:%.2f R:%.2f N:%d H:%d",
		perf.cpuParticleUpdateMs, perf.cpuParticleRenderMs,
		perf.particleActive, perf.heliDustActive);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 121, 2,
		180, 190, 200);
	sprintf(value, "STREAM Q:%d P:%d MEM:%llu/%lluM",
		perf.streamingRequested, perf.streamingPriority,
		(unsigned long long)(perf.streamingMemoryUsed/(1024*1024)),
		(unsigned long long)(perf.streamingMemoryAvailable/(1024*1024)));
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 136, 2,
		120, 210, 255);
	sprintf(value, "PED:%d/%.1f CAP:%d POOL:%d/%d",
		perf.ambientPeds, perf.targetAmbientPeds,
		perf.ambientPedCap, perf.pedPoolUsed, perf.pedPoolSize);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 151, 2,
		120, 210, 255);
	sprintf(value, "POOL V:%d/%d O:%d/%d E:%d/%d",
		perf.vehiclePoolUsed, perf.vehiclePoolSize,
		perf.objectPoolUsed, perf.objectPoolSize,
		perf.entryInfoPoolUsed, perf.entryInfoPoolSize);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 166, 2,
		120, 210, 255);
	sprintf(value, "CFG:%d/%d WANT:%.1f SERV:%.1f",
		perf.pedTrafficPercent, perf.carTrafficPercent,
		perf.trafficDesired, perf.trafficServed);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 181, 2,
		255, 220, 100);
	sprintf(value, "AMB:%d COP:%d PROXY:%d PEND:%d",
		perf.trafficAmbient, perf.trafficPursuit,
		perf.trafficProxies, perf.trafficPending);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 196, 2,
		255, 220, 100);
	sprintf(value, "DEB:%d STL:%d BLK:%d DIR:%.2f/%.2f",
		perf.trafficDebris, perf.trafficStalled,
		perf.trafficPoolReserveBlocks,
		perf.trafficDirectorMs, perf.trafficDirectorMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 211, 2,
		255, 180, 100);
	sprintf(value, "SPAWN:%d OK:%d DENY:%d TOP:%s:%d",
		perf.spawnAttemptsPerSecond, perf.spawnSuccessPerSecond,
		perf.spawnDeniedPerSecond,
		SpawnDenyReasonName(perf.spawnTopDenyReason),
		perf.spawnTopDenyCount);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 226, 2,
		perf.spawnDeniedPerSecond > perf.spawnSuccessPerSecond ? 255 : 220,
		perf.spawnDeniedPerSecond > perf.spawnSuccessPerSecond ? 90 : 180,
		120);
	sprintf(value, "JOB D:%d S:%d H:%d M:%d",
		perf.trafficJobDispatchesPerSecond,
		perf.trafficJobSkipsPerSecond,
		perf.trafficJobHitsPerSecond, perf.trafficJobMissesPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 241, 2,
		perf.trafficJobHitsPerSecond > perf.trafficJobMissesPerSecond ? 100 : 255,
		perf.trafficJobHitsPerSecond > perf.trafficJobMissesPerSecond ? 230 : 120,
		160);
	sprintf(value, "JOB W:%.2f/%.2f B:%.2f/%.2f",
		perf.trafficJobMs, perf.trafficJobMaxMs,
		perf.trafficJobBuildMs, perf.trafficJobBuildMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 256, 2,
		140, 215, 255);
	sprintf(value, "FLAT S/B/R:%d/%d/%d F:%d I:%d SAVE:%d",
		perf.collisionFlatScopesPerSecond,
		perf.collisionFlatBuildsPerSecond,
		perf.collisionFlatReusesPerSecond,
		perf.collisionFlatStalePerSecond+
			perf.collisionFlatOverflowsPerSecond,
		perf.collisionFlatItemsPerSecond,
		perf.collisionFlatSavedNodeVisitsPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 271, 2,
		perf.collisionFlatStalePerSecond == 0 &&
			perf.collisionFlatOverflowsPerSecond == 0 ? 100 : 255,
		perf.collisionFlatStalePerSecond == 0 &&
			perf.collisionFlatOverflowsPerSecond == 0 ? 230 : 120,
		160);
	sprintf(value, "R SKY:%.2f RD:%.2f WRLD:%.2f WTR:%.2f",
		perf.cpuRenderSkyMs, perf.cpuRenderRoadsMs,
		perf.cpuRenderWorldMs, perf.cpuRenderWaterMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 286, 2,
		140, 215, 255);
	sprintf(value, "R REF:%.2f FAD:%.2f WTH:%.2f FX:%.2f",
		perf.cpuRenderReflectionsMs, perf.cpuRenderFadingMs,
		perf.cpuRenderWeatherMs, perf.cpuRenderEffectsMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 301, 2,
		140, 215, 255);
	sprintf(value, "R OTHER:%.2f/%.2f TOTAL:%.2f/%.2f",
		perf.cpuRenderOtherMs, perf.cpuRenderOtherMaxMs,
		perf.cpuWorldRenderMs, perf.cpuWorldRenderMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 316, 2,
		140, 215, 255);
	sprintf(value, "STATIC:%d RD:%d OCC:%d D:%d/%d/%d",
		perf.visibleBuildings, perf.visibleRoads,
		perf.activeOccluders,
		perf.renderEntityCalls[QUEST_RENDER_ENTITY_VEHICLE],
		perf.renderEntityCalls[QUEST_RENDER_ENTITY_PED],
		perf.renderEntityCalls[QUEST_RENDER_ENTITY_STATIC]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 331, 2,
		140, 215, 255);
	sprintf(value, "RFAD V:%.2f/%d P:%.2f/%d S:%.2f/%d",
		perf.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_VEHICLE],
		perf.renderFadingEntityCalls[QUEST_RENDER_ENTITY_VEHICLE],
		perf.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_PED],
		perf.renderFadingEntityCalls[QUEST_RENDER_ENTITY_PED],
		perf.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_STATIC],
		perf.renderFadingEntityCalls[QUEST_RENDER_ENTITY_STATIC]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 346, 2,
		140, 215, 255);
	sprintf(value, "RV %.2f/%.2f/%.2f/%.2f V:%d/%d S:%d O:%d",
		perf.cpuVehicleRenderPhaseMs[QUEST_VEHICLE_RENDER_PHASE_LIGHTING],
		perf.cpuVehicleRenderPhaseMs[QUEST_VEHICLE_RENDER_PHASE_OCCUPANTS],
		perf.cpuVehicleRenderPhaseMs[QUEST_VEHICLE_RENDER_PHASE_BODY_SUBMIT],
		perf.cpuVehicleRenderPhaseMs[QUEST_VEHICLE_RENDER_PHASE_ALPHA_ATOMICS],
		perf.vehicleVisualHighPerSecond,
		perf.vehicleVisualVloPerSecond,
		perf.vehicleVisualAtomicsSkippedPerSecond,
		perf.vehicleVisualOccupantsSkippedPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 361, 2,
		140, 215, 255);
	gQuestCpuPerformanceMode = Min(Max(xrvk::getPerformanceMode(),
		(int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_COUNT-1);
	if(!xrvk::isPerformanceModeSupported())
		sprintf(value, "CPU PERF:%s UNSUPPORTED",
			QuestCpuPerformanceModeName(gQuestCpuPerformanceMode));
	else if(xrvk::isPerformanceBoostBlocked())
		sprintf(value, "CPU PERF:%s THERMAL BLOCKED",
			QuestCpuPerformanceModeName(gQuestCpuPerformanceMode));
	else if(xrvk::getActivePerformanceMode() < 0)
		sprintf(value, "CPU PERF REQ:%s ACTIVE:PENDING",
			QuestCpuPerformanceModeName(gQuestCpuPerformanceMode));
	else
		sprintf(value, "CPU PERF REQ:%s ACTIVE:%s",
			QuestCpuPerformanceModeName(gQuestCpuPerformanceMode),
			QuestCpuPerformanceModeName(
				xrvk::getActivePerformanceMode()));
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 376, 2,
		180, 205, 120);
	sprintf(value, "PHY M:%d F/RD/RL/P:%d/%d/%d/%d COL:%.2f",
		perf.physicsDirectorMode, perf.physicsFull,
		perf.physicsReduced, perf.physicsRail, perf.physicsProxy,
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_COLLISION]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 391, 2,
		120, 230, 185);
	sprintf(value, "PHY C/S/K:%d/%d/%d N/P:%d/%d",
		perf.physicsCheckCollisionPerSecond,
		perf.physicsCheckSimplePerSecond,
		perf.physicsSimpleSkippedPerSecond,
		perf.physicsSectorNodesPerSecond,
		perf.physicsPairsAfterFilteringPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 406, 2,
		120, 230, 185);
	sprintf(value, "PHY CM/W/T:%d/%d/%d ST/R:%d/%d",
		perf.physicsProcessColModelsPerSecond,
		perf.physicsWheelLineTestsPerSecond,
		perf.physicsTriangleTestsPerSecond,
		perf.physicsExtraSubstepsPerSecond,
		perf.physicsRetryPassesPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 421, 2,
		120, 230, 185);
	(void)bottleneck;
	/* LIMIT remains available in the CSV through CPU/GPU/budget columns; the
	 * compact screen spends its last lines on actionable traffic diagnostics. */
	return gDebugPixels;
}

bool
VrMenuConsumesInput(void)
{
	// Keep the service chord from leaking through as Pause when the menu is
	// opened or closed. Gameplay resumes only after Menu has been released.
	return gVrMenuVisible || gVrMenuShortcutDown;
}

bool
VrViceCityColorEnabled(void)
{
	LoadVrSettings();
	return gViceCityColorEnabled;
}

bool
VrFxaaEnabled(void)
{
	LoadVrSettings();
	return gSpatialAaMode != 0;
}

int
VrSpatialAaMode(void)
{
	LoadVrSettings();
	return gSpatialAaMode;
}

bool
VrGameplayHudEnabled(void)
{
	LoadVrSettings();
	return gGameplayHudEnabled;
}

void
VrGetGameplayHudSettings(int *widthPercent, int *scalePercent,
                         int *offsetXCm, int *offsetYCm)
{
	LoadVrSettings();
	if(widthPercent) *widthPercent = gHudWidthPercent;
	if(scalePercent) *scalePercent = gHudScalePercent;
	if(offsetXCm) *offsetXCm = gHudOffsetXCm;
	if(offsetYCm) *offsetYCm = gHudOffsetYCm;
}

bool
VrHeadBobbingEnabled(void)
{
	LoadVrSettings();
	return gQuestHeadBobbing != 0;
}

bool
VrUsesHeadRelativeMovement(void)
{
	LoadVrSettings();
	return gQuestMovementOrientation != QUEST_MOVEMENT_ORIENTATION_BODY;
}

bool
VrUsesExperimentalHeadTurning(void)
{
	LoadVrSettings();
	return gQuestMovementOrientation ==
		QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL;
}

float
VrHeadTurnScale(void)
{
	LoadVrSettings();
	return (float)gQuestHeadSteeringSensitivityPercent/100.0f;
}

bool
VrUsesSnapTurn(void)
{
	LoadVrSettings();
	return gQuestTurnMode == QUEST_TURN_SNAP;
}

float
VrSmoothTurnScale(void)
{
	LoadVrSettings();
	return (float)gQuestTurnSensitivityPercent/100.0f;
}

int
VrSnapTurnAngleDegrees(void)
{
	LoadVrSettings();
	return gQuestSnapTurnAngleDegrees;
}

float
VrScopeZoomFactor(void)
{
	if(!OculusVR::IsTrackedScopeActive())
		return 1.0f;
	switch(OculusVR::GetTrackedScopeWeaponType()){
	case WEAPONTYPE_SNIPERRIFLE: return 2.5f;
	case WEAPONTYPE_LASERSCOPE: return 3.0f;
	case WEAPONTYPE_ROCKETLAUNCHER: return 1.8f;
	case WEAPONTYPE_CAMERA: return 1.6f;
	default: return 1.0f;
	}
}

}
