#!/usr/bin/env bash
# Linux port of REPAIR_GAME_FILES.bat.
set -uo pipefail

title "Vice City VR - Repair Menu Text and Frontend Files" 2>/dev/null || true
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

echo "================================================================"
echo " VICE CITY VR - REPAIR MENU TEXT AND FRONTEND FILES"
echo "================================================================"
echo ""
echo "Run this if the OPTIONS menus show entries like \"FET_GFX missing\""
echo "instead of proper labels. It copies the port's own replacement"
echo "text and frontend files to an existing installation."
echo ""
echo "Saves, settings and models are not touched."
echo ""

INSTALLER="$PWD/tools/install-game-files.sh"
if [ ! -f "$INSTALLER" ]; then
    echo "ERROR: Required file is missing:"
    echo "  $INSTALLER"
    echo ""
    echo "Extract the complete source-kit ZIP before running this file."
    RESULT=1
    goto_finish
fi

bash "$INSTALLER" "$@"
RESULT=$?

goto_finish() {
    echo ""
    if [ "$RESULT" -ne 0 ]; then
        echo "FAILED. Read the error above; nothing was silently ignored."
    else
        echo "COMPLETE. Fully restart Vice City VR on the headset."
    fi
    echo ""
    echo "This window will remain open. Press any key when you are done reading it."
    read -n 1
    exit "$RESULT"
}
