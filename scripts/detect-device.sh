#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob
ports=(/dev/ttyACM* /dev/ttyUSB*)
matches=()
for candidate in "${ports[@]}"; do
  properties="$(udevadm info -q property -n "$candidate" 2>/dev/null || true)"
  if [[ "$properties" == *$'ID_VENDOR_ID=303a'* &&
        ( "$properties" == *$'ID_MODEL_ID=1001'* || "$properties" == *$'ID_MODEL_ID=0009'* ) ]]; then
    matches+=("$candidate")
  fi
done
((${#matches[@]})) || { echo "No ESP32-S3 USB Serial/JTAG device found (bootloader 303a:1001 or firmware 303a:0009)." >&2; exit 2; }
((${#matches[@]} == 1)) || { printf 'Ambiguous Espressif devices: %s\n' "${matches[*]}" >&2; exit 3; }
port="${matches[0]}"
[[ -r "$port" && -w "$port" ]] || { echo "No read/write permission for $port; check dialout group or udev rule." >&2; exit 4; }
printf '%s\n' "$port"
