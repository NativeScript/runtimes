// Node's half of Node-API over an engine that provides only js_native_api.h. See NodeApiHost.h.
// Built without the project's precompiled header: it needs nothing beyond Win32.
#include <windows.h>

#include "NodeApiHost.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

typedef struct napi_env__ *napi_env;
typedef struct napi_value__ *napi_value;
typedef struct napi_ref__ *napi_ref;
typedef struct napi_handle_scope__ *napi_handle_scope;
typedef int napi_status;
typedef void (*napi_finalize)(napi_env env, void *data, void *hint);
typedef void (*napi_threadsafe_function_call_js)(napi_env env, napi_value js_callback, void *context, void *data);
typedef void (*napi_async_execute_callback)(napi_env env, void *data);
typedef void (*napi_async_complete_callback)(napi_env env, napi_status status, void *data);

constexpr napi_status napi_ok = 0;
constexpr napi_status napi_invalid_arg = 1;
constexpr napi_status napi_generic_failure = 9;
constexpr int napi_uint8_array = 1;
constexpr int napi_tsfn_abort = 1;

struct Engine {
  napi_status(__cdecl *create_reference)(napi_env, napi_value, uint32_t, napi_ref *);
  napi_status(__cdecl *delete_reference)(napi_env, napi_ref);
  napi_status(__cdecl *get_reference_value)(napi_env, napi_ref, napi_value *);
  napi_status(__cdecl *get_undefined)(napi_env, napi_value *);
  napi_status(__cdecl *get_global)(napi_env, napi_value *);
  napi_status(__cdecl *call_function)(napi_env, napi_value, napi_value, size_t, const napi_value *, napi_value *);
  napi_status(__cdecl *open_handle_scope)(napi_env, napi_handle_scope *);
  napi_status(__cdecl *close_handle_scope)(napi_env, napi_handle_scope);
  napi_status(__cdecl *create_arraybuffer)(napi_env, size_t, void **, napi_value *);
  napi_status(__cdecl *create_external_arraybuffer)(napi_env, void *, size_t, napi_finalize, void *, napi_value *);
  napi_status(__cdecl *create_typedarray)(napi_env, int, size_t, napi_value, size_t, napi_value *);
  napi_status(__cdecl *get_typedarray_info)(napi_env, napi_value, int *, size_t *, void **, napi_value *, size_t *);
  napi_status(__cdecl *is_typedarray)(napi_env, napi_value, bool *);
};

Engine g_engine{};
std::function<void(std::function<void()>)> g_post;

// Runs `work` on the runtime's thread inside a handle scope, as Node runs a threadsafe function's
// call or an async work's completion.
void OnRuntimeThread(napi_env env, std::function<void()> work) {
  g_post([env, work = std::move(work)]() {
    napi_handle_scope scope{};
    g_engine.open_handle_scope(env, &scope);
    work();
    g_engine.close_handle_scope(env, scope);
  });
}

struct ThreadsafeFunction {
  napi_env env;
  napi_ref callback;
  void *context;
  napi_threadsafe_function_call_js callJs;
  napi_finalize finalize;
  void *finalizeData;
  std::atomic<int> threads;
  std::atomic<bool> closing{false};
};

void Finalize(ThreadsafeFunction *fn) {
  OnRuntimeThread(fn->env, [fn]() {
    if (fn->finalize) fn->finalize(fn->env, fn->finalizeData, fn->context);
    if (fn->callback) g_engine.delete_reference(fn->env, fn->callback);
    delete fn;
  });
}

struct AsyncWork {
  napi_env env;
  napi_async_execute_callback execute;
  napi_async_complete_callback complete;
  void *data;
};

struct NodeVersion {
  uint32_t major, minor, patch;
  const char *release;
};
const NodeVersion kVersion{22, 0, 0, "nativescript-react-native-windows"};

} // namespace

namespace NodeApiHost {

bool Init(HMODULE hermes, std::function<void(std::function<void()>)> post) {
  g_post = std::move(post);
  bool ok = true;
  auto get = [&](auto &fn, const char *name) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(hermes, name));
    ok = ok && fn != nullptr;
  };
  get(g_engine.create_reference, "napi_create_reference");
  get(g_engine.delete_reference, "napi_delete_reference");
  get(g_engine.get_reference_value, "napi_get_reference_value");
  get(g_engine.get_undefined, "napi_get_undefined");
  get(g_engine.get_global, "napi_get_global");
  get(g_engine.call_function, "napi_call_function");
  get(g_engine.open_handle_scope, "napi_open_handle_scope");
  get(g_engine.close_handle_scope, "napi_close_handle_scope");
  get(g_engine.create_arraybuffer, "napi_create_arraybuffer");
  get(g_engine.create_external_arraybuffer, "napi_create_external_arraybuffer");
  get(g_engine.create_typedarray, "napi_create_typedarray");
  get(g_engine.get_typedarray_info, "napi_get_typedarray_info");
  get(g_engine.is_typedarray, "napi_is_typedarray");
  return ok;
}

