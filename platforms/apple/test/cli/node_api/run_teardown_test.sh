#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
MARKER_FILE="$(mktemp /tmp/ns-node-api-cleanup.XXXXXX)"

cleanup() {
  rm -f "$MARKER_FILE"
}
trap cleanup EXIT

rm -f "$MARKER_FILE"

"$SCRIPT_DIR/build_addon.sh" >/dev/null
NS_NODE_API_CLEANUP_FILE="$MARKER_FILE" \
  "$ROOT_DIR/dist/nsr" run "$SCRIPT_DIR/cleanup_teardown.js" >/dev/null

for _ in $(seq 1 50); do
  if [[ -f "$MARKER_FILE" ]]; then
    break
  fi
  sleep 0.02
done

if [[ ! -f "$MARKER_FILE" ]]; then
  echo "cleanup teardown FAIL: marker file was not created"
  exit 1
fi

if ! rg -q '^env_cleanup$' "$MARKER_FILE"; then
  echo "cleanup teardown FAIL: env cleanup hook marker missing"
  cat "$MARKER_FILE"
  exit 1
fi

if ! rg -q '^async_cleanup$' "$MARKER_FILE"; then
  echo "cleanup teardown FAIL: async cleanup hook marker missing"
  cat "$MARKER_FILE"
  exit 1
fi

echo "cleanup teardown PASS"
