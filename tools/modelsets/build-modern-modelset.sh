#!/usr/bin/env bash
# Builds the optional MODERN asset overlay from two verified source packs.
# No game or third-party assets are distributed with Vice City VR.
# The Quest one-click wizard downloads and extracts the packs for the user.
#
# Linux port of build-modern-modelset.ps1. Uses the Python IMG/TXD helpers
# (imgextract.py, imginject.py, imgtexaudit.py, txdmerge.py) and the
# txdcompress binary built from the adjacent txdcompress.cpp.
#
# Arguments:
#   --game-dir DIR          / -GameDir DIR          (required) original GTA VC install
#   --hd-pack DIR           / -HdPack DIR          (required) extracted HD + Weapons pack
#   --atmosphere-pack DIR   / -AtmospherePack DIR  (required) extracted Mods/Atmosphere pack
#   --out DIR               / -Out DIR             output (default <game>/modelsets/modern)
#   --verify-only           / -VerifyOnly          validate inputs, write nothing
#   --force                 / -Force               replace an existing output after a full build

set -uo pipefail

BUILD_SCRIPT_VERSION="0.5.1-quest-3"
MINIMUM_FREE_GB=12

# These signatures identify the downloads used to prepare and test 0.5.0.
# A mismatch is a warning, not a hard failure: an author may update a pack at
# the same URL while retaining the folder layout expected by this builder.
declare -A TESTED_SIGNATURES=(
  ["HD pack gta3.img"]="4120871542825386411727C8EAC617F80EA2B2D0786FADA858C8CDDD3D718A7D"
  ["HD pack gta3.dir"]="DD606380513BB63841D3DAD517BCFA2F54598DF7EE6084E0C941C18D78C30D01"
  ["Atmosphere vehicles.col"]="64233744C532423ECDCAEFF088696E76BCFA4758A70CADA2FD6EDDA258911DF9"
  ["Atmosphere wheels.dff"]="3FAB043194EB5016D3F96D14090831D1ABCDDCB4B3793B465D30A581643D22DA"
  ["Atmosphere wheels.txd"]="23F5752BE920896E86DE3AE21CE00B9D301DBF04E321162373BD893915B488C5"
  ["HD pack generic.txd"]="66DF98073FDDC2840DABE486156830065227921C5622CF4E03FA9282AD810465"
)

GAME_DIR=""
HD_PACK=""
ATMOSPHERE_PACK=""
OUT=""
VERIFY_ONLY=0
FORCE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir|-GameDir)          GAME_DIR="$2"; shift 2 ;;
    --hd-pack|-HdPack)            HD_PACK="$2"; shift 2 ;;
    --atmosphere-pack|-AtmospherePack) ATMOSPHERE_PACK="$2"; shift 2 ;;
    --out|-Out)                   OUT="$2"; shift 2 ;;
    --verify-only|-VerifyOnly)    VERIFY_ONLY=1; shift ;;
    --force|-Force)               FORCE=1; shift ;;
    --help|-h)
      sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "Unknown argument: $1 (see --help)" >&2; exit 1 ;;
  esac
done

die() { echo "ERROR: $1" >&2; exit 1; }
warn() { echo "WARNING: $1" >&2; }
step() { echo "==> $1"; }

[ -n "$GAME_DIR" ] || die "--game-dir is required."
[ -n "$HD_PACK" ] || die "--hd-pack is required."
[ -n "$ATMOSPHERE_PACK" ] || die "--atmosphere-pack is required."

require_dir() { [ -d "$1" ] || die "$2 folder not found: $1"; }
require_file() {
  [ -f "$1" ] || die "$2 not found: $1"
  [ "$(stat -c%s "$1")" -gt 0 ] || die "$2 is empty: $1"
}

