#!/bin/bash
set -e
source "$(dirname "$0")/build_utils.sh"

IOS_VARIANT=${IOS_VARIANT:=ios}

checkpoint "Preparing npm package for $IOS_VARIANT..."
PACKAGE_DIR="packages/$IOS_VARIANT"
if [ ! -f "$PACKAGE_DIR/package.json" ]; then
  echo "Expected package directory '$PACKAGE_DIR' (IOS_VARIANT=$IOS_VARIANT) to contain a package.json." >&2
  exit 1
fi
OUTPUT_DIR="$PACKAGE_DIR/dist"
STAGING_DIR="$OUTPUT_DIR/package"
PACKAGE_NAME_OVERRIDE=${NPM_PACKAGE_NAME:-}
PACKAGE_VERSION_OVERRIDE=${NPM_PACKAGE_VERSION:-}
PACK_DESTINATION=${NPM_PACK_DESTINATION:-..}

rm -rf "$OUTPUT_DIR"
mkdir -p "$STAGING_DIR/framework/internal"
mkdir -p "$PACK_DESTINATION"
cp "$PACKAGE_DIR/package.json" "$STAGING_DIR"
cp "$PACKAGE_DIR/README.md" "$STAGING_DIR"
cp "$PACKAGE_DIR/LICENSE" "$STAGING_DIR"

if [ -n "$PACKAGE_NAME_OVERRIDE" ] || [ -n "$PACKAGE_VERSION_OVERRIDE" ]; then
    TMP_FILE=$(mktemp)
    jq \
        --arg name "$PACKAGE_NAME_OVERRIDE" \
        --arg version "$PACKAGE_VERSION_OVERRIDE" \
        'if $name != "" then .name = $name else . end | if $version != "" then .version = $version else . end' \
        "$STAGING_DIR/package.json" > "$TMP_FILE"
    mv "$TMP_FILE" "$STAGING_DIR/package.json"
fi

cp -R "./platforms/apple/templates/ios/." "$STAGING_DIR/framework"

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
npm pack --pack-destination "$PACK_DESTINATION"
popd

checkpoint "npm package created."
