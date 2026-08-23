#!/usr/bin/env bash
# Repairs an existing Vice City VR installation that is missing the port's
# replacement text and frontend assets.
#
# reVC does not run on retail Vice City data alone. Its frontend reads strings
# and button icons that a 2002 installation never shipped, and when they are
# absent the OPTIONS menus print "FET_GFX missing" and the like where the
# labels belong. Installations made before the build wizard copied these
# files show exactly that.
#
# This script copies nothing but those port assets, from the reVC source the
# wizard already downloaded. It touches no save, no setting and no model data.
#
# Linux port of install-game-files.ps1.
#
# Arguments:
#   --game-files-dir DIR / -GameFilesDir DIR   reVC source with gamefiles/
#   --work-dir DIR       / -WorkDir DIR        wizard work dir (default ~/VCVRBuild)
#   --android-sdk DIR    / -AndroidSdk DIR     Android SDK root
#   --serial SERIAL      / -Serial SERIAL      Quest USB serial
#   --log-path PATH      / -LogPath PATH       diagnostic log location
#   --non-interactive    / -NonInteractive     never prompt; fail instead

set -uo pipefail

GAME_FILES_DIR=""
WORK_DIR="${HOME}/VCVRBuild"
ANDROID_SDK=""
SERIAL=""
LOG_PATH="${TMPDIR:-/tmp}/ViceCityVR-Install-Game-Files.log"
NON_INTERACTIVE=0

INSTALLER_VERSION="0.5.2-gamefiles-1"
PACKAGE_NAME="com.miamivr.quest"
REMOTE_GAME_DATA="/sdcard/Android/data/${PACKAGE_NAME}/files/gamedata"
TESTED_REVC_COMMIT="026cd10f3fdbd92c089830e5067c4457c53c1b51"
REVC_URL="https://github.com/mrxenginner/reVC.git"
REVC_BRANCH="miami"
REVC_DIR_NAME="reVC-public-026cd10"
ADB=""

# The exact set the Quest build reads and a retail installation does not
# provide. CFont::Initialise loads MODELS/X360BTNS.TXD at start-up; the rest
# are reVC's replacements for the stock text and frontend art.
port_text_files=(american.gxt french.gxt german.gxt italian.gxt russian.gxt spanish.gxt)
port_model_files=(fonts_r.txd frontend_ds2.txd frontend_ds3.txd frontend_ds4.txd frontend_x360.txd frontend_xone.txd generic.txd particle.txd ps3btns.txd x360btns.txd)

while [ $# -gt 0 ]; do
  case "$1" in
    --game-files-dir|-GameFilesDir) GAME_FILES_DIR="$2"; shift 2 ;;
    --work-dir|-WorkDir)            WORK_DIR="$2"; shift 2 ;;
    --android-sdk|-AndroidSdk)      ANDROID_SDK="$2"; shift 2 ;;
    --serial|-Serial)               SERIAL="$2"; shift 2 ;;
    --log-path|-LogPath)            LOG_PATH="$2"; shift 2 ;;
    --non-interactive|-NonInteractive) NON_INTERACTIVE=1; shift ;;
    --help|-h)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "Unknown argument: $1 (see --help)" >&2; exit 1 ;;
  esac
done

LOG_DIR="$(dirname "$LOG_PATH")"
mkdir -p "$LOG_DIR" 2>/dev/null || true
exec > >(tee "$LOG_PATH") 2>&1

die() {
  echo ""
  echo "ERROR: $1" >&2
  echo "Nothing was silently ignored. Fix the reported item and rerun." >&2
  echo "Diagnostic log: $LOG_PATH" >&2
  exit 1
}

checked() {
  local msg="$1"; shift
  "$@"
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    die "$msg (exit code $rc)."
  fi
}

# Run a command capturing output; sets CAPTURED_OUTPUT / CAPTURED_EXIT.
capture() {
  CAPTURED_OUTPUT="$( "$@" 2>&1 )"
  CAPTURED_EXIT=$?
}

adb_args() {
  local out=()
  if [ -n "$SERIAL" ]; then out+=("-s" "$SERIAL"); fi
  out+=("$@")
  printf '%s\n' "${out[@]}"
}