# Users may select the outer directory made by 7-Zip. Find the shallowest
# descendant which contains the pack-specific marker.
find_marker() {
  local root="$1" marker="$2" what="$3"
  require_dir "$root" "$what"
  local candidates=("$root")
  local d
  while IFS= read -r d; do
    candidates+=("$d")
  done < <(find "$root" -type d 2>/dev/null)
  local matches=()
  for d in "${candidates[@]}"; do
    [ -e "$d/$marker" ] && matches+=("$d")
  done
  if [ "${#matches[@]}" -eq 0 ]; then
    die "$what: could not find '$marker' anywhere under '$root'. Check the expected folder tree in MODERN_MODELS.md."
  fi
  # shallowest (shortest path) wins
  local best="${matches[0]}"
  for d in "${matches[@]}"; do
    if [ "${#d}" -lt "${#best}" ]; then best="$d"; fi
  done
  if [ "${#matches[@]}" -gt 1 ]; then
    warn "$what: found more than one candidate; using the shallowest: $best"
  fi
  echo "$best"
}

# Validate a .img/.dir pair; prints "entries img_bytes dir_bytes".
validate_img_pair() {
  local img="$1" what="$2"
  local dir="${img%.*}.dir"
  require_file "$img" "$what IMG"
  require_file "$dir" "$what DIR"
  python3 - "$img" "$dir" "$what" <<'PYEOF'
import os, sys
img, dir_path, what = sys.argv[1], sys.argv[2], sys.argv[3]
with open(dir_path, "rb") as f:
    db = f.read()
if len(db) % 32 != 0:
    sys.exit(f"{what} DIR length is not a multiple of 32 bytes: {dir_path}")
n = len(db) // 32
if n < 5000:
    sys.exit(f"{what} has only {n} directory entries; expected a complete Vice City gta3 archive.")
img_len = os.path.getsize(img)
for i in range(n):
    import struct
    sector, count = struct.unpack_from("<II", db, i * 32)
    if (sector + count) * 2048 > img_len:
        sys.exit(f"{what} entry {i} points outside its IMG; the archive is incomplete or corrupt.")
print(f"{n} {img_len} {len(db)}")
PYEOF
}

file_sha256() {
  sha256sum "$1" | awk '{print toupper($1)}'
}

# Count files matching a glob under a directory (optionally recursive).
count_files() {
  local dir="$1" filter="$2" recurse="$3"
  if [ "$recurse" = "1" ]; then
    find "$dir" -type f -name "$filter" 2>/dev/null | wc -l
  else
    find "$dir" -maxdepth 1 -type f -name "$filter" 2>/dev/null | wc -l
  fi
}

require_minimum_files() {
  local dir="$1" filter="$2" minimum="$3" what="$4" recurse="${5:-0}"
  require_dir "$dir" "$what"
  local count
  count="$(count_files "$dir" "$filter" "$recurse")"
  if [ "$count" -lt "$minimum" ]; then
    die "$what contains only $count '$filter' files; expected at least $minimum. Check that the correct archive was fully extracted."
  fi
  echo "$count"
}

# Paths-Overlap: true if either path is inside the other.
paths_overlap() {
  local left right
  left="$(cd "$(dirname "$1")" 2>/dev/null && pwd)/$(basename "$1")/"
  right="$(cd "$(dirname "$2")" 2>/dev/null && pwd)/$(basename "$2")/"
  case "$left" in "$right"*) return 0 ;; esac
  case "$right" in "$left"*) return 0 ;; esac
  return 1
}

# ---- resolve and validate every input before writing anything ----
require_dir "$GAME_DIR" "Vice City VR game"
GAME_DIR="$(cd "$GAME_DIR" && pwd)"
HD="$(find_marker "$HD_PACK" "models/gta3.img" "GTA VC HD + Weapons")"
MODS="$(find_marker "$ATMOSPHERE_PACK" "Vehicles/gta3.img" "Mods / Atmosphere")"

if [ -z "$OUT" ]; then
  OUT="$GAME_DIR/modelsets/modern"
fi
OUT="$(cd "$(dirname "$OUT")" 2>/dev/null && pwd)/$(basename "$OUT")"

require_file "$GAME_DIR/models/gta3.img" "Original game models/gta3.img"
require_file "$GAME_DIR/models/gta3.dir" "Original game models/gta3.dir"
require_file "$GAME_DIR/models/generic.txd" "Original game models/generic.txd"

