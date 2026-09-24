#!/bin/bash
set -e
source "$(dirname "$0")/build_utils.sh"

# Downloads the prebuilt V8 both platforms share and unpacks it into the repo.
#
# Nothing this writes is committed. The Apple xcframework lands under
# /Frameworks/, which is gitignored, and build_nativescript.sh fetches it on
# demand when it is missing. The Android libs and the shared headers are written
# the same way; whether any of it should be tracked instead is still open --
# the Android static libs alone are ~380 MB.
#
# The whole point of pinning one release is that every slice is built from the
# same V8 commit, so include/ is byte-identical across platforms and a single
# napi/v8 backend can compile for both. Do not mix slices from two releases.

# The fork rather than NativeScript/v8-buildscripts: upstream's matrix has no
# macOS slices, and the macOS runtime needs one built from the same V8 commit as
# the iOS and Android slices.
V8_VERSION="v8-14.9.207.39-5"
V8_NUMERIC_VERSION="14.9.207.39"
V8_RELEASE_URL="https://github.com/ammarahm-ed/v8-buildscripts/releases/download/$V8_VERSION"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
DL_DIR="${V8_DOWNLOAD_DIR:-/tmp/v8-dl/$V8_VERSION}"

# Shared, engine-version-pinned headers used by both platforms.
VENDOR_DIR="$REPO_ROOT/vendor/v8"
# Android static libs, one per ABI.
ANDROID_LIBS_DIR="$REPO_ROOT/platforms/android/test-app/runtime/src/main/libs/v8"
# Apple xcframework.
APPLE_XCFRAMEWORK="$REPO_ROOT/Frameworks/libv8_monolith.xcframework"

# <android abi>:<release slice>
ANDROID_SLICES=(
    "arm64-v8a:android-arm64-v8a"
    "armeabi-v7a:android-armeabi-v7a"
    "x86:android-x86"
    "x86_64:android-x86_64"
)

# Apple slices that are genuinely distinct platforms, so xcodebuild can tell
# them apart:  <xcframework slice>:<release slice>
APPLE_XC_SLICES=(
    "ios-arm64:ios-arm64-device"
    "ios-arm64-simulator:ios-arm64-simulator"
    "macos-arm64:macos-arm64"
)

# visionOS has no V8 build of its own and needs none: V8's target_platform
# accepts only iphoneos/tvos, and the platform tag on a member of a *static*
# archive is advisory -- only the final linked image carries LC_BUILD_VERSION.
# So xros links the iOS archives verbatim, as v8-buildscripts' README
# prescribes. They cannot be passed to -create-xcframework (it rejects two
# slices with the same platform tag as duplicates), so they are copied in
# afterwards:  <xros slice>:<xcframework slice to copy>
APPLE_XROS_SLICES=(
    "xros-arm64:ios-arm64"
    "xrsimulator-arm64:ios-arm64-simulator"
)

function fetch_asset() {
    local asset="$1"
    if [ -f "$DL_DIR/$asset" ]; then
        return
    fi
    checkpoint "downloading $asset..."
    if ! curl -L --fail "$V8_RELEASE_URL/$asset" -o "$DL_DIR/$asset.part"; then
        rm -f "$DL_DIR/$asset.part"
        return 1
    fi
    mv "$DL_DIR/$asset.part" "$DL_DIR/$asset"
}

# The release publishes SHA256SUMS; a truncated or MITM'd archive would
# otherwise surface much later as a confusing link error.
function verify_checksums() {
    checkpoint 'verifying checksums...'
    (
        cd "$DL_DIR"
        # Only check the assets we actually downloaded.
        local present=()
        while read -r _ name; do
            [ -f "$name" ] && present+=("$name")
        done < SHA256SUMS
        grep -F -f <(printf '%s\n' "${present[@]}") SHA256SUMS | shasum -a 256 -c -
    )
}

