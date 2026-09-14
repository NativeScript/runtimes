#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/build_utils.sh"
source "$SCRIPT_DIR/react_native_app_utils.sh"

RN_VERSION=${RN_FFI_COMPAT_VERSION:-0.85.3}
RN_CLI_VERSION=${RN_FFI_COMPAT_CLI_VERSION:-20.1.3}
APP_NAME=${RN_FFI_COMPAT_APP_NAME:-NativeScriptNativeApiFfiCompat}
APP_ROOT=${RN_FFI_COMPAT_APP_ROOT:-"$REPO_ROOT/build/react-native-ffi-compat"}
APP_DIR="$APP_ROOT/$APP_NAME"
CONFIGURATION=${IOS_CONFIGURATION:-Release}
FORCE_RECREATE=${RN_FFI_COMPAT_FORCE_RECREATE:-1}
BUILD_TIMEOUT_SECONDS=${RN_FFI_COMPAT_BUILD_TIMEOUT_SECONDS:-1800}
LAUNCH_TIMEOUT_SECONDS=${RN_FFI_COMPAT_LAUNCH_TIMEOUT_SECONDS:-120}
BUNDLE_ID="org.reactjs.native.example.$APP_NAME"
MARKER="NATIVESCRIPT_RN_FFI_COMPAT"
MARKER_FILE_NAME="NativeScriptNativeApiSmoke.marker"
APP_TSX="$REPO_ROOT/platforms/apple/test/react-native/ffi-compat/App.tsx"
RUNTIME_TESTS_SOURCE="$REPO_ROOT/platforms/apple/test/runtime/runner/app/tests"
FIXTURES_SOURCE="$REPO_ROOT/platforms/apple/test/runtime/fixtures"
GENERATED_METADATA_DIR="$APP_ROOT/metadata"

function rn_generate_ffi_test_metadata() {
  local output_dir="$1"
  local generator_arch
  local generator
  local sdk_root
  local sdk_version

  generator_arch=$(uname -m)
  generator="$REPO_ROOT/metadata-generator/dist/$generator_arch/bin/objc-metadata-generator"
  if [[ ! -x "$generator" ]]; then
    generator="$REPO_ROOT/metadata-generator/dist/arm64/bin/objc-metadata-generator"
  fi
  if [[ ! -x "$generator" ]]; then
    "$SCRIPT_DIR/build_metadata_generator.sh"
  fi
  if [[ ! -x "$generator" ]]; then
    echo "Metadata generator not found at $generator" >&2
    exit 1
  fi

  sdk_root=$(xcrun --sdk iphonesimulator --show-sdk-path)
  sdk_version=$(xcrun --sdk iphonesimulator --show-sdk-version)

  rm -rf "$output_dir"
  mkdir -p "$output_dir"

  for arch in arm64 x86_64; do
    checkpoint "Generating RN FFI fixture metadata for $arch..."
    "$generator" \
      -verbose \
      -output-bin "$output_dir/metadata.ios-sim.$arch.nsmd" \
      -output-umbrella "$output_dir/umbrella-$arch.h" \
      Xclang \
      -isysroot "$sdk_root" \
      -std=gnu17 \
      -target "$arch-apple-ios$sdk_version-simulator" \
      -I"$FIXTURES_SOURCE" \
      -fmodules \
      -fmodule-map-file="$FIXTURES_SOURCE/module.modulemap" \
      -DDEBUG=1 \
      > "$output_dir/metagen-$arch.log" 2>&1
  done
}

function rn_install_ffi_runtime_specs() {
  local tests_destination="$APP_DIR/ns-runtime-tests"
  rm -rf "$tests_destination"
  mkdir -p \
    "$tests_destination/Infrastructure" \
    "$tests_destination/Marshalling/Primitives" \
    "$tests_destination/Marshalling"

  cp "$RUNTIME_TESTS_SOURCE/Infrastructure/utf8.js" "$tests_destination/Infrastructure/"
  cp "$RUNTIME_TESTS_SOURCE/FunctionsTests.js" "$tests_destination/"
  cp "$RUNTIME_TESTS_SOURCE/MethodCallsTests.js" "$tests_destination/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/Primitives/Function.js" "$tests_destination/Marshalling/Primitives/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/Primitives/Static.js" "$tests_destination/Marshalling/Primitives/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/Primitives/Instance.js" "$tests_destination/Marshalling/Primitives/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/Primitives/Derived.js" "$tests_destination/Marshalling/Primitives/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/ObjCTypesTests.js" "$tests_destination/Marshalling/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/ConstantsTests.js" "$tests_destination/Marshalling/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/RecordTests.js" "$tests_destination/Marshalling/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/VectorTests.js" "$tests_destination/Marshalling/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/NSStringTests.js" "$tests_destination/Marshalling/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/PointerTests.js" "$tests_destination/Marshalling/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/ReferenceTests.js" "$tests_destination/Marshalling/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/FunctionPointerTests.js" "$tests_destination/Marshalling/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/EnumTests.js" "$tests_destination/Marshalling/"
  cp "$RUNTIME_TESTS_SOURCE/Marshalling/ProtocolTests.js" "$tests_destination/Marshalling/"
}

