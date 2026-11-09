#!/bin/bash
set -euo pipefail

# Shared helpers for generated React Native apps used by smoke tests, FFI
# compatibility tests, and local demos. Source build_utils.sh before this file.

function rn_build_turbo_tarball() {
  checkpoint "Building @nativescript/react-native TurboModule tarball..."
  "$SCRIPT_DIR/build_react_native_turbomodule.sh"
}

function rn_latest_turbo_tarball() {
  ls -t "$REPO_ROOT/packages/react-native/dist"/*.tgz | head -n 1
}

function rn_create_app_if_missing() {
  local app_dir="$1"
  local app_root="$2"
  local app_name="$3"
  local rn_version="$4"
  local rn_cli_version="$5"
  local label="$6"

  if [[ ! -d "$app_dir" ]]; then
    checkpoint "Creating $label ($rn_version)..."
    mkdir -p "$app_root"
    npx --yes "@react-native-community/cli@$rn_cli_version" init "$app_name" \
      --version "$rn_version" \
      --directory "$app_dir" \
      --skip-git-init \
      --install-pods false \
      --pm npm
  fi
}

function rn_install_turbo_tarball() {
  local app_dir="$1"
  local tarball="$2"
  local label="$3"

  checkpoint "Installing local TurboModule tarball into $label..."
  (
    cd "$app_dir"
    npm install "$tarball"
  )
}

function rn_install_pods() {
  local app_dir="$1"
  local label="$2"

  checkpoint "Installing CocoaPods for $label..."
  (
    cd "$app_dir/ios"
    if [[ -f Gemfile ]]; then
      bundle install
      RCT_NEW_ARCH_ENABLED=1 USE_HERMES=1 bundle exec pod install
    else
      RCT_NEW_ARCH_ENABLED=1 USE_HERMES=1 pod install
    fi
  )
}

function rn_select_ios_simulator() {
  if [[ -n "${RN_IOS_SIMULATOR_UDID:-}" ]]; then
    node - "$RN_IOS_SIMULATOR_UDID" <<'NODE'
const cp = require('child_process');
const requested = process.argv[2];
const devices = JSON.parse(cp.execFileSync('xcrun', ['simctl', 'list', 'devices', 'available', '--json'], {encoding: 'utf8'})).devices;
const match = Object.values(devices).flat().find(device => device.udid === requested && device.isAvailable);
if (!match) process.exit(1);
console.log(match.udid);
NODE
    return
  fi

  node <<'NODE'
const cp = require('child_process');
const devices = JSON.parse(cp.execFileSync('xcrun', ['simctl', 'list', 'devices', 'available', '--json'], {encoding: 'utf8'}));
const runtimes = Object.keys(devices.devices).filter((runtime) => runtime.includes('iOS')).sort().reverse();
for (const runtime of runtimes) {
  const booted = devices.devices[runtime].find((device) => device.state === 'Booted' && device.name.includes('iPhone'));
  if (booted) {
    console.log(booted.udid);
    process.exit(0);
  }
}
for (const runtime of runtimes) {
  const candidate = devices.devices[runtime].find((device) => device.name.includes('iPhone'));
  if (candidate) {
    console.log(candidate.udid);
    process.exit(0);
  }
}
process.exit(1);
NODE
}

function rn_require_ios_simulator() {
  local udid
  if ! udid=$(rn_select_ios_simulator); then
    udid=""
  fi
  if [[ -z "$udid" ]]; then
    echo "No available iOS simulator found." >&2
    exit 1
  fi
  echo "$udid"
}

function rn_configure_nativescript_app() {
  local app_dir="$1"
  local label="$2"
  checkpoint "Configuring NativeScript transforms for $label..."
  (
    cd "$app_dir"
    npx --no-install nativescript-rn configure
  )
}

function rn_build_ios_app() {
  local app_dir="$1"
  local app_root="$2"
  local app_name="$3"
  local configuration="$4"
  local udid="$5"
  local timeout_seconds="$6"
  local label="$7"

  checkpoint "Building $label for simulator..."
  xcrun simctl boot "$udid" >/dev/null 2>&1 || true
  xcrun simctl bootstatus "$udid" -b

  xcodebuild \
    -workspace "$app_dir/ios/$app_name.xcworkspace" \
    -scheme "$app_name" \
    -configuration "$configuration" \
    -sdk iphonesimulator \
    -destination "platform=iOS Simulator,id=$udid" \
    -derivedDataPath "$app_dir/ios/build/DerivedData" \
    ONLY_ACTIVE_ARCH=YES \
    FORCE_BUNDLING=1 \
    build | tee "$app_root/xcodebuild.log" &
  local build_pid=$!

  local seconds_waited=0
  while kill -0 "$build_pid" >/dev/null 2>&1; do
    if [[ "$seconds_waited" -ge "$timeout_seconds" ]]; then
      kill "$build_pid" >/dev/null 2>&1 || true
      echo "$label build timed out after ${timeout_seconds}s." >&2
      exit 1
    fi
    sleep 5
    seconds_waited=$((seconds_waited + 5))
  done
  wait "$build_pid"

  RN_APP_BUNDLE=$(find "$app_dir/ios/build/DerivedData/Build/Products/$configuration-iphonesimulator" -maxdepth 1 -name "$app_name.app" -print -quit)
  if [[ -z "$RN_APP_BUNDLE" ]]; then
    echo "Built app bundle not found." >&2
    exit 1
  fi
}

function rn_launch_app_with_marker() {
  local udid="$1"
  local app_bundle="$2"
  local bundle_id="$3"
  local marker_file_name="$4"

  xcrun simctl install "$udid" "$app_bundle"
  local data_container
  data_container=$(xcrun simctl get_app_container "$udid" "$bundle_id" data)
  local marker_file="$data_container/tmp/$marker_file_name"
  rm -f "$marker_file"
  # A reinstall over an already-installed bundle ID preserves the app's data
  # container on the simulator, so a leftover dev-reload phase marker
  # (NativeScriptNativeApiModule's __writeReloadPhaseMarker) from a PRIOR
  # run of this same app can survive into a fresh launch and make it think
  # it's already in "phase 2"; confirmed on-sim (a Release-config run
  # right after a Debug-config JOB2 run misreported phase2-post-reload).
  # Harmless rm for scripts that never write this file.
  rm -f "$data_container/tmp/NativeScriptM1ReloadPhase.marker"

  SIMCTL_CHILD_NATIVESCRIPT_RN_TURBO_SMOKE_MARKER=1 \
    xcrun simctl launch --terminate-running-process "$udid" "$bundle_id" >/dev/null

  echo "$marker_file"
}

function rn_wait_for_marker_file() {
  local marker_file="$1"
  local marker="$2"
  local timeout_seconds="$3"

  node - "$marker_file" "$marker" "$timeout_seconds" <<'NODE'
const fs = require('fs');
const [markerFile, marker, timeoutSecondsText] = process.argv.slice(2);
const timeoutMs = Number(timeoutSecondsText) * 1000;
const startedAt = Date.now();
let lastContent = '';

function poll() {
  if (fs.existsSync(markerFile)) {
    const content = fs.readFileSync(markerFile, 'utf8').trim();
    if (content && content !== lastContent) {
      lastContent = content;
      if (content.startsWith('stage=')) {
        console.log(`${marker} ${JSON.stringify({markerFile, stage: content.slice('stage='.length)})}`);
      } else if (!marker || content.includes(marker)) {
        console.log(`${marker} ${JSON.stringify({markerFile, content})}`);
        process.exit(0);
      } else {
        console.error(`Unexpected ${marker} marker content at ${markerFile}: ${content}`);
        process.exit(1);
      }
    }
  }

  if (Date.now() - startedAt > timeoutMs) {
    console.error(`Timed out waiting for ${marker} file at ${markerFile}.`);
    if (lastContent) {
      console.error(`Last ${marker} marker content: ${lastContent}`);
    }
    process.exit(1);
  }

  setTimeout(poll, 2000);
}

poll();
NODE
}