function download_v8() {
    checkpoint "downloading V8 $V8_NUMERIC_VERSION ($V8_VERSION)..."
    mkdir -p "$DL_DIR"

    curl -L --fail "$V8_RELEASE_URL/SHA256SUMS" -o "$DL_DIR/SHA256SUMS"
    for entry in "${ANDROID_SLICES[@]}"; do
        fetch_asset "v8-$V8_NUMERIC_VERSION-${entry##*:}.tar.gz"
    done
    for entry in "${APPLE_XC_SLICES[@]}"; do
        # Tolerate a slice the pinned release does not carry yet; install_apple
        # reports what was skipped.
        fetch_asset "v8-$V8_NUMERIC_VERSION-${entry##*:}.tar.gz" || true
    done
    fetch_asset "v8-$V8_NUMERIC_VERSION-src-headers.tar.gz"

    verify_checksums
}

# include/ is identical in every slice (same build), so take it from one and
# share it. It is V8's public API and self-contained -- no header in it includes
# anything from src/ or third_party/.
#
# src/ and third_party/ come from src-headers.tar.gz: V8's internal headers,
# which the v8_inspector sources need. Both used to be a hand-picked subset
# committed once per platform under napi/*/v8_inspector. Those copies were cut
# from V8 13 and never refreshed, so they went stale the moment both platforms
# moved to 14.9 -- curating ~150 of 2400 headers by hand is not a maintainable
# way to track an engine. Fetching the whole set costs 5.4 MB over the wire and
# nothing in git.
function install_shared_headers() {
    checkpoint 'installing shared V8 headers...'
    local first_slice="${ANDROID_SLICES[0]##*:}"

    # Only the fetched subtrees are replaced. vendor/v8 also holds the
    # NativeScript-authored napi binding, which must survive a re-fetch --
    # this script installs headers and binaries, nothing else.
    rm -rf "$VENDOR_DIR/include" "$VENDOR_DIR/src" "$VENDOR_DIR/third_party"
    mkdir -p "$VENDOR_DIR"

    # strip only the slice dir, so include/ is preserved as vendor/v8/include
    tar -xzf "$DL_DIR/v8-$V8_NUMERIC_VERSION-$first_slice.tar.gz" \
        -C "$VENDOR_DIR" --strip-components=1 "$first_slice/include"

    # src-headers/ holds src/ and third_party/ at its root; the same strip puts
    # them beside include/, which is the layout the inspector sources expect
    # (they spell paths relative to the V8 checkout root).
    #
    # The release packages only the transitive include closure of the V8
    # internals the inspector glue reaches -- 66 headers, not the 2223 in a
    # checkout -- so the whole archive is installed as-is. If a new V8 internal
    # header is needed, add it to ENTRY_POINTS in v8-buildscripts'
    # scripts/matrix/package-src-headers.sh and cut a release; everything below
    # it comes along automatically.
    tar -xzf "$DL_DIR/v8-$V8_NUMERIC_VERSION-src-headers.tar.gz" \
        -C "$VENDOR_DIR" --strip-components=1 \
        "src-headers/src" \
        "src-headers/third_party"

    echo "$V8_NUMERIC_VERSION" > "$VENDOR_DIR/V8_VERSION"

    # Fail loudly if the release layout ever changes rather than leaving a
    # half-populated tree for the compiler to trip over.
    if [ ! -f "$VENDOR_DIR/include/v8-version.h" ]; then
        echo "error: include/v8-version.h missing; release layout changed?" >&2
        exit 1
    fi
    local got
    got=$(awk '/#define V8_MAJOR_VERSION/{maj=$3} /#define V8_MINOR_VERSION/{min=$3} END{print maj"."min}' \
        "$VENDOR_DIR/include/v8-version.h")
    checkpoint "shared headers installed (V8 $got)"
}

function install_android() {
    checkpoint 'installing Android V8 libraries...'
    rm -rf "$ANDROID_LIBS_DIR"
    for entry in "${ANDROID_SLICES[@]}"; do
        local abi="${entry%%:*}"
        local release_slice="${entry##*:}"
        mkdir -p "$ANDROID_LIBS_DIR/$abi"
        tar -xzf "$DL_DIR/v8-$V8_NUMERIC_VERSION-$release_slice.tar.gz" \
            -C "$ANDROID_LIBS_DIR/$abi" --strip-components=2 \
            "$release_slice/lib/libv8_monolith.a"
    done
}

