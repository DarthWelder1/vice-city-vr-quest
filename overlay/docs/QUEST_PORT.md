# Quest runtime and data layout

Vice City VR is a native arm64 Android/OpenXR application. The Meta runtime
creates the Vulkan instance/device and the renderer submits both eyes in one
multiview pass to a two-layer OpenXR swapchain.

## Runtime layout

- Package: `com.miamivr.quest`
- Game data: `/sdcard/Android/data/com.miamivr.quest/files/gamedata/`
- Saves/settings: `/sdcard/Android/data/com.miamivr.quest/files/userfiles/`
- VR settings: `gamedata/vr_settings.ini`

The game directory must contain the data from the user's own PC copy. This
project does not provide those files.

Expected top-level directories include:

```text
Audio/
TEXT/
anim/
data/
models/
movies/
mp3/
skins/
txd/
```

The Android filesystem is case-sensitive. The port resolves normal game-data
requests case-insensitively, but preserving the original names remains the
safest setup.

## Copy data with adb

For a completely fresh installation, install the APK and invoke its narrow save
provider **before** using `adb push`:

```sh
adb shell content query --uri content://com.miamivr.quest.saves/slot/1 --projection _display_name:_size
```

The expected result is a zero-byte `GTAVCsf1.b` row. This creates
`files/gamedata` and the standard retail-data children as the application UID.
Never create `/sdcard/Android/data/com.miamivr.quest/files` yourself before
this bootstrap, because current Horizon OS can assign it to the shell UID and
the game will be unable to enter its own data directory.

Scoped storage can refuse to create deep children during `adb push`. If that
happens, create the required tree with `adb shell mkdir -p` first. Push each
source directory to the `gamedata` parent:

```sh
adb push "C:\path\to\Vice City\models" /sdcard/Android/data/com.miamivr.quest/files/gamedata
adb push "C:\path\to\Vice City\data" /sdcard/Android/data/com.miamivr.quest/files/gamedata
adb push "C:\path\to\Vice City\TEXT" /sdcard/Android/data/com.miamivr.quest/files/gamedata
```

Do not push `models` to an already existing `.../gamedata/models` target;
`adb` can create an incorrect `models/models` nesting.

## Install the bundled VR hand assets

The hand meshes are MIT-licensed assets from this port, not original GTA data.
They remain outside the APK so the runtime loads them through the same game-data
filesystem. From the assembled source tree run:

```sh
adb shell mkdir -p /sdcard/Android/data/com.miamivr.quest/files/gamedata/models
adb push gamefiles/models/vrhands /sdcard/Android/data/com.miamivr.quest/files/gamedata/models
adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/models/vrhands
```

The final directory must directly contain `BigHandLeft.uxrh`,
`BigHandRight.uxrh` and `BigHandsAlbedo.png`.

## Rendering and performance defaults

Fresh installations request:

- 72 Hz
- 125% render scale relative to the OpenXR-recommended eye size
- sustained CPU and GPU performance hints
- Spatial AA enabled
- stereo-safe culling enabled
- adaptive Physics Director with an ORIGINAL/OFF fallback
- stock vehicle visual LOD behavior

The Graphics menu shows both requested and actually active render scale. If a
high-resolution allocation fails, the next launch recovers to 100% and records
the fallback instead of continuing to claim the unavailable scale.

Temporal AA/SGSR, experimental MSAA and runtime-only foveation are disabled in
the release profile. They remain research code only and are not presented as
working player features.

## Model assets

The standard setup uses original Classic assets. An optional user-built Modern
overlay can be placed under:

```text
gamedata/modelsets/modern/
```

No Modern pack is distributed here. Category changes require a full process
restart. The recommended Quest mix keeps vegetation, vehicles and pedestrians
Classic while allowing Modern world textures and weapons. See
[QUEST_MODELSETS.md](QUEST_MODELSETS.md).

## Logs and updates

Useful runtime log:

```sh
adb logcat -s MiamiVR:V librw-vk:V
```

Update APKs with `adb install -r` to preserve application data. Settings and
saves are not part of the APK and should be backed up separately before major
experiments.
