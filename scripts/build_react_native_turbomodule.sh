#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/build_utils.sh"

PACKAGE_DIR="packages/react-native"
OUTPUT_DIR="$PACKAGE_DIR/dist"
PACK_DESTINATION=${NPM_PACK_DESTINATION:-"$REPO_ROOT/build/npm-tarballs"}
VERSION_OVERRIDE=${NPM_PACKAGE_VERSION:-}
GENERATED_SIGNATURE_DISPATCH_OVERRIDE=${NS_SIGNATURE_BINDINGS_CPP_PATH:-${TNS_SIGNATURE_BINDINGS_CPP_PATH:-}}
GENERATED_SIGNATURE_DISPATCH=${GENERATED_SIGNATURE_DISPATCH_OVERRIDE:-"$REPO_ROOT/dist/intermediates/react-native/GeneratedSignatureDispatch.ios-sim.tmp.inc"}
DEVICE_SIGNATURE_DISPATCH="$REPO_ROOT/dist/intermediates/react-native/GeneratedSignatureDispatch.ios-device.tmp.inc"
SKIP_PACK=false

function metadata_generator_source_hash {
  find "$REPO_ROOT/metadata-generator/src" "$REPO_ROOT/metadata-generator/include" "$REPO_ROOT/metadata-generator/CMakeLists.txt" \
    -type f -print | LC_ALL=C sort | xargs shasum | shasum | awk '{print $1}'
}

function ensure_metadata_generator {
  local expected_hash
  expected_hash=$(metadata_generator_source_hash)
  local hash_file="$REPO_ROOT/metadata-generator/dist/.source_hash"
  if [ ! -x "$REPO_ROOT/metadata-generator/dist/arm64/bin/objc-metadata-generator" ] || \
     [ ! -x "$REPO_ROOT/metadata-generator/dist/x86_64/bin/objc-metadata-generator" ] || \
     [ ! -f "$hash_file" ] || \
     [ "$(cat "$hash_file")" != "$expected_hash" ]; then
    "$SCRIPT_DIR/build_metadata_generator.sh"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-pack)
      SKIP_PACK=true
      shift
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

checkpoint "Preparing @nativescript/react-native TurboModule package..."

ensure_metadata_generator

checkpoint "Generating iOS device metadata for the TurboModule..."
mkdir -p "$(dirname "$DEVICE_SIGNATURE_DISPATCH")"
NS_SIGNATURE_BINDINGS_CPP_PATH="$DEVICE_SIGNATURE_DISPATCH" npm run metagen ios
rm -f "$DEVICE_SIGNATURE_DISPATCH" "$DEVICE_SIGNATURE_DISPATCH.stamp"

checkpoint "Generating Hermes signature dispatch bindings for the TurboModule..."
mkdir -p "$(dirname "$GENERATED_SIGNATURE_DISPATCH")"
NS_SIGNATURE_BINDINGS_CPP_PATH="$GENERATED_SIGNATURE_DISPATCH" npm run metagen ios-sim

# Only the trees this script stages. It used to wipe native-api/ whole, which
# also deleted what build_react_native_android.sh had put there
# (native-api/runtime/android, native-api/ffi/jni, native-api/napi/common) --
# so whichever of the two ran last was the only half the tarball carried.
rm -rf \
  "$PACKAGE_DIR/native-api/ffi/objc" \
  "$PACKAGE_DIR/native-api/jsi" \
  "$PACKAGE_DIR/native-api/metadata" \
  "$PACKAGE_DIR/metadata" \
  "$PACKAGE_DIR/ios/vendor" \
  "$PACKAGE_DIR/types"
mkdir -p \
  "$PACKAGE_DIR/native-api/ffi/objc/hermes" \
  "$PACKAGE_DIR/native-api/ffi/objc/shared" \
  "$PACKAGE_DIR/native-api/ffi/objc/shared/bridge" \
  "$PACKAGE_DIR/native-api/jsi" \
  "$PACKAGE_DIR/native-api/metadata/include" \
  "$PACKAGE_DIR/metadata" \
  "$PACKAGE_DIR/ios/vendor/libffi/include" \
  "$PACKAGE_DIR/types/ios" \
  "$PACKAGE_DIR/types/objc-node-api" \
  "$PACK_DESTINATION"

# The platform-neutral engine layer. The Apple headers under ffi/objc are thin
# forwarders onto it -- NativeApiJsi.h is a one-line #include of
# "jsi/hermes/NativeApiJsi.h" -- so the pod does not compile without this tree,
# even though it compiles nothing out of it directly. Header-only as far as the
# TurboModule is concerned; it is on the pod's header search path via
# native-api. Staged here as well as by the Android step: either half of the
# package must be able to build from a tarball the other never contributed to.
cp -R "$REPO_ROOT/NativeScript/jsi/." "$PACKAGE_DIR/native-api/jsi/"