# adb_checked MSG SUBCMD [ARGS...] — runs adb SUBCMD ARGS, die on failure.
adb_checked() {
  local msg="$1"; shift
  local args=()
  if [ -n "$SERIAL" ]; then args+=("-s" "$SERIAL"); fi
  args+=("$@")
  "$ADB" "${args[@]}"
  local rc=$?
  [ "$rc" -eq 0 ] || die "$msg (exit code $rc)."
}

# adb_checked_quiet — same, but suppresses output.
adb_checked_quiet() {
  local msg="$1"; shift
  local args=()
  if [ -n "$SERIAL" ]; then args+=("-s" "$SERIAL"); fi
  args+=("$@")
  "$ADB" "${args[@]}" >/dev/null
  local rc=$?
  [ "$rc" -eq 0 ] || die "$msg (exit code $rc)."
}

find_adb() {
  local candidates=()
  [ -n "$ANDROID_SDK" ] && candidates+=("$ANDROID_SDK/platform-tools/adb")
  [ -n "${ANDROID_HOME:-}" ] && candidates+=("$ANDROID_HOME/platform-tools/adb")
  [ -n "${ANDROID_SDK_ROOT:-}" ] && candidates+=("$ANDROID_SDK_ROOT/platform-tools/adb")
  candidates+=("$WORK_DIR/.android-sdk/platform-tools/adb" "${HOME}/VCVRBuild/.android-sdk/platform-tools/adb" "${HOME}/Android/Sdk/platform-tools/adb")
  local installed
  installed="$(command -v adb || true)"
  [ -n "$installed" ] && candidates+=("$installed")

  local c
  for c in "${candidates[@]}"; do
    if [ -x "$c" ]; then
      echo "$c"
      return 0
    fi
  done
  die "ADB was not found. Run BUILD_AND_INSTALL.sh once, or pass --android-sdk with the SDK folder."
}

# True if $1 contains every port asset.
test_game_files_folder() {
  local path="$1"
  [ -n "$path" ] && [ -d "$path" ] || return 1
  local name
  for name in "${port_text_files[@]}"; do
    [ -f "$path/TEXT/$name" ] || return 1
  done
  for name in "${port_model_files[@]}"; do
    [ -f "$path/models/$name" ] || return 1
  done
  return 0
}

find_git() {
  local g
  g="$(command -v git || true)"
  [ -n "$g" ] && echo "$g"
  return 0
}

# Resolve the folder holding the port's gamefiles/.
resolve_game_files_folder() {
  if [ -n "$GAME_FILES_DIR" ]; then
    local requested
    requested="$(cd "$(dirname "$GAME_FILES_DIR")" 2>/dev/null && pwd)/$(basename "$GAME_FILES_DIR")"
    local candidate
    for candidate in "$requested" "$requested/gamefiles"; do
      if test_game_files_folder "$candidate"; then
        echo "$(cd "$candidate" && pwd)"
        return 0
      fi
    done
    die "-game-files-dir '$requested' does not contain the reVC gamefiles (TEXT/american.gxt and models/x360btns.txd)."
  fi

  local work
  work="$(cd "$(dirname "$WORK_DIR")" 2>/dev/null && pwd)/$(basename "$WORK_DIR")"
  mkdir -p "$WORK_DIR"
  work="$(cd "$WORK_DIR" && pwd)"

  local candidates=("$work/$REVC_DIR_NAME/gamefiles")
  # Newest assembled trees first.
  local tree
  while IFS= read -r tree; do
    candidates+=("$tree/gamefiles")
  done < <(find "$work" -maxdepth 1 -type d -name 'vice-city-vr-build*' -printf '%T@ %p\n' 2>/dev/null | sort -rn | cut -d' ' -f2-)

  local candidate
  for candidate in "${candidates[@]}"; do
    if test_game_files_folder "$candidate"; then
      echo "Using the port assets already on this PC: $candidate" >&2
      echo "$(cd "$candidate" && pwd)"
      return 0
    fi
  done

  local git_exe
  git_exe="$(find_git)"
  if [ -z "$git_exe" ]; then
    die "No reVC source was found under $work and Git is not available. Run BUILD_AND_INSTALL.sh once, or pass --game-files-dir with a reVC checkout."
  fi
  local revc_dir="$work/$REVC_DIR_NAME"
  if [ -d "$revc_dir" ]; then
    die "$revc_dir exists but has no usable gamefiles folder. Remove it and retry, or pass --game-files-dir."
  fi
  echo "Downloading the tested reVC source for its replacement assets..." >&2
  checked "reVC clone failed" "$git_exe" clone --no-checkout -b "$REVC_BRANCH" "$REVC_URL" "$revc_dir"
  checked "Could not select the tested reVC commit" "$git_exe" -c "safe.directory=$revc_dir" -C "$revc_dir" checkout --detach "$TESTED_REVC_COMMIT"

  local downloaded="$revc_dir/gamefiles"
  test_game_files_folder "$downloaded" || die "The downloaded reVC source does not contain the expected gamefiles."
  echo "$(cd "$downloaded" && pwd)"
}

