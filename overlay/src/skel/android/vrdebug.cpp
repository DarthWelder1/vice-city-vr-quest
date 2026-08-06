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
#include "WeaponType.h"
#include "vulkan/rwvk.h"

extern int GetVrCheatCount(void);
extern const char *GetVrCheatName(int index);
extern bool ActivateVrCheat(int index);
extern int GetVrWeaponTypeForSlot(int slot);
extern const char *GetVrWeaponName(int weaponType);

enum {
	VR_DEBUG_WIDTH = 512,
	VR_DEBUG_HEIGHT = 128,
	VR_MENU_WIDTH = 1024,
	VR_MENU_HEIGHT = 768,
	VR_CHEAT_ITEMS_PER_PAGE = 10,
};

// --- verbatim from OpenXRVR.cpp -------------------------------------------
struct DebugGlyph { char character; uint8 rows[7]; };
static const DebugGlyph gDebugGlyphs[] = {
	{' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, {'%',{0x11,0x12,0x02,0x04,0x08,0x09,0x11}},
	{'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
	{'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
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
static bool gTouchDebugShortcutDown;
static bool gTouchProfilerShortcutDown;
static bool gTouchRecenterShortcutDown;
static bool gVrMenuVisible;
static bool gVrMenuShortcutDown;
static bool gVrCheatShortcutDown;
static bool gVrMenuSelectDown;
static bool gVrMenuBackDown;
static bool gVrMenuNavigateDown;
static bool gVrMenuIncreaseDown;
static bool gVrMenuDecreaseDown;
static double gVrMenuIncreaseRepeatAt;
static double gVrMenuDecreaseRepeatAt;
static bool gVrSettingsLoaded;
static bool gViceCityColorEnabled = true;
static bool gFxaaEnabled = true;
static bool gGameplayHudEnabled = true;
static int gHudWidthPercent = 100;
static int gHudScalePercent = 130;
static int gHudOffsetXCm;
static int gHudOffsetYCm;
static int gQuestMovementOrientation = 1;
static int gQuestTurnMode;
static int gQuestTurnSensitivityPercent = 100;
static int gQuestHeadSteeringSensitivityPercent = 50;
static int gQuestSnapTurnAngleDegrees = 30;
static int gVrMenuPage;
static int gVrMenuSelection;
static int gVrHudSelection;
static int gVrCheatSelection;
static int gVrVehicleSelection;
static int gVrVehicleCalibrationSelection;
static int gVrVehicleCalibrationHand;
static int gVrLocomotionSelection;
static int gVrCalibrationSelection;
static int gVrCalibrationHand = 1;
static int gVrHolsterSelection;
static int gVrCheatStatusFrames;
static char gVrCheatStatus[48];

enum {
	VR_MENU_PAGE_SETTINGS,
	VR_MENU_PAGE_HUD,
	VR_MENU_PAGE_VEHICLE,
	VR_MENU_PAGE_VEHICLE_CALIBRATION,
	VR_MENU_PAGE_LOCOMOTION,
	VR_MENU_PAGE_CALIBRATION,
	VR_MENU_PAGE_HOLSTERS,
	VR_MENU_PAGE_CHEATS,
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
	VR_MAIN_FXAA = 0,
	VR_MAIN_COLOR,
	VR_MAIN_HUD,
	VR_MAIN_HANDS,
	VR_MAIN_LASER,
	VR_MAIN_HOLSTER_HIGHLIGHTS,
	VR_MAIN_MANUAL_RELOAD,
	VR_MAIN_SCOPE_AIM,
	VR_MAIN_VEHICLE_SETTINGS,
	VR_MAIN_LOCOMOTION_SETTINGS,
	VR_MAIN_DEBUG,
	VR_MAIN_GRIP_LOCK,
	VR_MAIN_CALIBRATION,
	VR_MAIN_HOLSTERS,
	VR_MAIN_CHEATS,
	VR_MAIN_ITEM_COUNT
};

enum eVrVehicleMenuItem {
	VR_VEHICLE_DRIVING_TYPE = 0,
	VR_VEHICLE_DRIVING_Y,
	VR_VEHICLE_SEAT_DISTANCE,
	VR_VEHICLE_MOTION_HAND,
	VR_VEHICLE_HANDLE_HIGHLIGHTS,
	VR_VEHICLE_BIKE_LOCK_HORIZON,
	VR_VEHICLE_CALIBRATION,
	VR_VEHICLE_BACK,
	VR_VEHICLE_ITEM_COUNT
};

enum eVrVehicleCalibrationMenuItem {
	VR_VEHICLE_CAL_HAND = 0,
	VR_VEHICLE_CAL_OFFSET_X,
	VR_VEHICLE_CAL_OFFSET_Y,
	VR_VEHICLE_CAL_OFFSET_Z,
	VR_VEHICLE_CAL_ROT_X,
	VR_VEHICLE_CAL_ROT_Y,
	VR_VEHICLE_CAL_ROT_Z,
	VR_VEHICLE_CAL_WHEELIE_HEIGHT,
	VR_VEHICLE_CAL_STAND_HEIGHT,
	VR_VEHICLE_CAL_BACK,
	VR_VEHICLE_CAL_ITEM_COUNT
};

enum eVrLocomotionMenuItem {
	VR_LOCOMOTION_MOVEMENT_ORIENTATION = 0,
	VR_LOCOMOTION_TURN_MODE,
	VR_LOCOMOTION_TURN_SENSITIVITY,
	VR_LOCOMOTION_SNAP_ANGLE,
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

static void
LoadVrSettings(void)
{
	if(gVrSettingsLoaded)
		return;
	gViceCityColorEnabled =
		GetPrivateProfileIntA("VR", "ViceCityColor", 1,
			".\\vr_settings.ini") != 0;
	gFxaaEnabled =
		GetPrivateProfileIntA("VR", "AntiAliasing", 1,
			".\\vr_settings.ini") != 0;
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
	WritePrivateProfileStringA("VR", "AntiAliasing",
		gFxaaEnabled ? "1" : "0", ".\\vr_settings.ini");
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
MenuRepeatPulse(bool down, bool &wasDown, double &repeatAt, double now,
                bool allowRepeat)
{
	if(!down){
		wasDown = false;
		repeatAt = 0.0;
		return false;
	}
	if(!wasDown){
		wasDown = true;
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
QuestWeaponSettingForMainItem(int item)
{
	switch(item){
	case VR_MAIN_HANDS: return 0;
	case VR_MAIN_LASER: return 1;
	case VR_MAIN_HOLSTER_HIGHLIGHTS: return 2;
	case VR_MAIN_MANUAL_RELOAD: return 3;
	case VR_MAIN_SCOPE_AIM: return 4;
	case VR_MAIN_GRIP_LOCK: return 5;
	default: return -1;
	}
}

static int *
CurrentMenuSelection(void)
{
	switch(gVrMenuPage){
	case VR_MENU_PAGE_HUD: return &gVrHudSelection;
	case VR_MENU_PAGE_VEHICLE: return &gVrVehicleSelection;
	case VR_MENU_PAGE_VEHICLE_CALIBRATION:
		return &gVrVehicleCalibrationSelection;
	case VR_MENU_PAGE_LOCOMOTION: return &gVrLocomotionSelection;
	case VR_MENU_PAGE_CALIBRATION: return &gVrCalibrationSelection;
	case VR_MENU_PAGE_HOLSTERS: return &gVrHolsterSelection;
	case VR_MENU_PAGE_CHEATS: return &gVrCheatSelection;
	default: return &gVrMenuSelection;
	}
}

static int
CurrentMenuItemCount(void)
{
	switch(gVrMenuPage){
	case VR_MENU_PAGE_SETTINGS: return VR_MAIN_ITEM_COUNT;
	case VR_MENU_PAGE_HUD: return VR_HUD_ITEM_COUNT;
	case VR_MENU_PAGE_VEHICLE: return VR_VEHICLE_ITEM_COUNT;
	case VR_MENU_PAGE_VEHICLE_CALIBRATION:
		return Max(1,
			OculusVR::GetQuestVehicleCalibrationItemCount()+1);
	case VR_MENU_PAGE_LOCOMOTION: return VR_LOCOMOTION_ITEM_COUNT;
	case VR_MENU_PAGE_CALIBRATION: return 17;
	case VR_MENU_PAGE_HOLSTERS:
		return OculusVR::GetQuestHolsterPointCount()+1;
	case VR_MENU_PAGE_CHEATS: return Max(1, GetVrCheatCount());
	default: return 1;
	}
}

static bool
CurrentMenuValueRepeats(void)
{
	if(gVrMenuPage == VR_MENU_PAGE_CALIBRATION)
		return gVrCalibrationSelection >= 1 &&
			gVrCalibrationSelection <= 15;
	if(gVrMenuPage == VR_MENU_PAGE_HUD)
		return gVrHudSelection >= VR_HUD_HORIZONTAL_SCALE &&
			gVrHudSelection <= VR_HUD_OFFSET_Y;
	if(gVrMenuPage == VR_MENU_PAGE_VEHICLE)
		return gVrVehicleSelection == VR_VEHICLE_DRIVING_Y ||
			gVrVehicleSelection == VR_VEHICLE_SEAT_DISTANCE;
	if(gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION)
		return gVrVehicleCalibrationSelection >=
				VR_VEHICLE_CAL_OFFSET_X &&
			gVrVehicleCalibrationSelection <
				OculusVR::GetQuestVehicleCalibrationItemCount();
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
	if(gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION){
		OculusVR::SetQuestVehicleCalibrationPreview(false);
		gVrMenuPage = VR_MENU_PAGE_VEHICLE;
		return;
	}
	gVrMenuPage = VR_MENU_PAGE_SETTINGS;
	gVrCheatStatusFrames = 0;
}

// Runs once per rendered frame from the pad path. All menu state changes are
// edge/repeat driven so a tap remains one exact step while a held trigger can
// traverse large calibration ranges without hundreds of clicks.
void
VrDebugUpdate(const PadInput &in)
{
	LoadVrSettings();
	const double now = MonotonicMilliseconds();
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
		gVrMenuNavigateDown = false;
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
		gVrMenuNavigateDown = false;
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
		const bool navigate =
			in.leftStickY >= 0.65f || in.leftStickY <= -0.65f;
		if(navigate && !gVrMenuNavigateDown){
			const int direction = in.leftStickY >= 0.65f ? -1 : 1;
			int *selection = CurrentMenuSelection();
			*selection = (*selection+direction+itemCount)%itemCount;
		}
		gVrMenuNavigateDown = navigate;

		const bool select = in.a || in.rightStickClick;
		const bool selectPulse = select && !gVrMenuSelectDown;
		gVrMenuSelectDown = select;
		const bool repeatValue = CurrentMenuValueRepeats();
		const bool increasePulse = MenuRepeatPulse(
			in.rightTrigger >= 0.55f, gVrMenuIncreaseDown,
			gVrMenuIncreaseRepeatAt, now, repeatValue);
		const bool decreasePulse = MenuRepeatPulse(
			in.leftTrigger >= 0.55f, gVrMenuDecreaseDown,
			gVrMenuDecreaseRepeatAt, now, repeatValue);
		const bool positivePulse = selectPulse || increasePulse;

		if(gVrMenuPage == VR_MENU_PAGE_SETTINGS &&
		   (positivePulse || decreasePulse)){
			const int weaponSetting =
				QuestWeaponSettingForMainItem(gVrMenuSelection);
			if(gVrMenuSelection == VR_MAIN_FXAA){
				gFxaaEnabled = !gFxaaEnabled;
				SaveFxaa();
			}else if(gVrMenuSelection == VR_MAIN_COLOR){
				gViceCityColorEnabled = !gViceCityColorEnabled;
				SaveViceCityColor();
			}else if(gVrMenuSelection == VR_MAIN_HUD){
				gVrMenuPage = VR_MENU_PAGE_HUD;
				gVrHudSelection = 0;
			}else if(weaponSetting >= 0 &&
			         weaponSetting <
			         	OculusVR::GetQuestWeaponSettingCount()){
				OculusVR::ToggleQuestWeaponSetting(weaponSetting);
			}else if(gVrMenuSelection == VR_MAIN_VEHICLE_SETTINGS){
				gVrMenuPage = VR_MENU_PAGE_VEHICLE;
				gVrVehicleSelection = 0;
			}else if(gVrMenuSelection ==
			         VR_MAIN_LOCOMOTION_SETTINGS){
				gVrMenuPage = VR_MENU_PAGE_LOCOMOTION;
				gVrLocomotionSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_DEBUG){
				gDebugVisible = !gDebugVisible;
			}else if(gVrMenuSelection == VR_MAIN_CALIBRATION){
				gVrMenuPage = VR_MENU_PAGE_CALIBRATION;
				gVrCalibrationSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_HOLSTERS){
				gVrMenuPage = VR_MENU_PAGE_HOLSTERS;
				gVrHolsterSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_CHEATS){
				gVrMenuPage = VR_MENU_PAGE_CHEATS;
				gVrCheatStatusFrames = 0;
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
			case VR_VEHICLE_DRIVING_TYPE:
				OculusVR::CycleQuestDrivingType(direction);
				break;
			case VR_VEHICLE_DRIVING_Y:
				OculusVR::AdjustQuestDrivingYOffsetCm(direction);
				break;
			case VR_VEHICLE_SEAT_DISTANCE:
				OculusVR::AdjustQuestVehicleSeatDistanceCm(
					direction);
				break;
			case VR_VEHICLE_MOTION_HAND:
				OculusVR::ToggleQuestMotionSteeringHand();
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
			if(gVrVehicleCalibrationSelection == 0)
				gVrVehicleCalibrationHand =
					1-gVrVehicleCalibrationHand;
			else if(gVrVehicleCalibrationSelection < valueCount)
				OculusVR::AdjustQuestVehicleCalibrationValue(
					gVrVehicleCalibrationHand,
					gVrVehicleCalibrationSelection,
					direction);
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
			        gVrCalibrationSelection <= 15 &&
			        (positivePulse || decreasePulse)){
				const int weaponType =
					OculusVR::GetQuestCalibrationWeaponType(
						gVrCalibrationHand);
				OculusVR::AdjustQuestCalibrationValue(
					gVrCalibrationHand, weaponType,
					gVrCalibrationSelection-1,
					decreasePulse ? -1 : 1);
			}else if(gVrCalibrationSelection == 16 &&
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
		}else if(gVrMenuPage == VR_MENU_PAGE_CHEATS &&
		         positivePulse){
			const bool activated =
				ActivateVrCheat(gVrCheatSelection);
			snprintf(gVrCheatStatus, sizeof(gVrCheatStatus),
				"%s", activated ? "CHEAT ACTIVATED" :
				"UNAVAILABLE RIGHT NOW");
			gVrCheatStatusFrames = 120;
		}

		const bool back = in.b || in.leftStickClick;
		if(back && !gVrMenuBackDown)
			ReturnFromCurrentMenuPage();
		gVrMenuBackDown = back;
		if(gVrCheatStatusFrames > 0)
			gVrCheatStatusFrames--;
	}else{
		const bool debugShortcut = modifier && in.a;
		if(debugShortcut && !gTouchDebugShortcutDown)
			gDebugVisible = !gDebugVisible;
		gTouchDebugShortcutDown = debugShortcut;
		gVrMenuIncreaseDown = false;
		gVrMenuDecreaseDown = false;
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
                  bool available = true)
{
	if(selected)
		FillVrMenuRect(85, y-5, VR_MENU_WIDTH-85,
			y+scale*7+4,
			available ? 25 : 55,
			available ? 95 : 62,
			available ? 135 : 72, 245);
	DrawVrMenuText(text, VR_MENU_WIDTH/2, y, scale,
		selected ? 255 : (available ? 205 : 120),
		selected ? (available ? 245 : 180) :
			(available ? 215 : 135),
		selected ? (available ? 110 : 130) :
			(available ? 225 : 145));
}

static bool
IsQuestMainItemAvailable(int item)
{
	return item == VR_MAIN_COLOR ||
		item == VR_MAIN_FXAA ||
		item == VR_MAIN_HUD ||
		item == VR_MAIN_HANDS ||
		item == VR_MAIN_LASER ||
		item == VR_MAIN_HOLSTER_HIGHLIGHTS ||
		item == VR_MAIN_MANUAL_RELOAD ||
		item == VR_MAIN_SCOPE_AIM ||
		item == VR_MAIN_VEHICLE_SETTINGS ||
		item == VR_MAIN_LOCOMOTION_SETTINGS ||
		item == VR_MAIN_DEBUG ||
		item == VR_MAIN_GRIP_LOCK ||
		item == VR_MAIN_CALIBRATION ||
		item == VR_MAIN_HOLSTERS ||
		item == VR_MAIN_CHEATS;
}

static void
DrawQuestSettingsPage(void)
{
	BeginFullVrMenuPage("SETTINGS");
	char rows[VR_MAIN_ITEM_COUNT][112];
	snprintf(rows[VR_MAIN_FXAA], sizeof(rows[0]),
		"FXAA  < %s >", gFxaaEnabled ? "ON" : "OFF");
	snprintf(rows[VR_MAIN_COLOR], sizeof(rows[0]),
		"COLOR FILTER  < %s >",
		gViceCityColorEnabled ? "ON" : "OFF");
	strcpy(rows[VR_MAIN_HUD], "HUD SETTINGS  < OPEN >");
	for(int item = VR_MAIN_HANDS;
	    item <= VR_MAIN_SCOPE_AIM; item++){
		const int setting = QuestWeaponSettingForMainItem(item);
		snprintf(rows[item], sizeof(rows[item]), "%s  < %s >",
			OculusVR::GetQuestWeaponSettingName(setting),
			OculusVR::GetQuestWeaponSetting(setting) ?
				"ON" : "OFF");
	}
	strcpy(rows[VR_MAIN_VEHICLE_SETTINGS],
		"VEHICLE SETTINGS  < OPEN >");
	strcpy(rows[VR_MAIN_LOCOMOTION_SETTINGS],
		"LOCOMOTION SETTINGS  < OPEN >");
	snprintf(rows[VR_MAIN_DEBUG], sizeof(rows[0]),
		"DEBUG OVERLAY  < %s >", gDebugVisible ? "ON" : "OFF");
	{
		const int setting =
			QuestWeaponSettingForMainItem(VR_MAIN_GRIP_LOCK);
		snprintf(rows[VR_MAIN_GRIP_LOCK], sizeof(rows[0]),
			"%s  < %s >",
			OculusVR::GetQuestWeaponSettingName(setting),
			OculusVR::GetQuestWeaponSetting(setting) ?
				"ON" : "OFF");
	}
	strcpy(rows[VR_MAIN_CALIBRATION],
		"WEAPON CALIBRATION  < OPEN >");
	strcpy(rows[VR_MAIN_HOLSTERS], "HOLSTER LOADOUT  < OPEN >");
	strcpy(rows[VR_MAIN_CHEATS], "CHEAT MENU  < OPEN >");

	for(int item = 0; item < VR_MAIN_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 145+item*22, 2,
			item == gVrMenuSelection,
			IsQuestMainItemAvailable(item));
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B CLOSE",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
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
	snprintf(rows[VR_VEHICLE_DRIVING_TYPE], sizeof(rows[0]),
		"DRIVING TYPE  < %s >",
		OculusVR::GetQuestDrivingTypeName());
	snprintf(rows[VR_VEHICLE_DRIVING_Y], sizeof(rows[0]),
		"DRIVING Y OFFSET  < %+d CM >",
		OculusVR::GetQuestDrivingYOffsetCm());
	if(OculusVR::HasQuestVehicleSeatCalibrationTarget())
		snprintf(rows[VR_VEHICLE_SEAT_DISTANCE], sizeof(rows[0]),
			"SEAT DISTANCE  < %+d CM >",
			OculusVR::GetQuestVehicleSeatDistanceCm());
	else
		strcpy(rows[VR_VEHICLE_SEAT_DISTANCE],
			"SEAT DISTANCE  < ENTER VEHICLE >");
	snprintf(rows[VR_VEHICLE_MOTION_HAND], sizeof(rows[0]),
		"MOTION STEERING HAND  < %s >",
		OculusVR::GetQuestMotionSteeringHand() == 0 ?
			"LEFT" : "RIGHT");
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
	for(int item = 0; item < VR_VEHICLE_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 170+item*49, 3,
			item == gVrVehicleSelection,
			item != VR_VEHICLE_SEAT_DISTANCE ||
				OculusVR::HasQuestVehicleSeatCalibrationTarget());
	DrawVrMenuText("VALUES ARE SAVED IN VR SETTINGS",
		VR_MENU_WIDTH/2, 660, 2, 125, 255, 145);
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
	static const char *labels[VR_VEHICLE_CAL_ITEM_COUNT] = {
		"EDIT HANDLE", "LOCAL X OFFSET", "LOCAL Y OFFSET",
		"LOCAL Z OFFSET", "LOCAL ROT X", "LOCAL ROT Y",
		"LOCAL ROT Z", "WHEELIE HAND HEIGHT", "STAND HAND DROP"
	};
	const int valueCount =
		OculusVR::GetQuestVehicleCalibrationItemCount();
	for(int item = 0; item <= valueCount; item++){
		char row[112];
		if(item == 0)
			snprintf(row, sizeof(row), "%s  < %s >", labels[item],
				gVrVehicleCalibrationHand == 0 ?
					"LEFT" : "RIGHT");
		else if(item < valueCount){
			const int value =
				OculusVR::GetQuestVehicleCalibrationValue(
					gVrVehicleCalibrationHand, item);
			if(item >= VR_VEHICLE_CAL_ROT_X &&
			   item <= VR_VEHICLE_CAL_ROT_Z)
				snprintf(row, sizeof(row),
					"%s  < %+.1f DEG >", labels[item],
					(float)value/2.0f);
			else if(item == VR_VEHICLE_CAL_WHEELIE_HEIGHT ||
			        item == VR_VEHICLE_CAL_STAND_HEIGHT)
				snprintf(row, sizeof(row), "%s  < %d CM >",
					labels[item], value);
			else
				snprintf(row, sizeof(row),
					"%s  < %+.1f CM >", labels[item],
					(float)value/2.0f);
		}else
			strcpy(row, "BACK TO VEHICLE SETTINGS");
		DrawFullVrMenuRow(row, 174+item*48, 3,
			item == gVrVehicleCalibrationSelection,
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
	static const char *labels[15] = {
		"AIM X OFFSET", "AIM Y OFFSET", "AIM Z OFFSET",
		"AIM LOCAL ROT X", "AIM LOCAL ROT Y", "AIM LOCAL ROT Z",
		"WEAPON X OFFSET", "WEAPON Y OFFSET", "WEAPON Z OFFSET",
		"WEAPON LOCAL ROT X", "WEAPON LOCAL ROT Y",
		"WEAPON LOCAL ROT Z", "SUPPORT GRIP X",
		"SUPPORT GRIP Y", "SUPPORT GRIP Z"
	};
	for(int item = 0; item < 17; item++){
		char row[112];
		if(item == 0)
			snprintf(row, sizeof(row), "EDIT HAND  < %s >",
				gVrCalibrationHand == 0 ? "LEFT" : "RIGHT");
		else if(item <= 15){
			const int value =
				OculusVR::GetQuestCalibrationValue(
					gVrCalibrationHand, weaponType, item-1);
			snprintf(row, sizeof(row), "%s  < %+.1f %s >",
				labels[item-1], (float)value/2.0f,
				(item >= 4 && item <= 6) ||
				(item >= 10 && item <= 12) ?
					"DEG" : "CM");
		}else
			strcpy(row, "BACK TO SETTINGS");
		DrawFullVrMenuRow(row, 158+item*28, 2,
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
	const int count = GetVrCheatCount();
	const int first = count > 0 ?
		(gVrCheatSelection/VR_CHEAT_ITEMS_PER_PAGE)*
			VR_CHEAT_ITEMS_PER_PAGE : 0;
	for(int row = 0;
	    row < VR_CHEAT_ITEMS_PER_PAGE && first+row < count;
	    row++){
		const int item = first+row;
		DrawFullVrMenuRow(GetVrCheatName(item), 158+row*50, 3,
			item == gVrCheatSelection);
	}
	char page[48];
	const int pageCount = Max(1,
		(count+VR_CHEAT_ITEMS_PER_PAGE-1)/VR_CHEAT_ITEMS_PER_PAGE);
	snprintf(page, sizeof(page), "PAGE %d OF %d",
		first/VR_CHEAT_ITEMS_PER_PAGE+1, pageCount);
	DrawVrMenuText(page, VR_MENU_WIDTH/2, 680, 3,
		120, 220, 255);
	DrawVrMenuText(
		gVrCheatStatusFrames > 0 ? gVrCheatStatus :
			"LEFT STICK SELECT   A OR R2 ACTIVATE   B BACK",
		VR_MENU_WIDTH/2, 718, 3,
		gVrCheatStatusFrames > 0 ? 255 : 170,
		gVrCheatStatusFrames > 0 ? 220 : 190,
		gVrCheatStatusFrames > 0 ? 80 : 210);
}

// Fills the same 1024x768 menu surface as desktop. The compact 512x128 layer
// is now reserved exclusively for the FPS/debug overlay.
const unsigned char *
VrDebugPixels(int *width, int *height)
{
	*width = gVrMenuVisible ? VR_MENU_WIDTH : VR_DEBUG_WIDTH;
	*height = gVrMenuVisible ? VR_MENU_HEIGHT : VR_DEBUG_HEIGHT;
	const bool profilerVisible = QuestProfilerIsEnabled();
	if(!gDebugVisible && !gVrMenuVisible && !profilerVisible)
		return nil;

	if(gVrMenuVisible){
		switch(gVrMenuPage){
		case VR_MENU_PAGE_HUD:
			DrawQuestHudPage();
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
		default:
			DrawQuestSettingsPage();
			break;
		}
		return gVrMenuPixels;
	}

	for(int pixel = 0; pixel < VR_DEBUG_WIDTH*VR_DEBUG_HEIGHT; pixel++){
		gDebugPixels[pixel*4+0] = 5; gDebugPixels[pixel*4+1] = 4;
		gDebugPixels[pixel*4+2] = 12; gDebugPixels[pixel*4+3] = 225;
	}

	char value[96];
	if(!profilerVisible){
		sprintf(value, "OPENXR FPS:%d", gDebugFps);
		DrawDebugText(value, VR_DEBUG_WIDTH/2, 10, 3,
			255, 230, 64);
		return gDebugPixels;
	}

	const QuestProfilerSnapshot perf = QuestProfilerGetSnapshot();
	sprintf(value, "FPS:%d CPU:%.2f/%.2f STEP:%.2f",
		gDebugFps, perf.cpuAppMs, perf.cpuAppMaxMs,
		perf.cpuStepMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 1, 2,
		perf.cpuAppMs > perf.frameBudgetMs ? 255 : 100,
		perf.cpuAppMs > perf.frameBudgetMs ? 90 : 230, 120);
	sprintf(value, "PRE:%.2f VKB:%.2f POST:%.2f VKE:%.2f",
		perf.cpuPreMs, perf.cpuVkBeginMs, perf.cpuPostMs,
		perf.cpuVkEndMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 16, 2,
		perf.cpuVkBeginMs > perf.frameBudgetMs ? 255 : 100,
		perf.cpuVkBeginMs > perf.frameBudgetMs ? 90 : 230, 120);
	sprintf(value, "GAME:%.2f/%.2f SIM:%.2f STR:%.2f/%.2f",
		perf.cpuGameMs, perf.cpuGameMaxMs,
		perf.cpuSimOtherMs, perf.cpuStreamingMs,
		perf.cpuStreamingMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 31, 2,
		perf.cpuGameMs > perf.frameBudgetMs ? 255 : 100,
		perf.cpuGameMs > perf.frameBudgetMs ? 90 : 230, 120);
	sprintf(value, "AUDIO:%.2f/%.2f LIST:%.2f/%.2f",
		perf.cpuAudioMs, perf.cpuAudioMaxMs,
		perf.cpuWorldListMs, perf.cpuWorldListMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 46, 2,
		120, 210, 255);
	sprintf(value, "PRER:%.2f/%.2f SETUP:%.2f/%.2f",
		perf.cpuPreRenderMs, perf.cpuPreRenderMaxMs,
		perf.cpuSceneSetupMs, perf.cpuSceneSetupMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 61, 2,
		120, 210, 255);
	sprintf(value, "WORLD:%.2f/%.2f UI:%.2f/%.2f",
		perf.cpuWorldRenderMs, perf.cpuWorldRenderMaxMs,
		perf.cpuUiMs, perf.cpuUiMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 76, 2,
		120, 210, 255);
	if(perf.gpuFrameValid)
		sprintf(value, "GPU:%.2f [%s] GPU VK:%.2f",
			perf.gpuFrameMs,
			perf.gpuFrameRuntime ? "META" : "VK",
			perf.gpuVulkanValid ? perf.gpuVulkanMs : -1.0f);
	else
		sprintf(value, "GPU:N A GPU VK:%.2f",
			perf.gpuVulkanValid ? perf.gpuVulkanMs : -1.0f);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 91, 2,
		perf.gpuFrameValid &&
		 perf.gpuFrameMs > perf.frameBudgetMs ? 255 : 100,
		perf.gpuFrameValid &&
		 perf.gpuFrameMs > perf.frameBudgetMs ? 90 : 210, 255);

	// The local APP bracket includes beginFrame's previous-GPU fence wait.
	// Do not call that a CPU bottleneck when META is unavailable; CPU STEP is
	// the wait-free game/recording workload in that fallback case.
	const float cpuMs = perf.cpuAppRuntime ?
		perf.cpuAppMs : perf.cpuStepMs;
	const char *bottleneck = "UNKNOWN";
	if(perf.gpuFrameValid){
		if(cpuMs < perf.frameBudgetMs*0.8f &&
		   perf.gpuFrameMs < perf.frameBudgetMs*0.8f)
			bottleneck = "HEADROOM";
		else if(cpuMs > perf.gpuFrameMs+0.5f)
			bottleneck = "CPU";
		else if(perf.gpuFrameMs > cpuMs+0.5f)
			bottleneck = "GPU";
		else
			bottleneck = "BALANCED";
	}
	sprintf(value, "BUDGET:%.2f GAP:%.2f LIMIT:%s",
		perf.frameBudgetMs > 0.0f ? perf.frameBudgetMs : 13.89f,
		perf.cpuUnaccountedMs,
		bottleneck);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 106, 2,
		220, 180, 255);
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
	return gFxaaEnabled;
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
