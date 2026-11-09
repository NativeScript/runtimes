#!/bin/bash
set -e
source "$(dirname "$0")/build_utils.sh"

BUILD_CATALYST=$(to_bool ${BUILD_CATALYST:=false}) # disable by default for now
BUILD_IPHONE=$(to_bool ${BUILD_IPHONE:=true})
BUILD_SIMULATOR=$(to_bool ${BUILD_SIMULATOR:=true})
BUILD_VISION=$(to_bool ${BUILD_VISION:=false}) # disable by default for now
BUILD_MACOS=$(to_bool ${BUILD_MACOS:=true})
VERBOSE=$(to_bool ${VERBOSE:=false})
BUILD_MACOS_CLI=$(to_bool ${BUILD_MACOS_CLI:=false})
BUILD_MACOS_NODE_API=$(to_bool ${BUILD_MACOS_NODE_API:=false})
EMBED_METADATA=$(to_bool ${EMBED_METADATA:=false})
CONFIG_BUILD=RelWithDebInfo

TARGET_ENGINE=${TARGET_ENGINE:=v8} # default to v8 for compat
NS_FFI_BACKEND=${NS_FFI_BACKEND:=auto}
NS_GSD_BACKEND=${NS_GSD_BACKEND:=auto}
METADATA_SIZE=${METADATA_SIZE:=0}
REQUESTED_SIGNATURE_DISPATCH=${NS_SIGNATURE_BINDINGS_CPP_PATH:-${TNS_SIGNATURE_BINDINGS_CPP_PATH:-}}

for arg in $@; do
  case $arg in
    --catalyst|--maccatalyst) BUILD_CATALYST=true ;;
    --no-catalyst|--no-maccatalyst) BUILD_CATALYST=false ;;
    --sim|--simulator) BUILD_SIMULATOR=true ;;
    --no-sim|--no-simulator) BUILD_SIMULATOR=false ;;
    --iphone|--device) BUILD_IPHONE=true ;;
    --no-iphone|--no-device|--no-phone|--no-ios) BUILD_IPHONE=false ;;
    --xr|--vision) BUILD_VISION=true ;;
    --no-xr|--no-vision) BUILD_VISION=false ;;
    --macos) BUILD_MACOS=true ;;
    --no-macos) BUILD_MACOS=false ;;
    --macos-napi) BUILD_MACOS_NODE_API=true ;;
    --no-macos-napi) BUILD_MACOS_NODE_API=false ;;
    --macos-cli) BUILD_MACOS_CLI=true ;;
    --no-macos-cli) BUILD_MACOS_CLI=false ;;
    --verbose|-v) VERBOSE=true ;;
    --v8) TARGET_ENGINE=v8 ;;
    --quickjs) TARGET_ENGINE=quickjs ;;
    --jsc) TARGET_ENGINE=jsc ;;
    --embed-metadata) EMBED_METADATA=true ;;
    --hermes) TARGET_ENGINE=hermes ;;
    --no-engine|--generic-napi) TARGET_ENGINE=none ;;
    --ffi-napi) NS_FFI_BACKEND=napi ;;
    --ffi-backend=*) NS_FFI_BACKEND="${arg#--ffi-backend=}" ;;
    --gsd-v8) NS_GSD_BACKEND=v8 ;;
    --gsd-jsc) NS_GSD_BACKEND=jsc ;;
    --gsd-quickjs) NS_GSD_BACKEND=quickjs ;;
    --gsd-hermes) NS_GSD_BACKEND=hermes ;;
    --gsd-napi) NS_GSD_BACKEND=napi ;;
    --gsd-none) NS_GSD_BACKEND=none ;;
    --gsd-backend=*) NS_GSD_BACKEND="${arg#--gsd-backend=}" ;;
    *) ;;
  esac
done

