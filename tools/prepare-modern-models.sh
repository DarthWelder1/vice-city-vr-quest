#!/usr/bin/env bash
# One-click preparation of the optional Modern overlay from a legal GTA Vice
# City installation and two externally hosted source packs. The packs are
# downloaded for the individual user and are never bundled in this repository.
#
# Linux port of prepare-modern-models.ps1.
#
# Arguments:
#   --game-dir DIR        / -GameDir DIR        original GTA Vice City PC install
#   --work-dir DIR        / -WorkDir DIR        scratch dir (default ~/VCVRBuild/modern-assets)
#   --output-dir DIR      / -OutputDir DIR      overlay output (default <game>/modelsets/modern)
#   --android-sdk DIR     / -AndroidSdk DIR     Android SDK root
#   --serial SERIAL       / -Serial SERIAL      Quest USB serial
#   --hd-archive FILE     / -HdArchive FILE     pre-verified HD pack archive
#   --mods-archive FILE   / -ModsArchive FILE   pre-verified Mods pack archive
#   --build-only          / -BuildOnly          stop after building the overlay
#   --accept-downloads    / -AcceptDownloads    skip the download confirmation
#   --non-interactive     / -NonInteractive     never prompt; fail instead
#   --log-path PATH       / -LogPath PATH       diagnostic log location

set -uo pipefail

GAME_DIR=""
WORK_DIR="${HOME}/VCVRBuild/modern-assets"
OUTPUT_DIR=""
ANDROID_SDK=""
SERIAL=""
HD_ARCHIVE=""
MODS_ARCHIVE=""
BUILD_ONLY=0
ACCEPT_DOWNLOADS=0
NON_INTERACTIVE=0
LOG_PATH="${TMPDIR:-/tmp}/ViceCityVR-Prepare-Modern-Models.log"

WIZARD_VERSION="0.5.1-models-3"
# The two externally hosted source packs used by the tested build.
HD_URL="https://drive.usercontent.google.com/download?id=1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj&export=download&confirm=t"
MODS_URL="https://drive.usercontent.google.com/download?id=1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d&export=download&confirm=t"
HD_SIZE=1878280127
MODS_SIZE=2377186981
HD_SHA256="81A7962479752F3A07004A2E12964815435CAF4B0A0F637EE982460171A5D94C"
MODS_SHA256="F33031091DBE50A3BEDCEEFAF3FDE6F6DDD96F9841AABE1ECD3713818DED9725"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDER="$REPO_ROOT/tools/modelsets/build-modern-modelset.sh"
QUEST_INSTALLER="$REPO_ROOT/tools/install-modern-models.sh"

while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir|-GameDir)          GAME_DIR="$2"; shift 2 ;;
    --work-dir|-WorkDir)          WORK_DIR="$2"; shift 2 ;;
    --output-dir|-OutputDir)      OUTPUT_DIR="$2"; shift 2 ;;
    --android-sdk|-AndroidSdk)    ANDROID_SDK="$2"; shift 2 ;;
    --serial|-Serial)             SERIAL="$2"; shift 2 ;;
    --hd-archive|-HdArchive)      HD_ARCHIVE="$2"; shift 2 ;;
    --mods-archive|-ModsArchive)  MODS_ARCHIVE="$2"; shift 2 ;;
    --build-only|-BuildOnly)          BUILD_ONLY=1; shift ;;
    --accept-downloads|-AcceptDownloads) ACCEPT_DOWNLOADS=1; shift ;;
    --non-interactive|-NonInteractive) NON_INTERACTIVE=1; shift ;;
    --log-path|-LogPath)          LOG_PATH="$2"; shift 2 ;;
    --help|-h)
      sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
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
  echo "Verified downloads, extractions and completed local builds are kept for reuse." >&2
  echo "No model pack is uploaded until the complete build succeeds." >&2
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

