//
// Created by Ammar Ahmed on 01/12/2024.
//

#ifndef TEST_APP_JSC_API_H
#define TEST_APP_JSC_API_H

#include <cassert>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_set>

// Angle brackets deliberately: on Apple this resolves to the SDK's
// JavaScriptCore.framework, on Android to the vendored jsc-android headers on
// the include path. A quoted include would find the vendored copy first on both.
#include <JavaScriptCore/JavaScript.h>
#include "js_native_api.h"
#include "js_native_api_types.h"

struct napi_env__ {
  JSGlobalContextRef context{};
  JSValueRef last_exception{};
  napi_extended_error_info last_error{nullptr, nullptr, 0, napi_ok};
  // Strong (protected) napi references, released on env teardown.
  std::list<napi_ref> strong_refs{};

#ifdef __APPLE__
  // Weak-reference liveness tracking, Apple only.
  //
  // Android backs weak refs with real JSC weak handles (JSWeakCreate et al),
  // which is both cheaper and exact. Those live in JSWeakPrivate.h: the symbols
  // are in Apple's JavaScriptCore.tbd, but no public SDK header declares them,
  // so using them here would mean shipping against JSC SPI. Instead the Apple
  // build keeps the original scheme -- a set of values that still have a live
  // ReferenceInfo finalizer attached -- which needs only public API. See
  // napi_ref__ in jsc-api.cpp for the two implementations.
  std::unordered_set<napi_value> active_ref_values{};
#endif
  // napi_set_instance_data / napi_get_instance_data.
  void* instance_data{};
  napi_finalize instance_data_finalize_cb{};
  void* instance_data_finalize_hint{};

  JSValueRef constructor_info_symbol{};
  JSValueRef function_info_symbol{};
  JSValueRef reference_info_symbol{};
  JSValueRef wrapper_info_symbol{};
  JSValueRef type_tag_symbol{};

  const std::thread::id thread_id{std::this_thread::get_id()};

  // Serializes all host->JS entries into this context. JSC's C API takes its
  // per-VM lock around each individual call, but a logical host->JS callback
  // spans many napi_* calls that must run atomically w.r.t. other threads
  // (background-thread JNI callbacks, timers, worker marshaling). Without this,
  // two threads interleave inside a single JS callback and corrupt the
  // runtime's shared C++ state (ObjectManager/ArgConverter) and JS heap ->
  // SIGSEGV. This mirrors QuickJS's per-env recursive mutex taken by NapiScope.
  // Recursive so a nested NapiScope on the same thread re-enters instead of
  // self-deadlocking.
  std::recursive_mutex js_mutex{};

  napi_env__(JSGlobalContextRef context) : context{context} {
    {
      std::lock_guard<std::mutex> lock(napi_envs_mutex);
      napi_envs[context] = this;
    }
    JSGlobalContextRetain(context);
    init_symbol(constructor_info_symbol, "NS_ConstructorInfo");
    init_symbol(function_info_symbol, "NS_FunctionInfo");
    init_symbol(reference_info_symbol, "NS_ReferenceInfo");
    init_symbol(wrapper_info_symbol, "NS_WrapperInfo");
    init_symbol(type_tag_symbol, "NS_TypeTag");
  }

  ~napi_env__() {
    // All of these (deinit_refs and deinit_symbol) call JSValueUnprotect
    // on `context`, so the context must stay alive until after they run.
    // Releasing it earlier frees the underlying JSC context when this env
    // holds the last reference (e.g. a terminating worker), turning the
    // subsequent unprotect calls into a use-after-free crash.
    deinit_refs();
    deinit_symbol(type_tag_symbol);
    deinit_symbol(wrapper_info_symbol);
    deinit_symbol(reference_info_symbol);
    deinit_symbol(function_info_symbol);
    deinit_symbol(constructor_info_symbol);
    {
      std::lock_guard<std::mutex> lock(napi_envs_mutex);
      napi_envs.erase(context);
    }
    JSGlobalContextRelease(context);
  }

  static napi_env get(JSGlobalContextRef context) {
    std::lock_guard<std::mutex> lock(napi_envs_mutex);
    auto it = napi_envs.find(context);
    if (it != napi_envs.end()) {
      return it->second;
    } else {
      return nullptr;
    }
  }

 private:
  static inline std::mutex napi_envs_mutex;
  static inline std::unordered_map<JSGlobalContextRef, napi_env> napi_envs{};
  void deinit_refs();
  void init_symbol(JSValueRef& symbol, const char* description);
  void deinit_symbol(JSValueRef symbol);
};

// Records `exception` as the env's pending exception, for callers outside
// jsc-api.cpp. The equivalent helper in that file lives in an anonymous
// namespace and so is not reachable from jsr.cpp; `inline` keeps this one from
// producing a duplicate symbol in the two TUs that include this header.
inline napi_status napi_set_pending_exception(napi_env env,
                                              JSValueRef exception) {
  env->last_exception = exception;
  env->last_error = {nullptr, nullptr, 0, napi_pending_exception};
  return napi_pending_exception;
}

// Returns the native pointer a wrapped object carries, or false when the value
// is not a wrapped object. Exported for the Apple FFI layer.
extern "C" bool nativescript_jsc_try_unwrap_native(napi_env env,
                                                   napi_value value,
                                                   void** result);

#define RETURN_STATUS_IF_FALSE(env, condition, status) \
  do {                                                 \
    if (!(condition)) {                                \
      return napi_set_last_error((env), (status));     \
    }                                                  \
  } while (0)

#define CHECK_ENV(env)         \
  do {                         \
    if ((env) == nullptr) {    \
      return napi_invalid_arg; \
    }                          \
  } while (0)

#define CHECK_ARG(env, arg) \
  RETURN_STATUS_IF_FALSE((env), ((arg) != nullptr), napi_invalid_arg)

#define CHECK_JSC(env, exception)                \
  do {                                           \
    if ((exception) != nullptr) {                \
      return napi_set_exception(env, exception); \
    }                                            \
  } while (0)

// This does not call napi_set_last_error because the expression
// is assumed to be a NAPI function call that already did.
#define CHECK_NAPI(expr)                  \
  do {                                    \
    napi_status status = (expr);          \
    if (status != napi_ok) return status; \
  } while (0)

#endif  // TEST_APP_JSC_API_H
