#!/usr/bin/env bash
# Runs the ESP-IDF-independent unit tests with the host compiler.
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
for name in wav text; do
  cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$PROJECT_DIR/main" "$PROJECT_DIR/main/$name.c" "$PROJECT_DIR/test/test_$name.c" -o "$out/test_$name"
  "$out/test_$name"
done
