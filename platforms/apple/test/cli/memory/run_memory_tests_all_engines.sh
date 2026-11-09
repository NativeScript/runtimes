#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
TEST_RUNNER="$SCRIPT_DIR/run_memory_tests.js"
RESULTS_DIR="$ROOT_DIR/build/test-results"
GREP_FILTER="${1:-}"
RUNNER_ARGS=(--repeat 1)
if [[ "${MEMTEST_WINDOW_TESTS:-0}" != "1" ]]; then
  RUNNER_ARGS+=(--exclude-window-tests)
fi
if [[ -n "${MEMTEST_ENGINES:-}" ]]; then
  read -r -a ENGINES <<< "$MEMTEST_ENGINES"
else
  ENGINES=(v8 quickjs jsc hermes)
fi
if [[ -n "${MEMTEST_BACKENDS:-}" ]]; then
  read -r -a BACKENDS <<< "$MEMTEST_BACKENDS"
else
  BACKENDS=(direct napi)
fi

for engine in "${ENGINES[@]}"; do
  for backend in "${BACKENDS[@]}"; do
    build_args=(--no-iphone --no-simulator --no-macos --macos-cli "--${engine}")
    if [[ "$backend" == "napi" ]]; then
      build_args+=(--ffi-napi --gsd-napi)
    elif [[ "$backend" != "direct" ]]; then
      echo "Unsupported memory-test backend: $backend" >&2
      exit 1
    fi

    echo
    echo "=== Building macOS CLI for ${engine} ${backend} ==="
    "$ROOT_DIR/scripts/build_nativescript.sh" "${build_args[@]}"

    echo "=== Running CLI memory and ownership suite for ${engine} ${backend} ==="
    if [[ -n "$GREP_FILTER" ]]; then
      node "$TEST_RUNNER" "${RUNNER_ARGS[@]}" --grep "$GREP_FILTER"
    else
      node "$TEST_RUNNER" "${RUNNER_ARGS[@]}"
    fi
    cp "$RESULTS_DIR/memory-cli-report.json" \
      "$RESULTS_DIR/memory-cli-report-${engine}-${backend}.json"
  done
done