function make_static_framework() {
    local dir="$1" fw="$2"
    # NativeScript/CMakeLists.txt imports the slice as
    #   <slice>/libv8_monolith.framework/{libv8_monolith,Headers}
    # i.e. a static framework whose binary has no extension. xcodebuild's
    # -library flag produces a bare .a + Headers instead, so assemble the
    # framework ourselves and pass -framework.
    rm -rf "$fw"
    mkdir -p "$fw/Headers"
    cp "$dir/libv8_monolith.a" "$fw/libv8_monolith"
    rsync -a "$dir/include/" "$fw/Headers/"
    cat > "$fw/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>libv8_monolith</string>
    <key>CFBundleIdentifier</key><string>org.nativescript.libv8monolith</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>libv8_monolith</string>
    <key>CFBundlePackageType</key><string>FMWK</string>
    <key>CFBundleShortVersionString</key><string>$V8_NUMERIC_VERSION</string>
    <key>CFBundleVersion</key><string>$V8_NUMERIC_VERSION</string>
</dict>
</plist>
PLIST
}

function install_apple() {
    checkpoint 'building libv8_monolith.xcframework...'
    local staging="$DL_DIR/apple-staging"
    rm -rf "$staging" "$APPLE_XCFRAMEWORK"
    mkdir -p "$staging"

    local args=() missing=()
    # Only the genuinely distinct platforms go through -create-xcframework;
    # xros reuses the iOS archives and is copied in afterwards.
    for entry in "${APPLE_XC_SLICES[@]}"; do
        local xc_slice="${entry%%:*}"
        local release_slice="${entry##*:}"
        local archive="$DL_DIR/v8-$V8_NUMERIC_VERSION-$release_slice.tar.gz"
        if [ ! -f "$archive" ]; then
            missing+=("$xc_slice ($release_slice)")
            continue
        fi
        local dir="$staging/$xc_slice"
        mkdir -p "$dir"
        tar -xzf "$archive" -C "$dir" --strip-components=1 \
            "$release_slice/lib" "$release_slice/include"

        # The Android slices ship a prebuilt libv8_monolith.a but the Apple ones
        # ship V8's ~17 component archives instead. Merge them so both platforms
        # (and the v8::monolith CMake target) see the same single library.
        # -no_warning_for_no_symbols silences the empty archive members V8's
        # build produces for header-only targets.
        libtool -static -no_warning_for_no_symbols \
            -o "$dir/libv8_monolith.a" "$dir"/lib/*.a
        make_static_framework "$dir" "$dir/libv8_monolith.framework"
        args+=(-framework "$dir/libv8_monolith.framework")
    done

    if [ ${#missing[@]} -gt 0 ]; then
        echo "warning: release $V8_VERSION has no asset for: ${missing[*]}" >&2
        echo "         those platforms will fail at configure time with" >&2
        echo "         'Missing V8 slice for target platform'." >&2
    fi
    [ ${#args[@]} -gt 0 ] || { echo "error: no Apple slices available" >&2; exit 1; }

    mkdir -p "$(dirname "$APPLE_XCFRAMEWORK")"
    xcodebuild -create-xcframework "${args[@]}" -output "$APPLE_XCFRAMEWORK"

    # visionOS: xcodebuild would reject these as duplicates of the iOS slices
    # (same platform tag), so copy the directories in after the fact -- which is
    # exactly what v8-buildscripts' README prescribes for consumers. The
    # CMakeLists addresses slices by path and never reads Info.plist, so the
    # copies are picked up as-is.
    for entry in "${APPLE_XROS_SLICES[@]}"; do
        local xros_slice="${entry%%:*}"
        local from_slice="${entry##*:}"
        if [ -d "$APPLE_XCFRAMEWORK/$from_slice" ]; then
            rm -rf "$APPLE_XCFRAMEWORK/$xros_slice"
            cp -R "$APPLE_XCFRAMEWORK/$from_slice" "$APPLE_XCFRAMEWORK/$xros_slice"
        fi
    done
}

download_v8
install_shared_headers
install_android
if [ "$(uname -s)" = "Darwin" ]; then
    install_apple
else
    checkpoint 'skipping xcframework (not macOS)'
fi

checkpoint "V8 $V8_NUMERIC_VERSION installed"
echo "  shared headers : $VENDOR_DIR"
echo "  android libs   : $ANDROID_LIBS_DIR"
echo "  apple          : $APPLE_XCFRAMEWORK"
