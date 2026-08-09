#!/bin/bash
# Builds the Java build-time tools @nativescript/react-native ships and stages
# them into the package.
#
#   android-metadata-generator.jar -- describes the app's classpath so JS can
#                                     resolve android.* and java.*
#   dts-generator.jar              -- TypeScript declarations for the same
#   static-binding-generator.jar   -- pre-generates the Java classes that named
#                                     (@JavaProxy) subclasses resolve to
#
# All are plain JVM tools with no NDK involvement, so unlike the runtime itself
# they can be prebuilt and shipped: there is no jsi ABI to track.
#
# The binding generator additionally shells out to jsparser, which it looks for
# under its own working directory, so that tree is staged beside the jars. Its
# Babel dependencies are declared in the package's package.json rather than
# vendored, so they resolve from the app's node_modules.
set -euo pipefail
source "$(dirname "$0")/build_utils.sh"

TEST_APP_DIR="$REPO_ROOT/platforms/android/test-app"
TOOLS_DIR="$REPO_ROOT/packages/react-native/android/tools"

checkpoint "Building the NativeScript Android build tools..."

(
  cd "$TEST_APP_DIR"
  ./gradlew --quiet :android-metadata-generator:jar :dts-generator:jar \
    :static-binding-generator:jar
)

mkdir -p "$TOOLS_DIR"

MDG_JAR=$(find "$TEST_APP_DIR/build-tools/android-metadata-generator/build/libs" \
  -name "*.jar" ! -name "*-sources.jar" | head -1)
DTS_JAR=$(find "$TEST_APP_DIR/build-tools/android-dts-generator/dts-generator/build/libs" \
  -name "*.jar" ! -name "*-sources.jar" | head -1)
SBG_JAR=$(find "$TEST_APP_DIR/build-tools/static-binding-generator/build/libs" \
  -name "*.jar" ! -name "*-sources.jar" | head -1)

if [ -z "$MDG_JAR" ] || [ -z "$DTS_JAR" ] || [ -z "$SBG_JAR" ]; then
  echo "Could not find the built tool jars." >&2
  exit 1
fi

cp "$MDG_JAR" "$TOOLS_DIR/android-metadata-generator.jar"
cp "$DTS_JAR" "$TOOLS_DIR/dts-generator.jar"
cp "$SBG_JAR" "$TOOLS_DIR/static-binding-generator.jar"

# The binding generator resolves jsparser relative to its working directory, so
# the tree ships with it. node_modules is deliberately excluded -- the Babel
# packages are dependencies of @nativescript/react-native and resolve from the
# app's own node_modules at run time.
rm -rf "$TOOLS_DIR/jsparser"
mkdir -p "$TOOLS_DIR/jsparser"
(
  cd "$TEST_APP_DIR/build-tools/jsparser"
  tar cf - --exclude node_modules --exclude tests --exclude test-results \
    --exclude package-lock.json .
) | (cd "$TOOLS_DIR/jsparser" && tar xf -)

# Derives the metadata whitelist from the app's own bundle. Plain node scripts
# that reuse the Babel packages jsparser already depends on.
rm -rf "$TOOLS_DIR/metadata-filter"
mkdir -p "$TOOLS_DIR/metadata-filter"
cp "$TEST_APP_DIR/build-tools/metadata-filter/harvest.js" \
   "$TEST_APP_DIR/build-tools/metadata-filter/seed.js" \
   "$TEST_APP_DIR/build-tools/metadata-filter/verify-coverage.js" \
   "$TEST_APP_DIR/build-tools/metadata-filter/check-runtime-keeplist.js" \
   "$TOOLS_DIR/metadata-filter/"

checkpoint "Staged:"
ls -la "$TOOLS_DIR"
