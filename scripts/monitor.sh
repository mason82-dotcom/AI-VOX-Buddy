#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$PROJECT_DIR/scripts/env.sh"
port="$($PROJECT_DIR/scripts/detect-device.sh)"
cd "$PROJECT_DIR"
idf.py -p "$port" monitor
