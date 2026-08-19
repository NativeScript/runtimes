#!/bin/bash
#
# Builds the NativeScript Android runtime for one JS engine.
#
# The gradle build itself lives in platforms/android; this script only picks the
# engine, maps it to the gradle properties, and runs it from the repo root so
# every build entry point is in scripts/.
#
# Usage:
#   ./scripts/build_all_android.sh                      # prompts for the engine
#   ./scripts/build_all_android.sh --engine=QUICKJS_NG
#   ./scripts/build_all_android.sh --engine=HERMES --disable-host-objects
#   ENGINE=V8-13 ./scripts/build_all_android.sh         # non-interactive
#
# Flags:
#   --engine=<ENGINE>        V8-10 | V8-11 | V8-13 | QUICKJS_NG | HERMES |
#                            SHERMES | JSC | PRIMJS
#   --binding=napi|jsi       which runtime tree to compile (default napi)
#   --disable-host-objects   build without host-object support (on by default;
#                            every engine supports it)
#   --dry-run                print the gradle command and exit
#   anything else            passed through to gradle verbatim
#
# The staged package lands in dist/android_<engine>_<binding>/ and the tarball in
# build/npm-tarballs/, alongside the Apple build output.
#
set -e
source "$(dirname "$0")/build_utils.sh"

ANDROID_ROOT="$REPO_ROOT/platforms/android"

# SHERMES is kept as an input alias for the unified Static Hermes backend.
VALID_ENGINES=(V8-10 V8-11 V8-13 QUICKJS_NG HERMES SHERMES JSC PRIMJS)
DEFAULT_ENGINE=V8-13

ENGINE=${ENGINE:-}
BINDING=${BINDING:-napi}
DISABLE_HOST_OBJECTS=false
DRY_RUN=false
PASSTHROUGH=()

for arg in "$@"; do
  case $arg in
    --engine=*) ENGINE="${arg#*=}" ;;
    --binding=*) BINDING="${arg#*=}" ;;
    --disable-host-objects) DISABLE_HOST_OBJECTS=true ;;
    --dry-run) DRY_RUN=true ;;
    *) PASSTHROUGH+=("$arg") ;;
  esac
done

is_valid_engine() {
  local candidate="$1"
  for e in "${VALID_ENGINES[@]}"; do
    [ "$e" = "$candidate" ] && return 0
  done
  return 1
}

# Ask only when nothing was passed and someone is there to answer; a CI run with
# no --engine gets the default rather than hanging on a prompt.
if [ -z "$ENGINE" ]; then
  if [ -t 0 ]; then
    echo "Select JS engine:"
    for i in "${!VALID_ENGINES[@]}"; do
      echo "  $((i + 1))) ${VALID_ENGINES[$i]}"
    done
    read -r -p "Choose number or name ($DEFAULT_ENGINE) " answer
    if [ -z "$answer" ]; then
      ENGINE=$DEFAULT_ENGINE
    elif [[ "$answer" =~ ^[0-9]+$ ]]; then
      ENGINE=${VALID_ENGINES[$((answer - 1))]:-$DEFAULT_ENGINE}
    else
      ENGINE=$answer
    fi
  else
    ENGINE=$DEFAULT_ENGINE
  fi
fi

if ! is_valid_engine "$ENGINE"; then
  echo "error: unknown engine '$ENGINE'. Expected one of: ${VALID_ENGINES[*]}" >&2
  exit 1
fi

case "$BINDING" in
  napi|jsi) ;;
  *) echo "error: --binding must be 'napi' or 'jsi', got '$BINDING'" >&2; exit 1 ;;
esac

GRADLE_ARGS=("-Pengine=$ENGINE" "-PbindingLayer=$BINDING")
# Host objects are on by default for every engine; opting out is explicit.
$DISABLE_HOST_OBJECTS || GRADLE_ARGS+=("-PuseHostObjects")
[ ${#PASSTHROUGH[@]} -gt 0 ] && GRADLE_ARGS+=("${PASSTHROUGH[@]}")

checkpoint "Building the Android runtime with $ENGINE ($BINDING runtime)"
echo "gradle command: ./gradlew ${GRADLE_ARGS[*]}"

if $DRY_RUN; then
  echo "Dry run requested. Exiting without executing gradle."
  exit 0
fi

cd "$ANDROID_ROOT"
./gradlew "${GRADLE_ARGS[@]}"

# Mirrors the slug the gradle build derives (see platforms/android/build.gradle).
ENGINE_SLUG=$(echo "$ENGINE" | tr '[:upper:]-' '[:lower:]_')
[ "$ENGINE_SLUG" = "quickjs_ng" ] && ENGINE_SLUG=quickjs
checkpoint "Done. Package staged in dist/android_${ENGINE_SLUG}_${BINDING}/, tarball in build/npm-tarballs/"