# The Apple side of the bridge, staged whole rather than file by file.
#
# Only NativeApiJsi.mm is compiled: everything else arrives through #include,
# either as a header or as a .mm folded into that translation unit (see
# HostObjects.mm, which assembles host_objects/*.mm). An enumerated list has to
# name every one of those transitively, so it silently goes stale whenever the
# sources grow a file or a directory -- which is what happened here. Copying the
# trees keeps the staged set equal to the source set by construction.
cp -R "$REPO_ROOT/NativeScript/ffi/objc/hermes/." "$PACKAGE_DIR/native-api/ffi/objc/hermes/"
cp -R "$REPO_ROOT/NativeScript/ffi/objc/shared/." "$PACKAGE_DIR/native-api/ffi/objc/shared/"
# Build leftovers, not sources.
find "$PACKAGE_DIR/native-api/ffi/objc" -name '*.stamp' -delete
cp "$GENERATED_SIGNATURE_DISPATCH" "$PACKAGE_DIR/native-api/ffi/objc/hermes/GeneratedSignatureDispatch.inc"
GENERATED_GSD_SIGNATURE_DISPATCH="$(dirname "$GENERATED_SIGNATURE_DISPATCH")/GeneratedGsdSignatureDispatch.inc"
if [ -f "$GENERATED_GSD_SIGNATURE_DISPATCH" ]; then
  cp "$GENERATED_GSD_SIGNATURE_DISPATCH" "$PACKAGE_DIR/native-api/ffi/objc/hermes/GeneratedGsdSignatureDispatch.inc"
fi
if [ -z "$GENERATED_SIGNATURE_DISPATCH_OVERRIDE" ]; then
  rm -f "$GENERATED_SIGNATURE_DISPATCH" "$GENERATED_SIGNATURE_DISPATCH.stamp" \
    "$GENERATED_GSD_SIGNATURE_DISPATCH"
fi
cp metadata-generator/include/Metadata.h "$PACKAGE_DIR/native-api/metadata/include/"
cp metadata-generator/include/MetadataReader.h "$PACKAGE_DIR/native-api/metadata/include/"
cp NativeScript/libffi/iphonesimulator-universal/include/ffi.h "$PACKAGE_DIR/ios/vendor/libffi/include/"
cp NativeScript/libffi/iphonesimulator-universal/include/ffitarget.h "$PACKAGE_DIR/ios/vendor/libffi/include/"

cp metadata-generator/metadata/metadata.ios-sim.arm64.nsmd "$PACKAGE_DIR/metadata/"
cp metadata-generator/metadata/metadata.ios-sim.x86_64.nsmd "$PACKAGE_DIR/metadata/"
cp metadata-generator/metadata/metadata.ios.arm64.nsmd "$PACKAGE_DIR/metadata/"