case "$TARGET_ENGINE" in
  v8)
    if [ ! -d "./Frameworks/libv8_monolith.xcframework" ]; then
      "$SCRIPT_DIR/download_v8.sh"
    fi
    ;;
  hermes)
    if [ ! -d "./Frameworks/hermes.xcframework" ]; then
      "$SCRIPT_DIR/download_hermes.sh"
    fi
    ;;
esac

QUIET=
if ! $VERBOSE; then
  QUIET=-quiet
fi

function assemble_node_api_xcframework () {
  local output_dir="$1"
  shift

  if command -v deno >/dev/null 2>&1; then
    deno run -A ./scripts/build_xcframework.mts --output "$output_dir" "$@"
    return
  fi

  if [ ! -d "$SCRIPT_DIR/node_modules/react-native-node-api" ] || [ ! -d "$SCRIPT_DIR/node_modules/yargs-parser" ]; then
    npm --prefix "$SCRIPT_DIR" install --no-audit --no-fund
  fi

  node ./scripts/build_xcframework.mts --output "$output_dir" "$@"
}

function effective_gsd_backend () {
  local is_macos_napi="${1:-false}"

  local ffi_backend
  ffi_backend=$(effective_ffi_backend "$is_macos_napi")
  if [ "$ffi_backend" != "napi" ]; then
    case "$NS_GSD_BACKEND" in
      auto)
        echo "$ffi_backend"
        ;;
      *)
        echo "$NS_GSD_BACKEND"
        ;;
    esac
    return
  fi

  case "$NS_GSD_BACKEND" in
    auto)
      if [ "$TARGET_ENGINE" == "none" ]; then
        echo none
      else
        echo napi
      fi
      ;;
    *)
      echo "$NS_GSD_BACKEND"
      ;;
  esac
}

function effective_ffi_backend () {
  local is_macos_napi="${1:-false}"

  if $is_macos_napi || [ "$TARGET_ENGINE" == "none" ]; then
    echo napi
    return
  fi

  case "$NS_FFI_BACKEND" in
    auto)
      if [[ "$TARGET_ENGINE" == "hermes" || "$TARGET_ENGINE" == "v8" || "$TARGET_ENGINE" == "jsc" || "$TARGET_ENGINE" == "quickjs" ]]; then
        echo "$TARGET_ENGINE"
      else
        echo napi
      fi
      ;;
    v8|jsc|quickjs|hermes)
      if [ "$NS_FFI_BACKEND" != "$TARGET_ENGINE" ]; then
        echo "NS_FFI_BACKEND=$NS_FFI_BACKEND requires TARGET_ENGINE=$NS_FFI_BACKEND" >&2
        exit 1
      fi
      echo "$NS_FFI_BACKEND"
      ;;
    *)
      echo "$NS_FFI_BACKEND"
      ;;
  esac
}

function signature_dispatch_path () {
  if [ -n "$REQUESTED_SIGNATURE_DISPATCH" ]; then
    echo "$REQUESTED_SIGNATURE_DISPATCH"
    return
  fi
  echo "./NativeScript/ffi/objc/shared/GeneratedSignatureDispatch.inc"
}

function metadata_generator_source_hash () {
  find ./metadata-generator/src ./metadata-generator/include ./metadata-generator/tests \
    ./metadata-generator/symbol-analyzer ./metadata-generator/CMakeLists.txt \
    ./metadata-generator/build-step-metadata-generator.py \
    \( -name target -type d -prune \) -o -type f -print | \
    LC_ALL=C sort | xargs shasum | awk '{print $1}' | shasum | awk '{print $1}'
}

function signature_dispatch_stamp () {
  local platform="$1"
  local generator_hash
  generator_hash=$(metadata_generator_source_hash)
  printf "platform=%s\nmetadata_size=%s\ngenerator_hash=%s\n" \
    "$platform" "$METADATA_SIZE" "$generator_hash"
}

