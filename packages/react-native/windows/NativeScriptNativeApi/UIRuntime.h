#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace NativeScriptRN {

// A Hermes runtime on React Native's UI thread with NativeScript's Windows runtime
// (@nativescript/windows-napi) attached. Everything here runs on the UI thread: post to it.
//
// - Engine: the hermes.dll react-native-windows loaded (or one beside the app), reached through
//   its jsr_* and napi_* exports, so the module links no engine of its own.
// - NativeScript: the windows-napi addon (a DLL exporting napi_register_module_v1) and its JS
//   helper layer nswinrt.js, embedded in this module as resources and extracted on first start.
//   The addon takes the engine half of Node-API from hermes.dll and Node's half from this module
//   (NodeApiHost.h).
class UIRuntime {
 public:
  using Done = std::function<void(std::string json)>;
  using Post = std::function<void(std::function<void()>)>;

  // `post` queues work on the UI thread; `resources` is the module holding the embedded addon.
  static std::shared_ptr<UIRuntime> Create(Post post, HMODULE resources);

  // Starts the runtime if it isn't running yet; returns why it can't start, or "".
  std::string Start();
  bool Started() const;

  // Runs a worklet's code (a function expression) with payload JSON { args, closure, globals };
  // calls `done` with JSON { ok: true, value, logs } or { ok: false, error, logs }, now or once a
  // returned promise settles.
  void Run(std::string const &code, std::string const &payload, Done done);

  struct Impl;

 private:
  std::shared_ptr<Impl> impl_;
};

} // namespace NativeScriptRN
