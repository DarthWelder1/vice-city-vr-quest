# Native Quest 3 port

Branch `questNative`, plus `questNative` in the nested `vendor/librw` repository.

Target: standalone Android arm64 on Quest 3, OpenXR bound to Vulkan, no PC in
the loop. This is a different platform from the Windows build in every layer
that touches the OS or the GPU, so it is being landed in stages rather than as
one switch.

## Current state

Verified on hardware (Quest 3, Adreno 740, OpenXR runtime "Oculus"):

- APK builds for `arm64-v8a` and installs
- OpenXR session reaches `XR_SESSION_STATE_FOCUSED`, no crashes
- Per-eye target 1680x1760, one swapchain of 3 images with `arraySize = 2`,
  format `VK_FORMAT_R8G8B8A8_SRGB`
- Both eyes rendered in a single pass with `VK_KHR_multiview`
- `STAGE` reference space (floor-relative), which is what a standing game wants
- librw compiles for arm64 with `PLATFORM_VULKAN` registered

What renders today is the bring-up scene, not the game.

## Why the Windows build cannot simply be recompiled

| Windows build | Quest |
| --- | --- |
| D3D12 backend (7.4k lines in `vendor/librw/src/d3d12`) | no D3D at all; Vulkan only |
| Double-wide stereo target, instanced x2 with clip distances | array target + `VK_KHR_multiview` |
| `XR_KHR_D3D12_enable`, app creates the device | `XR_KHR_vulkan_enable2`, **runtime** creates the device |
| Streamline / DLAA (`nvngx_dlss.dll`, `sl.interposer`) | NVIDIA-only, removed entirely |
| D3D12 Tier 2 VRS for fixed foveation | Meta's foveation extensions instead |
| `src/skel/win/win.cpp`, DirectInput, XInput | `native_app_glue`, OpenXR actions |
| `GetPrivateProfileIntA` etc. (53 call sites) | shimmed, see below |
| Game directory beside the exe | app-specific external storage |
| DXT/BC textures uploaded natively | **Adreno has no BC support** |

## Architecture decisions made so far

**OpenXR owns the Vulkan device.** On Android the runtime must create the
`VkInstance` and `VkDevice` itself so it can inject the extensions it needs to
share swapchain images. The librw backend therefore *adopts* a device through
`EngineOpenParams` instead of opening one. This inverts the D3D12 entry path and
is the single biggest structural difference in the renderer.

**Multiview instead of double-wide.** The D3D12 path renders both eyes into one
wide target using instancing and clip distances. On a tiler that is the wrong
shape: it doubles the tile footprint and defeats the binning. The Vulkan path
uses a two-layer array target with view mask `0x3`, and `gl_ViewIndex` selects
the per-eye matrix. One draw call, both eyes, no manual clipping.

**Depth is discarded on store.** Nothing reads depth after the pass, and
skipping the writeback of a two-layer depth surface every frame is one of the
larger easy wins on mobile.

**The INI API is shimmed, not rewritten.** The VR layer stores every setting and
all per-weapon calibration through the Win32 profile API. Reimplementing that
contract in `platform_android.cpp` is far cheaper than touching 53 call sites,
and it keeps `vr_settings.ini` byte-compatible with the desktop build, so a
calibration file can move between the two.

**Case-insensitive data paths.** The game asks for `DATA\\GTA_VC.DAT` and
`models/gta3.img` interchangeably. Windows does not care; ext4 does. Path
resolution walks components and matches case-insensitively, scanning a directory
only for components that do not already exist verbatim.

## Open problem: textures

Adreno exposes ETC2 and ASTC but **not BC/DXT**, and every Vice City TXD is
DXT1/3/5. The blocks cannot be handed to Vulkan the way they are on D3D12.
`allocateCompressed` reports failure on purpose so callers fall back rather than
creating an unsampleable image. Two viable routes:

1. Decode DXT to RGBA at load time. Simple, but inflates VRAM roughly 4-8x and
   costs CPU during streaming, which is already the frame-time hot spot.
2. Transcode the whole data set to ASTC once, offline, during staging onto the
   device. Better runtime cost and bandwidth, needs a staging tool and roughly
   doubles the on-device data size unless the originals are replaced.

Route 2 is the better answer for a shipping build. Route 1 is the faster answer
for bring-up.

## Remaining work

| Stage | State |
| --- | --- |
| Toolchain, Gradle/CMake, APK | done |
| OpenXR + Vulkan session, multiview | done |
| Android platform layer (paths, INI shim) | done |
| librw `PLATFORM_VULKAN` registration, device, raster | done |
| librw object pipeline (`vkpipe.cpp`) | written, compiles, not yet exercised |
| librw immediate mode (`vkim.cpp`) | written, compiles, not yet exercised |
| librw pipeline/descriptor state (`vkstate.cpp`) | written, compiles, not yet exercised |
| librw shaders -> SPIR-V | done |
| Game sources compile for arm64 | done: 242/242 clean |
| Game data staged on device | done: 1.6 GB |
| DXT decode or ASTC transcode | not started |
| Port the 8590-line VR layer off D3D12 | not started |
| Android skeleton (RsGlobal, event loop, file system) | not started |
| Audio: OpenAL Soft for arm64, replace mpg123 | not started |
| Link the game into the APK | not started |

