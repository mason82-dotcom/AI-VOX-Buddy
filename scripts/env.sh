#!/usr/bin/env bash
# Sourced by the other scripts and interactively; never change shell options
# here, or `set -e` would close the caller's interactive terminal on any error.
AIVOX3_PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_TOOLS_PATH="$AIVOX3_PROJECT_DIR/.tools/espressif"
export IDF_COMPONENT_CACHE_PATH="$AIVOX3_PROJECT_DIR/.tools/component-cache"
# shellcheck disable=SC1091
source "$AIVOX3_PROJECT_DIR/.tools/esp-idf-v5.5.5/export.sh"
