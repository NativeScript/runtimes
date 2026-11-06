#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace NodeApiHost {

// Node's half of Node-API (node_api.h) for a runtime whose engine provides only the engine half
// (js_native_api.h), as Hermes' does: threadsafe functions, env cleanup hooks, buffers, async work
// and callback scopes, exported from this module so a Node addon finds them.
//
// The addon's napi-sys takes each napi_* from the first module in NAPI_HOST_MODULES that exports
// it, so the embedder sets that to "<this module>;<hermes.dll>" before loading the addon.
//
// `post` runs work on the runtime's thread, where every napi_env call must happen; it may be
// called from any thread. `hermes` provides the engine half these functions are built on.
bool Init(HMODULE hermes, std::function<void(std::function<void()>)> post);

// The value for NAPI_HOST_MODULES: this module, then hermes.dll.
std::wstring HostModules(HMODULE hermes);

} // namespace NodeApiHost
