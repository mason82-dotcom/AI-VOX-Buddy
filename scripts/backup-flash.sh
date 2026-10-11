#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$PROJECT_DIR/scripts/env.sh"
port="$($PROJECT_DIR/scripts/detect-device.sh)"
identity="$(esptool.py --chip esp32s3 --port "$port" flash_id 2>&1)"
[[ "$identity" == *"Detected flash size: 16MB"* ]] || {
  echo "Expected a 16 MB ESP32-S3 flash; refusing a partial backup." >&2
  exit 5
}
mac="$(sed -n 's/^MAC: //p' <<<"$identity" | tail -1)"
[[ -n "$mac" ]] || { echo "Unable to read board MAC." >&2; exit 6; }
stamp="$(date +%Y%m%d-%H%M%S)"
chunk_dir="${AIVOX3_BACKUP_CHUNK_DIR:-$PROJECT_DIR/backup/chunks-${mac//:/}-$stamp}"
[[ "$chunk_dir" == "$PROJECT_DIR/backup/chunks-${mac//:/}-"* ]] || {
  echo "Resume directory must belong to the detected board MAC." >&2; exit 9;
}
mkdir -p "$chunk_dir"
echo "Backing up ESP32-S3 $mac from $port into $chunk_dir"

# Small blocks survive USB Serial/JTAG reconnects; completed blocks persist.
for ((index=0; index<64; index++)); do
  offset=$((index * 0x40000))
  part="$chunk_dir/$(printf '%02d' "$index").bin"
  if [[ -f "$part" && "$(stat -c %s "$part")" == 262144 ]]; then
    continue
  fi
  for attempt in 1 2 3; do
    echo "Block $((index + 1))/64 at $(printf '0x%06x' "$offset"), attempt $attempt"
    if esptool.py --chip esp32s3 --port "$port" read_flash --no-progress "$offset" 0x40000 "$part"; then
      [[ "$(stat -c %s "$part")" == 262144 ]] && break
    fi
    if ((attempt == 3)); then
      echo "Block $index failed. Existing blocks remain in $chunk_dir" >&2
      exit 7
    fi
    sleep 2
  done
done

out="$PROJECT_DIR/backup/ai-vox3-full-${mac//:/}-$stamp.bin"
for ((index=0; index<64; index++)); do
  part="$chunk_dir/$(printf '%02d' "$index").bin"
  dd if="$part" of="$out" bs=262144 seek="$index" count=1 conv=notrunc status=none
done
[[ "$(stat -c %s "$out")" == 16777216 ]] || { echo "Incomplete backup: $out" >&2; exit 8; }
sha256sum "$out" | tee "$out.sha256"
echo "Full backup: $out"
