#!/bin/bash
set -e
source "$(dirname "$0")/build_utils.sh"

checkpoint "Preparing npm package for visionOS..."
PACKAGE_DIR="packages/visionos"
OUTPUT_DIR="$PACKAGE_DIR/dist"
STAGING_DIR="$OUTPUT_DIR/package"

rm -rf "$OUTPUT_DIR"
mkdir -p "$STAGING_DIR/framework/internal"
cp "$PACKAGE_DIR/package.json" "$STAGING_DIR"
cp "$PACKAGE_DIR/README.md" "$STAGING_DIR"
cp "$PACKAGE_DIR/LICENSE" "$STAGING_DIR"

cp -R "./platforms/apple/templates/visionos/." "$STAGING_DIR/framework"

cp -R "dist/NativeScript.xcframework" "$STAGING_DIR/framework/internal"
cp -R "dist/TKLiveSync.xcframework" "$STAGING_DIR/framework/internal"

mkdir -p "$STAGING_DIR/framework/internal/metadata-generator-x86_64"
cp -R "metadata-generator/dist/x86_64/." "$STAGING_DIR/framework/internal/metadata-generator-x86_64"

mkdir -p "$STAGING_DIR/framework/internal/metadata-generator-arm64"
cp -R "metadata-generator/dist/arm64/." "$STAGING_DIR/framework/internal/metadata-generator-arm64"

# Add xcframeworks to .zip (NPM modules do not support symlinks, unzipping is done by {N} CLI)
(
    set -e
    cd "$STAGING_DIR/framework/internal"
    zip -qr --symlinks XCFrameworks.zip *.xcframework
    rm -rf *.xcframework
)

pushd "$STAGING_DIR"
npm pack --pack-destination ..
popd

checkpoint "npm package created."
