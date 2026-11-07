#ifndef CALLBACK_THREADING_H
#define CALLBACK_THREADING_H

#include "js_native_api.h"

#include <atomic>
#include <functional>
#include <memory>

#if defined(ENABLE_JS_RUNTIME)
#include "jsr.h"
#else
#include <Foundation/Foundation.h>

#include <unordered_map>

// Node-API entry points from node_api.h, which this tree does not vendor.
typedef struct napi_async_context__* napi_async_context;
typedef void(NAPI_CDECL* napi_cleanup_hook)(void* arg);
extern "C" {
NAPI_EXTERN napi_status NAPI_CDECL
napi_async_init(napi_env env, napi_value async_resource,
                napi_value async_resource_name, napi_async_context* result);
NAPI_EXTERN napi_status NAPI_CDECL
napi_async_destroy(napi_env env, napi_async_context async_context);
NAPI_EXTERN napi_status NAPI_CDECL napi_make_callback(
    napi_env env, napi_async_context async_context, napi_value recv,
    napi_value func, size_t argc, const napi_value* argv, napi_value* result);
NAPI_EXTERN napi_status NAPI_CDECL napi_add_env_cleanup_hook(
    node_api_basic_env env, napi_cleanup_hook fun, void* arg);
}
#endif

namespace nativescript {

// Reports a JS error thrown by a JS implementation of a native callback: to
// the JS→native call that is waiting for it, if any, otherwise as uncaught.
void reportNativeCallbackError(napi_env env, napi_value error);

namespace detail {

#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
inline std::atomic<int> native_call_unlocked_runtime_count{0};
inline thread_local int native_caller_thread_callback_depth = 0;
#endif

#if !defined(ENABLE_JS_RUNTIME)
// Depth of JS→native calls in progress on this thread. While it is above 0,
// JS is on the stack, so a V8 context is entered and native callbacks can
// call into JS directly.
inline thread_local int napi_native_call_depth = 0;

// Hosts that own the native run loop (Electron's main process, for one) can
// make Objective-C call back into JS with no V8 context entered and no
// HandleScope open. napi_make_callback is the only Node-API call that enters
// the env's context itself, so such callbacks run inside a native trampoline
// invoked that way. Node drains nextTicks and microtasks when the outermost
// callback scope closes.
//
// All callbacks share one async context created at init, so AsyncLocalStorage
// inside a callback sees the values from bridge initialization.
struct HostCallbackState {
  napi_ref trampoline = nullptr;
  napi_ref receiver = nullptr;
  napi_async_context asyncContext = nullptr;
};

struct HostCallback {
  void (*fn)(void*) = nullptr;
  void* data = nullptr;
  bool ran = false;
  NSException* exception = nil;
};

inline thread_local std::unordered_map<napi_env, HostCallbackState>
    host_callback_states;
inline thread_local HostCallback* pending_host_callback = nullptr;

inline napi_value runPendingHostCallback(napi_env env,
                                         napi_callback_info info) {
  HostCallback* callback = pending_host_callback;
  pending_host_callback = nullptr;
  if (callback != nullptr) {
    callback->ran = true;
    // An Objective-C exception must not unwind through V8 frames; it is
    // rethrown once napi_make_callback has returned.
    @try {
      callback->fn(callback->data);
      // Reported here, while the context is still entered: reporting can
      // call back into JS.
      bool pending = false;
      if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        napi_value error = nullptr;
        napi_get_and_clear_last_exception(env, &error);
        reportNativeCallbackError(env, error);
      }
    } @catch (NSException* exception) {
      callback->exception = exception;
    }
  }
  return nullptr;
}

inline void destroyHostCallbackState(void* arg) {
  napi_env env = (napi_env)arg;
  auto it = host_callback_states.find(env);
  if (it == host_callback_states.end()) {
    return;
  }
  napi_async_destroy(env, it->second.asyncContext);
  napi_delete_reference(env, it->second.trampoline);
  napi_delete_reference(env, it->second.receiver);
  host_callback_states.erase(it);
}

class HandleScopeGuard final {
 public:
  explicit HandleScopeGuard(napi_env env) : env_(env) {
    napi_open_handle_scope(env_, &scope_);
  }
  ~HandleScopeGuard() {
    if (scope_ != nullptr) {
      napi_close_handle_scope(env_, scope_);
    }
  }

  HandleScopeGuard(const HandleScopeGuard&) = delete;
  HandleScopeGuard& operator=(const HandleScopeGuard&) = delete;

 private:
  napi_env env_;
  napi_handle_scope scope_ = nullptr;
};
#endif  // !ENABLE_JS_RUNTIME

}  // namespace detail

inline bool isNativeCallRuntimeUnlockedForCallbacks() {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
  return detail::native_call_unlocked_runtime_count.load(
             std::memory_order_acquire) > 0;
#else
  return false;
#endif
}

inline bool isNativeCallerThreadCallbackActive() {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
  return detail::native_caller_thread_callback_depth > 0;
#else
  return false;
#endif
}

inline bool shouldInvokeCallbackOnNativeCallerThread() {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
  return isNativeCallRuntimeUnlockedForCallbacks() ||
         isNativeCallerThreadCallbackActive();
#else
  return false;
#endif
}

