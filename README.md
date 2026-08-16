# Vice City VR for Meta Quest — source build kit

> **NO APK IS PROVIDED. THIS IS A SOURCE-BUILD RELEASE.**
>
> On Windows, connect the Quest and double-click **`BUILD_AND_INSTALL.bat`**.
> The wizard obtains portable Git, JDK 21, the Android command-line SDK,
> the exact reVC source, assembles the port, builds your personal APK, installs
> it without clearing existing data, and can copy only the required folders
> from your legally owned Vice City installation.
> Missing build tools and SDK packages are downloaded from their official
> sources and verified against pinned SHA256 hashes automatically. Google SDK
> licenses are still shown for the user to accept.
> If an older installation has an incompatible signing key, the wizard warns
> that replacing it can erase saves/game data and uninstalls it only after an
> explicit `Y` confirmation. It validates the supplied Vice City PC folder
> before removal so the required game data can be restored afterward.
> You may also download only **`BUILD_AND_INSTALL.bat`**. If the rest of the
> source kit is not beside it, the BAT downloads and extracts the complete
> public repository automatically.
>
> Run the `.bat`, not the internal PowerShell file. The window remains open on
> errors and writes `%TEMP%\ViceCityVR-Build-And-Install.log`; send that log
> when asking for help.

This repository contains the original Quest/OpenXR port layer, Vulkan backend
changes, build scripts and reVC patch files needed to build Vice City VR for a
Meta Quest headset.

Current source-kit version: **v0.5.1 alpha**.

It intentionally contains **no APK, complete reVC source tree, original game
files, saves, logs or third-party Modern model packs**. Every user builds their
own APK and supplies data from a legally owned PC copy of GTA Vice City.

## What is included

| Path | Contents |
|---|---|
| `overlay/` | New Android, OpenXR, Quest VR and tooling files |
| `patches/` | Changes applied to a user-supplied reVC checkout |
| `librw/` | MIT-licensed librw fork with the Quest Vulkan backend |
| `tools/` | Windows and Unix source-assembly scripts |

Modified upstream reVC files are distributed as patches, not as a second copy
of the reVC repository. The only bundled art is the credited MIT-licensed VR
hand asset under `overlay/gamefiles/models/vrhands/`.

## Required external source

The exact patch base comes from the public upstream reVC project:

<https://github.com/mrxenginner/reVC>

The tested public `miami` commit is
`026cd10f3fdbd92c089830e5067c4457c53c1b51`. The assembly scripts never modify
that checkout: they copy it to a new directory, apply the public-base
compatibility patch followed by this project's patches, and add Quest-only
files. No private repository is required or referenced.

Complete prerequisites and copy-paste commands are in
[BUILDING.md](BUILDING.md).

For most Windows users, the recommended route is the guided
[`BUILD_AND_INSTALL.bat`](BUILD_AND_INSTALL.bat) wizard instead of entering
those commands manually.

The source-only maintainer checks are in [RELEASING.md](RELEASING.md).

## Current Quest build

- Native arm64 Android application using OpenXR and Vulkan multiview.
- Physical VR weapons, two-hand support, holsters and scoped aiming.
- Physical steering for cars and motorcycles, including model steering wheels.
- In-headset VR, calibration, cheats, traffic, graphics and model-set menus.
- Stereo-safe building culling with an exact OFF fallback.
- Physics Director V2 is enabled by default for CPU headroom, remains
  experimental, and has an exact ORIGINAL/OFF fallback in the Traffic menu.
- Classic/Modern asset categories; no external model pack is included.
- Default quality profile: 125% render scale, sustained CPU/GPU hints,
  Spatial AA, AUTHORED occlusion culling, Modern world textures/weapons and
  Classic vehicles, pedestrians and vegetation when a user-built Modern
  overlay is available. AUTHORED gives substantially better performance but
  remains experimental; use STEREO SAFE or OFF if geometry differs between
  eyes or disappears incorrectly.
- Modern vehicles can be expensive on Quest, especially with high traffic;
  Classic vehicles remain the default and the recommended fallback.
- Normal frontend/save loading on first launch; the developer Quick Test Start
  shortcut remains available but is off by default.

Experimental temporal AA, SGSR, MSAA and runtime-only foveation are disabled:
headset testing did not establish a safe visual or performance benefit.

Runtime/data layout is documented in
[overlay/docs/QUEST_PORT.md](overlay/docs/QUEST_PORT.md). Optional model mixing
is documented in [overlay/docs/QUEST_MODELSETS.md](overlay/docs/QUEST_MODELSETS.md).

## Install the optional Modern models after the APK

No APK rebuild is needed. Connect and authorize the Quest, then double-click
**`INSTALL_MODERN_MODELS.bat`**. Select the folder containing a legal original
GTA Vice City PC installation when asked. That is the only asset input the
player must supply.

The wizard automatically downloads and verifies the two external packs used by
the tested build, extracts them, builds the per-user `modelsets\modern` overlay,
and installs it on the connected Quest. Expect about 4 GB of downloads and keep
at least 24 GB free for downloads, extraction and the staged build. Interrupted
downloads are retained and resumed. These are the exact external inputs:

- [GTA VC HD + Weapons](https://drive.google.com/file/d/1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj/view)
- [Mods / Atmosphere](https://drive.google.com/file/d/1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d/view)

The HD vegetation/LibertyCity pack is deliberately **not** downloaded or used.
The builder also removes matching palm/tree geometry found in the HD base and
generates a manifest that routes those models to the original Classic assets.
No original game data or third-party pack is bundled in this repository, and
the generated overlay must not be redistributed.

Before replacing an existing overlay, the installer validates the complete
build, uploads it through a temporary directory, and verifies the required
files on the headset. A failed download, build or transfer does not silently
replace a working Modern folder.

The following manual PowerShell method remains available for troubleshooting.
Change `$modern` to the actual generated folder on the PC. The `$adb` path is
the default created by `BUILD_AND_INSTALL.bat`; use the matching path if
`-WorkDir` was changed.

```powershell
$adb = "C:\VCVRBuild\.android-sdk\platform-tools\adb.exe"
$modern = "C:\Games\Vice City VR\modelsets\modern"

& $adb devices
& $adb shell am force-stop com.miamivr.quest
& $adb shell mkdir -p /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets
& $adb push $modern /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets
& $adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern/models
& $adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern/vegetation_models.txt
```

For troubleshooting, an already generated folder can still be transferred by
running `tools\install-modern-models.ps1 -ModernDir "C:\path\to\modern"`.
The final Quest path must be exactly
`gamedata/modelsets/modern`, never `modern/modern`. Fully restart the game after
copying. The default/recommended Quest mix is Modern world textures and
weapons, with Classic vehicles, pedestrians and vegetation. Other categories
can be changed under `VR MENU > MODEL ASSETS`; every change requires a full
process restart. Modern vehicles are GPU-heavy, particularly with high traffic,
and vegetation is intentionally kept Classic.

## Distribution boundary

Do not upload assembled source trees, APKs, original game files, saves, built
Modern model overlays or signing keys to this repository. Publish only this
build kit. Each player must obtain reVC and the original game separately and
perform the documented build locally.

## No affiliation

This is an unofficial fan project. It is not affiliated with or endorsed by
the publishers or developers of GTA Vice City, Meta, Khronos, Qualcomm or
NVIDIA. All trademarks belong to their owners.
