# Build the Quest APK yourself

This repository does not distribute an APK or a complete reVC tree. The steps
below produce a personal debug-signed APK on your own machine.

## Easiest Windows method

1. Connect the Quest, enable USB debugging, and accept the authorization prompt
   inside the headset.
2. Double-click **`BUILD_AND_INSTALL.bat`** in this repository.

No Android Studio, system Git, system Java or manually prepared Android SDK is
required for this route. The wizard downloads portable Git, JDK 21, Android
command-line tools, Platform 35, Build-Tools 34.0.0, Platform-Tools, NDK
27.2.12479018, CMake 3.22.1 and Gradle 8.13. The user must accept Google's SDK
licenses when prompted.

Do not launch `tools\build-and-install.ps1` directly. The `.bat` wrapper keeps
the window open and always prints the location of the persistent diagnostic
log: `%TEMP%\ViceCityVR-Build-And-Install.log`. If the build fails, attach that
file instead of sending a video of a closing console window.

The wizard downloads and SHA256-verifies pinned official portable Git, Eclipse
Temurin JDK 21, Android command-line tools and Gradle. It then installs the
required SDK packages, obtains the exact tested public reVC commit, assembles
the private tree, builds a personal debug APK, installs it with
`adb install -r`, bootstraps application-owned storage, and asks for the legally
owned Vice City folder when game data must be copied. It never uninstalls the
app or clears its data silently. If Android reports that an older installation
was signed with a different key, the wizard explains that replacement can erase
saves and game data and asks for explicit confirmation before uninstalling it.
It validates the supplied Vice City PC folder before deleting the old app and
then restores the required game data after installing the new build.
It never uploads anything or bundles an APK/GTA data into this repository.

If somebody downloaded only `BUILD_AND_INSTALL.bat`, the BAT downloads and
extracts the complete public source kit automatically before starting the
wizard. Extracting the repository ZIP manually is still supported.

Advanced command-line example:

```powershell
.\BUILD_AND_INSTALL.bat -GameDir "C:\Games\Grand Theft Auto Vice City"
```

Use `-BuildOnly` to stop after producing the APK, or `-SkipGameData` when the
Quest already contains the required data. The manual steps below remain the
reference and troubleshooting path.

## 1. Install the toolchain

Tested versions:

- Git
- JDK 21
- Android SDK Platform 35 and Build Tools 34.0.0
- Android NDK `27.2.12479018`
- CMake 3.22.1
- Gradle 8.13, or Android Studio with an equivalent Gradle installation
- Android Platform Tools (`adb`)

On Windows, use short working paths such as `C:\src\reVC` and
`C:\src\vice-city-vr-build`; native object paths can otherwise exceed Windows
path limits.

## 2. Obtain the required reVC source

The source is supplied by the public upstream project, not by this repository:

```powershell
git clone --no-checkout -b miami https://github.com/mrxenginner/reVC.git C:\src\reVC
git -C C:\src\reVC checkout --detach 026cd10f3fdbd92c089830e5067c4457c53c1b51
```

Tested build-base repository: <https://github.com/mrxenginner/reVC>.

Use a clean checkout at that exact commit. A different revision or local edits
can make a patch fail; the script stops instead of silently producing a mixed
tree.

## 3. Assemble the private build tree

From this repository on Windows:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\assemble.ps1 `
  -Revc C:\src\reVC `
  -Out C:\src\vice-city-vr-build
