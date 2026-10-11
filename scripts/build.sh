#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$PROJECT_DIR/scripts/env.sh"
cd "$PROJECT_DIR"
if [[ ! -f sdkconfig ]]; then
    idf.py set-target esp32s3
fi
idf.py build
