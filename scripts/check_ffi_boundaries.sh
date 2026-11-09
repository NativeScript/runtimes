#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NAPI_ENGINE_DIR="$ROOT_DIR/NativeScript/ffi/objc/napi/engine"
FFI_DIR="$ROOT_DIR/NativeScript/ffi"
OBJC_FFI_DIR="$FFI_DIR/objc"
JNI_FFI_DIR="$FFI_DIR/jni"
JNI_NAPI_DIR="$JNI_FFI_DIR/napi"
SHARED_DIR="$OBJC_FFI_DIR/shared"
NAPI_DIR="$OBJC_FFI_DIR/napi"
HERMES_DIR="$OBJC_FFI_DIR/hermes"
V8_DIR="$OBJC_FFI_DIR/v8"
JSC_DIR="$OBJC_FFI_DIR/jsc"
QUICKJS_DIR="$OBJC_FFI_DIR/quickjs"

if [ -d "$NAPI_ENGINE_DIR" ] && find "$NAPI_ENGINE_DIR" -type f | grep -q .; then
  echo "ffi/objc/napi must remain a pure Node-API backend; do not add ffi/objc/napi/engine." >&2
  exit 1
fi

FORBIDDEN_DIRS=(
  "$FFI_DIR/direct"
  "$FFI_DIR/engine"
  "$OBJC_FFI_DIR/direct"
  "$OBJC_FFI_DIR/engine"
  "$JNI_FFI_DIR/direct"
  "$JNI_FFI_DIR/engine"
  "$JNI_NAPI_DIR/engine"
  "$SHARED_DIR/jsi"
)

for dir in "${FORBIDDEN_DIRS[@]}"; do
  if [ -e "$dir" ]; then
    echo "${dir#$ROOT_DIR/} is not an allowed FFI layer." >&2
    exit 1
  fi
done

ENGINE_AND_SHARED_DIRS=(
  "$SHARED_DIR"
  "$HERMES_DIR"
  "$V8_DIR"
  "$JSC_DIR"
  "$QUICKJS_DIR"
)

EXISTING_ENGINE_AND_SHARED_DIRS=()
for dir in "${ENGINE_AND_SHARED_DIRS[@]}"; do
  if [ -d "$dir" ]; then
    EXISTING_ENGINE_AND_SHARED_DIRS+=("$dir")
  fi
done

if [ "${#EXISTING_ENGINE_AND_SHARED_DIRS[@]}" -eq 0 ]; then
  exit 0
fi

search_sources() {
  local pattern="$1"
  shift

  if command -v rg >/dev/null 2>&1; then
    rg -n "$pattern" "$@" -g '*.{h,hh,hpp,c,cc,cpp,m,mm,inc}' \
      -g '!GeneratedSignatureDispatch.inc' \
      -g '!GeneratedGsdSignatureDispatch.inc'
    return
  fi

  find "$@" \
    -type f \( \
      -name '*.h' -o \
      -name '*.hh' -o \
      -name '*.hpp' -o \
      -name '*.c' -o \
      -name '*.cc' -o \
      -name '*.cpp' -o \
      -name '*.m' -o \
      -name '*.mm' -o \
      -name '*.inc' \
    \) ! -name 'GeneratedSignatureDispatch.inc' \
       ! -name 'GeneratedGsdSignatureDispatch.inc' -print0 | xargs -0 grep -nE "$pattern"
}

if search_sources '(^|[^[:alnum:]_])(napi_|napi_env|napi_value|js_native_api|node_api)($|[^[:alnum:]_])' \
  "${EXISTING_ENGINE_AND_SHARED_DIRS[@]}"; then
  echo "Node-API symbols are not allowed in shared or engine FFI folders." >&2
  exit 1
fi

ENGINE_NEUTRAL_DIRS=()
for dir in "$SHARED_DIR"; do
  if [ -d "$dir" ]; then
    ENGINE_NEUTRAL_DIRS+=("$dir")
  fi
done

if [ "${#ENGINE_NEUTRAL_DIRS[@]}" -gt 0 ] &&
  search_sources '(^|[^[:alnum:]_])(napi_|napi_env|napi_value|js_native_api|node_api|facebook::jsi|v8::|JSContextRef|JSValueRef|JSContext|JSValue|JSRuntime|quickjs)($|[^[:alnum:]_])|(<jsi/|<v8|JavaScriptCore|quickjs\.h)' \
    "${ENGINE_NEUTRAL_DIRS[@]}"; then
  echo "ffi/objc/shared must remain engine-neutral; JS engine APIs are not allowed there." >&2
  exit 1
fi

