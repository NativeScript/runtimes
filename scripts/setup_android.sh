#!/bin/bash
#
# Prepares a checkout for building the Android runtime:
#   1. Applies the NativeScript patches onto the vendored QuickJS-NG source
#      (shared with the Apple build -- see vendor/quickjs/patches/README.md).
#   2. Installs the jsparser build-tool dependencies.
#
# Fetching the engine source is a deliberate manual step, so nothing here can
# move you off the pinned commit:
#
#     git submodule update --init --force vendor/quickjs/source_ng
#
# Idempotent: re-running resets the submodule work tree to the pin and re-applies.
#
set -e
source "$(dirname "$0")/build_utils.sh"

checkpoint "Applying QuickJS patches"
node "$SCRIPT_DIR/apply_quickjs_patches.js"

checkpoint "Installing jsparser build-tool dependencies"
JSPARSER_DIR="$REPO_ROOT/platforms/android/test-app/build-tools/jsparser"
if [ -f "$JSPARSER_DIR/package-lock.json" ]; then
  npm --prefix "$JSPARSER_DIR" ci
else
  npm --prefix "$JSPARSER_DIR" install
fi

checkpoint "Android setup complete"