require_file "$HD/models/generic.txd" "HD pack models/generic.txd"
require_dir "$HD/txd" "HD pack txd"
HD_ARCHIVE="$(validate_img_pair "$HD/models/gta3.img" "GTA VC HD + Weapons")"

VEHICLE_FILES="$MODS/Vehicles/gta3.img"
VEHICLE_COLL="$MODS/Vehicles/coll/vehicles.col"
VEHICLE_GENERIC="$MODS/Vehicles/generic"
ATMOS_VEGETATION="$MODS/Vegitation"   # spelling used by the download
require_file "$VEHICLE_COLL" "Atmosphere Vehicles/coll/vehicles.col"
require_file "$VEHICLE_GENERIC/nowheel.DFF" "Atmosphere nowheel.DFF"
require_file "$VEHICLE_GENERIC/wheels.dff" "Atmosphere wheels.dff"
require_file "$VEHICLE_GENERIC/wheels.txd" "Atmosphere wheels.txd"
VEHICLE_DFF_COUNT="$(require_minimum_files "$VEHICLE_FILES" "*.dff" 90 "Atmosphere vehicle models" 1)"
VEHICLE_TXD_COUNT="$(require_minimum_files "$VEHICLE_FILES" "*.txd" 90 "Atmosphere vehicle textures" 1)"
ATMOS_VEGETATION_COUNT="$(require_minimum_files "$ATMOS_VEGETATION" "*.dff" 20 "Atmosphere vegetation" 1)"

# Unique vegetation basenames (case-insensitive), the runtime manifest.
VEGETATION_NAMES="$(find "$ATMOS_VEGETATION" -type f -name '*.dff' 2>/dev/null \
  | xargs -I{} basename {} .dff | tr '[:upper:]' '[:lower:]' | sort -u)"
VEGETATION_NAME_COUNT="$(echo "$VEGETATION_NAMES" | grep -c . || true)"
if [ "$VEGETATION_NAME_COUNT" -lt 20 ]; then
  die "Only $VEGETATION_NAME_COUNT vegetation models were found; refusing to create an incomplete manifest."
fi
if [ "$VEGETATION_NAME_COUNT" -gt 512 ]; then
  die "The vegetation manifest has $VEGETATION_NAME_COUNT names; the runtime supports at most 512."
fi
TOO_LONG="$(echo "$VEGETATION_NAMES" | awk 'length($0) > 23' | paste -sd ', ' -)"
if [ -n "$TOO_LONG" ]; then
  die "Vegetation model names exceed the runtime's 23-character limit: $TOO_LONG"
fi

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for tool in imginject.py imgtexaudit.py imgextract.py txdmerge.py txdcompress; do
  [ -e "$TOOLS_DIR/$tool" ] || die "Required model-set tool '$tool' is missing. Build it first: g++ -O2 -o txdcompress txdcompress.cpp"
done
[ -x "$TOOLS_DIR/txdcompress" ] || die "txdcompress is not executable. Build it: g++ -O2 -o txdcompress txdcompress.cpp"

# Reject paths which could recurse into a downloaded pack or replace game data.
GAME_MODELS="$GAME_DIR/models"
if [ "$OUT" = "$GAME_DIR" ] || paths_overlap "$OUT" "$GAME_MODELS"; then
  die "Unsafe output path '$OUT'. Use the default modelsets/modern folder, never the game root or models folder."
fi
if paths_overlap "$OUT" "$HD" || paths_overlap "$OUT" "$MODS"; then
  die "Unsafe output path '$OUT': it overlaps a source folder."
fi
OUT_ROOT="$(df --output=target "$OUT" 2>/dev/null | tail -1 | tr -d ' ')"
if [ "$OUT" = "$OUT_ROOT" ]; then
  die "Refusing to use a drive root as output: $OUT"
fi

