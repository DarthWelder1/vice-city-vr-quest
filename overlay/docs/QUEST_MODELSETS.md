# Classic and Modern model assets on Quest

The Quest port supports the same category-based model overlay shell as PC:

- World
- Vegetation
- Vehicles
- Peds
- Weapons

Classic vegetation is mandatory in the tested Quest build. HD palm/tree models
can be dramatically more expensive on a mobile GPU even when draw-call counts
look modest, so the one-button builder does not download or use the separate
vegetation pack.

## One-button download, build and installation

The player needs only:

- a legal original GTA Vice City PC installation;
- the Quest connected by USB with the computer authorized;
- the APK already installed by `BUILD_AND_INSTALL.bat`;
- about 4 GB of download bandwidth and at least 24 GB of temporary free space.

Double-click `INSTALL_MODERN_MODELS.bat` and choose the original GTA Vice City
folder. The wizard downloads and pins the exact tested
[GTA VC HD + Weapons](https://drive.google.com/file/d/1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj/view)
and [Mods / Atmosphere](https://drive.google.com/file/d/1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d/view)
archives. It then extracts, builds, validates and installs the overlay without
requiring 7-Zip, Git, Android Studio or any manual ADB command. Downloads are
cached under `C:\VCVRBuild\modern-assets` and resume after interruption. A
second run reuses verified downloads and extractions. If a complete overlay was
already built, it skips downloading, extracting and rebuilding and retries only
the Quest installation.

No third-party assets are stored in this source kit. The generated directory
contains original game and external mod data and must not be redistributed.
The completed overlay contains at least:

```text
modelsets/modern/models/gta3.img
modelsets/modern/models/gta3.dir
modelsets/modern/vegetation_models.txt
```

The LibertyCity/HD vegetation archive is not part of this workflow. The small
`vegetation_models.txt` file contains only names used by the runtime to select
the original Classic palm/tree entries. Matching Modern vegetation DFF entries
are physically removed from the generated archive as an additional fallback.

The wizard installs the complete folder at:

```text
/sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern
```

The installer stages and verifies the complete folder before replacing any
previous Modern overlay. Wait for `DOWNLOAD, BUILD AND QUEST INSTALL COMPLETED`,
then fully restart the game.

## Advanced manual transfer

If an overlay was already generated, it can be uploaded without rebuilding:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\install-modern-models.ps1 -ModernDir "C:\path\to\modern"
```

The commands below are a lower-level fallback.

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
Vegetation on Classic.

If the manifest is absent, the menu reports Vegetation as unavailable and the
loader forces that category to Classic instead of guessing model names.
