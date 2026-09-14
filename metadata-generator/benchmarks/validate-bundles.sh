#!/bin/bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "Usage: $0 <ns-metadata-symbols> [work-directory]" >&2
  exit 2
fi

analyzer=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
work_dir=${2:-$(mktemp -d "${TMPDIR:-/tmp}/ns-metadata-bundles.XXXXXX")}

if [ ! -x "$analyzer" ]; then
  echo "Analyzer is not executable: $analyzer" >&2
  exit 2
fi
if [ -e "$work_dir" ] && [ -n "$(find "$work_dir" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
  echo "Work directory is not empty: $work_dir" >&2
  exit 2
fi

mkdir -p "$work_dir/minified" "$work_dir/split"
sources=(
  "$repo_root/platforms/apple/examples/appkit.js"
  "$repo_root/platforms/apple/examples/view_controller.js"
  "$repo_root/platforms/apple/examples/webview.js"
)

# Pin the bundler so the emitted-bundle regression is repeatable. Dynamic
# imports exercise multiple emitted chunks while the imported code remains the
# repository's real app source.
npx --yes esbuild@0.25.8 "${sources[@]}" \
  --bundle --minify --format=esm --external:@nativescript/* \
  --outdir="$work_dir/minified"
npx --yes esbuild@0.25.8 "$script_dir/fixtures/split-entry.js" \
  --bundle --minify --splitting --format=esm --external:@nativescript/* \
  --chunk-names=chunks/[name]-[hash] --outdir="$work_dir/split"

if [ "$(find "$work_dir/split/chunks" -type f -name '*.js' | wc -l | tr -d ' ')" -lt 2 ]; then
  echo "Expected esbuild to emit at least two split chunks" >&2
  exit 1
fi

"$analyzer" --output "$work_dir/source.mdg" "${sources[@]}"
"$analyzer" --output "$work_dir/minified.mdg" "$work_dir/minified"
"$analyzer" --output "$work_dir/split.mdg" "$work_dir/split"

cmp "$work_dir/source.mdg" "$work_dir/minified.mdg"
cmp "$work_dir/source.mdg" "$work_dir/split.mdg"
if grep -q '^\*:\*$' "$work_dir/source.mdg"; then
  echo "Representative bundles unexpectedly disabled filtering" >&2
  exit 1
fi

# The runtime test corpus contains malformed fixtures, eval, and dynamic global
# access. It must conservatively disable filtering rather than under-strip.
"$analyzer" --output "$work_dir/fail-open.mdg" \
  "$repo_root/platforms/apple/test/runtime/runner/app"
grep -q '^\*:\*$' "$work_dir/fail-open.mdg"

echo "Bundle validation passed. Outputs: $work_dir"
shasum -a 256 \
  "$work_dir/source.mdg" \
  "$work_dir/minified.mdg" \
  "$work_dir/split.mdg"