```

Linux/macOS:

```sh
./tools/assemble.sh /path/to/reVC /path/to/vice-city-vr-build
```

The output directory must not already exist. The script copies reVC, applies
the Quest and runtime patch layers, overlays only new port files, and installs
the bundled librw fork under `vendor/librw`. The original checkout is not
modified. The assembled directory contains reVC-derived source and is a local
build product: do not commit or redistribute it.

## 4. Build a personal debug APK

Windows PowerShell:

```powershell
$env:JAVA_HOME = "C:\path\to\jdk-21"
$env:ANDROID_HOME = "C:\path\to\Android\Sdk"
$env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
Set-Location C:\src\vice-city-vr-build\android
gradle wrapper
.\gradlew.bat :app:assembleDebug
```

Linux/macOS:

```sh
cd /path/to/vice-city-vr-build/android
gradle wrapper
./gradlew :app:assembleDebug
```

Output:

```text
android/app/build/outputs/apk/debug/app-debug.apk
```

The debug build is signed with your machine's local Android debug key and is
sufficient for personal sideloading. `assembleRelease` is intentionally
blocked unless you create a private `android/release-signing.properties` from
the included example and provide your own keystore. Never commit either file.

## 5. Install without deleting existing data

Enable Developer Mode and USB debugging on the headset, then verify it appears:

```powershell
adb devices
adb install -r C:\src\vice-city-vr-build\android\app\build\outputs\apk\debug\app-debug.apk
```

`-r` updates the application in place and preserves its settings and saves.
Do not uninstall or clear application data when you want to retain them.

On a completely fresh install, bootstrap the external-data layout through the
APK's narrow save provider before copying any files:

```powershell
adb shell content query --uri content://com.miamivr.quest.saves/slot/1 `
  --projection _display_name:_size
```

The expected result is a zero-byte `GTAVCsf1.b` row. This creates the root and
the documented retail-data child directories as the application UID. Do
**not** pre-create `/sdcard/Android/data/com.miamivr.quest/files` with
`adb shell mkdir -p`: on current Horizon OS that can leave it owned by the ADB
shell and the game immediately exits with `cannot enter game data directory`.

## 6. Supply your own game data

The APK contains no GTA Vice City data. Copy the required directories from
your legally owned PC installation to:

```text
/sdcard/Android/data/com.miamivr.quest/files/gamedata/
```

The expected top-level data includes `data`, `TEXT`, `anim`, `txd`, `skins`,
`mp3`, `movies`, `models` and `Audio`. Saves/settings live separately under:

```text
/sdcard/Android/data/com.miamivr.quest/files/userfiles/
```

After the provider bootstrap has created the application-owned root, Android
scoped storage may still require individual child directories. At that point
they may be created with `adb shell mkdir -p`. Push each top-level folder into
`gamedata`, not into another folder of the same name. Detailed examples are in
[overlay/docs/QUEST_PORT.md](overlay/docs/QUEST_PORT.md).

The VR hand meshes are original MIT-licensed port assets, not GTA data and not
part of the APK. Copy them once from the assembled tree:

```powershell
adb shell mkdir -p /sdcard/Android/data/com.miamivr.quest/files/gamedata/models
adb push C:\src\vice-city-vr-build\gamefiles\models\vrhands `
  /sdcard/Android/data/com.miamivr.quest/files/gamedata/models
adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/models/vrhands
```

The final directory must directly contain `BigHandLeft.uxrh`,
`BigHandRight.uxrh` and `BigHandsAlbedo.png`; avoid a nested
`vrhands/vrhands` directory.

Optional third-party Modern models are installed separately after the APK and
base game data. Connect and authorize the Quest, then double-click
`INSTALL_MODERN_MODELS.bat` and select the legal original GTA Vice City PC
folder. The one-button wizard downloads and verifies the two tested external
packs, builds the personal overlay, and installs it; no APK rebuild is required.
The vegetation pack is not used and palms remain Classic. See the exact inputs,
space requirements and manual fallback in
[README.md](README.md#install-the-optional-modern-models-after-the-apk) and the
detailed category notes in
[overlay/docs/QUEST_MODELSETS.md](overlay/docs/QUEST_MODELSETS.md). No Modern
assets are included in this repository.

## Troubleshooting

- `patch does not apply`: reset reVC to the exact tested commit and update its
  submodules; do not assemble from a modified checkout.
- `ninja: mkdir ... No such file or directory`: use shorter source/output paths.
- SDK/NDK not found: set `ANDROID_HOME`/`ANDROID_SDK_ROOT` or create
  `android/local.properties` with `sdk.dir=...`.
- release signing error: build `assembleDebug`; release keys are optional and
  intentionally private.
- app exits or stays in theater mode: verify the game-data path and filename
  case, then inspect `adb logcat -s MiamiVR:V librw-vk:V`.
