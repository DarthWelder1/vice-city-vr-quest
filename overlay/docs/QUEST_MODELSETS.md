# Classic and Modern model assets on Quest

The Quest port supports the same category-based model overlay shell as PC:

- World
- Vegetation
- Vehicles
- Peds
- Weapons

Classic vegetation is the default and recommended choice. HD palm/tree models
can be dramatically more expensive on a mobile GPU even when draw-call counts
look modest.

## Prepare the overlay on PC

Use the public PC Modern model-set builder:

<https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr/blob/main/MODERN_MODELS.md>

It must produce at least:

```text
modelsets/modern/models/gta3.img
modelsets/modern/models/gta3.dir
modelsets/modern/vegetation_models.txt
```

Do not redistribute the generated directory: it contains data derived from the
player's original game and external model packs.

## Copy to Quest

Close the game, create the destination directories, then copy the complete
generated `modern` folder below:

```text
/sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern
```

The normal Windows route is simply:

1. Connect and authorize the Quest.
2. Double-click `INSTALL_MODERN_MODELS.bat` in the source kit.
3. Select the generated `modern` folder.
4. Wait for `MODERN MODELS INSTALLED`, then fully restart the game.

The installer validates and stages the complete folder before replacing any
previous Modern overlay. The commands below are the manual fallback.

The Windows wizard normally installs ADB under
`C:\VCVRBuild\.android-sdk\platform-tools\adb.exe`. For example, in PowerShell:

```powershell
$adb = "C:\VCVRBuild\.android-sdk\platform-tools\adb.exe"
$modern = "C:\Games\Vice City VR\modelsets\modern"
& $adb shell am force-stop com.miamivr.quest
& $adb shell mkdir -p /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets
& $adb push $modern /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets
```

The final device path must not contain `modern/modern`. Verify it with:

```powershell
& $adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern/models
& $adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern/vegetation_models.txt
```

In the headset open `VR MENU > MODEL ASSETS`. Category changes are startup
choices: select the desired categories and fully restart the game. Keep
Vegetation on Classic unless testing the performance cost intentionally.

If the manifest is absent, the menu reports Vegetation as unavailable and the
loader forces that category to Classic instead of guessing model names.
