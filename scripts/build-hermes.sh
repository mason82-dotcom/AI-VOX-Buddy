#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$PROJECT_DIR/scripts/env.sh"
cd "$PROJECT_DIR"
idf.py -B build-hermes -DSDKCONFIG=sdkconfig.hermes \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.hermes.defaults' build
