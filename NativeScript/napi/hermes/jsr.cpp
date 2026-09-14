#include "jsr.h"

#include "jsr_common.h"
#include "js_runtime.h"

#include <functional>

using namespace facebook::jsi;
std::unordered_map<napi_env, JSR*> JSR::env_to_jsr_cache;
std::mutex JSR::env_cache_mutex;

namespace {
std::mutex& UnsafeRuntimeMapMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::unordered_map<facebook::jsi::Runtime*, JSR*>& UnsafeRuntimeMap() {
  static auto* runtimes =
      new std::unordered_map<facebook::jsi::Runtime*, JSR*>();
  return *runtimes;
}

// Runtime teardown can happen from a process-exit destructor after ordinary
// thread-local objects have already been destroyed. Keep this state alive for
// the lifetime of the process so teardown can safely query the lock depth.
std::unordered_map<JSR*, int>& RuntimeLockDepths() {
  static thread_local auto* depths = new std::unordered_map<JSR*, int>();
  return *depths;
}

class RuntimeLockGuard {
 public:
  explicit RuntimeLockGuard(JSR* runtime) : runtime_(runtime) {
    runtime_->lock();
  }

  ~RuntimeLockGuard() {
    runtime_->unlock();
  }

 private:
  JSR* runtime_;
};
}  // namespace

void JSR::lock() {
  runtime->lock();
  js_mutex.lock();
  RuntimeLockDepths()[this] += 1;
}

void JSR::unlock() {
  auto depth = RuntimeLockDepths().find(this);
  if (depth != RuntimeLockDepths().end()) {
    depth->second -= 1;
    if (depth->second <= 0) {
      RuntimeLockDepths().erase(depth);
    }
  }
  js_mutex.unlock();
  runtime->unlock();
}

int JSR::currentLockDepth() const {
  auto depth = RuntimeLockDepths().find(const_cast<JSR*>(this));
  if (depth == RuntimeLockDepths().end()) {
    return 0;
  }
  return depth->second;
}

int js_current_env_lock_depth(napi_env env) {
  JSR* runtime = JSR::ForEnv(env);
  if (runtime == nullptr) {
    return 0;
  }
  return runtime->currentLockDepth();
}

JSR* JSR::ForEnv(napi_env env) {
  std::lock_guard<std::mutex> lock(env_cache_mutex);
  auto it = env_to_jsr_cache.find(env);
  return it == env_to_jsr_cache.end() ? nullptr : it->second;
}

void JSR::RegisterEnv(napi_env env, JSR* runtime) {
  std::lock_guard<std::mutex> lock(env_cache_mutex);
  env_to_jsr_cache[env] = runtime;
}

void JSR::UnregisterEnv(napi_env env) {
  std::lock_guard<std::mutex> lock(env_cache_mutex);
  env_to_jsr_cache.erase(env);
}

JSR::JSR() {
  hermes::vm::RuntimeConfig config = hermes::vm::RuntimeConfig::Builder()
                                         .withMicrotaskQueue(true)
                                         .withEnableEval(true)
                                         .build();
  runtime = facebook::hermes::makeThreadSafeHermesRuntime(config);
  rt = &runtime->getUnsafeRuntime();
  std::lock_guard<std::mutex> guard(UnsafeRuntimeMapMutex());
  UnsafeRuntimeMap()[rt] = this;
}

extern "C" void* js_lock_unsafe_jsi_runtime(
    facebook::jsi::Runtime* runtime) {
  if (runtime == nullptr) {
    return nullptr;
  }
  JSR* jsr = nullptr;
  {
    std::lock_guard<std::mutex> guard(UnsafeRuntimeMapMutex());
    auto it = UnsafeRuntimeMap().find(runtime);
    jsr = it == UnsafeRuntimeMap().end() ? nullptr : it->second;
  }
  if (jsr != nullptr) {
    jsr->lock();
  }
  return jsr;
}

extern "C" void js_unlock_unsafe_jsi_runtime(void* lockToken) {
  if (lockToken != nullptr) {
    static_cast<JSR*>(lockToken)->unlock();
  }
}

napi_status js_create_runtime(napi_runtime* runtime) {
  if (runtime == nullptr) return napi_invalid_arg;
  *runtime = new napi_runtime__();
  (*runtime)->hermes = new JSR();

  return napi_ok;
}

