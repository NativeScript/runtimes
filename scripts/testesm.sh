#!/bin/bash
set -e
source "$(dirname "$0")/build_utils.sh"

"$SCRIPT_DIR/build_nativescript.sh" --no-sim --no-iphone --v8 --macos-cli >/dev/null 2>/dev/null
./dist/nsr run platforms/apple/test/cli/esm_a.mjs