function rn_install_ffi_test_fixtures_pod() {
  local pod_dir="$APP_DIR/ios/NativeScriptFfiTestFixtures"
  local podspec="$pod_dir/NativeScriptFfiTestFixtures.podspec"
  local podfile="$APP_DIR/ios/Podfile"

  rm -rf "$pod_dir"
  mkdir -p "$pod_dir"
  rsync -a --delete "$FIXTURES_SOURCE/" "$pod_dir/fixtures/"

  cat > "$podspec" <<'RUBY'
fixture_keepalive_ldflags = if File.exist?(File.join(__dir__, "fixtures/exported-symbols.txt"))
  File.readlines(File.join(__dir__, "fixtures/exported-symbols.txt"), chomp: true)
    .map(&:strip)
    .reject(&:empty?)
    .map { |symbol| "-Wl,-u,#{symbol}" }
    .join(" ")
else
  ""
end

Pod::Spec.new do |s|
  s.name = "NativeScriptFfiTestFixtures"
  s.version = "0.0.1"
  s.summary = "NativeScript FFI runtime test fixtures for React Native compatibility tests"
  s.homepage = "https://github.com/NativeScript/napi-ios"
  s.license = "Apache-2.0"
  s.author = "NativeScript Team"
  s.platforms = { :ios => "13.0" }
  s.source = { :path => "." }
  s.requires_arc = true
  s.prefix_header_file = "fixtures/TestFixtures-Prefix.h"
  s.source_files = "fixtures/**/*.{h,m}"
  s.public_header_files = "fixtures/**/*.h"
  s.frameworks = "Foundation", "UIKit", "CoreGraphics", "SceneKit"
  s.compiler_flags = "-Wno-return-stack-address -Wno-strict-prototypes"
  s.pod_target_xcconfig = {
    "HEADER_SEARCH_PATHS" => "\"$(PODS_TARGET_SRCROOT)/fixtures\"",
    "CLANG_ENABLE_MODULES" => "YES",
  }
  s.user_target_xcconfig = {
    "OTHER_LDFLAGS" => "$(inherited) -ObjC -force_load $(PODS_CONFIGURATION_BUILD_DIR)/NativeScriptFfiTestFixtures/libNativeScriptFfiTestFixtures.a #{fixture_keepalive_ldflags}",
  }
end
RUBY

  if ! grep -q "NativeScriptFfiTestFixtures" "$podfile"; then
    perl -0pi -e "s/(target '$APP_NAME' do\\n)/\\1  pod 'NativeScriptFfiTestFixtures', :path => '.\\/NativeScriptFfiTestFixtures'\\n/" "$podfile"
  fi
}

function rn_override_turbo_metadata_for_ffi_tests() {
  local package_metadata_dir="$APP_DIR/node_modules/@nativescript/react-native/metadata"
  rm -f "$package_metadata_dir/metadata.ios-sim.arm64.nsmd" \
    "$package_metadata_dir/metadata.ios-sim.x86_64.nsmd"
  cp "$GENERATED_METADATA_DIR/metadata.ios-sim.arm64.nsmd" "$package_metadata_dir/"
  cp "$GENERATED_METADATA_DIR/metadata.ios-sim.x86_64.nsmd" "$package_metadata_dir/"
}

rn_build_turbo_tarball
TARBALL=$(rn_latest_turbo_tarball)

if [[ "$FORCE_RECREATE" == "1" ]]; then
  rm -rf "$APP_DIR"
fi

rn_create_app_if_missing "$APP_DIR" "$APP_ROOT" "$APP_NAME" "$RN_VERSION" "$RN_CLI_VERSION" "React Native FFI compatibility app"
rn_install_turbo_tarball "$APP_DIR" "$TARBALL" "FFI compatibility app"

checkpoint "Installing react-native-worklets for the FFI compatibility app..."
(cd "$APP_DIR" && npm install --silent react-native-worklets@0.9.1)

