#!/bin/bash
set -e
source "$(dirname "$0")/build_utils.sh"

BUILD_CATALYST=$(to_bool ${BUILD_CATALYST:=true})
BUILD_IPHONE=$(to_bool ${BUILD_IPHONE:=true})
BUILD_SIMULATOR=$(to_bool ${BUILD_SIMULATOR:=true})
BUILD_VISION=$(to_bool ${BUILD_VISION:=false})
VERBOSE=$(to_bool ${VERBOSE:=false})
BUILD_MACOS=$(to_bool ${BUILD_MACOS:=false})

for arg in $@; do
  case $arg in
    --catalyst|--maccatalyst) BUILD_CATALYST=true ;;
    --no-catalyst|--no-maccatalyst) BUILD_CATALYST=false ;;
    --sim|--simulator) BUILD_SIMULATOR=true ;;
    --no-sim|--no-simulator) BUILD_SIMULATOR=false ;;
    --iphone|--device) BUILD_IPHONE=true ;;
    --no-iphone|--no-device) BUILD_IPHONE=false ;;
    --xr|--vision) BUILD_VISION=true ;;
    --no-xr|--no-vision) BUILD_VISION=false ;;
    --macos) BUILD_MACOS=true ;;
    --no-macos) BUILD_MACOS=false ;;
    --verbose|-v) VERBOSE=true ;;
    *) ;;
  esac
done

DIST=$(PWD)/dist
mkdir -p $DIST

mkdir -p $DIST/intermediates

if $BUILD_SIMULATOR; then
# generates library for simulator targets (usually includes arm64, x86_64)
checkpoint "Building TKLiveSync for iphone simulators (multi-arch)"
xcodebuild archive -project platforms/apple/NativeScriptRuntime.xcodeproj \
                   -scheme TKLiveSync \
                   -configuration Release \
                   -destination "generic/platform=iOS Simulator" \
                   -quiet \
                   SKIP_INSTALL=NO \
                   BUILD_LIBRARY_FOR_DISTRIBUTION=YES \
                   -archivePath $DIST/intermediates/TKLiveSync.iphonesimulator.xcarchive
fi

if $BUILD_IPHONE; then
#generates library for device target
checkpoint "Building TKLiveSync for ARM64 device"
xcodebuild archive -project platforms/apple/NativeScriptRuntime.xcodeproj \
                   -scheme TKLiveSync \
                   -configuration Release \
                   -destination "generic/platform=iOS" \
                   -quiet \
                   SKIP_INSTALL=NO \
                   BUILD_LIBRARY_FOR_DISTRIBUTION=YES \
                   -archivePath $DIST/intermediates/TKLiveSync.iphoneos.xcarchive
fi

if $BUILD_CATALYST; then
#generates library for Mac Catalyst target
checkpoint "Building TKLiveSync for Mac Catalyst"
xcodebuild archive -project platforms/apple/NativeScriptRuntime.xcodeproj \
                   -scheme TKLiveSync \
                   -configuration Release \
                   -destination "generic/platform=macOS,variant=Mac Catalyst" \
                   -quiet \
                   SKIP_INSTALL=NO \
                   BUILD_LIBRARY_FOR_DISTRIBUTION=YES \
                   -archivePath $DIST/intermediates/TKLiveSync.maccatalyst.xcarchive
fi

if $BUILD_MACOS; then
#generates library for Mac OS target
checkpoint "Building TKLiveSync for Mac OS"
xcodebuild archive -project platforms/apple/NativeScriptRuntime.xcodeproj \
                   -scheme TKLiveSync \
                   -configuration Release \
                   -destination "generic/platform=macOS" \
                   -quiet \
                   SKIP_INSTALL=NO \
                   BUILD_LIBRARY_FOR_DISTRIBUTION=YES \
                   -archivePath $DIST/intermediates/TKLiveSync.macos.xcarchive
fi

if $BUILD_VISION; then
# generates frameworks for visionOS without going through archive destination
# resolution, which falls back to iOS/macOS on this project.
checkpoint "Building TKLiveSync for visionOS Simulators"
xcodebuild build -project platforms/apple/NativeScriptRuntime.xcodeproj \
                 -target TKLiveSync \
                 -configuration Release \
                 -sdk xrsimulator \
                 -quiet \
                 BUILD_LIBRARY_FOR_DISTRIBUTION=YES \
                 ONLY_ACTIVE_ARCH=YES \
                 CONFIGURATION_BUILD_DIR="$DIST/intermediates/TKLiveSync.xrsimulator"