function ensure_metadata_generator () {
  local expected_hash
  expected_hash=$(metadata_generator_source_hash)
  local hash_file="./metadata-generator/dist/.source_hash"
  local host_arch
  host_arch=$(uname -m)
  if [ ! -x "./metadata-generator/dist/$host_arch/bin/objc-metadata-generator" ] || \
     [ ! -x "./metadata-generator/dist/$host_arch/bin/ns-metadata-symbols" ] || \
     [ ! -f "$hash_file" ] || \
     [ "$(cat "$hash_file")" != "$expected_hash" ]; then
    METADATA_GENERATOR_ARCHS="$host_arch" "$SCRIPT_DIR/build_metadata_generator.sh"
  fi
}

function ensure_signature_dispatch_bindings () {
  local platform="$1"
  local is_macos_napi="${2:-false}"
  local backend
  backend=$(effective_gsd_backend "$is_macos_napi")
  if [ "$TARGET_ENGINE" == "none" ] || [ "$backend" == "none" ]; then
    return
  fi

  if [ -z "$platform" ]; then
    return
  fi

  local expected_stamp
  expected_stamp=$(signature_dispatch_stamp "$platform" "$is_macos_napi")
  local generated_signature_dispatch
  generated_signature_dispatch=$(signature_dispatch_path "$is_macos_napi")
  local generated_signature_dispatch_stamp="${generated_signature_dispatch}.stamp"
  if [ -f "$generated_signature_dispatch" ] && \
     [ -f "$generated_signature_dispatch_stamp" ] && \
     [ "$(cat "$generated_signature_dispatch_stamp")" == "$expected_stamp" ]; then
    return
  fi

  ensure_metadata_generator

  checkpoint "Generating signature dispatch bindings for $platform ($backend)..."
  NS_SIGNATURE_BINDINGS_CPP_PATH="$generated_signature_dispatch" npm run metagen "$platform"
  mkdir -p "$(dirname "$generated_signature_dispatch_stamp")"
  printf "%s" "$expected_stamp" > "$generated_signature_dispatch_stamp"
}

DEV_TEAM=${DEVELOPMENT_TEAM:-}
DIST=$(PWD)/dist
mkdir -p $DIST

mkdir -p $DIST/intermediates

