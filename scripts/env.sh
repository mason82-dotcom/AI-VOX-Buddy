#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_TOOLS_PATH="$PROJECT_DIR/.tools/espressif"
export IDF_COMPONENT_CACHE_PATH="$PROJECT_DIR/.tools/component-cache"
# shellcheck disable=SC1091
source "$PROJECT_DIR/.tools/esp-idf-v5.5.5/export.sh"
