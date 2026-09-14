#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
OUT_FILE="$SCRIPT_DIR/addon.dylib"
SRC_FILE="$SCRIPT_DIR/addon.cpp"

clang++ \
  -std=c++20 \
  -dynamiclib \
  -fPIC \
  -undefined dynamic_lookup \
  -I "$ROOT_DIR/NativeScript/napi/common" \
  -I "$ROOT_DIR/NativeScript/napi/hermes" \
  -o "$OUT_FILE" \
  "$SRC_FILE"

echo "Built $OUT_FILE"