echo ""
echo "Vice City VR Modern asset builder $BUILD_SCRIPT_VERSION"
echo "Game:          $GAME_DIR"
echo "HD + Weapons:  $HD"
echo "Atmosphere:    $MODS"
echo "Output:        $OUT"
echo ""
echo "Validated: $(echo "$HD_ARCHIVE" | awk '{print $1}') HD archive entries; $VEHICLE_DFF_COUNT vehicle DFFs; $VEHICLE_TXD_COUNT vehicle TXDs; $ATMOS_VEGETATION_COUNT vegetation names kept Classic."

step "Hashing source anchors"
declare -A SOURCE_HASHES=(
  ["Original game generic.txd"]="$(file_sha256 "$GAME_DIR/models/generic.txd")"
  ["HD pack gta3.img"]="$(file_sha256 "$HD/models/gta3.img")"
  ["HD pack gta3.dir"]="$(file_sha256 "$HD/models/gta3.dir")"
  ["HD pack generic.txd"]="$(file_sha256 "$HD/models/generic.txd")"
  ["Atmosphere vehicles.col"]="$(file_sha256 "$VEHICLE_COLL")"
  ["Atmosphere wheels.dff"]="$(file_sha256 "$VEHICLE_GENERIC/wheels.dff")"
  ["Atmosphere wheels.txd"]="$(file_sha256 "$VEHICLE_GENERIC/wheels.txd")"
)
for name in "Original game generic.txd" "HD pack gta3.img" "HD pack gta3.dir" "HD pack generic.txd" "Atmosphere vehicles.col" "Atmosphere wheels.dff" "Atmosphere wheels.txd"; do
  echo "    $name: ${SOURCE_HASHES[$name]}"
  if [ -n "${TESTED_SIGNATURES[$name]:-}" ] && [ "${SOURCE_HASHES[$name]}" != "${TESTED_SIGNATURES[$name]}" ]; then
    warn "$name differs from the pack tested for 0.5.0. The build may still work, but it will not be byte-identical to the tested source set."
  fi
done

if [ "$VERIFY_ONLY" -eq 1 ]; then
  echo ""
  echo "Verification passed. No files were written."
  exit 0
fi

if [ -e "$OUT" ]; then
  [ -d "$OUT" ] || die "Output exists and is not a directory: $OUT"
  if [ "$FORCE" -ne 1 ]; then
    die "Output already exists: $OUT
Re-run with --force to replace it only after a complete staged build succeeds."
  fi
fi

# Free-space guard.
free_kb="$(df -k --output=avail "$(dirname "$OUT")" | tail -1 | tr -d ' ')"
if [ "$free_kb" -lt $((MINIMUM_FREE_GB * 1024 * 1024)) ]; then
  die "At least ${MINIMUM_FREE_GB} GB free is required on $(dirname "$OUT"); only $((free_kb / 1024 / 1024)).1 GB is available."
fi

OUT_PARENT="$(dirname "$OUT")"
OUT_LEAF="$(basename "$OUT")"
mkdir -p "$OUT_PARENT"
UNIQUE="${$}-$(date +%s)-$$"
STAGE="$OUT_PARENT/.$OUT_LEAF.build-$UNIQUE"
WORK="$(mktemp -d /tmp/vcvr-modern-build-$UNIQUE.XXXXXX)"
COMMITTED=0
cleanup() {
  rm -rf "$WORK"
  if [ "$COMMITTED" -ne 1 ] && [ -d "$STAGE" ]; then
    rm -rf "$STAGE"
  fi
}
trap cleanup EXIT

mkdir -p "$STAGE" "$WORK"
STAGE_MODELS="$STAGE/models"
STAGE_TXD="$STAGE/txd"

# 1. The HD pack supplies the complete base archive and loose TXDs.
step "Copying the HD pack base (about 3.5 GB)"
cp -r "$HD/models" "$STAGE_MODELS"
cp -r "$HD/txd" "$STAGE_TXD"

