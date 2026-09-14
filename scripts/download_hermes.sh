#!/bin/bash
set -e
source "$(dirname "$0")/build_utils.sh"

HERMES_VERSION="build-d4ee40ef0ede"
HERMES_FRAMEWORK_ASSET="hermes-xcframework.zip"
HERMES_HEADERS_ASSET="hermes-headers.tar.gz"
HERMES_FRAMEWORK_URL="https://github.com/DjDeveloperr/build-hermes/releases/download/$HERMES_VERSION/$HERMES_FRAMEWORK_ASSET"
HERMES_HEADERS_URL="https://github.com/DjDeveloperr/build-hermes/releases/download/$HERMES_VERSION/$HERMES_HEADERS_ASSET"

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

function download_hermes() {
    checkpoint "Downloading Hermes (version $HERMES_VERSION)..."

    mkdir -p /tmp/hermes-dl/

    curl -L "$HERMES_FRAMEWORK_URL" -o "/tmp/hermes-dl/$HERMES_FRAMEWORK_ASSET"
    curl -L "$HERMES_HEADERS_URL" -o "/tmp/hermes-dl/$HERMES_HEADERS_ASSET"

    checkpoint 'extracting Hermes...'
    unzip -o "/tmp/hermes-dl/$HERMES_FRAMEWORK_ASSET" -d ./Frameworks

    rm -rf ./Frameworks/hermes-headers
    tar -xzf "/tmp/hermes-dl/$HERMES_HEADERS_ASSET" -C ./Frameworks
}

if [ ! -d "./Frameworks/hermes.xcframework" ] || [ ! -d "./Frameworks/hermes-headers" ]; then
    download_hermes
fi

normalize_hermes_xcframework