function cmake_build () {
  local platform="$1"
  shift
  local archs=("$@")
  local is_macos_cli=false
  local is_macos_napi=false

  if [ "$platform" == "macos-cli" ]; then
    platform="macos"
    is_macos_cli=true
  fi

  if [ "$platform" == "macos-napi" ]; then
    platform="macos"
    is_macos_napi=true
  fi

  ensure_signature_dispatch_bindings "$platform" "$is_macos_napi"

  local libffi_build_dir=
  case "$platform" in
    ios) libffi_build_dir="iphoneos-arm64" ;;
    ios-sim) libffi_build_dir="iphonesimulator-universal" ;;
    macos) libffi_build_dir="macosx-universal" ;;
    visionos) libffi_build_dir="xros-arm64" ;;
    visionos-sim) libffi_build_dir="xrsimulator-arm64" ;;
  esac

  if [ -n "$libffi_build_dir" ] && [ ! -f "./NativeScript/libffi/$libffi_build_dir/libffi.a" ]; then
    checkpoint "Building missing libffi artifacts for $platform"
    node ./scripts/build_libffi.js
  fi

  local build_dir="$DIST/intermediates/$platform"
  local cache_file="$build_dir/CMakeCache.txt"

  if [ -f "$cache_file" ]; then
    local needs_reconfigure=false
    local cached_engine
    cached_engine=$(grep '^TARGET_ENGINE:STRING=' "$cache_file" | sed 's/^TARGET_ENGINE:STRING=//' || true)
    if [ -n "$cached_engine" ] && [ "$cached_engine" != "$TARGET_ENGINE" ]; then
      echo "Reconfiguring $platform build directory for engine '$TARGET_ENGINE' (was '$cached_engine')."
      needs_reconfigure=true
    fi
    local cached_gsd_backend
    cached_gsd_backend=$(grep '^NS_GSD_BACKEND:STRING=' "$cache_file" | sed 's/^NS_GSD_BACKEND:STRING=//' || true)
    if [ -n "$cached_gsd_backend" ] && [ "$cached_gsd_backend" != "$NS_GSD_BACKEND" ]; then
      echo "Reconfiguring $platform build directory for GSD backend '$NS_GSD_BACKEND' (was '$cached_gsd_backend')."
      needs_reconfigure=true
    fi
    local cached_ffi_backend
    cached_ffi_backend=$(grep '^NS_FFI_BACKEND:STRING=' "$cache_file" | sed 's/^NS_FFI_BACKEND:STRING=//' || true)
    if [ -n "$cached_ffi_backend" ] && [ "$cached_ffi_backend" != "$NS_FFI_BACKEND" ]; then
      echo "Reconfiguring $platform build directory for FFI backend '$NS_FFI_BACKEND' (was '$cached_ffi_backend')."
      needs_reconfigure=true
    fi
    if $needs_reconfigure; then
      rm -rf "$build_dir"
    fi
  fi

  mkdir -p "$build_dir"

  if $EMBED_METADATA || $is_macos_cli || $is_macos_napi; then

    for arch in "${archs[@]}"; do

      METADATA_SIZE=$(($METADATA_SIZE > $(stat -f%z "./metadata-generator/metadata/metadata.$platform.$arch.nsmd") ? $METADATA_SIZE : $(stat -f%z "./metadata-generator/metadata/metadata.$platform.$arch.nsmd")))

    done

  fi

  cmake -S=./NativeScript -B="$build_dir" -GXcode -DTARGET_PLATFORM=$platform -DTARGET_ENGINE=$TARGET_ENGINE -DNS_FFI_BACKEND=$NS_FFI_BACKEND -DNS_GSD_BACKEND=$NS_GSD_BACKEND -DMETADATA_SIZE=$METADATA_SIZE -DBUILD_CLI_BINARY=$is_macos_cli -DBUILD_MACOS_NODE_API=$is_macos_napi

  cmake --build "$build_dir" --config $CONFIG_BUILD -- \
    CODE_SIGN_STYLE=Manual \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGN_IDENTITY= \
    DEVELOPMENT_TEAM=
}

if $BUILD_CATALYST; then
checkpoint "Building NativeScript for Mac Catalyst"

# cmake_build catalyst x86_64 arm64

fi

if $BUILD_SIMULATOR; then
checkpoint "Building NativeScript for iPhone (simulator)"

cmake_build ios-sim x86_64 arm64

fi

if $BUILD_IPHONE; then
checkpoint "Building NativeScript for iPhone (physical)"

cmake_build ios arm64

fi

if $BUILD_MACOS; then
checkpoint "Building NativeScript for macOS"

cmake_build macos x86_64 arm64

fi

if $BUILD_VISION; then

checkpoint "Building NativeScript for visionOS (physical)"

cmake_build visionos arm64

checkpoint "Building NativeScript for visionOS (simulator)"

cmake_build visionos-sim arm64

fi

if $BUILD_MACOS_CLI; then

checkpoint "Building NativeScript for macOS CLI"

cmake_build macos-cli x86_64 arm64

fi

if $BUILD_MACOS_NODE_API; then
  checkpoint "Building NativeScript for macOS Node API"

  cmake_build macos-napi x86_64 arm64
fi

XCFRAMEWORKS=()
if $BUILD_CATALYST; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/catalyst/$CONFIG_BUILD-maccatalyst/NativeScript.framework"
                  -debug-symbols "$DIST/intermediates/catalyst/$CONFIG_BUILD-maccatalyst/NativeScript.framework.dSYM" )
fi

