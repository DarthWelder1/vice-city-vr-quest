#!/usr/bin/env bash
# Installs a locally generated optional Modern model overlay on a connected
# Quest. No APK or third-party assets are downloaded by this script.
#
# Linux port of install-modern-models.ps1.
#
# Arguments:
#   --modern-dir DIR   / -ModernDir DIR    generated modelsets/modern folder
#   --android-sdk DIR  / -AndroidSdk DIR   Android SDK root
#   --serial SERIAL    / -Serial SERIAL    Quest USB serial
#   --log-path PATH    / -LogPath PATH     diagnostic log location
#   --non-interactive  / -NonInteractive   never prompt; fail instead

set -uo pipefail

MODERN_DIR=""
ANDROID_SDK=""
SERIAL=""
LOG_PATH="${TMPDIR:-/tmp}/ViceCityVR-Install-Modern-Models.log"
NON_INTERACTIVE=0

INSTALLER_VERSION="0.5.1-models-3"
PACKAGE_NAME="com.miamivr.quest"
REMOTE_MODELSETS="/sdcard/Android/data/${PACKAGE_NAME}/files/gamedata/modelsets"
STAGING_REMOTE=""

while [ $# -gt 0 ]; do
  case "$1" in
    --modern-dir|-ModernDir)    MODERN_DIR="$2"; shift 2 ;;
    --android-sdk|-AndroidSdk)  ANDROID_SDK="$2"; shift 2 ;;
    --serial|-Serial)           SERIAL="$2"; shift 2 ;;
    --log-path|-LogPath)        LOG_PATH="$2"; shift 2 ;;
    --non-interactive|-NonInteractive) NON_INTERACTIVE=1; shift ;;
    --help|-h)
      sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "Unknown argument: $1 (see --help)" >&2; exit 1 ;;
  esac
done

LOG_DIR="$(dirname "$LOG_PATH")"
mkdir -p "$LOG_DIR" 2>/dev/null || true
exec > >(tee "$LOG_PATH") 2>&1

die() {
  # Clean up any in-progress staging directory before reporting.
  if [ -n "$STAGING_REMOTE" ]; then
    adb_shell rm -rf "$STAGING_REMOTE" >/dev/null 2>&1 || true
  fi
  echo ""
  echo "ERROR: $1" >&2
  echo "The active Modern folder was not replaced before a complete verified copy was ready." >&2
  echo "Diagnostic log: $LOG_PATH" >&2
  exit 1
}

# Run adb with the device prefix; die on failure.
adb_checked() {
  local args=()
  if [ -n "$SERIAL" ]; then args+=("-s" "$SERIAL"); fi
  args+=("$@")
  "$ADB" "${args[@]}"
  local rc=$?
  [ "$rc" -eq 0 ] || die "ADB command failed (exit code $rc)."
}

# Run adb with the device prefix, suppressing output; die on failure.
adb_checked_quiet() {
  local args=()
  if [ -n "$SERIAL" ]; then args+=("-s" "$SERIAL"); fi
  args+=("$@")
  "$ADB" "${args[@]}" >/dev/null
  local rc=$?
  [ "$rc" -eq 0 ] || die "ADB command failed (exit code $rc)."
}

# Run adb with the device prefix, capturing output; sets CAPTURED_OUTPUT/EXIT.
adb_capture() {
  local args=()
  if [ -n "$SERIAL" ]; then args+=("-s" "$SERIAL"); fi
  args+=("$@")
  CAPTURED_OUTPUT="$("$ADB" "${args[@]}" 2>&1)"
  CAPTURED_EXIT=$?
}

# Plain adb shell (used by die() before ADB is resolved).
adb_shell() {
  command -v adb >/dev/null && adb shell "$@"
}

file_sha256() {
  sha256sum "$1" | awk '{print toupper($1)}'
}

