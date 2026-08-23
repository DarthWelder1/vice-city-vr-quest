#!/usr/bin/env bash
# Builds and optionally installs a personal Vice City VR Quest APK.
# No APK, reVC source, retail data, saves, or third-party model pack is
# distributed by this repository. The user supplies/downloads each input.
#
# Linux port of build-and-install.ps1. The one-click entry point is the
# top-level BUILD_AND_INSTALL.sh wrapper, which keeps the terminal open on
# error and always prints the diagnostic-log path.
#
# Arguments (Windows-style names are accepted for familiarity):
#   --game-dir DIR      / -GameDir DIR      your legally owned GTA Vice City PC folder
#   --work-dir DIR      / -WorkDir DIR      build scratch dir (default ~/VCVRBuild)
#   --android-sdk DIR   / -AndroidSdk DIR   Android SDK root
#   --java-home DIR     / -JavaHome DIR     JDK 21 home
#   --serial SERIAL     / -Serial SERIAL    Quest USB serial
#   --log-path PATH     / -LogPath PATH     diagnostic log location
#   --build-only        / -BuildOnly        stop after producing the APK
#   --skip-game-data    / -SkipGameData     do not copy game data
#   --non-interactive   / -NonInteractive   never prompt; fail instead
#   --help              / -h                show this help

set -uo pipefail

# ---- defaults -------------------------------------------------------------
GAME_DIR=""
WORK_DIR="${HOME}/VCVRBuild"
ANDROID_SDK=""
JAVA_HOME_ARG=""
SERIAL=""
LOG_PATH="${TMPDIR:-/tmp}/ViceCityVR-Build-And-Install.log"
BUILD_ONLY=0
SKIP_GAME_DATA=0
NON_INTERACTIVE=0

# ---- constants ------------------------------------------------------------
TESTED_REVC_COMMIT="026cd10f3fdbd92c089830e5067c4457c53c1b51"
REVC_URL="https://github.com/mrxenginner/reVC.git"
REVC_BRANCH="miami"
JDK_VERSION="21.0.11+10"
JDK_URL="https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.11%2B10/OpenJDK21U-jdk_x64_linux_hotspot_21.0.11_10.tar.gz"
# Verified against the official Adoptium download on 2026-08-23.
JDK_SHA256="4B2220E232A97997B436CA6AB15CBF70171ECFF52958A46159DFA5A8C44CA4DE"
ANDROID_CMDLINE_TOOLS_VERSION="15859902"
ANDROID_CMDLINE_TOOLS_URL="https://dl.google.com/android/repository/commandlinetools-linux-${ANDROID_CMDLINE_TOOLS_VERSION}_latest.zip"
# Verified against the official Google download on 2026-08-23.
ANDROID_CMDLINE_TOOLS_SHA256="4E4C464F145A7512B57D088AC6C278C03C9EEA610886B35A5E0804E74EEDF583"
GRADLE_VERSION="8.13"
GRADLE_URL="https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip"
# Verified against services.gradle.org on 2026-08-23 (platform-independent).
GRADLE_SHA256="20F1B1176237254A6FC204D8434196FA11A4CFB387567519C61556E8710AED78"
NDK_VERSION="27.2.12479018"
CMAKE_VERSION="3.22.1"
REMOTE_GAME_DATA="/sdcard/Android/data/com.miamivr.quest/files/gamedata"
SAVE_PROVIDER_URI="content://com.miamivr.quest.saves/slot/1"
PACKAGE_NAME="com.miamivr.quest"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ADB=""
REPLACED_INCOMPATIBLE_INSTALL=0
REPLACEMENT_GAME_DIR=""

# ---- logging --------------------------------------------------------------
LOG_DIR="$(dirname "$LOG_PATH")"
mkdir -p "$LOG_DIR" 2>/dev/null || true
# Mirror everything (stdout + stderr) to the diagnostic log as well as the
# terminal, so a failed run always leaves a persistent artifact.
exec > >(tee "$LOG_PATH") 2>&1

