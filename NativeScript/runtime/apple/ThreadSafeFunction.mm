#include <CoreFoundation/CFRunLoop.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "js_native_api.h"
#include "js_native_tsfn.h"
#include "jsr.h"
#include "runtime/apple/Runtime.h"

#ifdef TARGET_ENGINE_V8
#include "v8-api.h"
#endif

typedef struct napi_async_context__* napi_async_context;
typedef struct napi_callback_scope__* napi_callback_scope;
typedef napi_env node_api_basic_env;

typedef struct {
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  const char* release;
} napi_node_version;

typedef void(NAPI_CDECL* napi_cleanup_hook)(void* arg);
typedef struct napi_async_cleanup_hook_handle__* napi_async_cleanup_hook_handle;
typedef void(NAPI_CDECL* napi_async_cleanup_hook)(
    napi_async_cleanup_hook_handle handle, void* data);

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_make_callback(napi_env env, napi_async_context async_context,
                   napi_value recv, napi_value func, size_t argc,
                   const napi_value* argv, napi_value* result);

struct napi_async_context__ {
  napi_env env = nullptr;
  napi_ref resource_ref = nullptr;
  napi_ref resource_name_ref = nullptr;
};

struct napi_callback_scope__ {
  napi_env env = nullptr;
  napi_async_context async_context = nullptr;
  bool closed = false;
};

struct napi_async_cleanup_hook_handle__ {
  node_api_basic_env env = nullptr;
  napi_async_cleanup_hook hook = nullptr;
  void* data = nullptr;
  bool removed = false;
};

struct napi_threadsafe_function__ {
  napi_env env = nullptr;
  napi_ref callback_ref = nullptr;
  napi_threadsafe_function_call_js call_js_cb = nullptr;

  void* context = nullptr;
  void* thread_finalize_data = nullptr;
  napi_finalize thread_finalize_cb = nullptr;

  CFRunLoopRef run_loop = nullptr;

  std::mutex mutex;
  std::condition_variable queue_cv;
  size_t max_queue_size = 0;
  size_t queued_calls = 0;
  size_t thread_count = 0;
  bool closing = false;
  bool finalize_scheduled = false;
};

struct TSFNCall {
  napi_threadsafe_function__* tsfn = nullptr;
  void* data = nullptr;
  bool blocking = false;

  std::mutex done_mutex;
  std::condition_variable done_cv;
  bool done = false;
};

struct EnvCleanupState {
  std::vector<std::pair<napi_cleanup_hook, void*>> env_hooks;
  std::vector<napi_async_cleanup_hook_handle__*> async_hooks;
  std::vector<napi_async_cleanup_hook_handle__*> deferred_delete_async_hooks;
  bool draining_async_hooks = false;
};

static std::mutex& CleanupHooksMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

static std::condition_variable& CleanupHooksCV() {
  static auto* cv = new std::condition_variable();
  return *cv;
}

static std::unordered_map<node_api_basic_env, EnvCleanupState>& CleanupHooks() {
  static auto* hooks = new std::unordered_map<node_api_basic_env, EnvCleanupState>();
  return *hooks;
}

static bool IsCleanupStateEmpty(const EnvCleanupState& state) {
  return state.env_hooks.empty() && state.async_hooks.empty() &&
         state.deferred_delete_async_hooks.empty() && !state.draining_async_hooks;
}

static void EraseCleanupStateIfUnused(node_api_basic_env env) {
  auto& cleanupHooks = CleanupHooks();
  auto it = cleanupHooks.find(env);
  if (it != cleanupHooks.end() && IsCleanupStateEmpty(it->second)) {
    cleanupHooks.erase(it);
  }
}

static size_t ResolveLength(const char* value, size_t value_len) {
  if (value == nullptr) {
    return 0;
  }
  if (value_len == NAPI_AUTO_LENGTH) {
    return std::strlen(value);
  }
  return value_len;
}

static CFRunLoopRef GetRuntimeRunLoop(napi_env env) {
  auto runtime = nativescript::Runtime::GetRuntime(env);
  if (runtime != nullptr && runtime->RuntimeLoop() != nullptr) {
    return runtime->RuntimeLoop();
  }
  return CFRunLoopGetMain();
}

