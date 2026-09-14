# Native API Hermes JSI backend

This directory owns the Hermes-facing Native API entrypoint:

- `NativeApiJsi.h` exposes the public JSI install/create API.
- `NativeApiJsi.mm` binds Hermes JSI types to the Hermes-owned bridge
  implementation files in this directory.
- `NativeApiJsiReactNative.h` adapts React Native `CallInvoker`s to the JSI
  scheduler config used by the TurboModule.
- `NativeApiJsiSignatureDispatch.h` wires Hermes generated signature dispatch
  tables into native invocation.

Hermes is the only backend that exposes the real `facebook::jsi` API. V8, JSC,
and QuickJS own their bridge implementations in their respective engine
directories.

React Native integrations should include `NativeApiJsiReactNative.h` from a
TurboModule implementation and pass the module's JS/UI `CallInvoker`s:

```cpp
nativescript::InstallReactNativeNativeApiJSI(
    runtime, jsInvoker, uiInvoker, metadataPath, metadataPtr);
```
