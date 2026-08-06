#!/usr/bin/env bash

set -euo pipefail

PACKAGE="com.miamivr.quest"
SAVE_URI_BASE="content://com.miamivr.quest.saves/slot"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONVERTER="$SCRIPT_DIR/convert_vr_save.py"
BACKUP_ROOT="${MIAMIVR_SAVE_BACKUP_DIR:-$SCRIPT_DIR/backups}"

usage() {
	cat <<'EOF'
Transfer Vice City VR saves between a PC and a Meta Quest.

Usage:
  bash ./save_transfer.sh import SLOT [PC_SAVE]
  bash ./save_transfer.sh export SLOT [PC_SAVE]

Commands:
  import  Convert a PC save and install it into Quest slot SLOT.
          PC_SAVE defaults to ./GTAVCsfSLOT.b.
  export  Download Quest slot SLOT and convert it for the PC build.
          PC_SAVE defaults to ./GTAVCsfSLOT.b.

SLOT must be from 1 through 8. The game is force-stopped before accessing
its save. Existing Quest saves are backed up under tools/saves/backups.

Requirements:
  - adb available in PATH
  - python3 available in PATH
  - USB debugging enabled and the headset authorized

Optional environment variables:
  ADB_SERIAL                  Select a device when more than one is connected.
  MIAMIVR_SAVE_BACKUP_DIR     Override the local backup directory.

Examples:
  bash ./save_transfer.sh import 3 ~/Downloads/GTAVCsf3.b
  bash ./save_transfer.sh export 3 ~/Documents/GTAVCsf3.b
  ADB_SERIAL=1WMHH123456789 bash ./save_transfer.sh export 1
EOF
}

die() {
	printf 'Error: %s\n' "$*" >&2
	exit 1
}

command -v adb >/dev/null 2>&1 || die "adb was not found in PATH"
command -v python3 >/dev/null 2>&1 || die "python3 was not found in PATH"
[[ -f "$CONVERTER" ]] || die "converter not found: $CONVERTER"

[[ $# -ge 2 && $# -le 3 ]] || {
	usage >&2
	exit 2
}

ACTION="$1"
SLOT="$2"
[[ "$ACTION" == "import" || "$ACTION" == "export" ]] || die "command must be import or export"
[[ "$SLOT" =~ ^[1-8]$ ]] || die "slot must be a number from 1 through 8"

PC_SAVE="${3:-$PWD/GTAVCsf${SLOT}.b}"
URI="$SAVE_URI_BASE/$SLOT"
REMOTE_FILE="/data/local/tmp/miamivr_save_${SLOT}_$$.b"
LOCAL_TMP="$(mktemp -d "${TMPDIR:-/tmp}/miamivr-save.XXXXXX")"
RAW_QUEST_SAVE="$LOCAL_TMP/quest-slot-${SLOT}.b"
CONVERTED_SAVE="$LOCAL_TMP/converted-slot-${SLOT}.b"
VERIFY_SAVE="$LOCAL_TMP/verify-slot-${SLOT}.b"

ADB=(adb)
if [[ -n "${ADB_SERIAL:-}" ]]; then
	ADB+=(-s "$ADB_SERIAL")
fi

cleanup() {
	"${ADB[@]}" shell rm -f "$REMOTE_FILE" >/dev/null 2>&1 || true
	rm -rf -- "$LOCAL_TMP"
}
trap cleanup EXIT INT TERM

"${ADB[@]}" get-state >/dev/null 2>&1 || die "no authorized Quest was found by adb"
"${ADB[@]}" shell pm path "$PACKAGE" 2>/dev/null |
	grep -q '^package:' ||
	die "Vice City VR is not installed on the connected Quest (package: $PACKAGE)"

read_quest_slot() {
	local destination="$1"
	"${ADB[@]}" shell rm -f "$REMOTE_FILE" >/dev/null 2>&1 || true
	"${ADB[@]}" shell "content read --uri '$URI' > '$REMOTE_FILE'" >/dev/null
	"${ADB[@]}" pull "$REMOTE_FILE" "$destination" >/dev/null
	[[ -s "$destination" ]]
}

backup_quest_slot_if_present() {
	local stamp backup_dir backup_file
	stamp="$(date -u +%Y%m%d-%H%M%S)"
	backup_dir="$BACKUP_ROOT/$stamp"
	backup_file="$backup_dir/GTAVCsf${SLOT}.b"

	if read_quest_slot "$RAW_QUEST_SAVE" 2>/dev/null; then
		mkdir -p -- "$backup_dir"
		if python3 "$CONVERTER" quest-to-pc "$RAW_QUEST_SAVE" "$backup_file"; then
			printf 'Backed up existing Quest slot %s to %s\n' "$SLOT" "$backup_file"
		else
			cp -- "$RAW_QUEST_SAVE" "$backup_dir/GTAVCsf${SLOT}.quest-raw.b"
			printf 'Warning: existing slot could not be converted; raw backup saved to %s\n' \
				"$backup_dir/GTAVCsf${SLOT}.quest-raw.b" >&2
		fi
	else
		printf 'Quest slot %s is empty; no backup was needed.\n' "$SLOT"
	fi
}

case "$ACTION" in
	import)
		[[ -f "$PC_SAVE" ]] || die "PC save not found: $PC_SAVE"
		python3 "$CONVERTER" pc-to-quest "$PC_SAVE" "$CONVERTED_SAVE"

		"${ADB[@]}" shell am force-stop "$PACKAGE" >/dev/null
		backup_quest_slot_if_present

		"${ADB[@]}" push "$CONVERTED_SAVE" "$REMOTE_FILE" >/dev/null
		"${ADB[@]}" shell "content write --uri '$URI' < '$REMOTE_FILE'" >/dev/null

		read_quest_slot "$VERIFY_SAVE" || die "Quest provider returned an empty save after import"
		cmp -s -- "$CONVERTED_SAVE" "$VERIFY_SAVE" || die "Quest save verification failed after import"
		printf 'Imported %s into Quest slot %s and verified it successfully.\n' "$PC_SAVE" "$SLOT"
		;;

	export)
		"${ADB[@]}" shell am force-stop "$PACKAGE" >/dev/null
		read_quest_slot "$RAW_QUEST_SAVE" || die "Quest slot $SLOT is empty"

		mkdir -p -- "$(dirname -- "$PC_SAVE")"
		if [[ -e "$PC_SAVE" ]]; then
			stamp="$(date -u +%Y%m%d-%H%M%S)"
			mkdir -p -- "$BACKUP_ROOT/$stamp"
			cp -- "$PC_SAVE" "$BACKUP_ROOT/$stamp/GTAVCsf${SLOT}.b"
			printf 'Backed up existing PC destination to %s\n' \
				"$BACKUP_ROOT/$stamp/GTAVCsf${SLOT}.b"
		fi

		python3 "$CONVERTER" quest-to-pc "$RAW_QUEST_SAVE" "$PC_SAVE"
		printf 'Exported Quest slot %s to %s.\n' "$SLOT" "$PC_SAVE"
		;;
esac
