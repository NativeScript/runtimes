#!/bin/bash
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "Usage: $0 <objc-metadata-generator> <new-output-directory> [iterations]" >&2
  exit 2
fi

generator=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
output=$2
iterations=${3:-5}

if [ ! -x "$generator" ]; then
  echo "Generator is not executable: $generator" >&2
  exit 2
fi
if [ -e "$output" ]; then
  echo "Output path already exists (use a new path): $output" >&2
  exit 2
fi

sdk_root=$(xcrun --sdk macosx --show-sdk-path)
sdk_version=$(xcrun --sdk macosx --show-sdk-version)
mkdir -p "$output"

for iteration in $(jot "$iterations"); do
  run="$output/run-$iteration"
  mkdir -p "$run/types"
  /usr/bin/time -l -o "$run/timing.txt" "$generator" \
    "types=$run/types" \
    "ts-index-mode=frameworks-list" \
    "ts-index-frameworks=Foundation,AppKit" \
    -output-bin "$run/metadata.bin" \
    -output-umbrella "$run/umbrella.h" \
    -output-signature-bindings-cpp "$run/signatures.inc" \
    Xclang -isysroot "$sdk_root" -std=gnu99 \
    -target "arm64-apple-macosx$sdk_version" \
    >"$run/stdout.log" 2>"$run/stderr.log"

  (
    cd "$run"
    find . -type f ! -name '*.log' ! -name timing.txt ! -name hashes.txt -print0 | \
      LC_ALL=C sort -z | xargs -0 shasum -a 256
  ) >"$run/hashes.txt"
done

echo "Completed $iterations run(s) in $output"
echo "Compare timing.txt files and verify hashes.txt lists are identical."
