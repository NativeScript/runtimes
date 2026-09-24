#include "jsr.h"

#include <functional>

#include "jsr_common.h"

// Node-API surface exported by the prebuilt Hermes. Included here rather than
// in jsr.h on purpose: it drags in Hermes' own node_api_types.h, whose
// napi_threadsafe_function (struct pointer) and napi_tsfn_* enums collide with
// NativeScript's authoritative definitions in napi/common/js_native_tsfn.h.
// Keeping it out of the header confines it to this file, so translation units
// that include jsr.h -- notably runtime/apple/ThreadSafeFunction.mm -- never
// see it. jsr.h needs nothing from it.
#include "napi/hermes_napi.h"

#ifdef __ANDROID__
#include <cstdio>
#include <cstring>

#include "File.h"
#include "NativeScriptAssert.h"
#include "bytecode_container.h"
#else
#include "js_runtime.h"
#endif

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

  ~RuntimeLockGuard() { runtime_->unlock(); }

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
#ifdef __ANDROID__
  hermes::vm::RuntimeConfig config = hermes::vm::RuntimeConfig::Builder()
                                         .withMicrotaskQueue(true)
                                         .withES6BlockScoping(true)
                                         .withEnableAsyncGenerators(true)
                                         .withAsyncBreakCheckInEval(true)
                                         .build();
  runtime = facebook::hermes::makeThreadSafeHermesRuntime(config);
  rt = static_cast<facebook::hermes::HermesRuntime*>(
      &runtime->getUnsafeRuntime());
#else
  hermes::vm::RuntimeConfig config = hermes::vm::RuntimeConfig::Builder()
                                         .withMicrotaskQueue(true)
                                         .withEnableEval(true)
                                         .build();
  runtime = facebook::hermes::makeThreadSafeHermesRuntime(config);
  rt = static_cast<facebook::hermes::HermesRuntime*>(
      &runtime->getUnsafeRuntime());
#endif
#ifndef __ANDROID__
  std::lock_guard<std::mutex> guard(UnsafeRuntimeMapMutex());
  UnsafeRuntimeMap()[rt] = this;
#endif
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