resolve_game_folder() {
  local requested="${1:-}"
  if [ -z "$requested" ]; then
    if [ "$NON_INTERACTIVE" -eq 1 ]; then
      die "--game-dir is required in non-interactive mode."
    fi
    # stdout is captured as the resolved path, so the prompt goes to stderr.
    echo "Select the original GTA Vice City installation folder (paste a path):" >&2
    read -r -p "GTA Vice City folder " requested
  fi
  [ -n "$requested" ] && [ -d "$requested" ] || die "The GTA Vice City folder does not exist: $requested"
  local resolved
  resolved="$(cd "$requested" && pwd)"
  local rel
  for rel in "models/gta3.img" "models/gta3.dir" "models/generic.txd"; do
    [ -f "$resolved/$rel" ] || die "The selected folder is not a complete original GTA Vice City installation. Missing: $rel"
  done
  echo "$resolved"
}

assert_free_space() {
  local game="$1" work="$2"
  local work_free game_free
  work_free="$(df -k --output=avail "$(dirname "$work")" | tail -1 | tr -d ' ')"
  game_free="$(df -k --output=avail "$(dirname "$game")" | tail -1 | tr -d ' ')"
  # If work and game share a mount, the combined requirement applies.
  if [ "$(df --output=source "$work" | tail -1)" = "$(df --output=source "$game" | tail -1)" ]; then
    if [ "$work_free" -lt $((24 * 1024 * 1024)) ]; then
      die "At least 24 GB free is required on $(dirname "$work") for downloads, extraction and the staged build; only $((work_free / 1024 / 1024)) GB is available."
    fi
  else
    if [ "$work_free" -lt $((14 * 1024 * 1024)) ]; then
      die "At least 14 GB free is required on $(dirname "$work") for downloads and extraction; only $((work_free / 1024 / 1024)) GB is available."
    fi
    if [ "$game_free" -lt $((12 * 1024 * 1024)) ]; then
      die "At least 12 GB free is required on $(dirname "$game") for the staged model build; only $((game_free / 1024 / 1024)) GB is available."
    fi
  fi
}

# True if $1 exists with exactly $2 bytes and $3 SHA256.
test_file_identity() {
  local path="$1" size="$2" sha256="$3"
  [ -f "$path" ] || return 1
  [ "$(stat -c%s "$path")" -eq "$size" ] || return 1
  [ "$(file_sha256 "$path")" = "$sha256" ]
}

file_sha256() {
  sha256sum "$1" | awk '{print toupper($1)}'
}

# Download (resumable) and verify a pack; print the final path.
ensure_download() {
  local name="$1" url="$2" destination="$3" size="$4" sha256="$5" provided="$6"
  if [ -n "$provided" ]; then
    echo "Verifying supplied $name archive..." >&2
    test_file_identity "$provided" "$size" "$sha256" || die "The supplied $name archive does not match the tested size/SHA256: $provided"
    echo "$provided"
    return 0
  fi
  if test_file_identity "$destination" "$size" "$sha256"; then
    echo "Reusing verified download: $destination" >&2
    echo "$destination"
    return 0
  fi
  rm -f "$destination"
  local partial="$destination.partial"
  if [ -f "$partial" ] && [ "$(stat -c%s "$partial")" -gt "$size" ]; then
    rm -f "$partial"
  fi
  command -v curl >/dev/null || die "curl was not found; it is required to download the verified archives."
  echo "Downloading $name ($((size / 1024 / 1024)) MB). Interrupted downloads resume automatically..." >&2
  checked "$name download failed" curl --fail --location --retry 5 --retry-all-errors --continue-at - --output "$partial" "$url"
  # Google Drive serves a small HTML error page with HTTP 200 when a shared
  # file is no longer available (quota exceeded, virus scan, link revoked).
  # Detect it explicitly so the user gets the real reason, not just a hash
  # mismatch on a 2 KB file.
  if [ -f "$partial" ] && [ "$(stat -c%s "$partial")" -lt 100000 ] && [ "$(head -c 15 "$partial" 2>/dev/null)" = "<!DOCTYPE html>" ]; then
    local errtext
    errtext="$(grep -oiE "quota exceeded|virus scan|scanned|Sorry[^<]{0,80}" "$partial" 2>/dev/null | head -1 || true)"
    rm -f "$partial"
    die "The $name link is not serving the file. Google Drive returned an error page${errtext:+ ($errtext)}. The shared file has likely been revoked, hit a quota limit, or is stuck in a virus scan. Ask the pack author for a working link, or supply the archive with --hd-archive / --mods-archive."
  fi
  echo "Verifying $name SHA256..." >&2
  test_file_identity "$partial" "$size" "$sha256" || die "$name download completed but failed the pinned size/SHA256 check. Delete '$partial' and retry."
  mv -f "$partial" "$destination"
  echo "$destination"
}

