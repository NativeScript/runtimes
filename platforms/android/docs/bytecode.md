# Compile-time bytecode

For **release** builds, NativeScript Android can ship pre-compiled engine
bytecode instead of plain JavaScript. Parsing + compiling JS is a large part of
cold-start cost; shipping bytecode moves that work to build time and gives a much
faster time-to-interactive (TTI).

The mechanism is **engine-generic**. The only engine-specific piece is running
that engine's compiler binary, which is encapsulated in a small adapter under
[`tools/bytecode-compiler/lib`](../tools/bytecode-compiler/lib).

| Engine  | Bytecode | Compiler CLI | Status |
| ------- | -------- | ------------ | ------ |
| Hermes  | ✅ HBC   | `hermesc` (upstream) | wired end-to-end |
| QuickJS-NG | ✅    | `nsbc-quickjs-ng` shim (`native/qjs-compile.c`) | wired end-to-end; also serves the `QUICKJS` alias |
| PrimJS  | ✅       | `nsbc-primjs` shim (`native/primjs-compile.c`) | wired end-to-end |
| V8      | ⚠️       | runtime code-cache only (no AOT format) | see [v8-code-cache.md](./v8-code-cache.md) |
| JSC     | ⚠️       | runtime code-cache only (no AOT format) | `js_run_bytecode_file` falls back to source |

All three AOT engines are enabled (`ready: true`) and have a runtime read side.
Their compiler CLIs are built by the
[bytecode-compilers workflow](../../../.github/workflows/bytecode-compilers.yml);
an engine with no binary for the build host simply ships source. V8 and JSC have
no compile-time format at all — both cache code at runtime instead
(`js_cache_script` / `js_run_cached_script`), so their `js_run_bytecode_file`
returns `napi_cannot_run_js` by design, not because it is unimplemented.

There is no bellard QuickJS lane: that tree was dropped, and the runtime's
`QUICKJS` is an alias for `QUICKJS_NG`, which the `quickjs-ng` adapter answers to
under both names.

### Bytecode container (non-Hermes engines)

`hermesc` emits a self-identifying HBC blob. QuickJS's `qjsc` emits a C array, not
a loadable blob, so the QuickJS-family shims (`native/`) wrap the engine's
`JS_WriteObject` output in a small container the runtime detects:

```
[8-byte magic][4-byte format version, little-endian][JS_WriteObject payload]
```

Magic is per engine (`NSBCNGS\0`, `NSBCPJS\0`) and must match the adapter's
`magic` and the runtime detection. The runtime read side strips the 12-byte
header and hands the payload to `JS_ReadObject` + `JS_EvalFunction` (PrimJS: the
`LEPUS_*` equivalents).

## How it works

1. A normal app compiles its JS bundle and copies it into `assets/app` before the
   Android build starts.
2. During a **release** build, after Gradle merges the assets, the
   `compileBytecode` task runs the generic driver over the merged `app/` folder.
   The driver replaces every `.js` file with bytecode for the active engine.
   **The files keep their `.js` names**, so module resolution is unaffected.
3. At load time the runtime reads the file header. If it sees the engine's
   bytecode magic (for Hermes, `0x1F1903C103BC1FC6`) it runs the bytecode
   directly; otherwise it falls back to compiling the source. Detection is
   per-file and authoritative — there is no separate flag that can get out of
   sync.

The C++ loader calls a single engine hook for both the `require` path
([ModuleInternal.cpp](../../../NativeScript/runtime/android/napi/modules/module/ModuleInternal.cpp))
and the raw-script path
([Runtime::RunScript](../../../NativeScript/runtime/android/napi/Runtime.cpp)):

```c
// napi/common/jsr_common.h
napi_status js_run_bytecode_file(napi_env env, const char *file, napi_value *result);
```

The runtime has two trees — `runtime/android/napi` (the default) and
`runtime/android/jsi` — and each carries its own loader. The napi side goes
through the hook above; the jsi side detects the container in
[EngineHost::ExecuteBytecodeFile](../../../NativeScript/runtime/android/jsi/EngineHost.cpp).
A change to one is not a change to the other.

### Module wrapping

The runtime wraps every `require`d module in a function before executing it:

```js
(function(module, exports, require, __filename, __dirname){ /* module source */
})
```

Running a module's bytecode must yield that same wrapper function, so the build
step wraps the source with the **identical** prologue/epilogue before compiling
(the completion value of the compiled program is the wrapper function — verified
by disassembly: the Hermes global function ends with `CreateClosure; Ret`). This
wrapping is engine-independent, so it lives in the generic driver.