die() {
  echo ""
  echo "ERROR: $1" >&2
  echo "No failure was treated as success. Fix the reported item and rerun." >&2
  echo "Diagnostic log: $LOG_PATH" >&2
  exit 1
}

# Run a command, printing its output; die with a message if it fails.
checked() {
  local msg="$1"; shift
  "$@"
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    die "$msg (exit code $rc)."
  fi
}

# Run a command and capture its output without dying on a non-zero exit.
# Sets CAPTURED_OUTPUT and CAPTURED_EXIT.
capture() {
  CAPTURED_OUTPUT="$( "$@" 2>&1 )"
  CAPTURED_EXIT=$?
}

step() {
  echo ""
  echo "[$1/8] $2"
}

# ---- argument parsing -----------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir|-GameDir)      GAME_DIR="$2"; shift 2 ;;
    --work-dir|-WorkDir)      WORK_DIR="$2"; shift 2 ;;
    --android-sdk|-AndroidSdk) ANDROID_SDK="$2"; shift 2 ;;
    --java-home|-JavaHome)    JAVA_HOME_ARG="$2"; shift 2 ;;
    --serial|-Serial)         SERIAL="$2"; shift 2 ;;
    --log-path|-LogPath)      LOG_PATH="$2"; shift 2 ;;
    --build-only|-BuildOnly)      BUILD_ONLY=1; shift ;;
    --skip-game-data|-SkipGameData) SKIP_GAME_DATA=1; shift ;;
    --non-interactive|-NonInteractive) NON_INTERACTIVE=1; shift ;;
    --help|-h)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) die "Unknown argument: $1 (see --help)" ;;
  esac
done

# Re-point the log if --log-path was supplied after the initial redirect.
if [ "$LOG_PATH" != "${TMPDIR:-/tmp}/ViceCityVR-Build-And-Install.log" ]; then
  mkdir -p "$(dirname "$LOG_PATH")" 2>/dev/null || true
  exec > >(tee "$LOG_PATH") 2>&1
fi

# ---- toolchain discovery --------------------------------------------------
find_git() {
  local g
  g="$(command -v git || true)"
  if [ -n "$g" ]; then
    echo "$g"
    return 0
  fi
  die "git was not found on PATH. Install it (e.g. 'sudo pacman -S git' or 'sudo apt install git') and rerun."
}

# Return a JDK 21 home. Searches the supplied/env candidates, then common
# Android Studio JBR locations, then downloads a pinned official Temurin JDK.
find_java_home() {
  local candidates=()
  [ -n "$JAVA_HOME_ARG" ] && candidates+=("$JAVA_HOME_ARG")
  [ -n "${JAVA_HOME:-}" ] && candidates+=("$JAVA_HOME")
  candidates+=("/opt/android-studio/jbr" "${HOME}/Android/jbr" "${HOME}/.local/share/Android/jbr")

  local candidate java
  for candidate in "${candidates[@]}"; do
    [ -z "$candidate" ] && continue
    java="$candidate/bin/java"
    [ -x "$java" ] || continue
    if "$java" -version 2>&1 | grep -Eq 'version "21(\.|")'; then
      echo "$(cd "$candidate" && pwd)"
      return 0
    fi
  done

  # No suitable local Java: fetch a pinned official Temurin JDK 21.
  local downloads="$WORK_DIR/.downloads"
  local tools="$WORK_DIR/.tools"
  local zip="$downloads/OpenJDK21U-jdk_x64_linux_hotspot_21.0.11_10.tar.gz"
  local home="$tools/jdk-$JDK_VERSION"
  local bin="$home/bin/java"
  mkdir -p "$downloads" "$tools"

  if [ ! -x "$bin" ]; then
    if [ ! -f "$zip" ]; then
      echo "JDK 21 was not found; downloading official Eclipse Temurin $JDK_VERSION..."
      checked "Temurin JDK download failed" curl -fsSL --retry 5 --retry-all-errors -o "$zip" "$JDK_URL"
    fi
    local observed
    observed="$(sha256sum "$zip" | awk '{print toupper($1)}')"
    if [ "$observed" != "$JDK_SHA256" ]; then
      die "Temurin JDK archive hash mismatch. Expected $JDK_SHA256, got $observed. Delete $zip and retry."
    fi
    checked "Temurin JDK extraction failed" tar -xzf "$zip" -C "$tools"
  fi
  [ -x "$bin" ] || die "Temurin JDK extraction did not create $bin"
  echo "$home"
}

