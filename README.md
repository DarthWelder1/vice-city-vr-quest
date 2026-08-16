# Vice City VR for Meta Quest — source build kit

> **NO APK IS PROVIDED. THIS IS A SOURCE-BUILD RELEASE.**
>
> On Windows, install the Android Studio SDK components listed below, connect
> the Quest, then double-click **`BUILD_AND_INSTALL.bat`**. The wizard obtains
> the exact reVC source, assembles the port, builds your personal APK, installs
> it without clearing existing data, and can copy only the required folders
> from your legally owned Vice City installation.
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

The exact patch base is obtained separately from the public development fork:

<https://github.com/dubrovskiy-yevhen-stakelogic/re3-miami-vr>

The tested base is commit
`06d3ca5a7cce0021b84e6b7e1320a4f4e0ad3c87`. The assembly scripts never modify
that checkout: they copy it to a new directory, apply this project's patches,
and add the Quest-only files.

The upstream reVC project is <https://github.com/mrxenginner/reVC>. Its current
`miami` history does not contain the exact tested patch-base commit, so do not
substitute an arbitrary upstream revision when using the manual build path.

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

## Distribution boundary

Do not upload assembled source trees, APKs, original game files, saves, built
Modern model overlays or signing keys to this repository. Publish only this
build kit. Each player must obtain reVC and the original game separately and
perform the documented build locally.

## No affiliation

This is an unofficial fan project. It is not affiliated with or endorsed by
the publishers or developers of GTA Vice City, Meta, Khronos, Qualcomm or
NVIDIA. All trademarks belong to their owners.
