#!/bin/bash
# Runs the Android conformance fixture in a React Native app on a device.
#
#   scripts/test_react_native_android.sh <path-to-rn-app>
#
# The app must already have @nativescript/react-native installed and
# android/nativescript.gradle applied (npx nativescript-rn configure does both).
# Metro has to be reachable: the script starts one if nothing answers on 8081.
set -euo pipefail
source "$(dirname "$0")/build_utils.sh"

APP_DIR=${1:-}
if [ -z "$APP_DIR" ] || [ ! -d "$APP_DIR/android" ]; then
  echo "usage: $0 <path-to-react-native-app>" >&2
  exit 1
fi

ADB="${ANDROID_HOME:?ANDROID_HOME is not set}/platform-tools/adb"
FIXTURE="$REPO_ROOT/platforms/android/test/react-native/App.tsx"
MARKER="NS_ANDROID_SMOKE"
LOG=$(mktemp)

checkpoint "Installing the conformance fixture..."
cp "$FIXTURE" "$APP_DIR/App.tsx"

if ! curl -s -o /dev/null "http://localhost:8081/status"; then
  checkpoint "Starting Metro..."
  (cd "$APP_DIR" && nohup npx react-native start >/dev/null 2>&1 &)
  for _ in $(seq 1 30); do
    curl -s -o /dev/null "http://localhost:8081/status" && break
    sleep 2
  done
fi

PKG=$(sed -n 's/.*applicationId "\(.*\)".*/\1/p' "$APP_DIR/android/app/build.gradle" | head -1)
if [ -z "$PKG" ]; then
  echo "Could not read applicationId from android/app/build.gradle" >&2
  exit 1
fi

checkpoint "Building and installing $PKG..."
(cd "$APP_DIR/android" && ./gradlew --quiet :app:assembleDebug)
"$ADB" reverse tcp:8081 tcp:8081 >/dev/null
"$ADB" install -r -t "$APP_DIR/android/app/build/outputs/apk/debug/app-debug.apk" >/dev/null

checkpoint "Running..."
"$ADB" shell am force-stop "$PKG" >/dev/null 2>&1 || true
"$ADB" logcat -c
"$ADB" logcat > "$LOG" 2>&1 &
LOGPID=$!
sleep 1
"$ADB" shell am start -n "$PKG/.MainActivity" >/dev/null

for _ in $(seq 1 60); do
  grep -q "${MARKER}_DONE\|has died\|SyntaxError" "$LOG" 2>/dev/null && break
  sleep 2
done
sleep 2
kill $LOGPID 2>/dev/null || true

grep -oE "$MARKER (PASS|FAIL)[^\"\\\\]*" "$LOG" || true
FAILURES=$(grep -c "$MARKER FAIL" "$LOG" || true)
TOTAL=$(grep -cE "$MARKER (PASS|FAIL)" "$LOG" || true)

echo
if [ "$TOTAL" -eq 0 ]; then
  echo "No results. Full log: $LOG" >&2
  exit 1
fi
echo "$((TOTAL - FAILURES))/$TOTAL passed"
[ "$FAILURES" -eq 0 ]