static bool ScheduleOnRunLoop(
    CFRunLoopRef runLoop, std::shared_ptr<std::function<void()>> task) {
  if (runLoop == nullptr) {
    return false;
  }

  if (CFRunLoopGetCurrent() == runLoop) {
    (*task)();
    return true;
  }

  CFRunLoopPerformBlock(runLoop, kCFRunLoopCommonModes, ^{
    (*task)();
  });
  CFRunLoopWakeUp(runLoop);
  return true;
}

static inline void IncrementCallbackScopes(napi_env env) {
#ifdef TARGET_ENGINE_V8
  env->open_callback_scopes++;
#else
  (void)env;
#endif
}

static inline napi_status DecrementCallbackScopes(napi_env env) {
#ifdef TARGET_ENGINE_V8
  if (env->open_callback_scopes <= 0) {
    return napi_callback_scope_mismatch;
  }
  env->open_callback_scopes--;
#else
  (void)env;
#endif
  return napi_ok;
}

static void ScheduleFinalizeIfNeeded(napi_threadsafe_function__* tsfn);

static void FinalizeTSFN(napi_threadsafe_function__* tsfn) {
  NapiScope scope(tsfn->env);

  if (tsfn->thread_finalize_cb != nullptr) {
    tsfn->thread_finalize_cb(tsfn->env, tsfn->thread_finalize_data, nullptr);
  }

  if (tsfn->callback_ref != nullptr) {
    napi_delete_reference(tsfn->env, tsfn->callback_ref);
    tsfn->callback_ref = nullptr;
  }

  if (tsfn->run_loop != nullptr) {
    CFRelease(tsfn->run_loop);
    tsfn->run_loop = nullptr;
  }

  delete tsfn;
}

static void ScheduleFinalizeIfNeeded(napi_threadsafe_function__* tsfn) {
  bool shouldFinalize = false;
  {
    std::lock_guard<std::mutex> lock(tsfn->mutex);
    shouldFinalize = tsfn->closing && tsfn->thread_count == 0 &&
                     tsfn->queued_calls == 0 && !tsfn->finalize_scheduled;
    if (shouldFinalize) {
      tsfn->finalize_scheduled = true;
    }
  }

  if (!shouldFinalize) {
    return;
  }

  auto task = std::make_shared<std::function<void()>>(
      [tsfn]() { FinalizeTSFN(tsfn); });
  if (!ScheduleOnRunLoop(tsfn->run_loop, task)) {
    // If scheduling is impossible (loop already gone), finalize eagerly.
    (*task)();
  }
}