# Ensure the Android command-line tools exist under $1/cmdline-tools/latest.
ensure_android_command_line_tools() {
  local sdk_root="$1"
  local candidates=(
    "$sdk_root/cmdline-tools/latest/bin/sdkmanager"
    "$sdk_root/cmdline-tools/bin/sdkmanager"
  )
  local c
  for c in "${candidates[@]}"; do
    [ -x "$c" ] && { echo "$c"; return 0; }
  done

  local downloads="$WORK_DIR/.downloads"
  local tools="$WORK_DIR/.tools"
  local zip="$downloads/commandlinetools-linux-${ANDROID_CMDLINE_TOOLS_VERSION}_latest.zip"
  local extract_root="$tools/android-command-line-tools-${ANDROID_CMDLINE_TOOLS_VERSION}"
  local latest_root="$sdk_root/cmdline-tools/latest"
  local sdkmanager="$latest_root/bin/sdkmanager"
  mkdir -p "$downloads" "$tools" "$sdk_root"

  if [ ! -x "$sdkmanager" ]; then
    if [ ! -f "$zip" ]; then
      echo "Android SDK command-line tools are missing; downloading them from Google..."
      checked "Android command-line tools download failed" curl -fsSL --retry 5 --retry-all-errors -o "$zip" "$ANDROID_CMDLINE_TOOLS_URL"
    fi
    local observed
    observed="$(sha256sum "$zip" | awk '{print toupper($1)}')"
    if [ "$observed" != "$ANDROID_CMDLINE_TOOLS_SHA256" ]; then
      die "Android command-line tools hash mismatch. Expected $ANDROID_CMDLINE_TOOLS_SHA256, got $observed. Delete $zip and retry."
    fi
    checked "Android command-line tools extraction failed" unzip -q -o "$zip" -d "$extract_root"
    local extracted_tools="$extract_root/cmdline-tools"
    [ -f "$extracted_tools/bin/sdkmanager" ] || die "Android command-line tools archive has an unexpected layout."
    mkdir -p "$latest_root"
    cp -rf "$extracted_tools/." "$latest_root/"
  fi
  [ -x "$sdkmanager" ] || die "Android SDK bootstrap did not create $sdkmanager"
  echo "$sdkmanager"
}

# Return an Android SDK root. Searches candidates, then bootstraps a private
# one under $WORK_DIR/.android-sdk.
find_android_sdk() {
  local candidates=()
  [ -n "$ANDROID_SDK" ] && candidates+=("$ANDROID_SDK")
  [ -n "${ANDROID_HOME:-}" ] && candidates+=("$ANDROID_HOME")
  [ -n "${ANDROID_SDK_ROOT:-}" ] && candidates+=("$ANDROID_SDK_ROOT")
  candidates+=("${HOME}/Android/Sdk" "${HOME}/.android/sdk")

  local candidate
  for candidate in "${candidates[@]}"; do
    [ -z "$candidate" ] && continue
    if [ -f "$candidate/platform-tools/adb" ] || \
       [ -f "$candidate/cmdline-tools/latest/bin/sdkmanager" ] || \
       [ -f "$candidate/cmdline-tools/bin/sdkmanager" ]; then
      echo "$(cd "$candidate" && pwd)"
      return 0
    fi
  done

  local sdk_root="$WORK_DIR/.android-sdk"
  mkdir -p "$sdk_root"
  ensure_android_command_line_tools "$sdk_root" >/dev/null
  echo "$sdk_root"
}