check_no_backend_dependency() {
  local owner_name="$1"
  local owner_dir="$2"
  shift 2

  if [ ! -d "$owner_dir" ]; then
    return
  fi

  local pattern=""
  local backend
  for backend in "$@"; do
    if [ -n "$pattern" ]; then
      pattern="$pattern|"
    fi
    # The bare "<backend>/ form matches a file sitting directly in that
    # backend's directory, which is how ffi/objc/<backend> is laid out -- they
    # are flat. Requiring no further path segment is what keeps this from also
    # matching the engine tree at NativeScript/napi/<engine>/, which collides in
    # include space with the ffi/objc/napi backend but is a different thing
    # entirely: ffi/objc/hermes including "napi/hermes/jsr.h" is that backend
    # reaching for its own engine's JSR, not a dependency on another backend.
    # The fully qualified ffi/objc/<backend>/ form below still catches any
    # depth, so nothing is lost.
    pattern="${pattern}(ffi/objc/${backend}/|\"${backend}/[^/]*\")"
  done

  if [ -n "$pattern" ] && search_sources "$pattern" "$owner_dir"; then
    echo "ffi/objc/$owner_name must not include another FFI backend's private files." >&2
    exit 1
  fi
}

check_no_backend_dependency "napi" "$NAPI_DIR" hermes v8 jsc quickjs
check_no_backend_dependency "hermes" "$HERMES_DIR" napi v8 jsc quickjs
check_no_backend_dependency "v8" "$V8_DIR" napi hermes jsc quickjs
check_no_backend_dependency "jsc" "$JSC_DIR" napi hermes v8 quickjs
check_no_backend_dependency "quickjs" "$QUICKJS_DIR" napi hermes v8 jsc

NON_HERMES_JSI_DIRS=()
for dir in "$SHARED_DIR" "$NAPI_DIR" "$V8_DIR" "$JSC_DIR" "$QUICKJS_DIR"; do
  if [ -d "$dir" ]; then
    NON_HERMES_JSI_DIRS+=("$dir")
  fi
done

# "JSI" here means facebook::jsi, the Hermes-only API -- not NativeScript/jsi,
# which is our own platform-neutral engine layer that every backend shares.
# Hence #include "jsi/hermes/..." rather than a blanket #include "jsi/": the
# quoted form now names our tree, and only its hermes subdirectory is off
# limits to the other backends.
if [ "${#NON_HERMES_JSI_DIRS[@]}" -gt 0 ] &&
  search_sources '(NativeApiJsi|facebook::jsi|<jsi/|#include[[:space:]]+"jsi/hermes/)' \
    "${NON_HERMES_JSI_DIRS[@]}"; then
  echo "JSI is Hermes-only; shared, V8, JSC, and QuickJS FFI code must not reference NativeApiJsi or JSI APIs." >&2
  exit 1
fi

if search_sources '(^|[^[:alnum:]_])(EngineDispatch|FastNative|HermesFast|V8Fast|JSCFast|QuickJSFast)($|[^[:alnum:]_])' \
  "$ROOT_DIR/NativeScript/ffi/objc/napi"; then
  echo "Engine FFI code is not allowed in ffi/objc/napi." >&2
  exit 1
fi

if [ -d "$JNI_NAPI_DIR" ] &&
  search_sources '(^|[^[:alnum:]_])(facebook::jsi|v8::|JSContextRef|JSValueRef|JSContext|JSValue|JSRuntime|quickjs)($|[^[:alnum:]_])|(<jsi/|#include[[:space:]]+"jsi/|<v8|JavaScriptCore|quickjs\.h|hermes/)' \
    "$JNI_NAPI_DIR"; then
  # Note: ffi/jni/napi is the *Node-API* JNI backend, so it must not reach into
  # the shared engine layer either -- the blanket #include "jsi/ is deliberate
  # here. The JSI-based JNI backend lands in ffi/jni/jsi, a separate directory.
  echo "ffi/jni/napi must remain a pure Node-API backend until a JNI engine backend is added." >&2
  exit 1
fi

if command -v rg >/dev/null 2>&1; then
  STALE_FFI_PATTERN='NS_FFI_BACKEND=''engine|--ffi-''engine|native-api-''jsi|ffi/(direct|engine)|ffi/objc/(direct|engine|shared/jsi)'
  if rg -n "$STALE_FFI_PATTERN" \
    "$ROOT_DIR/NativeScript" "$ROOT_DIR/scripts" "$ROOT_DIR/packages" \
    "$ROOT_DIR/metadata-generator" "$ROOT_DIR/platforms/apple/benchmarks" \
    -g '!GeneratedSignatureDispatch.inc' \
    -g '!GeneratedGsdSignatureDispatch.inc'; then
    echo "Stale FFI layer names are not allowed." >&2
    exit 1
  fi
fi