static void ExecuteTSFNCall(const std::shared_ptr<TSFNCall>& call) {
  auto* tsfn = call->tsfn;

  {
    NapiScope scope(tsfn->env);

    napi_value jsCallback = nullptr;
    if (tsfn->callback_ref != nullptr) {
      napi_get_reference_value(tsfn->env, tsfn->callback_ref, &jsCallback);
    }

    if (tsfn->call_js_cb != nullptr) {
      tsfn->call_js_cb(tsfn->env, jsCallback, tsfn->context, call->data);
    } else if (jsCallback != nullptr) {
      napi_value recv = nullptr;
      napi_get_undefined(tsfn->env, &recv);
      napi_make_callback(tsfn->env, nullptr, recv, jsCallback, 0, nullptr,
                         nullptr);
    }
  }

  {
    std::lock_guard<std::mutex> lock(tsfn->mutex);
    if (tsfn->queued_calls > 0) {
      tsfn->queued_calls--;
    }
  }
  tsfn->queue_cv.notify_all();

  if (call->blocking) {
    {
      std::lock_guard<std::mutex> lock(call->done_mutex);
      call->done = true;
    }
    call->done_cv.notify_one();
  }

  ScheduleFinalizeIfNeeded(tsfn);
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_async_init(napi_env env, napi_value async_resource,
                napi_value async_resource_name, napi_async_context* result) {
  if (env == nullptr || result == nullptr) {
    return napi_invalid_arg;
  }

  auto* context = new napi_async_context__();
  context->env = env;

  if (async_resource != nullptr) {
    napi_status status =
        napi_create_reference(env, async_resource, 1, &context->resource_ref);
    if (status != napi_ok) {
      delete context;
      return status;
    }
  }

  if (async_resource_name != nullptr) {
    napi_status status = napi_create_reference(env, async_resource_name, 1,
                                               &context->resource_name_ref);
    if (status != napi_ok) {
      if (context->resource_ref != nullptr) {
        napi_delete_reference(env, context->resource_ref);
      }
      delete context;
      return status;
    }
  }

  *result = context;
  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_async_destroy(napi_env env, napi_async_context async_context) {
  if (env == nullptr || async_context == nullptr) {
    return napi_invalid_arg;
  }

  if (async_context->env != env) {
    return napi_invalid_arg;
  }

  if (async_context->resource_ref != nullptr) {
    napi_delete_reference(env, async_context->resource_ref);
    async_context->resource_ref = nullptr;
  }

  if (async_context->resource_name_ref != nullptr) {
    napi_delete_reference(env, async_context->resource_name_ref);
    async_context->resource_name_ref = nullptr;
  }

  delete async_context;
  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_open_callback_scope(napi_env env, napi_value resource_object,
                         napi_async_context context,
                         napi_callback_scope* result) {
  if (env == nullptr || resource_object == nullptr || context == nullptr ||
      result == nullptr) {
    return napi_invalid_arg;
  }

  auto* scope = new napi_callback_scope__();
  scope->env = env;
  scope->async_context = context;
  IncrementCallbackScopes(env);
  *result = scope;
  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_close_callback_scope(napi_env env, napi_callback_scope scope) {
  if (env == nullptr || scope == nullptr) {
    return napi_invalid_arg;
  }

  if (scope->env != env || scope->closed) {
    return napi_callback_scope_mismatch;
  }

  napi_status status = DecrementCallbackScopes(env);
  scope->closed = true;
  delete scope;
  return status;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_make_callback(napi_env env, napi_async_context async_context,
                   napi_value recv, napi_value func, size_t argc,
                   const napi_value* argv, napi_value* result) {
  if (env == nullptr || recv == nullptr || func == nullptr) {
    return napi_invalid_arg;
  }
  if (argc > 0 && argv == nullptr) {
    return napi_invalid_arg;
  }

  auto invoke = std::make_shared<std::function<napi_status()>>([=]() {
    NapiScope scope(env);

    napi_callback_scope callbackScope = nullptr;
    if (async_context != nullptr) {
      napi_value resourceObject = recv;
      if (async_context->resource_ref != nullptr) {
        napi_get_reference_value(env, async_context->resource_ref,
                                 &resourceObject);
      }

      napi_status openStatus =
          napi_open_callback_scope(env, resourceObject, async_context,
                                   &callbackScope);
      if (openStatus != napi_ok) {
        return openStatus;
      }
    }

    napi_status callStatus =
        napi_call_function(env, recv, func, argc, argv, result);

    if (callbackScope != nullptr) {
      napi_status closeStatus = napi_close_callback_scope(env, callbackScope);
      if (callStatus == napi_ok) {
        callStatus = closeStatus;
      }
    }

    return callStatus;
  });

  CFRunLoopRef runLoop = GetRuntimeRunLoop(env);
  if (runLoop != nullptr && CFRunLoopGetCurrent() != runLoop) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    napi_status status = napi_generic_failure;

    auto task = std::make_shared<std::function<void()>>([&]() {
      status = (*invoke)();
      {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
      }
      cv.notify_one();
    });

    if (!ScheduleOnRunLoop(runLoop, task)) {
      return napi_generic_failure;
    }

    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() { return done; });
    return status;
  }

  return (*invoke)();
}

extern "C" NAPI_EXTERN void NAPI_CDECL
napi_fatal_error(const char* location, size_t location_len,
                 const char* message, size_t message_len) {
  const size_t resolved_location_len = ResolveLength(location, location_len);
  const size_t resolved_message_len = ResolveLength(message, message_len);

  std::string location_str =
      location == nullptr ? std::string() : std::string(location, resolved_location_len);
  std::string message_str =
      message == nullptr ? std::string() : std::string(message, resolved_message_len);

  if (!location_str.empty()) {
    std::fprintf(stderr, "FATAL ERROR: %s: %s\n", location_str.c_str(),
                 message_str.c_str());
  } else {
    std::fprintf(stderr, "FATAL ERROR: %s\n", message_str.c_str());
  }
  std::fflush(stderr);
  std::abort();
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_fatal_exception(napi_env env, napi_value err) {
  if (env == nullptr || err == nullptr) {
    return napi_invalid_arg;
  }
  return napi_throw(env, err);
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_get_node_version(node_api_basic_env env, const napi_node_version** version) {
  (void)env;
  if (version == nullptr) {
    return napi_invalid_arg;
  }

  static const napi_node_version kVersion = {
      0,
      0,
      0,
      "nativescript",
  };
  *version = &kVersion;
  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_add_env_cleanup_hook(node_api_basic_env env, napi_cleanup_hook fun,
                          void* arg) {
  if (env == nullptr || fun == nullptr) {
    return napi_invalid_arg;
  }

  std::lock_guard<std::mutex> lock(CleanupHooksMutex());
  auto& state = CleanupHooks()[env];
  state.env_hooks.emplace_back(fun, arg);
  return napi_ok;
}

napi_status js_add_env_cleanup_hook(napi_env env, js_env_cleanup_hook hook,
                                    void* data) {
  return napi_add_env_cleanup_hook(env, hook, data);
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_remove_env_cleanup_hook(node_api_basic_env env, napi_cleanup_hook fun,
                             void* arg) {
  if (env == nullptr || fun == nullptr) {
    return napi_invalid_arg;
  }

  std::lock_guard<std::mutex> lock(CleanupHooksMutex());
  auto& cleanupHooks = CleanupHooks();
  auto it = cleanupHooks.find(env);
  if (it == cleanupHooks.end()) {
    return napi_invalid_arg;
  }

  auto& hooks = it->second.env_hooks;
  for (auto hook_it = hooks.rbegin(); hook_it != hooks.rend(); ++hook_it) {
    if (hook_it->first == fun && hook_it->second == arg) {
      hooks.erase(std::next(hook_it).base());
      EraseCleanupStateIfUnused(env);
      return napi_ok;
    }
  }

  return napi_invalid_arg;
}

napi_status js_remove_env_cleanup_hook(napi_env env, js_env_cleanup_hook hook,
                                       void* data) {
  return napi_remove_env_cleanup_hook(env, hook, data);
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_add_async_cleanup_hook(node_api_basic_env env, napi_async_cleanup_hook hook,
                            void* arg,
                            napi_async_cleanup_hook_handle* remove_handle) {
  if (env == nullptr || hook == nullptr) {
    return napi_invalid_arg;
  }

  auto* handle = new napi_async_cleanup_hook_handle__();
  handle->env = env;
  handle->hook = hook;
  handle->data = arg;

  std::lock_guard<std::mutex> lock(CleanupHooksMutex());
  auto& state = CleanupHooks()[env];
  state.async_hooks.push_back(handle);
  if (remove_handle != nullptr) {
    *remove_handle = handle;
  }
  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_remove_async_cleanup_hook(napi_async_cleanup_hook_handle remove_handle) {
  if (remove_handle == nullptr) {
    return napi_invalid_arg;
  }

  auto* handle = static_cast<napi_async_cleanup_hook_handle__*>(remove_handle);
  node_api_basic_env env = handle->env;

  std::lock_guard<std::mutex> lock(CleanupHooksMutex());
  auto& cleanupHooks = CleanupHooks();
  auto it = cleanupHooks.find(env);
  if (it == cleanupHooks.end() || handle->removed) {
    return napi_invalid_arg;
  }

  auto& state = it->second;
  auto vec_it =
      std::find(state.async_hooks.begin(), state.async_hooks.end(), handle);
  if (vec_it == state.async_hooks.end()) {
    return napi_invalid_arg;
  }

  handle->removed = true;
  state.async_hooks.erase(vec_it);

  if (state.draining_async_hooks) {
    state.deferred_delete_async_hooks.push_back(handle);
    if (state.async_hooks.empty()) {
      CleanupHooksCV().notify_all();
    }
  } else {
    delete handle;
    EraseCleanupStateIfUnused(env);
  }

  return napi_ok;
}

void js_run_env_cleanup_hooks(napi_env env) {
  if (env == nullptr) {
    return;
  }

  std::vector<std::pair<napi_cleanup_hook, void*>> env_hooks_to_run;
  std::vector<napi_async_cleanup_hook_handle__*> async_hooks_to_run;
  {
    std::lock_guard<std::mutex> lock(CleanupHooksMutex());
    auto& cleanupHooks = CleanupHooks();
    auto state_it = cleanupHooks.find(env);
    if (state_it == cleanupHooks.end()) {
      return;
    }

    auto& state = state_it->second;
    env_hooks_to_run.swap(state.env_hooks);
    state.draining_async_hooks = true;
    async_hooks_to_run = state.async_hooks;
  }

  for (auto it = env_hooks_to_run.rbegin(); it != env_hooks_to_run.rend(); ++it) {
    if (it->first != nullptr) {
      it->first(it->second);
    }
  }

  for (auto it = async_hooks_to_run.rbegin(); it != async_hooks_to_run.rend(); ++it) {
    auto* handle = *it;

    napi_async_cleanup_hook hook = nullptr;
    void* data = nullptr;
    bool should_invoke = false;
    {
      std::lock_guard<std::mutex> lock(CleanupHooksMutex());
      auto& cleanupHooks = CleanupHooks();
      auto state_it = cleanupHooks.find(env);
      if (state_it == cleanupHooks.end()) {
        break;
      }

      auto& state = state_it->second;
      auto vec_it =
          std::find(state.async_hooks.begin(), state.async_hooks.end(), handle);
      if (vec_it != state.async_hooks.end() && !handle->removed) {
        hook = handle->hook;
        data = handle->data;
        should_invoke = true;
      }
    }

    if (should_invoke && hook != nullptr) {
      hook(handle, data);
    }
  }

  std::vector<napi_async_cleanup_hook_handle__*> handles_to_delete;
  {
    std::unique_lock<std::mutex> lock(CleanupHooksMutex());
    CleanupHooksCV().wait(lock, [&]() {
      auto& cleanupHooks = CleanupHooks();
      auto state_it = cleanupHooks.find(env);
      return state_it == cleanupHooks.end() ||
             state_it->second.async_hooks.empty();
    });

    auto& cleanupHooks = CleanupHooks();
    auto state_it = cleanupHooks.find(env);
    if (state_it == cleanupHooks.end()) {
      return;
    }

    auto& state = state_it->second;
    state.draining_async_hooks = false;
    handles_to_delete.swap(state.deferred_delete_async_hooks);

    if (IsCleanupStateEmpty(state)) {
      cleanupHooks.erase(state_it);
    }
  }

  for (auto* handle : handles_to_delete) {
    delete handle;
  }
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_create_threadsafe_function(
    napi_env env, napi_value func, napi_value async_resource,
    napi_value async_resource_name, size_t max_queue_size,
    size_t initial_thread_count, void* thread_finalize_data,
    napi_finalize thread_finalize_cb, void* context,
    napi_threadsafe_function_call_js call_js_cb,
    napi_threadsafe_function* result) {
  (void)async_resource;
  (void)async_resource_name;

  if (env == nullptr || result == nullptr || initial_thread_count == 0) {
    return napi_invalid_arg;
  }

  auto* tsfn = new napi_threadsafe_function__();
  tsfn->env = env;
  tsfn->call_js_cb = call_js_cb;
  tsfn->context = context;
  tsfn->thread_finalize_data = thread_finalize_data;
  tsfn->thread_finalize_cb = thread_finalize_cb;
  tsfn->max_queue_size = max_queue_size;
  tsfn->thread_count = initial_thread_count;
  tsfn->run_loop = GetRuntimeRunLoop(env);
  if (tsfn->run_loop != nullptr) {
    CFRetain(tsfn->run_loop);
  }

  if (func != nullptr) {
    napi_status status = napi_create_reference(env, func, 1, &tsfn->callback_ref);
    if (status != napi_ok) {
      if (tsfn->run_loop != nullptr) {
        CFRelease(tsfn->run_loop);
      }
      delete tsfn;
      return status;
    }
  }

  *result = tsfn;
  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_get_threadsafe_function_context(napi_threadsafe_function funcOpaque,
                                     void** result) {
  if (funcOpaque == nullptr || result == nullptr) {
    return napi_invalid_arg;
  }

  auto* func = static_cast<napi_threadsafe_function__*>(funcOpaque);
  *result = func->context;
  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_call_threadsafe_function(napi_threadsafe_function funcOpaque, void* data,
                              napi_threadsafe_function_call_mode is_blocking) {
  if (funcOpaque == nullptr) {
    return napi_invalid_arg;
  }

  auto* tsfn = static_cast<napi_threadsafe_function__*>(funcOpaque);
  {
    std::unique_lock<std::mutex> lock(tsfn->mutex);
    while (tsfn->max_queue_size > 0 &&
           tsfn->queued_calls >= tsfn->max_queue_size && !tsfn->closing) {
      if (is_blocking == napi_tsfn_nonblocking) {
        return napi_queue_full;
      }
      if (CFRunLoopGetCurrent() == tsfn->run_loop) {
        return napi_would_deadlock;
      }
      tsfn->queue_cv.wait(lock, [&]() {
        return tsfn->closing ||
               (tsfn->queued_calls < tsfn->max_queue_size);
      });
    }

    if (tsfn->closing) {
      return napi_closing;
    }

    tsfn->queued_calls++;
  }

  auto call = std::make_shared<TSFNCall>();
  call->tsfn = tsfn;
  call->data = data;
  call->blocking = (is_blocking == napi_tsfn_blocking);

  auto task = std::make_shared<std::function<void()>>(
      [call]() { ExecuteTSFNCall(call); });
  if (!ScheduleOnRunLoop(tsfn->run_loop, task)) {
    {
      std::lock_guard<std::mutex> lock(tsfn->mutex);
      if (tsfn->queued_calls > 0) {
        tsfn->queued_calls--;
      }
    }
    tsfn->queue_cv.notify_all();
    return napi_generic_failure;
  }

  if (call->blocking) {
    std::unique_lock<std::mutex> lock(call->done_mutex);
    call->done_cv.wait(lock, [&]() { return call->done; });
  }

  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_acquire_threadsafe_function(napi_threadsafe_function funcOpaque) {
  if (funcOpaque == nullptr) {
    return napi_invalid_arg;
  }

  auto* tsfn = static_cast<napi_threadsafe_function__*>(funcOpaque);
  std::lock_guard<std::mutex> lock(tsfn->mutex);
  if (tsfn->closing) {
    return napi_closing;
  }
  tsfn->thread_count++;
  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_release_threadsafe_function(
    napi_threadsafe_function funcOpaque,
    napi_threadsafe_function_release_mode mode) {
  if (funcOpaque == nullptr) {
    return napi_invalid_arg;
  }

  auto* tsfn = static_cast<napi_threadsafe_function__*>(funcOpaque);
  {
    std::lock_guard<std::mutex> lock(tsfn->mutex);
    if (tsfn->thread_count == 0) {
      return napi_closing;
    }

    if (mode == napi_tsfn_abort) {
      tsfn->closing = true;
      tsfn->thread_count = 0;
    } else {
      tsfn->thread_count--;
      if (tsfn->thread_count == 0) {
        tsfn->closing = true;
      }
    }
  }

  tsfn->queue_cv.notify_all();
  ScheduleFinalizeIfNeeded(tsfn);
  return napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_ref_threadsafe_function(napi_env env, napi_threadsafe_function funcOpaque) {
  (void)env;
  return funcOpaque == nullptr ? napi_invalid_arg : napi_ok;
}

extern "C" NAPI_EXTERN napi_status NAPI_CDECL
napi_unref_threadsafe_function(napi_env env,
                               napi_threadsafe_function funcOpaque) {
  (void)env;
  return funcOpaque == nullptr ? napi_invalid_arg : napi_ok;
}