if $BUILD_SIMULATOR; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/ios-sim/$CONFIG_BUILD-iphonesimulator/NativeScript.framework"
                  -debug-symbols "$DIST/intermediates/ios-sim/$CONFIG_BUILD-iphonesimulator/NativeScript.framework.dSYM" )
fi

if $BUILD_IPHONE; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/ios/$CONFIG_BUILD-iphoneos/NativeScript.framework"
                  -debug-symbols "$DIST/intermediates/ios/$CONFIG_BUILD-iphoneos/NativeScript.framework.dSYM" )
fi

if $BUILD_MACOS; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/macos/$CONFIG_BUILD/NativeScript.framework"
                  -debug-symbols "$DIST/intermediates/macos/$CONFIG_BUILD/NativeScript.framework.dSYM" )
fi

if $BUILD_VISION; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/visionos/$CONFIG_BUILD-xros/NativeScript.framework"
                  -debug-symbols "$DIST/intermediates/visionos/$CONFIG_BUILD-xros/NativeScript.framework.dSYM" )

  XCFRAMEWORKS+=( -framework "$DIST/intermediates/visionos-sim/$CONFIG_BUILD-xrsimulator/NativeScript.framework"
                  -debug-symbols "$DIST/intermediates/visionos-sim/$CONFIG_BUILD-xrsimulator/NativeScript.framework.dSYM" )
fi

if [[ -n "${XCFRAMEWORKS[@]}" ]]; then
  if [[ "$TARGET_ENGINE" == "none" ]]; then
    checkpoint "Creating the XCFramework for iOS (NativeScript.apple.node)"

    # We adhere to the prebuilds standard as described here:
    # https://github.com/callstackincubator/react-native-node-api/blob/9b231c14459b62d7df33360f930a00343d8c46e6/docs/PREBUILDS.md
    OUTPUT_DIR="packages/ios-node-api/build/$CONFIG_BUILD/NativeScript.apple.node"
    rm -rf $OUTPUT_DIR
    assemble_node_api_xcframework "$OUTPUT_DIR" "${XCFRAMEWORKS[@]}"
  else
    checkpoint "Creating NativeScript.xcframework"

    OUTPUT_DIR="$DIST/NativeScript.xcframework"
    rm -rf $OUTPUT_DIR
    xcodebuild -create-xcframework ${XCFRAMEWORKS[@]} -output "$OUTPUT_DIR"
  fi
fi

# We're currently distributing two separate packages:
# 1. UIKit-based (@nativescript/ios-node-api)
# 2. AppKit-based (@nativescript/macos-node-api)
# As such, there's no point bundling both UIKit-based and AppKit-based into a
# single XCFramework.
if $BUILD_MACOS; then
  if [[ "$TARGET_ENGINE" == "none" ]]; then
    checkpoint "Creating the XCFramework for macOS (NativeScript.apple.node)"

    # We adhere to the prebuilds standard as described here:
    # https://github.com/callstackincubator/react-native-node-api/blob/9b231c14459b62d7df33360f930a00343d8c46e6/docs/PREBUILDS.md
    OUTPUT_DIR="packages/macos-node-api/build/$CONFIG_BUILD/NativeScript.apple.node"
    rm -rf $OUTPUT_DIR
    assemble_node_api_xcframework "$OUTPUT_DIR" "${XCFRAMEWORKS[@]}"
  fi
fi

if $BUILD_MACOS_NODE_API; then
  checkpoint "Creating NativeScript.node for macOS"
  cp -r "$DIST/intermediates/macos/$CONFIG_BUILD/libNativeScript.dylib" "$DIST/NativeScript.node"
fi

if $BUILD_MACOS_CLI; then

checkpoint "Creating NativeScript CLI"

cp -r "$DIST/intermediates/macos/$CONFIG_BUILD/NativeScript" "$DIST/nsr"

fi

# rm -rf "$DIST/intermediates"
