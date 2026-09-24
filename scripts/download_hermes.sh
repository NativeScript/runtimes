#!/bin/bash
set -e
source "$(dirname "$0")/build_utils.sh"

# Installs the prebuilt Hermes both platforms share.
#
# Apple and Android link different artifacts -- an xcframework vs a set of
# per-ABI .so files -- but they come from the same release, so they are the same
# Hermes commit and share one set of headers. That is what lets napi/hermes be a
# single backend; do not bump one platform's artifact without the other.
#
# Nothing installed here is committed: the xcframework lands under /Frameworks/
# and the Android libs under a gitignored path, both fetched on demand.

HERMES_VERSION="build-d207b501aecb"
HERMES_FRAMEWORK_ASSET="hermes-xcframework.zip"
HERMES_HEADERS_ASSET="hermes-headers.tar.gz"
HERMES_ANDROID_ASSET="hermes-android.tar.gz"
HERMES_RELEASE_URL="https://github.com/ammarahm-ed/build-hermes/releases/download/$HERMES_VERSION"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
DL_DIR="${HERMES_DOWNLOAD_DIR:-/tmp/hermes-dl/$HERMES_VERSION}"

# Shared headers, used by both platforms.
HERMES_HEADERS_DIR="$REPO_ROOT/vendor/hermes/include"
# Android .so files, one set per build variant per ABI.
ANDROID_LIBS_DIR="$REPO_ROOT/platforms/android/test-app/runtime/src/main/libs/hermes"

function normalize_hermes_xcframework() {
    if [ ! -d "./Frameworks/hermes.xcframework" ]; then
        return
    fi

    checkpoint 'normalizing Hermes framework install names...'

    local ios_device_binary="./Frameworks/hermes.xcframework/ios-arm64/hermes.framework/hermes"
    local ios_sim_binary="./Frameworks/hermes.xcframework/ios-arm64_x86_64-simulator/hermes.framework/hermes"
    local macos_binary="./Frameworks/hermes.xcframework/macos-arm64_x86_64/hermes.framework/Versions/1/hermesvm"

    if [ -f "$ios_device_binary" ]; then
        install_name_tool -id "@rpath/hermes.framework/hermes" "$ios_device_binary"
    fi

    if [ -f "$ios_sim_binary" ]; then
        install_name_tool -id "@rpath/hermes.framework/hermes" "$ios_sim_binary"
    fi

    if [ -f "$macos_binary" ]; then
        install_name_tool -id "@rpath/hermes.framework/Versions/1/hermesvm" "$macos_binary"
    fi
}

function fetch_asset() {
    local asset="$1"
    if [ -f "$DL_DIR/$asset" ]; then
        return
    fi
    checkpoint "downloading $asset..."
    if ! curl -L --fail "$HERMES_RELEASE_URL/$asset" -o "$DL_DIR/$asset.part"; then
        rm -f "$DL_DIR/$asset.part"
        echo "error: could not download $asset from $HERMES_VERSION" >&2
        exit 1
    fi
    mv "$DL_DIR/$asset.part" "$DL_DIR/$asset"
}

# The header set is platform-neutral -- the Apple and Android archives are built
# from one Hermes tree -- so it is installed once and both CMakeLists point at
# it. It used to be committed twice (napi/hermes/include and
# napi/android/hermes/include); those copies drifted apart against the prebuilt
# binaries, which is the failure this replaces.
function install_headers() {
    checkpoint 'installing shared Hermes headers...'
    rm -rf "$HERMES_HEADERS_DIR"
    mkdir -p "$HERMES_HEADERS_DIR"
    # The archive has a hermes-headers/ prefix; strip it so the include root is
    # the directory itself.
    tar -xzf "$DL_DIR/$HERMES_HEADERS_ASSET" -C "$HERMES_HEADERS_DIR" --strip-components=1

    if [ ! -f "$HERMES_HEADERS_DIR/jsi/jsi.h" ]; then
        echo "error: jsi/jsi.h missing; release layout changed?" >&2
        exit 1
    fi
}

function install_apple() {
    checkpoint 'installing Hermes xcframework...'
    unzip -q -o "$DL_DIR/$HERMES_FRAMEWORK_ASSET" -d ./Frameworks
}

# hermes-android.tar.gz is laid out as hermes-android/<variant>/<abi>/*.so,
# which is exactly what the runtime's CMakeLists expects under libs/hermes.
function install_android() {
    checkpoint 'installing Android Hermes libraries...'
    rm -rf "$ANDROID_LIBS_DIR"
    mkdir -p "$ANDROID_LIBS_DIR"
    tar -xzf "$DL_DIR/$HERMES_ANDROID_ASSET" -C "$ANDROID_LIBS_DIR" --strip-components=1

    for variant in debug release; do
        if [ ! -f "$ANDROID_LIBS_DIR/$variant/arm64-v8a/libhermesvm.so" ]; then
            echo "error: $variant/arm64-v8a/libhermesvm.so missing; release layout changed?" >&2
            exit 1
        fi
    done
}

mkdir -p "$DL_DIR"

fetch_asset "$HERMES_HEADERS_ASSET"
install_headers

# Only fetch what the host can actually use: the xcframework is macOS-only, and
# a Linux CI box building the Android runtime should not need it.
if [ "$(uname -s)" = "Darwin" ]; then
    fetch_asset "$HERMES_FRAMEWORK_ASSET"
    install_apple
    normalize_hermes_xcframework
fi

fetch_asset "$HERMES_ANDROID_ASSET"
install_android

checkpoint "Hermes $HERMES_VERSION installed"
echo "  shared headers : $HERMES_HEADERS_DIR"
echo "  android libs   : $ANDROID_LIBS_DIR"
echo "  apple          : $REPO_ROOT/Frameworks/hermes.xcframework"