checkpoint "Enabling NativeScript and Worklets Babel plugins for the FFI compatibility app..."
node - "$APP_DIR/babel.config.js" <<'NODE'
const fs = require('fs');
const target = process.argv[2];
let source = fs.existsSync(target)
  ? fs.readFileSync(target, 'utf8')
  : [
      'module.exports = {',
      "  presets: ['module:@react-native/babel-preset'],",
      '};',
      '',
    ].join('\n');

const plugins = ['@nativescript/react-native/babel-plugin', 'react-native-worklets/plugin'];
const missingPlugins = plugins.filter((plugin) => !source.includes(plugin));
if (missingPlugins.length > 0) {
  const pluginEntry = missingPlugins.map((plugin) => `'${plugin}'`).join(', ') + ', ';
  if (/plugins\s*:\s*\[/.test(source)) {
    source = source.replace(/plugins\s*:\s*\[/, (match) => `${match}${pluginEntry}`);
  } else if (/return\s*\{/.test(source)) {
    source = source.replace(
      /return\s*\{/,
      (match) => `${match}\n    plugins: [${pluginEntry}],`,
    );
  } else if (/module\.exports\s*=\s*\{/.test(source)) {
    source = source.replace(
      /module\.exports\s*=\s*\{/,
      (match) => `${match}\n  plugins: [${pluginEntry}],`,
    );
  } else {
    source += `\n// NativeScript FFI compatibility: add ${missingPlugins.map((plugin) => `'${plugin}'`).join(' and ')} to Babel plugins.\n`;
  }
  fs.writeFileSync(target, source);
}
NODE

checkpoint "Installing FFI compatibility entrypoint..."
cp "$APP_TSX" "$APP_DIR/App.tsx"
rn_install_ffi_runtime_specs
rn_generate_ffi_test_metadata "$GENERATED_METADATA_DIR"
rn_override_turbo_metadata_for_ffi_tests
rn_install_ffi_test_fixtures_pod

rn_install_pods "$APP_DIR" "FFI compatibility app"
UDID=$(rn_require_ios_simulator)
rn_build_ios_app "$APP_DIR" "$APP_ROOT" "$APP_NAME" "$CONFIGURATION" "$UDID" "$BUILD_TIMEOUT_SECONDS" "FFI compatibility app"
APP_BUNDLE="$RN_APP_BUNDLE"

checkpoint "Launching FFI compatibility app and waiting for test marker..."
MARKER_FILE=$(rn_launch_app_with_marker "$UDID" "$APP_BUNDLE" "$BUNDLE_ID" "$MARKER_FILE_NAME")

node - "$MARKER_FILE" "$MARKER" "$LAUNCH_TIMEOUT_SECONDS" <<'NODE'
const fs = require('fs');
const [markerFile, marker, timeoutSecondsText] = process.argv.slice(2);
const timeoutMs = Number(timeoutSecondsText) * 1000;
const startedAt = Date.now();
let lastContent = '';
let lastPayload = null;
let lastStage = '';

function finish(payload) {
  console.log(`${marker} ${JSON.stringify({markerFile, payload})}`);
  if (!payload || payload.status !== 'pass') {
    process.exit(1);
  }
  process.exit(0);
}

function poll() {
  if (fs.existsSync(markerFile)) {
    const content = fs.readFileSync(markerFile, 'utf8').trim();
    if (content && content !== lastContent) {
      lastContent = content;
      try {
        lastPayload = JSON.parse(content);
      } catch (error) {
        if (content.startsWith('stage=')) {
          lastStage = content;
          console.log(`${marker} ${JSON.stringify({markerFile, stage: content.slice('stage='.length)})}`);
          setTimeout(poll, 2000);
          return;
        }
        console.error(`Invalid ${marker} marker content at ${markerFile}: ${content}`);
        process.exit(1);
      }
      console.log(`${marker} ${JSON.stringify({markerFile, payload: lastPayload})}`);
      if (lastPayload.status !== 'running') {
        finish(lastPayload);
      }
    }
  }

  if (Date.now() - startedAt > timeoutMs) {
    console.error(`Timed out waiting for ${marker} file at ${markerFile}.`);
    if (lastPayload) {
      console.error(`Last ${marker} payload: ${JSON.stringify(lastPayload)}`);
    } else if (lastStage) {
      console.error(`Last ${marker} native stage: ${lastStage}`);
    }
    process.exit(1);
  }

  setTimeout(poll, 2000);
}

poll();
NODE

checkpoint "React Native NativeScript FFI compatibility suite passed."
