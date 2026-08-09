#!/bin/bash
# Stages the Android half of @nativescript/react-native into the package.
#
# The module compiles from source in the consuming app (see
# packages/react-native/android/build.gradle for why), so what ships is source:
# the runtime's C++ and its com.tns.* Java, plus the two build-time tools. In a
# checkout the Gradle build reads these straight out of the repository instead --
# that fallback is what lets the demo app build with no staging step.
set -euo pipefail
source "$(dirname "$0")/build_utils.sh"

PACKAGE_DIR="$REPO_ROOT/packages/react-native"
ANDROID_DIR="$PACKAGE_DIR/android"
NATIVE_API="$PACKAGE_DIR/native-api"
RUNTIME_JAVA_SRC="$REPO_ROOT/platforms/android/test-app/runtime/src/main/java"
BINDINGS_JAVA_SRC="$REPO_ROOT/platforms/android/test-app/runtime-binding-generator/src/main/java"

checkpoint "Staging the NativeScript Android runtime sources..."

# Only the Android trees; the iOS staging step owns the rest of native-api/.
rm -rf \
  "$NATIVE_API/runtime/android" \
  "$NATIVE_API/ffi/jni" \
  "$NATIVE_API/jsi" \
  "$NATIVE_API/napi/common" \
  "$ANDROID_DIR/src/main/java-runtime"

mkdir -p \
  "$NATIVE_API/runtime/android" \
  "$NATIVE_API/ffi/jni" \
  "$NATIVE_API/jsi" \
  "$NATIVE_API/napi/common" \
  "$NATIVE_API/metadata/include" \
  "$ANDROID_DIR/src/main/java-runtime"

# The jsi runtime and its JNI interop. The napi lane is not shipped: a guest has
# no Node-API anywhere in it.
cp -R "$REPO_ROOT/NativeScript/runtime/android/jsi" "$NATIVE_API/runtime/android/"
cp -R "$REPO_ROOT/NativeScript/ffi/jni/jsi" "$NATIVE_API/ffi/jni/"

# Parts of the standalone runtime the guest build excludes (see the source
# filters in packages/react-native/android/CMakeLists.txt). They are dropped
# here too so the package does not carry sources no consumer can compile.
#
# workers/ keeps LooperTasks: despite living beside the Worker implementation it
# is the main-thread task queue every guest needs. ConcurrentQueue has no user
# other than the Worker wrapper, so it goes with it.
rm -rf \
  "$NATIVE_API/runtime/android/jsi/assetextractor" \
  "$NATIVE_API/runtime/android/jsi/modules/module" \
  "$NATIVE_API/runtime/android/jsi/modules/console" \
  "$NATIVE_API/runtime/android/jsi/modules/timers" \
  "$NATIVE_API/runtime/android/jsi/modules/performance" \
  "$NATIVE_API/runtime/android/jsi/profiler" \
  "$NATIVE_API/runtime/android/jsi/instrumentation"
rm -f \
  "$NATIVE_API/runtime/android/jsi/workers/WorkerWrapper.cpp" \
  "$NATIVE_API/runtime/android/jsi/workers/WorkerWrapper.h" \
  "$NATIVE_API/runtime/android/jsi/workers/WorkerMessage.h" \
  "$NATIVE_API/runtime/android/jsi/workers/ConcurrentQueue.cpp" \
  "$NATIVE_API/runtime/android/jsi/workers/ConcurrentQueue.h"

# The engine layer. Only the Hermes adapter is reachable -- it is the one written
# against plain facebook::jsi -- but the tree is copied whole so that a rooted
# include of a sibling resolves the same way it does in the repository.
cp -R "$REPO_ROOT/NativeScript/jsi/." "$NATIVE_API/jsi/"

# ConcurrentMap.h, robin_hood.h, spinmutex.h: plain containers the runtime uses
# on both binding layers, which happen to live under napi/.
cp "$REPO_ROOT/NativeScript/napi/common/ConcurrentMap.h" \
   "$REPO_ROOT/NativeScript/napi/common/robin_hood.h" \
   "$REPO_ROOT/NativeScript/napi/common/spinmutex.h" \
   "$NATIVE_API/napi/common/"

cp "$REPO_ROOT/metadata-generator/include/Metadata.h" \
   "$REPO_ROOT/metadata-generator/include/MetadataReader.h" \
   "$NATIVE_API/metadata/include/" 2>/dev/null || true

# com.tns.* and com.tns.bindings.*, exactly as the standalone runtime composes them.
cp -R "$RUNTIME_JAVA_SRC/." "$ANDROID_DIR/src/main/java-runtime/"
cp -R "$BINDINGS_JAVA_SRC/." "$ANDROID_DIR/src/main/java-runtime/"

# ts_helpers.js -- evaluated at attach; there is no module loader to require it.
mkdir -p "$ANDROID_DIR/src/main/assets/nativescript/internal"
cp "$REPO_ROOT/platforms/android/test-app/app/src/main/assets/internal/ts_helpers.js" \
   "$ANDROID_DIR/src/main/assets/nativescript/internal/ts_helpers.js"

"$SCRIPT_DIR/build_react_native_android_tools.sh"

checkpoint "Staged the Android runtime:"
echo "  C++ sources : $(find "$NATIVE_API/runtime/android" "$NATIVE_API/ffi/jni" "$NATIVE_API/jsi" -name '*.cpp' -o -name '*.h' | wc -l | tr -d ' ') files"
echo "  Java sources: $(find "$ANDROID_DIR/src/main/java-runtime" -name '*.java' | wc -l | tr -d ' ') files"
echo "  Build tools : $(ls "$ANDROID_DIR/tools" | tr '\n' ' ')"