### Getting the game sources to compile

Measured with `clang -fsyntax-only --target=aarch64-linux-android32` over every
`.cpp` outside `src/skel/win` and `src/vr`. It started at 218/244 clean, which
was already better than expected, and almost all of the failures came from two
root causes rather than from genuinely Windows-specific code:

**`#define clamp` vs libc++.** `common.h` defines a function-like `clamp` macro,
which rewrites the declaration of `std::clamp` in any translation unit that
reaches `<algorithm>` afterwards. On Windows that rarely happens; against libc++
the STL arrives transitively nearly everywhere, and this single macro accounted
for 216 of 257 error lines across 26 files. Fixed by parsing `<algorithm>` in
`common.h` before the macro is defined, so the 208 existing call sites are
untouched. 218 -> 231 clean from one line.

**`RW_GL3` doubling as "not Windows".** Throughout the platform layer `RW_GL3`
gates windowing, mouse and joystick code that has nothing to do with the
graphics API, in two-way `_WIN32`-or-GLFW splits with no third option. The
Vulkan target fell into the DirectInput branch and hit `DIJOYSTATE2`,
`DIDEVCAPS`, `AllValidWinJoys`. Fixed by adding an Android branch: a
`psGlobalType` without the GLFW window handle, `GlfwJoyState` (which contains no
GLFW types) reused for the pad, and no-op mouse paths, since Quest has no mouse
and input arrives through OpenXR actions. 231 -> 240.

The last two were one-liners: `re3.cpp` hid its `crossplatform.h` include behind
`DETECT_JOYSTICK_MENU`, which is off without XINPUT, so the Win32 shims it uses
were missing; and `CapturePad` was declared only under `RW_GL3`.

Excluded from the build rather than fixed: `src/audio/eax` (DirectSound) and
`sampman_null.cpp` (conflicts with the OpenAL backend).

None of this is linked yet -- `CapturePad` and the whole `RsGlobal` skeleton
still need Android implementations.

Nothing calls `Engine::open` yet, so the drawing path is compiled and linked but
has never run. The first thing the game bring-up will do is prove or disprove
it.

The largest remaining item is the VR layer: 184 D3D12 references and 60
`RW_D3D12` guards across 8590 lines.

## Suggested order from here

1. Initialise the librw engine against the OpenXR-created device and draw one
   DFF. This exercises the whole backend before 700 translation units land on
   top of it, and it is where the DXT problem will first bite.
2. Bring the game sources across, starting with the non-rendering ones. re3
   already builds for linux-arm64, so the Linux/glfw skeleton is a much closer
   starting point than the Windows one.
3. Port the VR layer, keeping the OpenXR action system and replacing only the
   graphics binding.

## Building

Toolchain lives in `C:\Dev\android-toolchain` (JDK 21, SDK platform 35,
build-tools 35.0.0, NDK 27.2.12479018, CMake 3.22.1, Gradle 8.13, platform-tools).

```bash
cd android && gradle assembleDebug
```

Install and run:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Watch the app's own log:

```bash
adb logcat -s MiamiVR:V librw-vk:V
```

`MIAMIVR_BRINGUP` in `android/app/src/main/cpp/CMakeLists.txt` selects the
bring-up target; turning it off is what will eventually pull in the game.

## Game data

The original Vice City data goes to app-specific external storage, which needs
no runtime permission. Only the data directories are wanted; a desktop build
directory also contains Windows executables, DLLs and logs that have no use
here. On this machine the source is `C:\Users\user\Documents\VICE CITY VR`, and
the relevant set is `data`, `TEXT`, `anim`, `txd`, `skins`, `mp3`, `movies`,
`models`, `Audio` -- 1.6 GB of the 2.8 GB total.

Two traps, both hit while staging it the first time:

**`adb push` cannot create directories under `/sdcard/Android/data/<pkg>/`.**
Scoped storage refuses its internal `secure_mkdirs`, so a recursive push of a
tree with subdirectories fails with `Operation not permitted` while still
creating the top-level directory, which looks like a partial success. Writing
*files* into a directory that already exists works fine, and `adb shell mkdir`
works fine. So create the whole tree first, then push:

```bash
adb shell mkdir -p /sdcard/Android/data/com.miamivr.quest/files/gamedata/data/maps/downtown
```

...for every subdirectory (`data/maps/*`, `data/paths`, `models/coll`,
`models/generic`, `models/vrhands`), then push each top-level directory into the
*parent*:

```bash
adb push "<src>/models" /sdcard/Android/data/com.miamivr.quest/files/gamedata
```

**Push into the parent, not into the target itself.** `adb push local remote`
appends `basename(local)` when `remote` already exists, so pushing `models` to
`.../gamedata/models` produces `.../gamedata/models/models`.

Case matters. The source has `Audio` with a capital A while the game asks for
`audio`; ext4 is case sensitive and Windows was not, which is what
`platform::resolveGamePathCaseInsensitive` exists to absorb.
