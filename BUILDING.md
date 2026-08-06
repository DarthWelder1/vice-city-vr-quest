# Building the Quest APK

You build the APK yourself, on your own machine, from source you assemble
locally. Nothing prebuilt is distributed.

## Prerequisites

- **JDK 17**
- **Android SDK** (API 32+) and **NDK r26+** — easiest via Android Studio
- **Gradle 8+** (or Android Studio, which brings its own)
- **git** (used by the assemble script to apply the patch)
- **The reVC source tree** — final master state (development stopped
  September 2021), obtained by you. The assemble script refuses trees that
  do not look like reVC and reports if the patch does not apply cleanly.

## Assemble the source tree

Windows:

```powershell
.\tools\assemble.ps1 -Revc C:\path\to\revc -Out C:\path\to\build-tree
```

Linux/macOS:

```sh
tools/assemble.sh /path/to/revc /path/to/build-tree
```

The script copies the reVC tree, applies `patches/revc-quest.patch`, overlays
the port's own sources and puts the librw fork into `vendor/librw`. Your
original reVC checkout is not modified.

## Build

```
cd <build-tree>/android
gradle wrapper        # first time only; or just open the folder in Android Studio
./gradlew assembleDebug
```

The APK appears in `app/build/outputs/apk/debug/`. It is signed with your
local debug key — sufficient for sideloading to your own headset.

## Install and game data

```
adb install app-debug.apk
```

Then push the data files from your own copy of the game to the headset as
described in [overlay/docs/QUEST_PORT.md](overlay/docs/QUEST_PORT.md) —
including the quirk that `adb push` cannot create directories under
`/sdcard/Android/data/`, so the directory tree must be created with
`adb shell mkdir -p` first.

## Troubleshooting

- **Patch fails to apply** — your reVC tree is either modified or not the
  final master state. Start from a pristine tree.
- **Gradle cannot find the SDK** — create `android/local.properties` with
  `sdk.dir=<path to Android SDK>`, or set `ANDROID_HOME`.
- **App starts flat / immediately exits** — game data missing or in the
  wrong place; see the data section of QUEST_PORT.md.