checkpoint "Staging iOS SDK TypeScript declarations..."
cp packages/objc-node-api/index.d.ts "$PACKAGE_DIR/types/objc-node-api/"
cp packages/objc-node-api/inline_functions.d.ts "$PACKAGE_DIR/types/objc-node-api/"
cp packages/ios/types/*.d.ts "$PACKAGE_DIR/types/ios/"
perl -0pi -e 's#/// <reference types="\@nativescript/objc-node-api" />#/// <reference path="../objc-node-api/index.d.ts" />#g' \
  "$PACKAGE_DIR"/types/ios/*.d.ts
node - "$PACKAGE_DIR/types/ios" <<'NODE'
const fs = require('fs');
const path = require('path');

const typesDir = process.argv[2];
const reservedParameterNames = [
  'abstract',
  'as',
  'asserts',
  'async',
  'await',
  'break',
  'case',
  'catch',
  'class',
  'const',
  'continue',
  'debugger',
  'declare',
  'default',
  'delete',
  'do',
  'else',
  'enum',
  'export',
  'extends',
  'false',
  'finally',
  'for',
  'from',
  'function',
  'if',
  'implements',
  'import',
  'in',
  'infer',
  'instanceof',
  'interface',
  'is',
  'keyof',
  'let',
  'module',
  'namespace',
  'never',
  'new',
  'null',
  'of',
  'package',
  'private',
  'protected',
  'public',
  'readonly',
  'require',
  'return',
  'satisfies',
  'static',
  'super',
  'switch',
  'throw',
  'true',
  'try',
  'type',
  'typeof',
  'undefined',
  'unique',
  'unknown',
  'var',
  'void',
  'while',
  'with',
  'yield',
];
const reservedPattern = new RegExp(
  `([,(]\\s*)(${reservedParameterNames.join('|')})(\\??\\s*:)`,
  'g',
);

for (const entry of fs.readdirSync(typesDir)) {
  if (!entry.endsWith('.d.ts')) {
    continue;
  }

  const file = path.join(typesDir, entry);
  let source = fs.readFileSync(file, 'utf8');
  source = source.replace(reservedPattern, '$1_$2$3');
  source = source.replace(/^[ \t]*:[^;\n]+;[ \t]*\n/gm, '');
  fs.writeFileSync(file, source);
}
NODE
{
  echo '/// <reference path="../objc-node-api/index.d.ts" />'
  while IFS= read -r declaration; do
    echo "/// <reference path=\"./$declaration\" />"
  done < <(find "$PACKAGE_DIR/types/ios" -maxdepth 1 -name '*.d.ts' ! -name 'index.d.ts' -exec basename {} \; | sort)
} > "$PACKAGE_DIR/types/ios/index.d.ts"

checkpoint "Creating Libffi.xcframework for the TurboModule pod..."
xcodebuild -create-xcframework \
  -library NativeScript/libffi/iphoneos-arm64/libffi.a \
  -headers NativeScript/libffi/iphoneos-arm64/include \
  -library NativeScript/libffi/iphonesimulator-universal/libffi.a \
  -headers NativeScript/libffi/iphonesimulator-universal/include \
  -output "$PACKAGE_DIR/ios/vendor/Libffi.xcframework"

if [[ -n "$VERSION_OVERRIDE" ]]; then
  TMP_FILE=$(mktemp)
  jq --arg version "$VERSION_OVERRIDE" '.version = $version' \
    "$PACKAGE_DIR/package.json" > "$TMP_FILE"
  mv "$TMP_FILE" "$PACKAGE_DIR/package.json"
fi

if [[ "$SKIP_PACK" == "true" ]]; then
  checkpoint "@nativescript/react-native package staged."
  exit 0
fi

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# The package ships an iOS half and an Android half, and this script only stages
# the iOS one. The Android half is generated by build_react_native_android.sh and
# build_react_native_android_tools.sh, and both of its trees are gitignored -- so
# a checkout that never ran them packs an android/ directory containing gradle
# files and nothing to build. That publishes cleanly and fails in every consuming
# app, which is why it is a hard error here rather than a warning.
missing=()
[ -d "$PACKAGE_DIR/android/src/main/java-runtime" ] || missing+=("android/src/main/java-runtime (scripts/build_react_native_android.sh)")
[ -d "$PACKAGE_DIR/native-api/runtime/android" ] || missing+=("native-api/runtime/android (scripts/build_react_native_android.sh)")
for jar in android-metadata-generator dts-generator static-binding-generator; do
  [ -f "$PACKAGE_DIR/android/tools/$jar.jar" ] || missing+=("android/tools/$jar.jar (scripts/build_react_native_android_tools.sh)")
done
[ -d "$PACKAGE_DIR/android/tools/jsparser" ] || missing+=("android/tools/jsparser (scripts/build_react_native_android_tools.sh)")
[ -f "$PACKAGE_DIR/android/tools/metadata-filter/seed.js" ] || missing+=("android/tools/metadata-filter/seed.js (scripts/build_react_native_android_tools.sh)")
[ -f "$PACKAGE_DIR/android/tools/metadata-filter/harvest.js" ] || missing+=("android/tools/metadata-filter/harvest.js (scripts/build_react_native_android_tools.sh)")

if [ ${#missing[@]} -gt 0 ]; then
  echo "error: the Android half of @nativescript/react-native is not staged; refusing to pack:" >&2
  printf '  - %s\n' "${missing[@]}" >&2
  echo "Run the staging scripts named above before packing, or pass --no-pack to stage only." >&2
  exit 1
fi

checkpoint "Packing @nativescript/react-native..."
(
  cd "$PACKAGE_DIR"
  npm pack --pack-destination "$REPO_ROOT/$OUTPUT_DIR"
)

# npm honours .npmignore and the files list; check the artifact itself rather
# than trusting that what was staged is what shipped.
tarball=$(find "$OUTPUT_DIR" -name '*.tgz' | head -1)
for required in package/android/tools/static-binding-generator.jar \
                package/android/src/main/java-runtime \
                package/android/nativescript.gradle \
                package/android/tools/metadata-filter/seed.js \
                package/android/tools/metadata-filter/harvest.js \
                package/native-api/runtime/android \
                package/native-api/ffi/jni \
                package/native-api/ffi/objc \
                package/ios; do
  if ! tar tzf "$tarball" | grep -q "^${required}"; then
    echo "error: ${tarball##*/} is missing ${required#package/}." >&2
    exit 1
  fi
done

cp "$OUTPUT_DIR"/*.tgz "$PACK_DESTINATION/"

checkpoint "@nativescript/react-native npm package created."