std::wstring HostModules(HMODULE hermes) {
  auto path = [](HMODULE module) {
    std::wstring out(MAX_PATH, L'\0');
    for (;;) {
      auto n = GetModuleFileNameW(module, out.data(), static_cast<DWORD>(out.size()));
      if (n < out.size()) {
        out.resize(n);
        return out;
      }
      out.resize(out.size() * 2);
    }
  };
  HMODULE self{};
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&HostModules),
      &self);
  return path(self) + L";" + path(hermes);
}

} // namespace NodeApiHost

#define NODE_API extern "C" __declspec(dllexport) napi_status __cdecl

// ── threadsafe functions ──────────────────────────────────────────────────────

NODE_API napi_create_threadsafe_function(
    napi_env env,
    napi_value func,
    napi_value /*async_resource*/,
    napi_value /*async_resource_name*/,
    size_t /*max_queue_size*/,
    size_t initial_thread_count,
    void *thread_finalize_data,
    napi_finalize thread_finalize_cb,
    void *context,
    napi_threadsafe_function_call_js call_js_cb,
    void **result) {
  if (!result || (!func && !call_js_cb)) return napi_invalid_arg;
  auto *fn = new ThreadsafeFunction{env, nullptr, context, call_js_cb, thread_finalize_cb, thread_finalize_data};
  fn->threads = static_cast<int>(initial_thread_count);
  if (func && g_engine.create_reference(env, func, 1, &fn->callback) != napi_ok) {
    delete fn;
    return napi_generic_failure;
  }
  *result = fn;
  return napi_ok;
}

NODE_API napi_call_threadsafe_function(void *func, void *data, int /*is_blocking*/) {
  auto *fn = static_cast<ThreadsafeFunction *>(func);
  if (!fn || fn->closing) return napi_generic_failure;
  OnRuntimeThread(fn->env, [fn, data]() {
    napi_value callback{};
    if (fn->callback) g_engine.get_reference_value(fn->env, fn->callback, &callback);
    if (fn->callJs) {
      fn->callJs(fn->env, callback, fn->context, data);
    } else if (callback) {
      napi_value receiver, ignored;
      g_engine.get_undefined(fn->env, &receiver);
      g_engine.call_function(fn->env, receiver, callback, 0, nullptr, &ignored);
    }
  });
  return napi_ok;
}

NODE_API napi_acquire_threadsafe_function(void *func) {
  auto *fn = static_cast<ThreadsafeFunction *>(func);
  if (!fn || fn->closing) return napi_generic_failure;
  fn->threads++;
  return napi_ok;
}

NODE_API napi_release_threadsafe_function(void *func, int mode) {
  auto *fn = static_cast<ThreadsafeFunction *>(func);
  if (!fn) return napi_invalid_arg;
  if (mode == napi_tsfn_abort || --fn->threads <= 0) {
    if (!fn->closing.exchange(true)) Finalize(fn);
  }
  return napi_ok;
}

NODE_API napi_get_threadsafe_function_context(void *func, void **result) {
  if (!func || !result) return napi_invalid_arg;
  *result = static_cast<ThreadsafeFunction *>(func)->context;
  return napi_ok;
}

// The runtime's thread is React Native's UI thread, which outlives every function: nothing to
// keep alive or let go.
NODE_API napi_ref_threadsafe_function(napi_env, void *) {
  return napi_ok;
}

NODE_API napi_unref_threadsafe_function(napi_env, void *) {
  return napi_ok;
}

// ── env cleanup hooks: the runtime lives as long as the app, so they never run ─────

NODE_API napi_add_env_cleanup_hook(napi_env, void (*)(void *), void *) {
  return napi_ok;
}

NODE_API napi_remove_env_cleanup_hook(napi_env, void (*)(void *), void *) {
  return napi_ok;
}

NODE_API napi_add_async_cleanup_hook(napi_env, void (*)(void *, void *), void *, void **remove_handle) {
  if (remove_handle) *remove_handle = nullptr;
  return napi_ok;
}