napi_status js_create_runtime(jsr_ns_runtime* runtime) {
  if (runtime == nullptr) return napi_invalid_arg;
  *runtime = new jsr_ns_runtime__();
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

napi_status js_create_napi_env(napi_env* env, jsr_ns_runtime runtime) {
  if (env == nullptr) return napi_invalid_arg;
  RuntimeLockGuard lock(runtime->hermes);
  // Extract the underlying hermes::vm::Runtime from the JSI HermesRuntime via
  // the IHermes interface, then create the Node-API env on top of it. This is
  // the same path Hermes' own tools (repl, test-runner, napi-runner) use and
  // relies only on symbols the prebuilt Hermes exports.
  //
  // Apple used to take a different route through a NativeScript-local
  // jsi::Runtime::createNodeApiEnv hook. That hook no longer exists upstream,
  // and both platforms now build against the same headers, so there is one path.
  auto hermesInterface =
      facebook::jsi::castInterface<facebook::hermes::IHermes>(
          runtime->hermes->rt);
  if (!hermesInterface) {
    // The linked Hermes does not expose IHermes, so there is no way to reach
    // the VM runtime. Fail here rather than dereferencing null.
    return napi_generic_failure;
  }
  void* vmRuntime = hermesInterface->getVMRuntimeUnsafe();
  if (vmRuntime == nullptr) return napi_generic_failure;
  *env = hermes_napi_create_env(vmRuntime);
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

napi_status js_free_runtime(jsr_ns_runtime runtime) {
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
#ifdef __ANDROID__
  // Pull the UTF-8 source out of the napi string value and compile+run it via
  // the Hermes NAPI entry point so we can attach the source URL for stack
  // traces.
  size_t len = 0;
  napi_status status =
      napi_get_value_string_utf8(env, script, nullptr, 0, &len);
  if (status != napi_ok) return status;

  DEBUG_WRITE("[script] loading script: %s", file);

  uint8_t* source = new uint8_t[len + 1];
  status = napi_get_value_string_utf8(
      env, script, reinterpret_cast<char*>(source), len + 1, &len);
  if (status != napi_ok) {
    delete[] source;
    return status;
  }

  hermes_run_script_flags flags{};
  flags.struct_size = sizeof(flags);
  // Pass size = len + 1 so the trailing '\0' lets Hermes run the source
  // zero-copy. Hermes takes ownership of the buffer and frees it via the
  // finalizer below.
  return hermes_run_script(
      env, source, len + 1,
      [](const uint8_t* data, size_t, void*) {
        delete[] const_cast<uint8_t*>(data);
      },
      nullptr, file, &flags, result);
#else
  return napi_run_script_source(env, script, file, result);
#endif
}

#ifdef __ANDROID__
// Hermes bytecode (HBC) magic, first 8 bytes little-endian
// (0x1F1903C103BC1FC6). Hermes stores raw HBC (no NativeScript container), so
// the whole file is the bytecode buffer.
static const uint8_t kHermesMagic[8] = {0xc6, 0x1f, 0xbc, 0x03,
                                        0xc1, 0x03, 0x19, 0x1f};

napi_status js_run_bytecode_file(napi_env env, const char* file,
                                 napi_value* result) {
  std::string path;
  if (!nsbc::ResolvePath(file, path)) {
    DEBUG_WRITE("[bytecode] Unable to resolve file: %s", path.c_str());
    return napi_cannot_run_js;
  }
  if (!nsbc::HasMagic(path, reinterpret_cast<const char*>(kHermesMagic))) {
    DEBUG_WRITE("[bytecode] Unable to find hermes header: %s", path.c_str());
    return napi_cannot_run_js;
  }

  int length = 0;
  auto data = tns::File::ReadBinary(path, length);
  if (!data) return napi_cannot_run_js;

  DEBUG_WRITE("[bytecode] loading Hermes HBC bytecode: %s (%d bytes)", file,
              length);

  hermes_bytecode_flags flags{};
  flags.struct_size = sizeof(flags);
  // App modules live for the whole runtime lifetime, so keep the bytecode
  // resident and let Hermes reference it zero-copy for faster loads.
  flags.persistent = true;
  // Hermes takes ownership of the buffer and frees it via the finalizer.
  return hermes_run_bytecode(
      env, static_cast<const uint8_t*>(data), static_cast<size_t>(length),
      [](const uint8_t* d, size_t, void*) { delete[] const_cast<uint8_t*>(d); },
      nullptr, file, &flags, result);
}
#else
napi_status js_run_bytecode_file(napi_env env, const char* file,
                                 napi_value* result) {
  // The Apple Hermes build does not expose the bytecode entry points.
  return napi_cannot_run_js;
}
#endif

napi_status js_execute_pending_jobs(napi_env env) {
#ifdef __ANDROID__
  JSR* runtime = JSR::ForEnv(env);
  if (runtime == nullptr) {
    return napi_invalid_arg;
  }
  try {
    runtime->rt->drainMicrotasks();
  } catch (const facebook::jsi::JSError&) {
    return napi_pending_exception;
  } catch (const facebook::jsi::JSIException&) {
    return napi_generic_failure;
  }
  return napi_ok;
#else
  bool result;
  return jsr_drain_microtasks(env, -1, &result);
#endif
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
#ifdef __ANDROID__
  int length = 0;
  // tns::File::ReadBinary allocates with new uint8_t[length].
  auto data = tns::File::ReadBinary(file, length);
  if (!data) {
    return napi_cannot_run_js;
  }

  hermes_bytecode_flags flags{};
  flags.struct_size = sizeof(flags);
  // Hermes takes ownership of the buffer and frees it via the finalizer.
  return hermes_run_bytecode(
      env, static_cast<const uint8_t*>(data), static_cast<size_t>(length),
      [](const uint8_t* d, size_t, void*) { delete[] const_cast<uint8_t*>(d); },
      nullptr, file, &flags, result);
#else
  return napi_ok;
#endif
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
  *result = false;

  // drainMicrotasks() is JSI, so it reports a failed job by *throwing* a C++
  // jsi::JSError (HermesRuntimeImpl::checkStatus -> throwPendingError). Callers
  // here are C and Objective-C block contexts -- notably the CFRunLoop block in
  // runtime/apple/Runtime.cpp -- and letting a C++ exception unwind through
  // those aborts the process. Worse, building the JSError runs
  // JSError::recordStackTrace against a runtime that is already in a thrown
  // state, which is where this used to die.
  //
  // Catching converts the failure into a napi status, and consuming the
  // exception is what returns the runtime to a usable state.
  try {
    *result = runtime->rt->drainMicrotasks(max_count_hint);
  } catch (const facebook::jsi::JSError&) {
    return napi_pending_exception;
  } catch (const facebook::jsi::JSIException&) {
    return napi_generic_failure;
  }

  return napi_ok;
}