# Extract a verified archive into $3 (idempotent via a marker file).
ensure_extracted() {
  local name="$1" archive="$2" destination="$3" sha256="$4"
  local marker="$destination/.vcvr-source-sha256"
  if [ -f "$marker" ] && [ "$(cat "$marker" | tr -d '[:space:]')" = "$sha256" ]; then
    echo "Reusing verified extraction: $destination" >&2
    echo "$destination"
    return 0
  fi
  rm -rf "$destination"
  mkdir -p "$destination"
  command -v tar >/dev/null || die "tar was not found; it is required to extract the verified archives."
  echo "Extracting $name..." >&2
  checked "$name extraction failed" tar -xf "$archive" -C "$destination"
  printf '%s' "$sha256" > "$marker"
  echo "$destination"
}

# Read the builder version the wizard must expect in BUILD_INFO.txt.
get_expected_builder_version() {
  local v
  v="$(grep -m1 'BUILD_SCRIPT_VERSION=' "$BUILDER" | head -1 | sed 's/.*BUILD_SCRIPT_VERSION="\([^"]*\)".*/\1/')"
  [ -n "$v" ] || die "Could not read the bundled Modern builder version from: $BUILDER"
  echo "$v"
}

# True if $1 is a complete overlay built by $2.
test_completed_overlay() {
  local path="$1" expected="$2"
  [ -n "$path" ] && [ -d "$path" ] || return 1
  local rel
  for rel in "vegetation_models.txt" "BUILD_INFO.txt" "models/gta3.img" "models/gta3.dir" "models/generic/wheels.dff" "models/generic/wheels.txd"; do
    [ -f "$path/$rel" ] && [ "$(stat -c%s "$path/$rel")" -gt 0 ] || return 1
  done
  local img_size dir_size
  img_size="$(stat -c%s "$path/models/gta3.img")"
  dir_size="$(stat -c%s "$path/models/gta3.dir")"
  [ "$img_size" -ge $((1024 * 1024 * 1024)) ] || return 1
  [ "$dir_size" -ge 32 ] || return 1
  [ $((dir_size % 32)) -eq 0 ] || return 1
  grep -q "^BuilderVersion=$expected$" "$path/BUILD_INFO.txt" || return 1
  grep -q "^ModernVegetationGeometry=REMOVED$" "$path/BUILD_INFO.txt" || return 1
  return 0
}

echo "Vice City VR - download, build and install Modern models ($WIZARD_VERSION)"
echo "Required input: only a legal original GTA Vice City PC installation."
echo "The wizard downloads two external packs used by the tested build; no pack is bundled or redistributed by this repository."
echo "Modern vegetation/palms are deliberately excluded and remain Classic on Quest."

[ -f "$BUILDER" ] || die "Required source-kit tool is missing: $BUILDER"
[ -f "$QUEST_INSTALLER" ] || die "Required source-kit tool is missing: $QUEST_INSTALLER"

GAME="$(resolve_game_folder "$GAME_DIR")"
mkdir -p "$WORK_DIR"
WORK="$(cd "$WORK_DIR" && pwd)"
if [ -z "$OUTPUT_DIR" ]; then
  OUTPUT_DIR="$GAME/modelsets/modern"
fi
OUTPUT="$(cd "$(dirname "$OUTPUT_DIR")" 2>/dev/null && pwd)/$(basename "$OUTPUT_DIR")"
EXPECTED_BUILDER_VERSION="$(get_expected_builder_version)"

if test_completed_overlay "$OUTPUT" "$EXPECTED_BUILDER_VERSION"; then
  echo "Reusing completed verified Modern overlay: $OUTPUT"
  echo "Downloads, extraction and rebuilding are not needed on this run."
else
  if [ -e "$OUTPUT" ]; then
    echo "Existing Modern output is incomplete or from an older builder; rebuilding it safely."
  fi
  assert_free_space "$GAME" "$WORK"

  if [ "$ACCEPT_DOWNLOADS" -eq 0 ] && [ -z "$HD_ARCHIVE" ] && [ -z "$MODS_ARCHIVE" ]; then
    if [ "$NON_INTERACTIVE" -eq 1 ]; then
      die "--accept-downloads is required in non-interactive mode when local verified archives are not supplied."
    fi
    echo ""
    echo "About 4.0 GB will be downloaded from the two external links supplied for this project."
    echo "The extracted/build workspace can temporarily use about 20-24 GB."
    read -r -p "Download, verify and build the Modern overlay now? [Y/n] " answer
    case "$answer" in
      [Yy]*|'') ;;
      *) die "Modern model download was cancelled; nothing was changed on the Quest." ;;
    esac
  fi

  DOWNLOADS="$WORK/downloads"
  SOURCES="$WORK/sources"
  mkdir -p "$DOWNLOADS" "$SOURCES"
  # A die() inside these functions runs in a subshell (the command
  # substitution), so its exit only kills the subshell. Catch the non-zero
  # exit here so the parent stops instead of cascading into extraction and
  # install with a garbage path.
  HD_ZIP="$(ensure_download "GTA VC HD + Weapons" "$HD_URL" "$DOWNLOADS/GTA VC HD + Weapons.zip" "$HD_SIZE" "$HD_SHA256" "$HD_ARCHIVE")" || die "GTA VC HD + Weapons download failed; stopping before extraction and install."
  MODS_ZIP="$(ensure_download "Mods / Atmosphere" "$MODS_URL" "$DOWNLOADS/Mods.zip" "$MODS_SIZE" "$MODS_SHA256" "$MODS_ARCHIVE")" || die "Mods / Atmosphere download failed; stopping before extraction and install."

  HD_SOURCE="$(ensure_extracted "GTA VC HD + Weapons" "$HD_ZIP" "$SOURCES/hd-pack" "$HD_SHA256")" || die "GTA VC HD + Weapons extraction failed; stopping before the build."
  MODS_SOURCE="$(ensure_extracted "Mods / Atmosphere" "$MODS_ZIP" "$SOURCES/mods-pack" "$MODS_SHA256")" || die "Mods / Atmosphere extraction failed; stopping before the build."

  echo "Building the optimized Quest Modern overlay. This can take several minutes..."
  bash "$BUILDER" \
    --game-dir "$GAME" \
    --hd-pack "$HD_SOURCE" \
    --atmosphere-pack "$MODS_SOURCE" \
    --out "$OUTPUT" \
    --force
fi

if [ "$BUILD_ONLY" -eq 1 ]; then
  echo ""
  echo "MODERN OVERLAY BUILT: $OUTPUT"
  echo "Build-only mode complete."
  echo "Diagnostic log: $LOG_PATH"
  exit 0
fi

echo "Installing the completed overlay on the connected Quest..."
install_args=(bash "$QUEST_INSTALLER" --modern-dir "$OUTPUT")
if [ -n "$ANDROID_SDK" ]; then install_args+=("--android-sdk" "$ANDROID_SDK"); fi
if [ -n "$SERIAL" ]; then install_args+=("--serial" "$SERIAL"); fi
if [ "$NON_INTERACTIVE" -eq 1 ]; then install_args+=("--non-interactive"); fi
"${install_args[@]}"
rc=$?
[ "$rc" -eq 0 ] || die "Quest Modern model installation failed (exit code $rc)."

echo ""
echo "DOWNLOAD, BUILD AND QUEST INSTALL COMPLETED."
echo "Fully restart Vice City VR. Modern World/Weapons are selected by default; Vehicles/Peds/Vegetation remain Classic."
echo "Diagnostic log: $LOG_PATH"
exit 0