find_adb() {
  local candidates=()
  [ -n "$ANDROID_SDK" ] && candidates+=("$ANDROID_SDK/platform-tools/adb")
  [ -n "${ANDROID_HOME:-}" ] && candidates+=("$ANDROID_HOME/platform-tools/adb")
  [ -n "${ANDROID_SDK_ROOT:-}" ] && candidates+=("$ANDROID_SDK_ROOT/platform-tools/adb")
  candidates+=("${HOME}/VCVRBuild/.android-sdk/platform-tools/adb" "${HOME}/Android/Sdk/platform-tools/adb")
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

# True if $1 is a generated Modern overlay.
test_modern_folder() {
  local path="$1"
  [ -n "$path" ] && [ -d "$path" ] || return 1
  local rel
  for rel in "vegetation_models.txt" "models/gta3.img" "models/gta3.dir" "models/generic/wheels.dff" "models/generic/wheels.txd"; do
    [ -f "$path/$rel" ] || return 1
  done
  return 0
}

resolve_modern_folder() {
  local requested="${1:-}"
  if [ -z "$requested" ]; then
    if [ "$NON_INTERACTIVE" -eq 1 ]; then
      die "--modern-dir is required in non-interactive mode."
    fi
    # stdout is captured as the resolved path, so the prompt goes to stderr.
    echo "Paste the generated modern folder path:" >&2
    read -r -p "Modern folder " requested
  fi

  local root
  root="$(cd "$(dirname "$requested")" 2>/dev/null && pwd)/$(basename "$requested")"
  local candidates=("$root" "$root/modern" "$root/modelsets/modern")
  local candidate
  for candidate in "${candidates[@]}"; do
    if test_modern_folder "$candidate"; then
      echo "$(cd "$candidate" && pwd)"
      return 0
    fi
  done
  die "The selected folder is not a generated Modern overlay. Required: vegetation_models.txt, models/gta3.img and models/gta3.dir."
}

select_quest_device() {
  adb_capture devices -l
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

echo "Vice City VR - Modern model installer ($INSTALLER_VERSION)"
echo "This copies a model overlay generated locally by the player; it downloads no model assets."

MODERN="$(resolve_modern_folder "$MODERN_DIR")"
ADB="$(find_adb)"
select_quest_device

echo "Modern folder: $MODERN"
echo "ADB: $ADB"
echo "Quest: $SERIAL"

adb_capture shell pm path "$PACKAGE_NAME"
if [ "$CAPTURED_EXIT" -ne 0 ] || ! echo "$CAPTURED_OUTPUT" | grep -q "package:"; then
  die "Vice City VR is not installed on the selected Quest. Run BUILD_AND_INSTALL.sh first."
fi

# Starting the provider creates the application-owned external directory on
# a fresh install before the ADB shell writes any children into it.
adb_checked_quiet shell content query --uri "content://${PACKAGE_NAME}.saves/slot/1" --projection _display_name:_size
adb_checked_quiet shell am force-stop "$PACKAGE_NAME"

stamp="$(date +%Y%m%d-%H%M%S)"
STAGING_REMOTE="$REMOTE_MODELSETS/.modern-incoming-$stamp"
BACKUP_REMOTE="$REMOTE_MODELSETS/.modern-backup-$stamp"
FINAL_REMOTE="$REMOTE_MODELSETS/modern"

adb_checked_quiet shell mkdir -p "$STAGING_REMOTE"

# Create every subdirectory first (shortest paths first), then push files.
directories=()
while IFS= read -r d; do
  directories+=("$d")
done < <(find "$MODERN" -type d | awk -v root="$MODERN" '{ rel = substr($0, length(root) + 2); print length(rel) " " rel }' | sort -n | cut -d' ' -f2-)
for d in "${directories[@]}"; do
  [ "$d" = "" ] && continue
  adb_checked_quiet shell mkdir -p "$STAGING_REMOTE/$d"
done

files=()
while IFS= read -r f; do
  files+=("$f")
done < <(find "$MODERN" -type f)
if [ "${#files[@]}" -eq 0 ]; then
  die "The selected Modern folder is empty."
fi
echo "Copying ${#files[@]} Modern files into pre-created app storage. This can take several minutes..."
file_index=0
for file in "${files[@]}"; do
  file_index=$((file_index + 1))
  relative="${file#$MODERN/}"
  destination_remote="$STAGING_REMOTE/$relative"
  echo "  [$file_index/${#files[@]}] $relative"
  adb_checked_quiet push "$file" "$destination_remote"
  # Verify the copied size.
  remote_size="$(adb_capture shell stat -c %s "$destination_remote"; echo "$CAPTURED_OUTPUT" | grep -oE '^[0-9]+$' | head -1)"
  local_size="$(stat -c%s "$file")"
  if [ -z "$remote_size" ] || [ "$remote_size" != "$local_size" ]; then
    die "Copied size mismatch for $relative (PC $local_size, Quest ${remote_size:-unknown})."
  fi
done

for required_remote in \
  "$STAGING_REMOTE/vegetation_models.txt" \
  "$STAGING_REMOTE/models/gta3.img" \
  "$STAGING_REMOTE/models/gta3.dir" \
  "$STAGING_REMOTE/models/generic/wheels.dff" \
  "$STAGING_REMOTE/models/generic/wheels.txd"; do
  adb_checked_quiet shell test -f "$required_remote"
done

# White wheels are the characteristic result of a missing/corrupt loose
# wheel TXD. Hash the two small wheel assets explicitly; hashing the
# multi-gigabyte archive here would needlessly extend every installation.
for relative in "models/generic/wheels.dff" "models/generic/wheels.txd"; do
  local_wheel="$MODERN/$relative"
  remote_wheel="$STAGING_REMOTE/$relative"
  local_hash="$(file_sha256 "$local_wheel")"
  hash_output="$(adb_capture shell sha256sum "$remote_wheel"; echo "$CAPTURED_OUTPUT" | grep -oiE '\b[0-9a-f]{64}\b' | head -1 | tr '[:upper:]' '[:lower:]')"
  if [ -z "$hash_output" ] || [ "$hash_output" != "$local_hash" ]; then
    die "Copied SHA256 mismatch for $relative. The previous Modern folder remains active."
  fi
done

existing_result="$(adb_capture shell test -d "$FINAL_REMOTE"; echo "$CAPTURED_EXIT")"
had_existing=0
if [ "$(echo "$existing_result" | tail -1)" = "0" ]; then
  had_existing=1
  adb_checked_quiet shell rm -rf "$BACKUP_REMOTE"
  adb_checked_quiet shell mv "$FINAL_REMOTE" "$BACKUP_REMOTE"
fi

commit_rc=0
adb_capture shell mv "$STAGING_REMOTE" "$FINAL_REMOTE"
commit_rc=$CAPTURED_EXIT
if [ "$commit_rc" -ne 0 ]; then
  if [ "$had_existing" -eq 1 ]; then
    adb_capture shell mv "$BACKUP_REMOTE" "$FINAL_REMOTE"
  fi
  die "Could not activate the copied Modern folder. The previous folder was restored when possible. $CAPTURED_OUTPUT"
fi
STAGING_REMOTE=""
if [ "$had_existing" -eq 1 ]; then
  adb_checked_quiet shell rm -rf "$BACKUP_REMOTE"
fi

adb_checked_quiet shell ls "$FINAL_REMOTE/models/gta3.img"
adb_checked_quiet shell ls "$FINAL_REMOTE/vegetation_models.txt"
adb_checked_quiet shell am force-stop "$PACKAGE_NAME"

echo ""
echo "MODERN MODELS INSTALLED."
echo "Fully restart Vice City VR. Default: Modern World/Weapons; Classic Vehicles/Peds/Vegetation."
echo "Diagnostic log: $LOG_PATH"
exit 0
