#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/build_utils.sh"
source "$SCRIPT_DIR/react_native_app_utils.sh"

RN_VERSION=${RN_DEMO_VERSION:-0.85.3}
RN_CLI_VERSION=${RN_DEMO_CLI_VERSION:-20.1.3}
APP_NAME=${RN_DEMO_APP_NAME:-NativeScriptNativeApiDemo}
APP_ROOT=${RN_DEMO_APP_ROOT:-"$REPO_ROOT/build/react-native-demo"}
APP_DIR="$APP_ROOT/$APP_NAME"
CONFIGURATION=${IOS_CONFIGURATION:-Release}
FORCE_RECREATE=${RN_DEMO_FORCE_RECREATE:-0}
RUN_BUILD=${RN_DEMO_BUILD:-1}
RUN_LAUNCH=${RN_DEMO_LAUNCH:-1}
BUILD_TIMEOUT_SECONDS=${RN_DEMO_BUILD_TIMEOUT_SECONDS:-1800}
LAUNCH_TIMEOUT_SECONDS=${RN_DEMO_LAUNCH_TIMEOUT_SECONDS:-90}
BUNDLE_ID="org.reactjs.native.example.$APP_NAME"
MARKER="NATIVESCRIPT_RN_TURBO_DEMO_PASS"
MARKER_FILE_NAME="NativeScriptNativeApiSmoke.marker"
DEMO_APP_TSX="$REPO_ROOT/platforms/apple/examples/react-native-demo/App.tsx"

rn_build_turbo_tarball
TARBALL=$(rn_latest_turbo_tarball)

if [[ "$FORCE_RECREATE" == "1" ]]; then
  rm -rf "$APP_DIR"
fi

rn_create_app_if_missing "$APP_DIR" "$APP_ROOT" "$APP_NAME" "$RN_VERSION" "$RN_CLI_VERSION" "React Native demo app"
rn_install_turbo_tarball "$APP_DIR" "$TARBALL" "demo app"

checkpoint "Installing react-native-worklets for the demo app..."
(cd "$APP_DIR" && npm install --silent react-native-worklets@0.9.1)

checkpoint "Enabling NativeScript and Worklets Babel plugins for the demo app..."
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
    source += `\n// NativeScript demo: add ${missingPlugins.map((plugin) => `'${plugin}'`).join(' and ')} to Babel plugins.\n`;
  }
  fs.writeFileSync(target, source);
}
NODE

checkpoint "Installing demo entrypoint..."
cp "$DEMO_APP_TSX" "$APP_DIR/App.tsx"

rn_install_pods "$APP_DIR" "demo app"

if [[ "$RUN_BUILD" != "1" ]]; then
  checkpoint "React Native demo app is ready at $APP_DIR"
  exit 0
fi

UDID=$(rn_require_ios_simulator)
rn_build_ios_app "$APP_DIR" "$APP_ROOT" "$APP_NAME" "$CONFIGURATION" "$UDID" "$BUILD_TIMEOUT_SECONDS" "demo app"
APP_BUNDLE="$RN_APP_BUNDLE"

if [[ "$RUN_LAUNCH" == "1" ]]; then
  checkpoint "Launching demo app..."
  MARKER_FILE=$(rn_launch_app_with_marker "$UDID" "$APP_BUNDLE" "$BUNDLE_ID" "$MARKER_FILE_NAME")
  rn_wait_for_marker_file "$MARKER_FILE" "$MARKER" "$LAUNCH_TIMEOUT_SECONDS"
fi

checkpoint "React Native NativeScript demo app is ready at $APP_DIR"