# 2. The Atmosphere pack supplies loose vehicle collision and wheel files.
step "Copying Atmosphere vehicle collision and wheel files"
STAGE_COLL="$STAGE_MODELS/coll"
STAGE_GENERIC_DIR="$STAGE_MODELS/generic"
mkdir -p "$STAGE_COLL" "$STAGE_GENERIC_DIR"
cp -f "$MODS/Vehicles/coll/"*.col "$STAGE_COLL/" 2>/dev/null || true
cp -f "$VEHICLE_GENERIC/"* "$STAGE_GENERIC_DIR/"

# 3. Inject Atmosphere vehicles and physically remove expensive palm/tree
# DFFs. Missing entries fall back to the original Classic archive.
step "Injecting Atmosphere vehicles and removing Modern vegetation"
EXCLUDE_ARGS=()
while IFS= read -r vname; do
  EXCLUDE_ARGS+=("$vname.dff")
done <<< "$VEGETATION_NAMES"
python3 "$TOOLS_DIR/imginject.py" \
  --img "$STAGE_MODELS/gta3.img" \
  --from "$VEHICLE_FILES" "$VEHICLE_GENERIC" \
  --exclude "${EXCLUDE_ARGS[@]}"

# Record exact vegetation membership for category-selective streaming.
VEGETATION_MANIFEST="$STAGE/vegetation_models.txt"
echo "$VEGETATION_NAMES" > "$VEGETATION_MANIFEST"
echo "    vegetation category: $VEGETATION_NAME_COUNT Modern DFFs removed; CLASSIC forced"

# 5. Start from the user's original generic.txd, adding only leaf textures
# absent from it. This avoids the pack's full uncompressed dictionary.
step "Rebuilding generic.txd (original + missing HD leaf textures)"
MERGED_GENERIC="$WORK/generic_merged.txd"
python3 "$TOOLS_DIR/txdmerge.py" \
  --base "$GAME_DIR/models/generic.txd" \
  --extra "$HD/models/generic.txd" \
  --out "$MERGED_GENERIC"
cp -f "$MERGED_GENERIC" "$STAGE_MODELS/generic.txd"
MERGED_INJECT="$WORK/inject_merged"
mkdir -p "$MERGED_INJECT"
cp -f "$MERGED_GENERIC" "$MERGED_INJECT/Generic.txd"
python3 "$TOOLS_DIR/imginject.py" \
  --img "$STAGE_MODELS/gta3.img" \
  --from "$MERGED_INJECT"

# 6. Recompress uncompressed or no-mip texture dictionaries for VR.
step "Auditing the archive for expensive texture dictionaries"
CANDIDATE_LIST="$WORK/txd_candidates.txt"
python3 "$TOOLS_DIR/imgtexaudit.py" \
  --img "$STAGE_MODELS/gta3.img" \
  --candidate-list "$CANDIDATE_LIST"
CANDIDATES=()
if [ -f "$CANDIDATE_LIST" ]; then
  while IFS= read -r line; do
    [ -n "$line" ] && CANDIDATES+=("$line")
  done < <(sort -u "$CANDIDATE_LIST")
fi
if [ "${#CANDIDATES[@]}" -gt 0 ]; then
  step "Extracting and recompressing ${#CANDIDATES[@]} texture dictionaries"
  IMAGE_TXD="$WORK/imgtxd"
  python3 "$TOOLS_DIR/imgextract.py" \
    --img "$STAGE_MODELS/gta3.img" \
    --out "$IMAGE_TXD" \
    --entries "${CANDIDATES[@]}"
  EXTRACTED_COUNT="$(find "$IMAGE_TXD" -maxdepth 1 -type f -name '*.txd' | wc -l)"
  if [ "$EXTRACTED_COUNT" -ne "${#CANDIDATES[@]}" ]; then
    die "Requested ${#CANDIDATES[@]} TXDs but extracted $EXTRACTED_COUNT; refusing a partial recompression pass."
  fi
  while IFS= read -r txd; do
    "$TOOLS_DIR/txdcompress" "$txd"
  done < <(find "$IMAGE_TXD" -maxdepth 1 -type f -name '*.txd')
  step "Injecting recompressed texture dictionaries"
  python3 "$TOOLS_DIR/imginject.py" \
    --img "$STAGE_MODELS/gta3.img" \
    --from "$IMAGE_TXD"