NODE_API napi_remove_async_cleanup_hook(void *) {
  return napi_ok;
}

// ── buffers, as Uint8Arrays ────────────────────────────────────────────────────

NODE_API napi_create_buffer(napi_env env, size_t length, void **data, napi_value *result) {
  napi_value buffer;
  if (auto status = g_engine.create_arraybuffer(env, length, data, &buffer); status != napi_ok) return status;
  return g_engine.create_typedarray(env, napi_uint8_array, length, buffer, 0, result);
}

NODE_API napi_create_buffer_copy(napi_env env, size_t length, const void *source, void **data, napi_value *result) {
  void *bytes{};
  if (auto status = napi_create_buffer(env, length, &bytes, result); status != napi_ok) return status;
  if (length) memcpy(bytes, source, length);
  if (data) *data = bytes;
  return napi_ok;
}

NODE_API napi_create_external_buffer(
    napi_env env,
    size_t length,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  napi_value buffer;
  if (auto status = g_engine.create_external_arraybuffer(env, data, length, finalize_cb, finalize_hint, &buffer);
      status != napi_ok) {
    return status;
  }
  return g_engine.create_typedarray(env, napi_uint8_array, length, buffer, 0, result);
}

NODE_API napi_get_buffer_info(napi_env env, napi_value value, void **data, size_t *length) {
  int type;
  size_t count, offset;
  napi_value buffer;
  return g_engine.get_typedarray_info(env, value, &type, length ? length : &count, data, &buffer, &offset);
}

NODE_API napi_is_buffer(napi_env env, napi_value value, bool *result) {
  return g_engine.is_typedarray(env, value, result);
}

// ── async work: executes on a thread of its own, completes on the runtime's ──────

NODE_API napi_create_async_work(
    napi_env env,
    napi_value,
    napi_value,
    napi_async_execute_callback execute,
    napi_async_complete_callback complete,
    void *data,
    void **result) {
  if (!execute || !result) return napi_invalid_arg;
  *result = new AsyncWork{env, execute, complete, data};
  return napi_ok;
}

NODE_API napi_queue_async_work(napi_env, void *work) {
  auto *w = static_cast<AsyncWork *>(work);
  if (!w) return napi_invalid_arg;
  std::thread([w]() {
    w->execute(w->env, w->data);
    OnRuntimeThread(w->env, [w]() {
      if (w->complete) w->complete(w->env, napi_ok, w->data);
    });
  }).detach();
  return napi_ok;
}

NODE_API napi_delete_async_work(napi_env, void *work) {
  // Deleted by its owner from the completion callback, as Node allows.
  delete static_cast<AsyncWork *>(work);
  return napi_ok;
}

NODE_API napi_cancel_async_work(napi_env, void *) {
  return napi_generic_failure;
}

// ── callback scopes and async context: microtasks drain after each task anyway ──

NODE_API napi_async_init(napi_env, napi_value, napi_value, void **result) {
  if (result) *result = nullptr;
  return napi_ok;
}

NODE_API napi_async_destroy(napi_env, void *) {
  return napi_ok;
}

NODE_API napi_open_callback_scope(napi_env, napi_value, void *, void **result) {
  if (result) *result = nullptr;
  return napi_ok;
}

NODE_API napi_close_callback_scope(napi_env, void *) {
  return napi_ok;
}

NODE_API napi_make_callback(
    napi_env env,
    void *,
    napi_value recv,
    napi_value func,
    size_t argc,
    const napi_value *argv,
    napi_value *result) {
  return g_engine.call_function(env, recv, func, argc, argv, result);
}

// ── the rest of node_api.h ─────────────────────────────────────────────────────

NODE_API napi_get_node_version(napi_env, const NodeVersion **result) {
  if (!result) return napi_invalid_arg;
  *result = &kVersion;
  return napi_ok;
}

NODE_API napi_get_uv_event_loop(napi_env, void **) {
  return napi_generic_failure;
}

extern "C" __declspec(dllexport) void __cdecl napi_module_register(void *) {}

extern "C" __declspec(dllexport) void __cdecl napi_fatal_error(
    const char *location,
    size_t location_len,
    const char *message,
    size_t message_len) {
  std::string text = "Node-API fatal error";
  if (location) text += " at " + std::string(location, location_len == SIZE_MAX ? strlen(location) : location_len);
  if (message) text += ": " + std::string(message, message_len == SIZE_MAX ? strlen(message) : message_len);
  OutputDebugStringA(text.c_str());
  fprintf(stderr, "%s\n", text.c_str());
  std::abort();
}
