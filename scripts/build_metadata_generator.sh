#!/bin/bash
set -e
source "$(dirname "$0")/build_utils.sh"

function metadata_generator_source_hash {
    find src include CMakeLists.txt -type f -print | LC_ALL=C sort | xargs shasum | shasum | awk '{print $1}'
}

# Mirrors the libclang lookup in metadata-generator/CMakeLists.txt.
function libclang_has_arch {
    local clang_path libclang
    clang_path=$(xcrun --find clang 2>/dev/null || true)
    libclang="$(dirname "$(dirname "$clang_path")")/lib/libclang.dylib"
    [ -f "$libclang" ] || libclang="/Library/Developer/CommandLineTools/usr/lib/libclang.dylib"
    lipo -archs "$libclang" 2>/dev/null | tr ' ' '\n' | grep -qx "$1"
}

function build {
    rm -rf build
    mkdir build
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DMETADATA_BINARY_ARCH=$1 -DCMAKE_OSX_ARCHITECTURES=$1
    cmake --build build --target clean
    cmake --build build -j$NUMJOBS
    mkdir "dist/$1"
    cp -r "build/bin" "dist/$1"
}

pushd "metadata-generator"
rm -rf dist
mkdir dist
# Xcode 27+ ships an arm64-only libclang, so an x86_64 generator cannot be linked there.
if libclang_has_arch x86_64; then
    checkpoint "Building metadata generator for x86_64 ..."
    build "x86_64"
    # make sure the binary is linked against the system libc++ instead of an @rpath one (which happens when compiling on arm64)
    # todo: perhaps there is a better way to do this with cmake?
    #install_name_tool -change @rpath/libc++.1.dylib /usr/lib/libc++.1.dylib dist/x86_64/bin/objc-metadata-generator
    otool -L  dist/x86_64/bin/objc-metadata-generator
else
    echo "warning: skipping x86_64 metadata generator: the active toolchain's libclang has no x86_64 slice" >&2
fi

checkpoint "Building metadata generator for arm64 ..."
build "arm64"
otool -L  dist/arm64/bin/objc-metadata-generator
metadata_generator_source_hash > dist/.source_hash
rm -rf build
popd
