#!/bin/bash
# Builds the Java build-time tools @nativescript/react-native ships and stages
# them into the package.
#
#   android-metadata-generator.jar -- describes the app's classpath so JS can
#                                     resolve android.* and java.*
#   dts-generator.jar              -- TypeScript declarations for the same
#
# Both are plain JVM tools with no NDK involvement, so unlike the runtime itself
# they can be prebuilt and shipped: there is no jsi ABI to track.
set -euo pipefail
source "$(dirname "$0")/build_utils.sh"

TEST_APP_DIR="$REPO_ROOT/platforms/android/test-app"
TOOLS_DIR="$REPO_ROOT/packages/react-native/android/tools"

checkpoint "Building the NativeScript Android build tools..."

(
  cd "$TEST_APP_DIR"
  ./gradlew --quiet :android-metadata-generator:jar :dts-generator:jar
)

mkdir -p "$TOOLS_DIR"

MDG_JAR=$(find "$TEST_APP_DIR/build-tools/android-metadata-generator/build/libs" \
  -name "*.jar" ! -name "*-sources.jar" | head -1)
DTS_JAR=$(find "$TEST_APP_DIR/build-tools/android-dts-generator/dts-generator/build/libs" \
  -name "*.jar" ! -name "*-sources.jar" | head -1)

if [ -z "$MDG_JAR" ] || [ -z "$DTS_JAR" ]; then
  echo "Could not find the built tool jars." >&2
  exit 1
fi

cp "$MDG_JAR" "$TOOLS_DIR/android-metadata-generator.jar"
cp "$DTS_JAR" "$TOOLS_DIR/dts-generator.jar"

checkpoint "Staged:"
ls -la "$TOOLS_DIR"