else
  echo "    no archive texture dictionaries require recompression"
fi
step "Recompressing loose generic.txd"
"$TOOLS_DIR/txdcompress" "$STAGE_MODELS/generic.txd"

# Validate the complete staged result before replacing any prior overlay.
step "Validating and hashing the completed overlay"
RESULT_ARCHIVE="$(validate_img_pair "$STAGE_MODELS/gta3.img" "Built Modern overlay")"
require_file "$STAGE_MODELS/generic.txd" "Built Modern generic.txd"
require_file "$VEGETATION_MANIFEST" "Built vegetation manifest"
declare -A OUTPUT_HASHES=(
  ["models/gta3.img"]="$(file_sha256 "$STAGE_MODELS/gta3.img")"
  ["models/gta3.dir"]="$(file_sha256 "$STAGE_MODELS/gta3.dir")"
  ["models/generic.txd"]="$(file_sha256 "$STAGE_MODELS/generic.txd")"
  ["vegetation_models.txt"]="$(file_sha256 "$VEGETATION_MANIFEST")"
)

# BUILD_INFO.txt
{
  echo "Vice City VR Modern asset overlay"
  echo "BuilderVersion=$BUILD_SCRIPT_VERSION"
  echo "BuiltUtc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "ArchiveEntries=$(echo "$RESULT_ARCHIVE" | awk '{print $1}')"
  echo "DefaultVegetation=CLASSIC"
  echo "ModernVegetationGeometry=REMOVED"
  echo ""
  echo "SOURCE URLS (assets are not redistributed by Vice City VR)"
  echo "GTA VC HD + Weapons=https://drive.google.com/file/d/1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj/view"
  echo "Mods / Atmosphere=https://drive.google.com/file/d/1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d/view"
  echo ""
  echo "SOURCE SHA256"
  for name in "Original game generic.txd" "HD pack gta3.img" "HD pack gta3.dir" "HD pack generic.txd" "Atmosphere vehicles.col" "Atmosphere wheels.dff" "Atmosphere wheels.txd"; do
    echo "$name=${SOURCE_HASHES[$name]}"
  done
  echo ""
  echo "OUTPUT SHA256"
  for name in "models/gta3.img" "models/gta3.dir" "models/generic.txd" "vegetation_models.txt"; do
    echo "$name=${OUTPUT_HASHES[$name]}"
  done
  echo ""
  echo "Do not redistribute this built folder; it contains original game and third-party mod assets."
} > "$STAGE/BUILD_INFO.txt"

# Commit only after all operations and validation have succeeded. With
# --force, the old output is moved aside and restored if the final rename
# fails. This avoids turning a failed build into a broken installation.
BACKUP=""
if [ -e "$OUT" ]; then
  BACKUP="$OUT_PARENT/.$OUT_LEAF.previous-$UNIQUE"
  mv "$OUT" "$BACKUP"
fi
if mv "$STAGE" "$OUT" 2>/dev/null; then
  COMMITTED=1
else
  if [ -n "$BACKUP" ] && [ ! -e "$OUT" ] && [ -e "$BACKUP" ]; then
    mv "$BACKUP" "$OUT"
  fi
  die "Could not commit the staged Modern overlay to $OUT. The previous copy was restored when possible."
fi
if [ -n "$BACKUP" ] && [ -e "$BACKUP" ]; then
  if ! rm -rf "$BACKUP"; then
    warn "The new overlay is installed, but the previous copy could not be removed: $BACKUP"
  fi
fi

IMG_MB=$(( $(stat -c%s "$OUT/models/gta3.img") / 1024 / 1024 ))
echo ""
echo "Done. The Modern overlay is ready (gta3.img: $IMG_MB MB)."
echo "In VR MENU > MODEL ASSETS, enable the Modern categories you want, then restart."
echo "VEGETATION / PALMS is deliberately kept CLASSIC for performance."
exit 0