class NativeCallRuntimeUnlockScope final {
 public:
  explicit NativeCallRuntimeUnlockScope(napi_env env) {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
    if (isNativeCallerThreadCallbackActive()) {
      return;
    }

    jsr_ = JSR::FromEnv(env);
    if (jsr_ == nullptr) {
      return;
    }

    unlockedDepth_ = js_current_env_lock_depth(env);
    for (int i = 0; i < unlockedDepth_; i++) {
      jsr_->unlock();
    }
    if (unlockedDepth_ == 0 && jsr_->runtime != nullptr) {
      auto* runtime = jsr_->runtime.get();
      runtime->unlock();
      relockRuntime_ = [runtime]() { runtime->lock(); };
      unlockedRuntime_ = true;
    }
    if (unlockedDepth_ > 0 || unlockedRuntime_) {
      didUnlock_ = true;
      detail::native_call_unlocked_runtime_count.fetch_add(
          1, std::memory_order_release);
    }
#else
    (void)env;
#if !defined(ENABLE_JS_RUNTIME)
    detail::napi_native_call_depth += 1;
#endif
#endif
  }

  ~NativeCallRuntimeUnlockScope() {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
    if (didUnlock_) {
      detail::native_call_unlocked_runtime_count.fetch_sub(
          1, std::memory_order_release);
    }
    if (jsr_ != nullptr) {
      for (int i = 0; i < unlockedDepth_; i++) {
        jsr_->lock();
      }
    }
    if (unlockedRuntime_ && relockRuntime_) {
      relockRuntime_();
    }
#elif !defined(ENABLE_JS_RUNTIME)
    detail::napi_native_call_depth -= 1;
#endif
  }

  NativeCallRuntimeUnlockScope(const NativeCallRuntimeUnlockScope&) = delete;
  NativeCallRuntimeUnlockScope& operator=(const NativeCallRuntimeUnlockScope&) =
      delete;

 private:
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
  JSR* jsr_ = nullptr;
  std::function<void()> relockRuntime_;
#endif
  int unlockedDepth_ = 0;
  bool unlockedRuntime_ = false;
  bool didUnlock_ = false;
};

class NativeCallbackScope final {
 public:
  explicit NativeCallbackScope(napi_env env) : env_(env) {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
    if (isNativeCallerThreadCallbackActive() ||
        js_current_env_lock_depth(env_) > 0) {
      napi_open_handle_scope(env_, &napiHandleScope_);
      return;
    }

    if (JSR* jsr = JSR::FromEnv(env_)) {
      jsr_ = jsr;
      jsr_->lock();
      detail::native_caller_thread_callback_depth += 1;
      napi_open_handle_scope(env_, &napiHandleScope_);
      return;
    }
#endif
#if defined(ENABLE_JS_RUNTIME)
    napiScope_ = std::make_unique<NapiScope>(env_);
#else
    (void)env_;
#endif
  }

  ~NativeCallbackScope() {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
    if (napiHandleScope_ != nullptr) {
      napi_close_handle_scope(env_, napiHandleScope_);
    }
    if (jsr_ != nullptr) {
      detail::native_caller_thread_callback_depth -= 1;
      jsr_->unlock();
    }
#endif
  }

  NativeCallbackScope(const NativeCallbackScope&) = delete;
  NativeCallbackScope& operator=(const NativeCallbackScope&) = delete;

#if !defined(ENABLE_JS_RUNTIME)
  // Must run while JS is on the stack (e.g. from module init): it allocates
  // JS objects, which needs an entered V8 context.
  static void initialize(napi_env env) {
    if (detail::host_callback_states.count(env) != 0) {
      return;
    }

    napi_value trampoline = nullptr;
    napi_value receiver = nullptr;
    napi_value resourceName = nullptr;
    detail::HostCallbackState state;
    if (napi_create_function(env, "nativeScriptHostCallback", NAPI_AUTO_LENGTH,
                             detail::runPendingHostCallback, nullptr,
                             &trampoline) != napi_ok ||
        napi_create_object(env, &receiver) != napi_ok ||
        napi_create_string_utf8(env, "NativeScriptHostCallback",
                                NAPI_AUTO_LENGTH, &resourceName) != napi_ok ||
        napi_async_init(env, receiver, resourceName, &state.asyncContext) !=
            napi_ok) {
      return;
    }

    napi_create_reference(env, trampoline, 1, &state.trampoline);
    napi_create_reference(env, receiver, 1, &state.receiver);
    detail::host_callback_states[env] = state;
    napi_add_env_cleanup_hook(env, detail::destroyHostCallbackState, env);
  }

  // Runs fn(data) on the JS thread as an entry from native code into JS.
  // Returns false if fn did not run, in which case the caller must still
  // produce a return value for its native caller.
  static bool run(napi_env env, void (*fn)(void*), void* data) {
    if (detail::napi_native_call_depth > 0) {
      detail::HandleScopeGuard handleScope(env);
      fn(data);
      return true;
    }

    auto it = detail::host_callback_states.find(env);
    if (it == detail::host_callback_states.end()) {
      detail::HandleScopeGuard handleScope(env);
      fn(data);
      return true;
    }

    detail::HostCallback callback;
    callback.fn = fn;
    callback.data = data;
    {
      detail::HandleScopeGuard handleScope(env);
      napi_value receiver = nullptr;
      napi_value trampoline = nullptr;
      napi_get_reference_value(env, it->second.receiver, &receiver);
      napi_get_reference_value(env, it->second.trampoline, &trampoline);

      detail::HostCallback* previous = detail::pending_host_callback;
      detail::pending_host_callback = &callback;
      napi_value result = nullptr;
      napi_make_callback(env, it->second.asyncContext, receiver, trampoline, 0,
                         nullptr, &result);
      detail::pending_host_callback = previous;
    }

    if (callback.exception != nil) {
      @throw callback.exception;
    }
    return callback.ran;
  }
#endif  // !ENABLE_JS_RUNTIME

 private:
  napi_env env_;
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
  JSR* jsr_ = nullptr;
  napi_handle_scope napiHandleScope_ = nullptr;
#endif
#if defined(ENABLE_JS_RUNTIME)
  std::unique_ptr<NapiScope> napiScope_;
#endif
};

}  // namespace nativescript

#endif  // CALLBACK_THREADING_H
