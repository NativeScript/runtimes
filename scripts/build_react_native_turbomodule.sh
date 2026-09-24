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
  find "$REPO_ROOT/metadata-generator/src" "$REPO_ROOT/metadata-generator/include" \
    "$REPO_ROOT/metadata-generator/tests" "$REPO_ROOT/metadata-generator/symbol-analyzer" \
    "$REPO_ROOT/metadata-generator/CMakeLists.txt" "$REPO_ROOT/metadata-generator/build-step-metadata-generator.py" \
    \( -name target -type d -prune \) -o -type f -print | \
    LC_ALL=C sort | xargs shasum | awk '{print $1}' | shasum | awk '{print $1}'
}

function ensure_metadata_generator {
  local expected_hash
  expected_hash=$(metadata_generator_source_hash)
  local hash_file="$REPO_ROOT/metadata-generator/dist/.source_hash"
  local host_arch
  host_arch=$(uname -m)
  if [ ! -x "$REPO_ROOT/metadata-generator/dist/$host_arch/bin/objc-metadata-generator" ] || \
     [ ! -x "$REPO_ROOT/metadata-generator/dist/$host_arch/bin/ns-metadata-symbols" ] || \
     [ ! -f "$hash_file" ] || \
     [ "$(cat "$hash_file")" != "$expected_hash" ]; then
    METADATA_GENERATOR_ARCHS="$host_arch" "$SCRIPT_DIR/build_metadata_generator.sh"
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

rm -rf \
  "$PACKAGE_DIR/native-api" \
  "$PACKAGE_DIR/metadata" \
  "$PACKAGE_DIR/ios/vendor" \
  "$PACKAGE_DIR/types"
mkdir -p \
  "$PACKAGE_DIR/native-api/ffi/objc/hermes" \
  "$PACKAGE_DIR/native-api/ffi/objc/shared" \
  "$PACKAGE_DIR/native-api/ffi/objc/shared/bridge" \
  "$PACKAGE_DIR/native-api/ffi/objc/shared/bridge/host_objects" \
  "$PACKAGE_DIR/native-api/jsi/hermes" \
  "$PACKAGE_DIR/native-api/jsi/shared" \
  "$PACKAGE_DIR/native-api/metadata/include" \
  "$PACKAGE_DIR/metadata" \
  "$PACKAGE_DIR/ios/vendor/libffi/include" \
  "$PACKAGE_DIR/types/ios" \
  "$PACKAGE_DIR/types/objc-node-api" \
  "$PACK_DESTINATION"

cp NativeScript/ffi/objc/hermes/NativeApiJsi*.mm "$PACKAGE_DIR/native-api/ffi/objc/hermes/"
cp NativeScript/ffi/objc/hermes/NativeApiJsi*.h "$PACKAGE_DIR/native-api/ffi/objc/hermes/"
cp NativeScript/ffi/objc/hermes/NativeApiJsiReactNative.h "$PACKAGE_DIR/native-api/ffi/objc/hermes/"
cp NativeScript/ffi/objc/shared/bridge/*.mm "$PACKAGE_DIR/native-api/ffi/objc/shared/bridge/"
cp NativeScript/ffi/objc/shared/bridge/*.h "$PACKAGE_DIR/native-api/ffi/objc/shared/bridge/"
cp NativeScript/ffi/objc/shared/bridge/host_objects/*.mm \
  "$PACKAGE_DIR/native-api/ffi/objc/shared/bridge/host_objects/"
cp NativeScript/ffi/objc/shared/NativeApiBackendConfig.h "$PACKAGE_DIR/native-api/ffi/objc/shared/"
cp NativeScript/ffi/objc/shared/SignatureDispatchCore.h "$PACKAGE_DIR/native-api/ffi/objc/shared/"
cp NativeScript/ffi/objc/shared/PreparedSignatureDispatch.h "$PACKAGE_DIR/native-api/ffi/objc/shared/"
cp NativeScript/jsi/hermes/*.h "$PACKAGE_DIR/native-api/jsi/hermes/"
cp NativeScript/jsi/shared/*.h "$PACKAGE_DIR/native-api/jsi/shared/"
cp "$GENERATED_SIGNATURE_DISPATCH" "$PACKAGE_DIR/native-api/ffi/objc/shared/GeneratedSignatureDispatch.inc"
GENERATED_GSD_SIGNATURE_DISPATCH="$(dirname "$GENERATED_SIGNATURE_DISPATCH")/GeneratedGsdSignatureDispatch.inc"
if [ -f "$GENERATED_GSD_SIGNATURE_DISPATCH" ]; then
  cp "$GENERATED_GSD_SIGNATURE_DISPATCH" "$PACKAGE_DIR/native-api/ffi/objc/shared/GeneratedGsdSignatureDispatch.inc"
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

checkpoint "Packing @nativescript/react-native..."
(
  cd "$PACKAGE_DIR"
  npm pack --pack-destination "$REPO_ROOT/$OUTPUT_DIR"
)

cp "$OUTPUT_DIR"/*.tgz "$PACK_DESTINATION/"

checkpoint "@nativescript/react-native npm package created."