# Print the sdkmanager packages that are still missing (one per line).
get_missing_sdk_packages() {
  local sdk_root="$1"
  [ -f "$sdk_root/platforms/android-35/android.jar" ] || echo "platforms;android-35"
  [ -d "$sdk_root/build-tools/34.0.0" ] || echo "build-tools;34.0.0"
  [ -f "$sdk_root/platform-tools/adb" ] || echo "platform-tools"
  [ -d "$sdk_root/ndk/$NDK_VERSION" ] || echo "ndk;$NDK_VERSION"
  [ -d "$sdk_root/cmake/$CMAKE_VERSION" ] || echo "cmake;$CMAKE_VERSION"
}

ensure_sdk_packages() {
  local sdk_root="$1"
  local missing
  missing="$(get_missing_sdk_packages "$sdk_root")"
  [ -z "$missing" ] && return 0

  echo "Missing Android SDK components:"
  echo "$missing" | sed 's/^/  /'
  local sdkmanager
  sdkmanager="$(ensure_android_command_line_tools "$sdk_root")"
  if [ "$NON_INTERACTIVE" -eq 1 ]; then
    die "Required Android SDK components are missing; automatic installation needs an interactive license confirmation."
  fi
  local answer
  read -r -p "Install the missing SDK components now? You may need to accept Google licenses [Y/n] " answer
  case "$answer" in
    [Yy]*|'') ;;
    *) die "Required SDK components were not installed." ;;
  esac
  yes | checked "Android SDK license step failed" "$sdkmanager" --sdk_root="$sdk_root" --licenses
  # shellcheck disable=SC2086
  checked "Android SDK component installation failed" "$sdkmanager" --sdk_root="$sdk_root" $missing
  local still
  still="$(get_missing_sdk_packages "$sdk_root")"
  if [ -n "$still" ]; then
    die "Android SDK Manager completed, but required components are still missing: $(echo "$still" | tr '\n' ', ')"
  fi
}

# Prepend -s SERIAL to adb arguments when a serial is pinned.
adb_args() {
  local out=()
  if [ -n "$SERIAL" ]; then out+=("-s" "$SERIAL"); fi
  out+=("$@")
  printf '%s\n' "${out[@]}"
}

# Run adb with a failure message; die on non-zero.
adb_checked() {
  local msg="${2:-ADB command failed}"
  local args=()
  if [ -n "$SERIAL" ]; then args+=("-s" "$SERIAL"); fi
  args+=("$1")
  "${args[@]}"
  local rc=$?
  [ "$rc" -eq 0 ] || die "$msg (exit code $rc)."
}

select_quest_device() {
  local output
  capture "$ADB" devices -l
  [ "$CAPTURED_EXIT" -eq 0 ] || die "adb devices failed."
  local devices=()
  local line
  while IFS= read -r line; do
    if [[ "$line" =~ ^([A-Za-z0-9._-]+)[[:space:]]+device([[:space:]]|$) ]]; then
      devices+=("${BASH_REMATCH[1]}")
    fi
  done <<< "$CAPTURED_OUTPUT"

  if [ -n "$SERIAL" ]; then
    local d
    for d in "${devices[@]}"; do [ "$d" = "$SERIAL" ] && return 0; done
    die "Quest '$SERIAL' is not connected and authorized. Check the USB debugging prompt inside the headset."
  fi
  if [ "${#devices[@]}" -eq 0 ]; then
    die "No authorized Android device found. Connect the Quest, enable USB debugging, and accept the prompt inside the headset."
  fi
  if [ "${#devices[@]}" -eq 1 ]; then
    SERIAL="${devices[0]}"
    return 0
  fi
  if [ "$NON_INTERACTIVE" -eq 1 ]; then
    die "More than one Android device is connected. Rerun with --serial DEVICE_SERIAL."
  fi
  echo "Connected devices:"
  local i
  for i in "${!devices[@]}"; do
    echo "  $((i + 1)). ${devices[$i]}"
  done
  local selection
  read -r -p "Select the Quest device number " selection
  [[ "$selection" =~ ^[0-9]+$ ]] || die "Invalid device selection."
  local idx=$((selection - 1))
  if [ "$idx" -lt 0 ] || [ "$idx" -ge "${#devices[@]}" ]; then
    die "Invalid device selection."
  fi
  SERIAL="${devices[$idx]}"
}

