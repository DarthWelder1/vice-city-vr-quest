#!/bin/sh
# Assembles a buildable source tree for the MiamiVR Quest port.
#
#   tools/assemble.sh <path to reVC source> <output dir>
#
# The reVC source tree is not part of this repository and is not distributed
# with it. Obtain it separately; the patch targets the final (September 2021)
# state of the reVC master branch.
set -e
REVC="$1"
OUT="$2"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

[ -n "$REVC" ] && [ -n "$OUT" ] || { echo "usage: $0 <revc-source> <output-dir>"; exit 1; }
[ -f "$REVC/src/core/main.cpp" ] || { echo "'$REVC' does not look like a reVC source tree"; exit 1; }
[ ! -e "$OUT" ] || { echo "'$OUT' already exists; refusing to overwrite"; exit 1; }

echo "[1/4] Copying the reVC tree..."
mkdir -p "$OUT"
cp -r "$REVC/." "$OUT/"
rm -rf "$OUT/.git"

echo "[2/4] Applying the port patch..."
cd "$OUT"
git init -q
git apply --whitespace=nowarn "$REPO/patches/revc-quest.patch" || {
    echo "Patch did not apply. Make sure the reVC tree is the final master state and unmodified."
    exit 1
}
rm -rf "$OUT/.git"

echo "[3/4] Copying the port sources..."
cp -r "$REPO/overlay/." "$OUT/"

echo "[4/4] Copying librw with the Vulkan backend..."
mkdir -p "$OUT/vendor/librw"
cp -r "$REPO/librw/." "$OUT/vendor/librw/"

echo ""
echo "Done. Next steps:"
echo "  cd $OUT/android"
echo "  gradle wrapper   (first time only, or open in Android Studio)"
echo "  ./gradlew assembleDebug"
echo "See BUILDING.md for prerequisites and installing to the headset."
