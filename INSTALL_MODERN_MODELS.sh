#!/usr/bin/env bash
# Linux port of INSTALL_MODERN_MODELS.bat.
set -uo pipefail

title "Vice City VR - Download Build and Install Modern Models" 2>/dev/null || true
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

echo "================================================================"
echo " VICE CITY VR - DOWNLOAD, BUILD AND INSTALL MODERN MODELS"
echo "================================================================"
echo ""

INSTALLER="$PWD/tools/prepare-modern-models.sh"
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
        echo "FAILED. Read the error above; the previous Modern folder was not replaced silently."
    else
        echo "COMPLETE. Fully restart Vice City VR on the headset."
    fi
    echo ""
    echo "This window will remain open. Press any key when you are done reading it."
    read -n 1
    exit "$RESULT"
}