select_quest_device() {
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
    die "Quest '$SERIAL' is not connected and authorized. Accept the USB debugging prompt inside the headset."
  fi
  if [ "${#devices[@]}" -eq 0 ]; then
    die "No authorized Quest was found. Connect it by USB, enable USB debugging and accept the prompt inside the headset."
  fi
  if [ "${#devices[@]}" -eq 1 ]; then
    SERIAL="${devices[0]}"
    return 0
  fi
  if [ "$NON_INTERACTIVE" -eq 1 ]; then die "Multiple Android devices are connected; pass --serial."; fi
  echo "Connected Android devices:"
  local i
  for i in "${!devices[@]}"; do
    echo "  $((i + 1)). ${devices[$i]}"
  done
  local answer selection
  read -r -p "Select the Quest number " answer
  [[ "$answer" =~ ^[0-9]+$ ]] || die "Invalid device selection."
  selection=$((answer - 1))
  if [ "$selection" -lt 0 ] || [ "$selection" -ge "${#devices[@]}" ]; then
    die "Invalid device selection."
  fi
  SERIAL="${devices[$selection]}"
}

echo "Vice City VR - port text and frontend asset repair ($INSTALLER_VERSION)"
echo "This replaces only the port's own text and frontend files. Saves, settings and models are untouched."
echo ""

SOURCE="$(resolve_game_files_folder)"
ADB="$(find_adb)"
echo "ADB: $ADB"
select_quest_device
echo "Quest: $SERIAL"

# A missing gamedata root means the game was never given its data; copying
# the port assets into an empty tree would look like success and still fail
# on launch, so say what is actually wrong instead.
probe_argv=("$ADB")
if [ -n "$SERIAL" ]; then probe_argv+=("-s" "$SERIAL"); fi
probe_argv+=("shell" "ls" "$REMOTE_GAME_DATA/models/gta3.img")
probe_output="$("${probe_argv[@]}" 2>&1)"
probe_rc=$?
if [ "$probe_rc" -ne 0 ] || echo "$probe_output" | grep -q "No such file"; then
  die "No Vice City data was found at $REMOTE_GAME_DATA. Install the game with BUILD_AND_INSTALL.sh first; this tool only repairs an existing installation."
fi

copied=0
adb_checked_quiet "Could not create $REMOTE_GAME_DATA/TEXT on the headset" shell mkdir -p "$REMOTE_GAME_DATA/TEXT"
adb_checked_quiet "Could not create $REMOTE_GAME_DATA/models on the headset" shell mkdir -p "$REMOTE_GAME_DATA/models"
for name in "${port_text_files[@]}"; do
  echo "  TEXT/$name"
  if [ -n "$SERIAL" ]; then
    "$ADB" -s "$SERIAL" push "$SOURCE/TEXT/$name" "$REMOTE_GAME_DATA/TEXT/$name" >/dev/null || die "Failed to copy TEXT/$name"
  else
    "$ADB" push "$SOURCE/TEXT/$name" "$REMOTE_GAME_DATA/TEXT/$name" >/dev/null || die "Failed to copy TEXT/$name"
  fi
  copied=$((copied + 1))
done
for name in "${port_model_files[@]}"; do
  echo "  models/$name"
  if [ -n "$SERIAL" ]; then
    "$ADB" -s "$SERIAL" push "$SOURCE/models/$name" "$REMOTE_GAME_DATA/models/$name" >/dev/null || die "Failed to copy models/$name"
  else
    "$ADB" push "$SOURCE/models/$name" "$REMOTE_GAME_DATA/models/$name" >/dev/null || die "Failed to copy models/$name"
  fi
  copied=$((copied + 1))
done

adb_checked_quiet "Could not leave the app stopped" shell am force-stop "$PACKAGE_NAME"

echo ""
echo "$copied files installed. Launch Vice City VR again."
echo "Diagnostic log: $LOG_PATH"
exit 0