resolve_game_folder() {
  local requested="${1:-}"
  local required="${2:-0}"
  if [ -z "$requested" ]; then
    if [ "$NON_INTERACTIVE" -eq 1 ]; then
      die "--game-dir is required unless --skip-game-data or --build-only is used."
    fi
    echo ""
    echo "Enter your legally owned GTA Vice City PC installation folder."
    if [ "$required" -eq 1 ]; then
      echo "The previous app was removed, so its Quest data may have been erased. This folder is required to restore the game data."
    else
      echo "Press Enter to skip copying data if it is already installed on the Quest."
    fi
    read -r -p "Vice City folder " requested
    if [ -z "$requested" ]; then
      if [ "$required" -eq 1 ]; then
        die "A GTA Vice City PC folder is required after replacing an incompatible installed app."
      fi
      return 0
    fi
  fi
  [ -d "$requested" ] || die "Vice City folder does not exist: $requested"
  (cd "$requested" && pwd)
}

# Find a case-insensitive child directory of $1 named $2; print its path.
find_child_directory() {
  local parent="$1" name="$2"
  local entry
  for entry in "$parent"/*/; do
    [ -d "$entry" ] || continue
    local base
    base="$(basename "$entry")"
    if [ "${base,,}" = "${name,,}" ]; then
      echo "$entry"
      return 0
    fi
  done
  return 1
}

# ---- main -----------------------------------------------------------------
echo "Vice City VR v0.5.1 - personal Quest build wizard"
echo "This repository supplies source/build tooling only; no APK or GTA data is bundled."

step 1 "Preparing Git, JDK 21 and the Android SDK"
GIT_EXE="$(find_git)"
RESOLVED_JAVA_HOME="$(find_java_home)"
export JAVA_HOME="$RESOLVED_JAVA_HOME"
RESOLVED_SDK="$(find_android_sdk)"
export ANDROID_HOME="$RESOLVED_SDK"
export ANDROID_SDK_ROOT="$RESOLVED_SDK"
ensure_sdk_packages "$RESOLVED_SDK"
ADB="$RESOLVED_SDK/platform-tools/adb"
echo "Git: $GIT_EXE"
echo "JDK: $RESOLVED_JAVA_HOME"
echo "Android SDK: $RESOLVED_SDK"

step 2 "Preparing the short local build directory"
RESOLVED_WORK="$(cd "$(dirname "$WORK_DIR")" 2>/dev/null && pwd)/$(basename "$WORK_DIR")"
mkdir -p "$WORK_DIR"
RESOLVED_WORK="$(cd "$WORK_DIR" && pwd)"
if [ "${#RESOLVED_WORK}" -gt 80 ]; then
  echo "Warning: '$RESOLVED_WORK' is long; a short path is safer for native builds."
fi

step 3 "Obtaining the exact tested reVC source"
REVC_DIR="$RESOLVED_WORK/reVC-public-026cd10"
if [ ! -d "$REVC_DIR" ]; then
  checked "reVC clone failed" "$GIT_EXE" clone --no-checkout -b "$REVC_BRANCH" "$REVC_URL" "$REVC_DIR"
  checked "Could not select the tested reVC commit" "$GIT_EXE" -c "safe.directory=$REVC_DIR" -C "$REVC_DIR" checkout --detach "$TESTED_REVC_COMMIT"
else
  [ -d "$REVC_DIR/.git" ] || die "$REVC_DIR exists but is not the wizard's reVC checkout. Choose another --work-dir."
  local_head="$("$GIT_EXE" -c "safe.directory=$REVC_DIR" -C "$REVC_DIR" rev-parse HEAD 2>&1)"
  local_dirty="$("$GIT_EXE" -c "safe.directory=$REVC_DIR" -C "$REVC_DIR" status --porcelain 2>&1)"
  head_trim="$(echo "$local_head" | tr -d '[:space:]')"
  if [ "$head_trim" != "$TESTED_REVC_COMMIT" ] || [ -n "$local_dirty" ]; then
    die "$REVC_DIR is not a clean checkout of $TESTED_REVC_COMMIT. Preserve your work and choose another --work-dir."
  fi
  echo "Reusing verified clean reVC source."
fi

step 4 "Assembling the private Quest source tree"
BUILD_NAME="vice-city-vr-build"
ASSEMBLED_DIR="$RESOLVED_WORK/$BUILD_NAME"
if [ -e "$ASSEMBLED_DIR" ]; then
  ASSEMBLED_DIR="$RESOLVED_WORK/${BUILD_NAME}-$(date +%Y%m%d-%H%M%S)"
  echo "Previous build preserved; using $ASSEMBLED_DIR"
fi
checked "Quest source assembly failed" bash "$REPO_ROOT/tools/assemble.sh" "$REVC_DIR" "$ASSEMBLED_DIR"
[ -d "$ASSEMBLED_DIR/android" ] || die "Quest source assembly failed."

step 5 "Preparing verified Gradle $GRADLE_VERSION"
DOWNLOADS_DIR="$RESOLVED_WORK/.downloads"
TOOLS_DIR="$RESOLVED_WORK/.tools"
GRADLE_ZIP="$DOWNLOADS_DIR/gradle-${GRADLE_VERSION}-bin.zip"
GRADLE_HOME="$TOOLS_DIR/gradle-${GRADLE_VERSION}"
GRADLE_BIN="$GRADLE_HOME/bin/gradle"
mkdir -p "$DOWNLOADS_DIR" "$TOOLS_DIR"
if [ ! -x "$GRADLE_BIN" ]; then
  if [ ! -f "$GRADLE_ZIP" ]; then
    echo "Downloading Gradle $GRADLE_VERSION from services.gradle.org..."
    checked "Gradle download failed" curl -fsSL --retry 5 --retry-all-errors -o "$GRADLE_ZIP" "$GRADLE_URL"
  fi
  observed="$(sha256sum "$GRADLE_ZIP" | awk '{print toupper($1)}')"
  if [ "$observed" != "$GRADLE_SHA256" ]; then
    die "Gradle archive hash mismatch. Expected $GRADLE_SHA256, got $observed. Delete $GRADLE_ZIP and retry."
  fi
  checked "Gradle extraction failed" unzip -q -o "$GRADLE_ZIP" -d "$TOOLS_DIR"
fi
[ -x "$GRADLE_BIN" ] || die "Gradle extraction did not create $GRADLE_BIN"

step 6 "Building the personal debug-signed APK"
(
  cd "$ASSEMBLED_DIR/android"
  "$GRADLE_BIN" :app:assembleDebug --no-daemon
)
rc=$?
[ "$rc" -eq 0 ] || die "Android/ARM64 build failed (exit code $rc)."
APK="$ASSEMBLED_DIR/android/app/build/outputs/apk/debug/app-debug.apk"
[ -f "$APK" ] || die "Build completed without the expected APK: $APK"
APK_HASH="$(sha256sum "$APK" | awk '{print toupper($1)}')"
echo "APK: $APK"
echo "SHA256: $APK_HASH"

if [ "$BUILD_ONLY" -eq 1 ]; then
  echo "Build-only mode complete."
  echo "Diagnostic log: $LOG_PATH"
  exit 0
fi

step 7 "Installing on the connected Quest"
select_quest_device
echo "Quest: $SERIAL"
# Build the full adb argv (with -s SERIAL when pinned) and run it, capturing
# output because a failed install must be inspected, not treated as fatal
# before the incompatible-signature handling below.
adb_argv=("$ADB")
if [ -n "$SERIAL" ]; then adb_argv+=("-s" "$SERIAL"); fi
adb_argv+=("install" "-r" "$APK")
install_output="$("${adb_argv[@]}" 2>&1)"
install_rc=$?
echo "$install_output"
if [ "$install_rc" -ne 0 ]; then
  if echo "$install_output" | grep -q 'INSTALL_FAILED_UPDATE_INCOMPATIBLE'; then
    echo ""
    echo "The Quest already contains Vice City VR signed with a different key."
    echo "Android cannot update it in place."
    echo "WARNING: uninstalling the old app can erase its saves and all GTA data stored under the app."
    if [ "$NON_INTERACTIVE" -eq 1 ]; then
      die "Incompatible installed signature. Interactive confirmation is required before uninstalling $PACKAGE_NAME."
    fi
    read -r -p "Uninstall the old Vice City VR and install this build now? [y/N] " replace_answer
    case "$replace_answer" in
      [Yy]*) ;;
      *) die "The old app was left untouched. Back up anything you need, then rerun and approve replacement." ;;
    esac
    if [ "$SKIP_GAME_DATA" -eq 1 ]; then
      die "The old app was left untouched. Rerun without --skip-game-data and provide the GTA Vice City PC folder so erased game data can be restored after replacement."
    fi
    REPLACEMENT_GAME_DIR="$(resolve_game_folder "$GAME_DIR" 1)"
    [ -n "$REPLACEMENT_GAME_DIR" ] || die "A GTA Vice City PC folder is required to restore game data after replacement."
    adb_checked "uninstall" "Could not uninstall the incompatible Vice City VR package"
    adb_checked "install" "APK installation after removing the incompatible package failed"
    REPLACED_INCOMPATIBLE_INSTALL=1
    echo "The incompatible app was removed and the new APK was installed."
  else
    die "APK installation failed (exit code $install_rc)."
  fi
fi
# Run the save-provider bootstrap query with the same device prefix.
provider_argv=("$ADB")
if [ -n "$SERIAL" ]; then provider_argv+=("-s" "$SERIAL"); fi
provider_argv+=("shell" "content" "query" "--uri" "$SAVE_PROVIDER_URI" "--projection" "_display_name:_size")
provider_output="$("${provider_argv[@]}" 2>&1)"
provider_rc=$?
if [ "$provider_rc" -ne 0 ] || ! echo "$provider_output" | grep -q '_display_name=GTAVCsf1\.b'; then
  die "The APK installed, but the safe external-data bootstrap failed. Output: $provider_output"
fi
echo "Application storage bootstrap verified."

step 8 "Copying the user's game data and bundled VR hands"
if [ "$SKIP_GAME_DATA" -eq 0 ]; then
  if [ "$REPLACED_INCOMPATIBLE_INSTALL" -eq 1 ]; then
    RESOLVED_GAME="$REPLACEMENT_GAME_DIR"
  else
    RESOLVED_GAME="$(resolve_game_folder "$GAME_DIR")"
  fi
  if [ -n "$RESOLVED_GAME" ]; then
    required_folders=(data TEXT anim txd skins mp3 movies models Audio)
    declare -A resolved_folders
    for name in "${required_folders[@]}"; do
      folder="$(find_child_directory "$RESOLVED_GAME" "$name")" || die "Required game-data folder '$name' is missing from $RESOLVED_GAME. Use a complete PC installation."
      resolved_folders[$name]="${folder%/}"
    done
    for name in "${required_folders[@]}"; do
      folder="${resolved_folders[$name]}"
      echo "Copying $(basename "$folder")..."
      if [ -n "$SERIAL" ]; then
        "$ADB" -s "$SERIAL" push "$folder" "$REMOTE_GAME_DATA" || die "Failed to copy $folder"
      else
        "$ADB" push "$folder" "$REMOTE_GAME_DATA" || die "Failed to copy $folder"
      fi
    done
  else
    echo "Game-data copy skipped. The app requires existing data under $REMOTE_GAME_DATA."
  fi
else
  echo "Game-data copy skipped by --skip-game-data."
fi

# reVC does not run on retail data alone: it replaces several stock assets and
# its frontend reads strings and button icons that a 2002 installation never
# had. Without them the OPTIONS menus print "FET_GFX missing". These are part
# of the port, not the player's game data, so they are copied after it and also
# when --skip-game-data left the data alone.
port_text_files=(american.gxt french.gxt german.gxt italian.gxt russian.gxt spanish.gxt)
port_model_files=(fonts_r.txd frontend_ds2.txd frontend_ds3.txd frontend_ds4.txd frontend_x360.txd frontend_xone.txd generic.txd particle.txd ps3btns.txd x360btns.txd)
echo "Copying the port's replacement text and frontend assets..."
for name in "${port_text_files[@]}"; do
  [ -f "$ASSEMBLED_DIR/gamefiles/TEXT/$name" ] || die "Required port asset is missing from the assembled tree: gamefiles/TEXT/$name"
done
for name in "${port_model_files[@]}"; do
  [ -f "$ASSEMBLED_DIR/gamefiles/models/$name" ] || die "Required port asset is missing from the assembled tree: gamefiles/models/$name"
done
if [ -n "$SERIAL" ]; then
  "$ADB" -s "$SERIAL" shell mkdir -p "$REMOTE_GAME_DATA/TEXT"
  "$ADB" -s "$SERIAL" shell mkdir -p "$REMOTE_GAME_DATA/models"
else
  "$ADB" shell mkdir -p "$REMOTE_GAME_DATA/TEXT"
  "$ADB" shell mkdir -p "$REMOTE_GAME_DATA/models"
fi
[ $? -eq 0 ] || die "Could not create the port asset directories on the headset"
for name in "${port_text_files[@]}"; do
  if [ -n "$SERIAL" ]; then "$ADB" -s "$SERIAL" push "$ASSEMBLED_DIR/gamefiles/TEXT/$name" "$REMOTE_GAME_DATA/TEXT/$name"; else "$ADB" push "$ASSEMBLED_DIR/gamefiles/TEXT/$name" "$REMOTE_GAME_DATA/TEXT/$name"; fi
  [ $? -eq 0 ] || die "Failed to copy port asset TEXT/$name"
done
for name in "${port_model_files[@]}"; do
  if [ -n "$SERIAL" ]; then "$ADB" -s "$SERIAL" push "$ASSEMBLED_DIR/gamefiles/models/$name" "$REMOTE_GAME_DATA/models/$name"; else "$ADB" push "$ASSEMBLED_DIR/gamefiles/models/$name" "$REMOTE_GAME_DATA/models/$name"; fi
  [ $? -eq 0 ] || die "Failed to copy port asset models/$name"
done

hands_source="$ASSEMBLED_DIR/gamefiles/models/vrhands"
hands_remote="$REMOTE_GAME_DATA/models/vrhands"
hand_files=(BigHandLeft.uxrh BigHandRight.uxrh BigHandsAlbedo.png)
for name in "${hand_files[@]}"; do
  [ -f "$hands_source/$name" ] || die "Bundled VR hand file is missing: $name"
done
if [ -n "$SERIAL" ]; then
  "$ADB" -s "$SERIAL" shell mkdir -p "$hands_remote"
else
  "$ADB" shell mkdir -p "$hands_remote"
fi
[ $? -eq 0 ] || die "Could not create the VR hand data directory"
for name in "${hand_files[@]}"; do
  if [ -n "$SERIAL" ]; then "$ADB" -s "$SERIAL" push "$hands_source/$name" "$hands_remote/$name"; else "$ADB" push "$hands_source/$name" "$hands_remote/$name"; fi
  [ $? -eq 0 ] || die "Failed to copy VR hand asset $name"
done
if [ -n "$SERIAL" ]; then
  "$ADB" -s "$SERIAL" shell am force-stop "$PACKAGE_NAME"
else
  "$ADB" shell am force-stop "$PACKAGE_NAME"
fi
[ $? -eq 0 ] || die "Could not leave the app stopped after installation"

echo ""
echo "VICE CITY VR IS READY."
echo "The app was left stopped. Put on the headset and launch Vice City VR."
echo "Build directory: $ASSEMBLED_DIR"
echo "APK SHA256: $APK_HASH"
echo "Diagnostic log: $LOG_PATH"
exit 0