checkpoint "Building TKLiveSync for visionOS Device"
xcodebuild build -project platforms/apple/NativeScriptRuntime.xcodeproj \
                 -target TKLiveSync \
                 -configuration Release \
                 -sdk xros \
                 -quiet \
                 BUILD_LIBRARY_FOR_DISTRIBUTION=YES \
                 ONLY_ACTIVE_ARCH=YES \
                 CONFIGURATION_BUILD_DIR="$DIST/intermediates/TKLiveSync.xros"
fi

#Creates directory for fat-library
OUTPUT_DIR="$DIST/TKLiveSync.xcframework"
rm -rf "${OUTPUT_PATH}"

#Create fat library for simulator
# rm -rf "$DIST/TKLiveSync.iphonesimulator.xcarchive"

# cp -r \
    # "$DIST/TKLiveSync.x86_64-iphonesimulator.xcarchive/." \
    # "$DIST/TKLiveSync.iphonesimulator.xcarchive"

# rm "$DIST/TKLiveSync.iphonesimulator.xcarchive/Products/Library/Frameworks/TKLiveSync.framework/TKLiveSync"

# lipo -create \
#     "$DIST/TKLiveSync.x86_64-iphonesimulator.xcarchive/Products/Library/Frameworks/TKLiveSync.framework/TKLiveSync" \
#     "$DIST/TKLiveSync.arm64-iphonesimulator.xcarchive/Products/Library/Frameworks/TKLiveSync.framework/TKLiveSync" \
#     -output \
#     "$DIST/TKLiveSync.iphonesimulator.xcarchive/Products/Library/Frameworks/TKLiveSync.framework/TKLiveSync"

#Creates xcframework
XCFRAMEWORKS=()
if $BUILD_CATALYST; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/TKLiveSync.maccatalyst.xcarchive/Products/Library/Frameworks/TKLiveSync.framework" \
                  -debug-symbols "$DIST/intermediates/TKLiveSync.maccatalyst.xcarchive/dSYMs/TKLiveSync.framework.dSYM" )
fi

if $BUILD_SIMULATOR; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/TKLiveSync.iphonesimulator.xcarchive/Products/Library/Frameworks/TKLiveSync.framework" \
                  -debug-symbols "$DIST/intermediates/TKLiveSync.iphonesimulator.xcarchive/dSYMs/TKLiveSync.framework.dSYM" )
fi

if $BUILD_IPHONE; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/TKLiveSync.iphoneos.xcarchive/Products/Library/Frameworks/TKLiveSync.framework" \
                  -debug-symbols "$DIST/intermediates/TKLiveSync.iphoneos.xcarchive/dSYMs/TKLiveSync.framework.dSYM" )
fi

if $BUILD_MACOS; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/TKLiveSync.macos.xcarchive/Products/Library/Frameworks/TKLiveSync.framework" \
                  -debug-symbols "$DIST/intermediates/TKLiveSync.macos.xcarchive/dSYMs/TKLiveSync.framework.dSYM" )
fi

if $BUILD_VISION; then
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/TKLiveSync.xros/TKLiveSync.framework" \
                  -debug-symbols "$DIST/intermediates/TKLiveSync.xros/TKLiveSync.framework.dSYM" )
  XCFRAMEWORKS+=( -framework "$DIST/intermediates/TKLiveSync.xrsimulator/TKLiveSync.framework" \
                  -debug-symbols "$DIST/intermediates/TKLiveSync.xrsimulator/TKLiveSync.framework.dSYM" )
fi

checkpoint "Creating TKLiveSync.xcframework"
OUTPUT_DIR="$DIST/TKLiveSync.xcframework"
rm -rf $OUTPUT_DIR
echo xcodebuild -create-xcframework ${XCFRAMEWORKS[@]} -output "$OUTPUT_DIR"
xcodebuild -create-xcframework ${XCFRAMEWORKS[@]} -output "$OUTPUT_DIR"

rm -rf "$DIST/intermediates"

# rm -rf "$DIST/TKLiveSync.maccatalyst.xcarchive"
# rm -rf "$DIST/TKLiveSync.x86_64-iphonesimulator.xcarchive"
# rm -rf "$DIST/TKLiveSync.arm64-iphonesimulator.xcarchive"
# rm -rf "$DIST/TKLiveSync.iphonesimulator.xcarchive"
# rm -rf "$DIST/TKLiveSync.iphoneos.xcarchive"
