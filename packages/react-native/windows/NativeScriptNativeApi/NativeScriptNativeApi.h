#pragma once

#include "pch.h"
#include "resource.h"

#include "NativeModules.h"

namespace winrt::NativeScriptNativeApi {

// NativeScript for React Native on Windows (react-native-windows, New Architecture).
//
// On iOS, macOS and Android, NativeScript installs into a jsi::Runtime React Native owns: the
// main one, or the Worklets UI runtime. On Windows neither is reachable from a module:
// - A module DLL sees the main runtime only through the JsiRuntime ABI (JsiAbiRuntime), not the
//   engine's own runtime object, and NativeScript's Windows runtime binds through Node-API, not
//   JSI.
// - react-native-worklets has no Windows implementation.
// So the module runs its own Hermes runtime on the UI thread, made through the jsr_* API of the
// hermes.dll react-native-windows already loaded, with @nativescript/windows-napi attached to its
// napi_env: the whole Windows Runtime under `Windows`, plus `XamlWindow` and `NSWinRT`. Worklets
// run there (runWorklet), from the code the Worklets Babel plugin embeds in each one, with their
// captured values and arguments as JSON. See UIRuntime.h.
REACT_MODULE(NativeScriptNativeApi)
struct NativeScriptNativeApi {
  REACT_INIT(Initialize)
  void Initialize(React::ReactContext const &reactContext) noexcept;

  // Starts the UI-thread runtime. Windows has no metadata file: the runtime reads WinRT
  // metadata (.winmd) from the system, so metadataPath is ignored.
  REACT_SYNC_METHOD(install)
  bool install(std::string metadataPath) noexcept;

  // There is no Worklets UI runtime on Windows; runWorklet replaces it.
  REACT_SYNC_METHOD(installWorkletRuntime)
  bool installWorkletRuntime(React::JSValue runtimeHolder, std::string metadataPath) noexcept;

  REACT_SYNC_METHOD(isInstalled)
  bool isInstalled() noexcept;

  REACT_SYNC_METHOD(defaultMetadataPath)
  std::string defaultMetadataPath() noexcept;

  REACT_SYNC_METHOD(getRuntimeBackend)
  std::string getRuntimeBackend() noexcept;

  REACT_SYNC_METHOD(__writeTestMarker)
  bool __writeTestMarker(std::string content) noexcept;

  // Windows only: runs a worklet's code on the UI thread. `payload` is JSON
  // { args, closure, globals } and the result is JSON { ok, value | error, logs }.
  REACT_METHOD(runWorklet)
  void runWorklet(std::string code, std::string payload, React::ReactPromise<std::string> &&result) noexcept;

 private:
  React::ReactContext m_context;
};

} // namespace winrt::NativeScriptNativeApi