napi_status js_lock_env(napi_env env) {
  JSR* runtime = JSR::ForEnv(env);
  if (runtime == nullptr) {
    return napi_invalid_arg;
  }
  runtime->lock();

  return napi_ok;
}

napi_status js_unlock_env(napi_env env) {
  JSR* runtime = JSR::ForEnv(env);
  if (runtime == nullptr) {
    return napi_invalid_arg;
  }
  runtime->unlock();

  return napi_ok;
}

napi_status js_create_napi_env(napi_env* env, napi_runtime runtime) {
  if (env == nullptr) return napi_invalid_arg;
  RuntimeLockGuard lock(runtime->hermes);
  *env = (napi_env)runtime->hermes->rt->createNodeApiEnv(9);
  if (*env == nullptr) return napi_generic_failure;
  JSR::RegisterEnv(*env, runtime->hermes);

  napi_value global = nullptr;
  napi_value gc = nullptr;
  napi_get_global(*env, &global);
  napi_create_function(
      *env, "gc", NAPI_AUTO_LENGTH,
      [](napi_env callbackEnv, napi_callback_info) -> napi_value {
        JSR* jsr = JSR::ForEnv(callbackEnv);
        if (jsr != nullptr && jsr->rt != nullptr) {
          jsr->rt->instrumentation().collectGarbage("explicit gc()");
        }
        napi_value undefined = nullptr;
        napi_get_undefined(callbackEnv, &undefined);
        return undefined;
      },
      nullptr, &gc);
  napi_set_named_property(*env, global, "gc", gc);
  return napi_ok;
}

facebook::jsi::Runtime* js_get_jsi_runtime(napi_env env) {
  JSR* runtime = JSR::ForEnv(env);
  return runtime == nullptr ? nullptr : runtime->rt;
}

napi_status js_set_runtime_flags(const char* flags) { return napi_ok; }

napi_status js_free_napi_env(napi_env env) {
#ifndef NS_HERMES_SKIP_ENV_CLEANUP_HOOKS
  js_run_env_cleanup_hooks(env);
#endif
  JSR::UnregisterEnv(env);
  return napi_ok;
}

napi_status js_free_runtime(napi_runtime runtime) {
  if (runtime == nullptr) return napi_invalid_arg;
  {
    std::lock_guard<std::mutex> guard(UnsafeRuntimeMapMutex());
    UnsafeRuntimeMap().erase(runtime->hermes->rt);
  }
  runtime->hermes->runtime.reset();
  runtime->hermes->rt = nullptr;
  delete runtime->hermes;
  delete runtime;

  return napi_ok;
}

napi_status js_execute_script(napi_env env, napi_value script, const char* file,
                              napi_value* result) {
  return napi_run_script_source(env, script, file, result);
}

napi_status js_execute_pending_jobs(napi_env env) {
  bool result;
  return jsr_drain_microtasks(env, -1, &result);
}

napi_status js_get_engine_ptr(napi_env env, int64_t* engine_ptr) {
  return napi_ok;
}

napi_status js_adjust_external_memory(napi_env env, int64_t changeInBytes,
                                      int64_t* externalMemory) {
  napi_adjust_external_memory(env, changeInBytes, externalMemory);
  return napi_ok;
}

napi_status js_cache_script(napi_env env, const char* source,
                            const char* file) {
  return napi_ok;
}

napi_status js_run_cached_script(napi_env env, const char* file,
                                 napi_value script, void* cache,
                                 napi_value* result) {
  return napi_ok;
}

napi_status js_get_runtime_version(napi_env env, napi_value* version) {
  napi_create_string_utf8(env, "Hermes", NAPI_AUTO_LENGTH, version);
  return napi_ok;
}

extern "C" napi_status napi_run_script_source(napi_env env, napi_value script,
                                              const char* source_url,
                                              napi_value* result) {
  (void)source_url;
  return napi_run_script(env, script, result);
}

extern "C" napi_status jsr_run_script(napi_env env, napi_value source,
                                      const char* source_url,
                                      napi_value* result) {
  return napi_run_script_source(env, source, source_url, result);
}

extern "C" napi_status jsr_drain_microtasks(napi_env env,
                                            int32_t max_count_hint,
                                            bool* result) {
  JSR* runtime = JSR::ForEnv(env);
  if (runtime == nullptr || result == nullptr) {
    return napi_invalid_arg;
  }

  NapiScope scope(env, false);
  *result = runtime->rt->drainMicrotasks(max_count_hint);
  return napi_ok;
}
