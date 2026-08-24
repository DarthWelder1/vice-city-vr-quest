#!/usr/bin/env bash
# Linux port of BUILD_AND_INSTALL.bat.
#
# A double-click normally starts the terminal, which can disappear before a
# user can read an early host/security error. Run the real wizard inside a
# nested shell that remains open even if the wizard fails, and always print
# the diagnostic-log path.
set -uo pipefail

# Re-exec inside a persistent shell when launched by a file manager.
if [ -z "${VCVR_KEEP_OPEN:-}" ]; then
  export VCVR_KEEP_OPEN=1
  exec bash -c "bash '$0' \"\$@\"; echo; echo 'This window will remain open. Press any key when you are done reading it.'; read -n 1" _ "$@"
fi

title "Vice City VR - Personal Quest Builder" 2>/dev/null || true

echo ""
echo "============================================================"
echo "  VICE CITY VR - PERSONAL QUEST BUILD AND INSTALL WIZARD"
echo "============================================================"
echo ""
echo "This builds a personal APK from source. No APK or GTA data is"
echo "included in this repository."
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCVR_SCRIPT="$SCRIPT_DIR/tools/build-and-install.sh"
VCVR_LOG="${TMPDIR:-/tmp}/ViceCityVR-Build-And-Install.log"

if [ ! -f "$VCVR_SCRIPT" ]; then
  echo "The complete source kit is not beside this script."
  echo "Downloading and unpacking it automatically..."
  echo ""

  VCVR_BOOT="${TMPDIR:-/tmp}/ViceCityVR-SourceKit-$$-$(date +%s)"
  VCVR_ZIP="${VCVR_BOOT}.zip"

  if ! curl -fsSL --retry 5 --retry-all-errors \
      -o "$VCVR_ZIP" \
      "https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr-quest/archive/refs/heads/master.zip" \
      || ! unzip -q -o "$VCVR_ZIP" -d "$VCVR_BOOT"; then
    echo ""
    echo "ERROR: The complete source kit could not be downloaded."
    echo "Check the internet connection and the diagnostic log, then retry."
    exit 1
  fi
  rm -f "$VCVR_ZIP"

  found=""
  for candidate in "$VCVR_BOOT"/*/tools/build-and-install.sh; do
    [ -f "$candidate" ] && found="$candidate"
  done
  if [ -z "$found" ]; then
    echo "ERROR: The downloaded source kit is incomplete."
    echo "Expected tools/build-and-install.sh was not found after extraction."
    exit 1
  fi
  VCVR_SCRIPT="$found"

  echo "Source kit ready."
  echo ""
fi

bash "$VCVR_SCRIPT" --log-path "$VCVR_LOG" "$@"
VCVR_EXIT=$?

echo ""
if [ "$VCVR_EXIT" -ne 0 ]; then
  echo "FAILED. Read the error above; nothing was silently ignored."
else
  echo "SUCCESS. The wizard completed all requested steps."
fi
echo ""
echo "Diagnostic log:"
echo "  $VCVR_LOG"
echo ""
exit "$VCVR_EXIT"
