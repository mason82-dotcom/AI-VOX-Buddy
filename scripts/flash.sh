#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$PROJECT_DIR/scripts/env.sh"
port="$($PROJECT_DIR/scripts/detect-device.sh)"
chip="$(esptool.py --chip esp32s3 --port "$port" chip_id 2>&1)"
grep -q "ESP32-S3" <<<"$chip" || { echo "Not an ESP32-S3; refusing." >&2; exit 5; }
mac="$(sed -n 's/^MAC: //p' <<<"$chip" | tail -1)"
[[ -n "$mac" && "$mac" == "${AIVOX3_EXPECTED_MAC:-}" ]] || {
  echo "Set AIVOX3_EXPECTED_MAC to the independently verified board MAC ($mac)." >&2; exit 8;
}
build_dir="${AIVOX3_BUILD_DIR:-build}"
case "$build_dir" in
  build) ;;
  build-format)
    [[ "${AIVOX3_SD_FORMAT_APPROVED:-}" == yes ]] || {
      echo "Format profile requires AIVOX3_SD_FORMAT_APPROVED=yes." >&2; exit 13;
    } ;;
  build-hermes)
    [[ "${AIVOX3_HERMES_FLASH_APPROVED:-}" == yes ]] || {
      echo "Hermes profile embeds Wi-Fi/MQTT credentials; set AIVOX3_HERMES_FLASH_APPROVED=yes." >&2; exit 15;
    } ;;
  *) echo "Only build, build-format or build-hermes may be flashed by this script." >&2; exit 14 ;;
esac
[[ -f "$PROJECT_DIR/$build_dir/ai_vox_buddy.bin" ]] || { echo "Build first." >&2; exit 6; }
[[ "${AIVOX3_FLASH_APPROVED:-}" == yes ]] || {
  echo "Flash blocked pending explicit approval and AIVOX3_FLASH_APPROVED=yes." >&2; exit 7;
}
backup="${AIVOX3_BACKUP_FILE:-}"
if [[ -n "$backup" ]]; then
  [[ -f "$backup" ]] || { echo "Backup file does not exist." >&2; exit 9; }
  [[ "$(stat -c %s "$backup")" == 16777216 ]] || { echo "Backup size is not 16 MB." >&2; exit 10; }
  [[ -f "$backup.sha256" ]] || { echo "Backup checksum file is missing." >&2; exit 11; }
  sha256sum -c "$backup.sha256" || { echo "Backup checksum mismatch." >&2; exit 12; }
else
  [[ "${AIVOX3_NO_BACKUP_APPROVED:-}" == yes ]] || {
    echo "No backup: set AIVOX3_NO_BACKUP_APPROVED=yes only after explicit user approval." >&2; exit 9;
  }
  echo "WARNING: flashing without a firmware backup; original firmware may be unrecoverable." >&2
fi
cd "$PROJECT_DIR"
idf.py -B "$build_dir" -p "$port" flash
