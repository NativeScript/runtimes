# Multi-engine JSI

`@nativescript/ios-jsi` and `@nativescript/android-jsi` give native code a `facebook::jsi::Runtime` for the NativeScript JS thread it runs on. That lets libraries written against JSI (Nitro Modules, MMKV and so on) run inside a NativeScript app.

Right now this only works on V8. This doc covers what it would take for `tns::GetJSIRuntime()` to work on every engine NativeScript ships.

## Where things are today

- Both packages wrap the runtime's V8 isolate in `rnv8::V8Runtime`, which comes from react-native-v8. There is one `jsi::Runtime` per `v8::Isolate::GetCurrent()`, so the main thread and each worker get their own.
- Both packages ship their own copy of the V8 headers (14.9). Android resolves V8 symbols from the `libNativeScript.so` that's already loaded.
- So each package has to be built against the same V8 version as the runtime it runs with. Nothing enforces that yet: neither `package.json` depends on a runtime version.

## What each engine needs

| Engine | Has a `facebook::jsi::Runtime` today? | Work needed |
| :--- | :--- | :--- |
| V8 | Through `rnv8::V8Runtime` (this PR) | Done |
| Hermes | Yes, Hermes *is* a JSI runtime. On iOS, `NativeScript/napi/hermes/jsr.h` already exposes `js_get_jsi_runtime(napi_env)` | Return that pointer. No wrapper |
| JavaScriptCore | No | Wrap the runtime's existing `JSGlobalContextRef` with React Native's `JSCRuntime` (now maintained as `@react-native-community/javascriptcore`) |
| QuickJS | No | There's no maintained upstream implementation. Write one, or adopt a community one |

The same rule applies to every wrapper: it borrows the engine and never owns it. It must not create or dispose the VM, and it must not drain the microtask queue on its own schedule. The runtime controls both.

## Option 1: one JSI implementation per engine

Use Hermes as it is, `rnv8::V8Runtime` for V8, `JSCRuntime` for JSC, and a QuickJS implementation once one exists.

- **Pros:** most of the code already exists and is used in React Native apps. Each engine can be done on its own, starting with whichever matters most.
- **Cons:** on V8, JSC and QuickJS, each VM ends up with two wrapper layers, NativeScript's own and the JSI one. That's a third-party codebase per engine to keep in sync with upstream JSI. QuickJS has no good starting point.

## Option 2: one adapter over `nativescript::engine`

On the `android-react-native` line (#69), `NativeScript/jsi/` holds `nativescript::engine` with backends for V8, JSC, QuickJS and Hermes. It's modelled on JSI but isn't `facebook::jsi`:

- `nativescript::engine` values can be copied. `jsi::Value` and `jsi::Object` can only be moved.
- Each side has its own `JSError`, and neither catches the other's.
- `nativescript::engine` host objects get a `receiver()`. JSI host objects don't.

A single `facebook::jsi::Runtime` built on top of `nativescript::engine` would cover every engine those backends cover. It's the reverse of what `NativeScript/jsi/hermes/HermesRuntime.h` already does: translate exceptions both ways, map copyable values onto JSI's move-only `PointerValue`s, and dispatch host objects without a receiver.

- **Pros:** one implementation for all engines, built on the same code the runtime already runs and tests. No V8-specific code in the package.
- **Cons:** it's real work, and every call goes through an extra layer. Hermes should skip the adapter and return its own runtime directly. It can't start until the engine layer is on `main`.

## Packaging

The public API stays as it is: `tns::GetJSIRuntime()`, `NativeScriptJSIRuntime()`, `JSIRuntime.runtime()` / `runtimePointer()`. Which engine is used gets decided at build time from the installed runtime package (`@nativescript/ios-v8`, `@nativescript/ios-hermes` and so on). The package then:

- depends on an exact runtime version and is released alongside it.
- builds against the headers the runtime itself is built with, instead of shipping its own copy.

## Suggested order

1. **Hermes pass-through.** It's the smallest step, and it shows the API isn't tied to V8.
2. **Pin the runtime version and drop the copied V8 headers.**
3. **JSC** with Option 1, or wait for Option 2 if the engine layer is landing soon.
4. **QuickJS** with Option 2. Writing a standalone QuickJS JSI runtime isn't worth it if the adapter is coming.

## Open questions

- **Android.** `main` doesn't build an Android runtime yet, so on Android these packages currently link against the separately published `@nativescript/android` (V8). Multi-engine Android depends on the engine work in #69.
- **Where it lives.** If the plan is multi-engine, the Hermes pass-through and Option 2 both need runtime APIs and the runtime's own engine layer. That argues for keeping these packages next to the runtime. If it stays V8-only, they can live in a separate repo and depend on a published runtime version.