Files the runtime runs **unwrapped** as raw scripts (via `Runtime::RunScript`,
e.g. `internal/ts_helpers.js`) are compiled as-is. The build's `--raw` list keeps
these in sync (default: `internal/ts_helpers.js`).

The prologue/epilogue live in several places that must stay byte-for-byte
identical (`MODULE_PROLOGUE` / `MODULE_EPILOGUE` in each):

- `NativeScript/runtime/android/napi/modules/module/ModuleInternal.cpp`
- `NativeScript/runtime/android/jsi/modules/module/ModuleInternal.cpp`
- `tools/bytecode-compiler/compile-bytecode.js`

(The Apple runtime carries its own copy under `runtime/apple/`.)

## The compiler tooling

The compiler lives in [`tools/bytecode-compiler`](../tools/bytecode-compiler) and
is copied into the framework template under `build-tools/bytecode-compiler` when
the runtime is packaged (see `copyFilesToProjectTemeplate` in the root
`build.gradle`). Only the active engine's host binaries are shipped, so a V8
template doesn't carry `hermesc`.

Compiler binaries are host-specific, laid out as
`bin/<engine>/<host>/<compiler>` where `<host>` is `<platform>-<arch>`
(`darwin-arm64`, `darwin-x64`, `linux-x64`, `linux-arm64`, `win32-x64`). A host
with no binary is simply absent — PrimJS, for instance, ships only the three Unix
hosts its GN toolchain can build. The driver detects the current host
automatically and, if there is no compiler for it, leaves the app as source.

## Enabling / disabling

Bytecode is **on by default** for release builds when the active engine has a
ready compiler for the build host. To disable it (ship plain JS):

```
./gradlew assembleRelease -Prelease -PnsBytecodeDisabled
```

Gradle properties (all optional):

| Property                       | Effect                                            |
| ------------------------------ | ------------------------------------------------- |
| `-PnsBytecodeDisabled`         | Disable bytecode for this release build           |
| `-PnsBytecodeSourceMaps`       | Emit `<file>.map` next to each file when supported |
| `-PbytecodeToolsDir=<dir>`     | Override the bytecode-compiler tools folder       |
| `-PbytecodeCompilerBinary=<p>` | Force a specific compiler binary                  |
| `-PnodePath=<path>`            | Override the `node` executable                    |

The compile step never touches the app source — it only rewrites the merged-assets
build intermediate that gets packaged into the APK, and it is idempotent (files
already in bytecode form are skipped), so incremental builds are safe.

## Source maps

With `-PnsBytecodeSourceMaps`, engines whose compiler supports it (Hermes) emit a
source map next to each file (`<file>.map`). Because the module wrapper is a
single-line prologue with no newlines, line numbers are preserved (only a
column offset on line 1).

## Version compatibility

A compiler and the runtime's engine library must share the same bytecode version,
so both must come from the same engine checkout. The workflow inputs are pinned to
exact commits for this reason — see the header of
[bytecode-compilers.yml](../../../.github/workflows/bytecode-compilers.yml).

- **Hermes** — `BYTECODE_VERSION`, currently 99. The pinned `hermes_ref` must match
  `UPSTREAM_COMMIT` in build-hermes, which produced the prebuilt both platforms link.
- **QuickJS-NG** — `BC_VERSION`, currently 21 (`vendor/quickjs/source_ng/quickjs.c`,
  upstream v0.11.0). The compiler stamps its own value and `JS_ReadObject` accepts
  only its own, so a compiler built from a different ref produces blobs that load
  nowhere.

A mismatch is caught at load: the magic/version check fails and the file is
rejected rather than misinterpreted. Note the failure mode is a **fallback**, not a
crash — the runtime falls back to source, so the app still works and the only
visible symptom is that the TTI win quietly disappears.

## Adding an engine

1. Add `tools/bytecode-compiler/lib/<engine>.js` implementing the adapter contract
   (documented in `lib/index.js`) and register it in `lib/index.js`.
2. Drop the host compiler binaries under `tools/bytecode-compiler/bin/<engine>/<host>/`.
3. Implement runtime execution in `napi/<engine>/jsr.cpp` — `js_run_bytecode_file`
   must detect that engine's bytecode magic and run it, setting `*result` to the
   completion value (the module wrapper function for modules). Returning
   `napi_cannot_run_js` means "fall back to source", which is what V8 and JSC do.
   If the engine is also wired into the jsi tree, add the matching detection to
   `runtime/android/jsi/EngineHost.cpp`.
4. Flip the adapter's `ready` flag to `true` **only** once both the compiler and
   the runtime support exist — otherwise the build would ship bytecode the runtime
   can't execute.
