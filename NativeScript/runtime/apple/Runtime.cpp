#include "native_api_util.h"
#include "runtime/apple/SpinLock.h"
#ifdef ENABLE_JS_RUNTIME

#include "Runtime.h"
#include "RuntimeConfig.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "jsr.h"
#include "jsr_common.h"
#include "runtime/apple/Util.h"
#include "runtime/apple/modules/RuntimeModules.h"
#ifdef TARGET_ENGINE_V8
#include "ffi/objc/v8/NativeApiV8.h"
#include "v8-api.h"
#endif  // TARGET_ENGINE_V8
#ifdef TARGET_ENGINE_HERMES
#include "ffi/objc/hermes/NativeApiJsi.h"
#endif  // TARGET_ENGINE_HERMES
#ifdef TARGET_ENGINE_JSC
#include "ffi/objc/jsc/NativeApiJSC.h"
#endif  // TARGET_ENGINE_JSC
#ifdef TARGET_ENGINE_QUICKJS
#include "ffi/objc/quickjs/NativeApiQuickJS.h"
#endif  // TARGET_ENGINE_QUICKJS
#include <CoreFoundation/CFRunLoop.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "NativeScript.h"
#include "robin_hood.h"

extern "C" {
void node_module_register(const char* name, napi_module_init init) {
  nativescript::napiModuleRegistry[name] = init;
}
}

namespace nativescript {

std::unordered_map<std::string, napi_module_init> napiModuleRegistry;

static robin_hood::unordered_map<napi_env, Runtime*> runtimes_;

std::atomic<int> Runtime::nextIsolateId{0};

void unregisterRuntimePromiseRunLoop(CFRunLoopRef runLoop);

Runtime* Runtime::GetRuntime(napi_env env) {
  SpinLock lock(envsMutex_);
  auto it = runtimes_.find(env);
  if (it != runtimes_.end()) {
    return it->second;
  }
  return nullptr;
}

#ifdef TARGET_ENGINE_HERMES
class HermesRuntimeUnlockScope final {
 public:
  explicit HermesRuntimeUnlockScope(napi_env env) {
    jsr_ = JSR::ForEnv(env);
    if (jsr_ == nullptr) {
      return;
    }

    unlockedDepth_ = js_current_env_lock_depth(env);
    for (int i = 0; i < unlockedDepth_; i++) {
      jsr_->unlock();
    }
    if (unlockedDepth_ == 0 && jsr_->runtime != nullptr) {
      jsr_->runtime->unlock();
      unlockedRuntime_ = true;
    }
  }

  ~HermesRuntimeUnlockScope() {
    if (unlockedRuntime_ && jsr_ != nullptr && jsr_->runtime != nullptr) {
      jsr_->runtime->lock();
    }
    if (jsr_ != nullptr) {
      for (int i = 0; i < unlockedDepth_; i++) {
        jsr_->lock();
      }
    }
  }

  HermesRuntimeUnlockScope(const HermesRuntimeUnlockScope&) = delete;
  HermesRuntimeUnlockScope& operator=(const HermesRuntimeUnlockScope&) = delete;

 private:
  JSR* jsr_ = nullptr;
  int unlockedDepth_ = 0;
  bool unlockedRuntime_ = false;
};

void InvokeWithUnlockedHermesRuntime(napi_env env,
                                     const std::function<void()>& task) {
  HermesRuntimeUnlockScope scope(env);
  task();
}
#endif  // TARGET_ENGINE_HERMES

Runtime::Runtime() {
  currentRuntime_ = this;
  workerId_ = -1;
  runtimeLoop_ = nullptr;
  microtaskObserver_ = nullptr;
  // workerCache_ = Caches::Workers;
}

Runtime::~Runtime() {
  currentRuntime_ = nullptr;
  if (microtaskObserver_ != nullptr) {
    if (runtimeLoop_ != nullptr) {
      CFRunLoopRemoveObserver(runtimeLoop_, microtaskObserver_,
                              kCFRunLoopCommonModes);
    }
    CFRelease(microtaskObserver_);
    microtaskObserver_ = nullptr;
  }
  if (runtimeLoop_ != nullptr) {
    unregisterRuntimePromiseRunLoop(runtimeLoop_);
    runtimeLoop_ = nullptr;
  }

  if (env_) {
    {
      // Enter isolate/context for deinit work without creating another
      // temporary N-API handle scope. We must close the long-lived
      // `globalScope_` in LIFO order.
      NapiScope scope(env_, false);

      modules_.DeInit();
#if NS_FFI_BACKEND_V8 && defined(TARGET_ENGINE_V8)
      CleanupNativeApi(env_->isolate);
#elif NS_FFI_BACKEND_QUICKJS && defined(TARGET_ENGINE_QUICKJS)
      CleanupNativeApi(qjs_get_context(env_));
#elif NS_FFI_BACKEND_JSC && defined(TARGET_ENGINE_JSC)
      CleanupNativeApi(env_->context);
#endif
      if (globalScope_ != nullptr) {
        napi_close_handle_scope(env_, globalScope_);
        globalScope_ = nullptr;
      }
      js_free_napi_env(env_);
    }

    js_free_runtime(runtime_);
  } else {
    modules_.DeInit();
  }

  {
    SpinLock lock(envsMutex_);
    runtimes_.erase(env_);
  }
}

napi_value drainMicrotasks(napi_env env, napi_callback_info cbinfo) {
  js_execute_pending_jobs(env);
  return nullptr;
}

// Leaked, never-destroyed singletons: the Runtime destructor can run during
// process teardown after file-scope statics are destroyed, so a destroyed
// mutex would fail to lock (std::system_error). Heap-allocating and never
// freeing avoids the static-destruction-order fiasco.
std::mutex& gRuntimePromiseRunLoopMutex() {
  static std::mutex* mutex = new std::mutex();
  return *mutex;
}
std::unordered_map<std::string, CFRunLoopRef>& gRuntimePromiseRunLoops() {
  static auto* runLoops = new std::unordered_map<std::string, CFRunLoopRef>();
  return *runLoops;
}

std::string runtimePromiseRunLoopToken(CFRunLoopRef runLoop) {
  char buffer[(sizeof(void*) * 2) + 3] = {};
  std::snprintf(buffer, sizeof(buffer), "%p", runLoop);
  return buffer;
}

std::string registerRuntimePromiseRunLoop(CFRunLoopRef runLoop) {
  if (runLoop == nullptr) {
    return "";
  }

  std::string token = runtimePromiseRunLoopToken(runLoop);
  std::lock_guard<std::mutex> lock(gRuntimePromiseRunLoopMutex());
  if (gRuntimePromiseRunLoops().find(token) == gRuntimePromiseRunLoops().end()) {
    gRuntimePromiseRunLoops().emplace(token, (CFRunLoopRef)CFRetain(runLoop));
  }
  return token;
}

void unregisterRuntimePromiseRunLoop(CFRunLoopRef runLoop) {
  if (runLoop == nullptr) {
    return;
  }

  std::string token = runtimePromiseRunLoopToken(runLoop);
  std::lock_guard<std::mutex> lock(gRuntimePromiseRunLoopMutex());
  auto it = gRuntimePromiseRunLoops().find(token);
  if (it == gRuntimePromiseRunLoops().end()) {
    return;
  }
  CFRelease(it->second);
  gRuntimePromiseRunLoops().erase(it);
}

CFRunLoopRef copyRuntimePromiseRunLoop(const std::string& token) {
  std::lock_guard<std::mutex> lock(gRuntimePromiseRunLoopMutex());
  auto it = gRuntimePromiseRunLoops().find(token);
  if (it == gRuntimePromiseRunLoops().end()) {
    return nullptr;
  }
  return (CFRunLoopRef)CFRetain(it->second);
}

bool readRuntimeStringArgument(napi_env env, napi_value value,
                               std::string* result) {
  if (result == nullptr) {
    return false;
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_string) {
    return false;
  }

  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    return false;
  }

  std::string buffer(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(),
                                 &copied) != napi_ok) {
    return false;
  }
  buffer.resize(copied);
  *result = std::move(buffer);
  return true;
}

napi_value runtimeCurrentRunLoopToken(napi_env env, napi_callback_info) {
  std::string token = registerRuntimePromiseRunLoop(CFRunLoopGetCurrent());
  napi_value result;
  napi_create_string_utf8(env, token.c_str(), token.size(), &result);
  return result;
}

napi_value runtimeIsCurrentRunLoopToken(napi_env env, napi_callback_info cbinfo) {
  napi_value argv[1];
  size_t argc = 1;
  napi_value jsThis;
  void* data;
  if (napi_get_cb_info(env, cbinfo, &argc, argv, &jsThis, &data) != napi_ok ||
      argc < 1) {
    napi_throw_type_error(env, nullptr, "Expected a run loop token.");
    return nullptr;
  }

  std::string token;
  if (!readRuntimeStringArgument(env, argv[0], &token)) {
    napi_throw_type_error(env, nullptr, "Expected a run loop token string.");
    return nullptr;
  }

  napi_value result;
  napi_get_boolean(env,
                   token == runtimePromiseRunLoopToken(CFRunLoopGetCurrent()),
                   &result);
  return result;
}

napi_value runtimeScheduleOnRunLoop(napi_env env, napi_callback_info cbinfo) {
  napi_value argv[2];
  size_t argc = 2;
  napi_value jsThis;
  void* data;
  if (napi_get_cb_info(env, cbinfo, &argc, argv, &jsThis, &data) != napi_ok ||
      argc < 2) {
    napi_throw_type_error(env, nullptr,
                          "Expected a run loop token and callback.");
    return nullptr;
  }

  std::string token;
  if (!readRuntimeStringArgument(env, argv[0], &token)) {
    napi_throw_type_error(env, nullptr, "Expected a run loop token string.");
    return nullptr;
  }

  napi_valuetype callbackType = napi_undefined;
  if (napi_typeof(env, argv[1], &callbackType) != napi_ok ||
      callbackType != napi_function) {
    napi_throw_type_error(env, nullptr, "Expected a callback function.");
    return nullptr;
  }

  CFRunLoopRef runLoop = copyRuntimePromiseRunLoop(token);
  if (runLoop == nullptr) {
    napi_throw_error(env, nullptr, "Run loop token is no longer valid.");
    return nullptr;
  }

  napi_ref callbackRef = nullptr;
  if (napi_create_reference(env, argv[1], 1, &callbackRef) != napi_ok) {
    CFRelease(runLoop);
    napi_throw_error(env, nullptr, "Unable to retain scheduled callback.");
    return nullptr;
  }

  CFRunLoopPerformBlock(runLoop, kCFRunLoopCommonModes, ^{
    NapiScope scope(env);
    napi_value callback = nullptr;
    napi_value global = nullptr;
    if (napi_get_reference_value(env, callbackRef, &callback) == napi_ok &&
        callback != nullptr && napi_get_global(env, &global) == napi_ok) {
      napi_call_function(env, global, callback, 0, nullptr, nullptr);
      js_execute_pending_jobs(env);
    }
    napi_delete_reference(env, callbackRef);
    CFRelease(runLoop);
  });
  CFRunLoopWakeUp(runLoop);
  return nullptr;
}

void Runtime::Init(bool isWorker) {
  js_set_runtime_flags("");

  auto now = std::chrono::steady_clock::now();
  startTime_ = std::chrono::duration<double>(now.time_since_epoch()).count();

  auto sysNow = std::chrono::system_clock::now();
  realtimeOrigin_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                        sysNow.time_since_epoch())
                        .count();

  if (js_create_runtime(&runtime_) != napi_ok || runtime_ == nullptr) {
    throw NativeScriptException("Unable to create JavaScript runtime.");
  }
  if (js_create_napi_env(&env_, runtime_) != napi_ok || env_ == nullptr) {
    js_free_runtime(runtime_);
    runtime_ = nullptr;
    throw NativeScriptException("Unable to create Node-API environment.");
  }

  runtimeLoop_ = CFRunLoopGetCurrent();
  {
    Runtime* runtime = this;
    microtaskObserver_ = CFRunLoopObserverCreateWithHandler(
        kCFAllocatorDefault, kCFRunLoopBeforeWaiting, true, 0,
        ^(CFRunLoopObserverRef, CFRunLoopActivity) {
          napi_env env = runtime->env_;
          if (env == nullptr || !Runtime::IsAlive(env)) {
            return;
          }

          NapiScope scope(env);
          js_execute_pending_jobs(env);
        });
    if (microtaskObserver_ != nullptr) {
      CFRunLoopAddObserver(runtimeLoop_, microtaskObserver_,
                           kCFRunLoopCommonModes);
    }
  }

  {
    SpinLock lock(envsMutex_);
    runtimes_[env_] = this;
  }

#ifdef TARGET_ENGINE_V8
  v8::Locker locker(env_->isolate);
  v8::Isolate::Scope isolate_scope(env_->isolate);
  v8::Context::Scope context_scope(env_->context());
#endif  // TARGET_ENGINE_V8

  if (napi_open_handle_scope(env_, &globalScope_) != napi_ok) {
    throw NativeScriptException("Unable to open runtime handle scope.");
  }

  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env_, &scope) != napi_ok) {
    throw NativeScriptException("Unable to open initialization handle scope.");
  }
  struct InitScopeGuard {
    napi_env env;
    napi_handle_scope scope;
    ~InitScopeGuard() { napi_close_handle_scope(env, scope); }
  } initScopeGuard{env_, scope};

  napi_value global;
  napi_get_global(env_, &global);
  napi_set_named_property(env_, global, "global", global);

  const char* CompatScript = R"(
    if (typeof globalThis.__extends !== "function") {
      var __extendStatics = Object.setPrototypeOf ||
        ({ __proto__: [] } instanceof Array && function(d, b) { d.__proto__ = b; }) ||
        function(d, b) {
          for (var p in b) {
            if (Object.prototype.hasOwnProperty.call(b, p)) {
              d[p] = b[p];
            }
          }
        };
      globalThis.__extends = function(d, b) {
        if (typeof b !== "function" && b !== null) {
          throw new TypeError("Class extends value " + String(b) + " is not a constructor or null");
        }
        __extendStatics(d, b);
        function __() { this.constructor = d; }
        d.prototype = b === null ? Object.create(b) : (__.prototype = b.prototype, new __());
      };
    }

    if (typeof globalThis.__decorate !== "function") {
      globalThis.__decorate = function(decorators, target, key, desc) {
        var c = arguments.length;
        var r = c < 3 ? target : (desc === null ? desc = Object.getOwnPropertyDescriptor(target, key) : desc);
        var d;
        if (typeof Reflect === "object" && typeof Reflect.decorate === "function") {
          r = Reflect.decorate(decorators, target, key, desc);
        } else {
          for (var i = decorators.length - 1; i >= 0; i--) {
            d = decorators[i];
            if (d) {
              r = (c < 3 ? d(r) : c > 3 ? d(target, key, r) : d(target, key)) || r;
            }
          }
        }
        if (c > 3 && r) {
          Object.defineProperty(target, key, r);
        }
        return r;
      };
    }

    if (typeof globalThis.__param !== "function") {
      globalThis.__param = function(paramIndex, decorator) {
        return function(target, key) { decorator(target, key, paramIndex); };
      };
    }

    if (typeof globalThis.ObjCClass !== "function") {
      globalThis.ObjCClass = function ObjCClass(...protocols) {
        return function(constructor) {
          constructor.ObjCProtocols = protocols;
        };
      };
    }

    if (typeof WeakRef === "function") {
      if (!WeakRef.prototype.get && typeof WeakRef.prototype.deref === "function") {
        WeakRef.prototype.get = WeakRef.prototype.deref;
      }

      if (!WeakRef.prototype.clear) {
        WeakRef.prototype.clear = function() {
          console.warn("WeakRef.clear() is non-standard and has been deprecated. It does nothing and the call can be safely removed.");
        };
      }
    }

    if (!globalThis.__time) {
      globalThis.__time = function() {
        if (globalThis.performance && typeof performance.now === "function") {
          return performance.now();
        }
        return Date.now();
      };
    }

    if (globalThis.performance && typeof performance.now === "function" &&
        typeof performance.timeOrigin !== "number") {
      const now = performance.now();
      const origin = Date.now() - now;
      try {
        Object.defineProperty(performance, "timeOrigin", {
          configurable: true,
          enumerable: true,
          writable: false,
          value: origin
        });
      } catch (_) {
        performance.timeOrigin = origin;
      }
    }

    if (!globalThis.__collect) {
      globalThis.__collect = function() {
        gc();
      };
    }

    if (!globalThis.__dynamicImport) {
      globalThis.__dynamicImport = function(specifier) {
        return Promise.resolve().then(function() {
          const runtimeRequire =
            typeof globalThis.require === "function" ? globalThis.require : null;
          if (!runtimeRequire) {
            throw new ReferenceError("require is not available");
          }

          const loaded = runtimeRequire(specifier);
          if (loaded !== null &&
              (typeof loaded === "object" || typeof loaded === "function")) {
            if (loaded.__esModule) {
              return loaded;
            }
            return Object.assign({ default: loaded }, loaded);
          }

          return { default: loaded };
        });
      };
    }

    if (!globalThis.gc) {
      globalThis.gc = function() {
        console.warn('gc() is not exposed');
      };
    }

    if (typeof globalThis.URLPattern !== "function") {
      globalThis.URLPattern = class URLPattern {
        constructor(input, baseURL) {
          if (typeof input !== "string") {
            throw new TypeError("Failed to construct 'URLPattern': input must be a string");
          }

          let url;
          if (baseURL === undefined || baseURL === null) {
            url = new URL(input);
          } else {
            url = new URL(input, baseURL);
          }

          const normalizeProtocol = (value) => value.endsWith(":") ? value.slice(0, -1) : value;

          this.protocol = normalizeProtocol(url.protocol);
          this.username = url.username || "*";
          this.password = url.password || "*";
          this.hostname = url.hostname || "*";
          this.port = url.port || "";
          this.pathname = url.pathname || "/";
          this.search = url.search || "*";
          this.hash = url.hash || "*";
          this.hasRegExpGroups = false;
        }
      };
    }
  )";

  napi_value compatScript, result;
  napi_create_string_utf8(env_, CompatScript, NAPI_AUTO_LENGTH, &compatScript);
  napi_run_script(env_, compatScript, &result);

  napi_property_descriptor runtimeProps[] = {
      napi_util::desc("__drainMicrotaskQueue", drainMicrotasks, nullptr),
      napi_util::desc("__runtimeCurrentRunLoopToken",
                      runtimeCurrentRunLoopToken, nullptr),
      napi_util::desc("__runtimeIsCurrentRunLoopToken",
                      runtimeIsCurrentRunLoopToken, nullptr),
      napi_util::desc("__runtimeScheduleOnRunLoop", runtimeScheduleOnRunLoop,
                      nullptr),
  };
  napi_define_properties(env_, global,
                         sizeof(runtimeProps) / sizeof(runtimeProps[0]),
                         runtimeProps);

#if defined(TARGET_ENGINE_V8) || defined(TARGET_ENGINE_HERMES)
  const char* PromiseProxyScript = R"(
        // Ensure that Promise callbacks are executed on the
        // same thread on which they were created
        (() => {
            const currentRunLoopToken = globalThis.__runtimeCurrentRunLoopToken;
            const isCurrentRunLoop = globalThis.__runtimeIsCurrentRunLoopToken;
            const scheduleRunLoop = globalThis.__runtimeScheduleOnRunLoop;
            if (typeof currentRunLoopToken !== "function" ||
                typeof isCurrentRunLoop !== "function" ||
                typeof scheduleRunLoop !== "function") {
                return;
            }

            const runLoopQueues = [];

            function getRunLoopQueue(runloop) {
                for (let i = 0; i < runLoopQueues.length; i++) {
                    if (runLoopQueues[i].runloop === runloop) {
                        return runLoopQueues[i];
                    }
                }

                const queue = {
                    runloop,
                    pending: false,
                    callbacks: [],
                    drain() {
                        queue.pending = false;
                        const callbacks = queue.callbacks.splice(0);
                        for (let i = 0; i < callbacks.length; i++) {
                            callbacks[i]();
                        }
                        if (queue.callbacks.length > 0 && !queue.pending) {
                            queue.pending = true;
                            scheduleRunLoop(queue.runloop, queue.drain);
                        }
                    }
                };
                runLoopQueues.push(queue);
                return queue;
            }

            function scheduleOnRunLoop(queue, callback) {
                queue.callbacks.push(callback);
                if (queue.pending) {
                    return;
                }
                queue.pending = true;
                scheduleRunLoop(queue.runloop, queue.drain);
            }

            global.Promise = new Proxy(global.Promise, {
                construct: function(target, args) {
                    let origFunc = args[0];
                    let runloop = currentRunLoopToken();
                    let runloopQueue = getRunLoopQueue(runloop);

                    let promise = new target(function(resolve, reject) {
                        function isFulfilled() {
                            return !resolve;
                        }
                        function markFulfilled() {
                            origFunc = null;
                            resolve = null;
                            reject = null;
                        }
                        origFunc(value => {
                            if (isFulfilled()) {
                                return;
                            }
                            const resolveFn = resolve;
                            const resolveCall = function() {
                                resolveFn(value);
                            };
                            if (isCurrentRunLoop(runloop)) {
                                markFulfilled();
                                resolveCall();
                            } else {
                                markFulfilled();
                                scheduleOnRunLoop(runloopQueue, resolveCall);
                            }
                        }, reason => {
                            if (isFulfilled()) {
                                return;
                            }
                            const rejectFn = reject;
                            const rejectCall = function() {
                                rejectFn(reason);
                            };
                            if (isCurrentRunLoop(runloop)) {
                                markFulfilled();
                                rejectCall();
                            } else {
                                markFulfilled();
                                scheduleOnRunLoop(runloopQueue, rejectCall);
                            }
                        });
                    });

                    return new Proxy(promise, {
                        get: function(target, name) {
                            let orig = target[name];
                            if (name === "then" || name === "catch" || name === "finally") {
                                return orig.bind(target);
                            }
                            return typeof orig === 'function' ? function(x) {
                                if (isCurrentRunLoop(runloop)) {
                                    orig.bind(target, x)();
                                    return target;
                                }
                                scheduleRunLoop(runloop, orig.bind(target, x));
                                return target;
                            } : orig;
                        }
                    });
                }
            });
        })();
    )";

  napi_value promiseProxyScript;
  napi_create_string_utf8(env_, PromiseProxyScript, NAPI_AUTO_LENGTH,
                          &promiseProxyScript);
  napi_run_script(env_, promiseProxyScript, &result);
#endif

  if (isWorker) {
    napi_property_descriptor prop = napi_util::desc("self", global);
    napi_define_properties(env_, global, 1, &prop);
  }
  modules_.Init(env_, global);

#ifdef TARGET_ENGINE_HERMES
  const char* HermesGlobalBootstrap = R"(
    if (typeof globalThis.Console === "function" && typeof globalThis.console === "undefined") {
      globalThis.console = Object.create(globalThis.Console.prototype);
    }
    if (typeof globalThis.Performance === "function" && typeof globalThis.performance === "undefined") {
      globalThis.performance = Object.create(globalThis.Performance.prototype);
    }
  )";

  napi_value hermesGlobalBootstrapScript;
  napi_create_string_utf8(env_, HermesGlobalBootstrap, NAPI_AUTO_LENGTH,
                          &hermesGlobalBootstrapScript);
  napi_run_script(env_, hermesGlobalBootstrapScript, &result);
#endif

  const char* metadata_path = std::getenv("NS_METADATA_PATH");
#if NS_FFI_BACKEND_NAPI
  nativescript_init(env_, metadata_path, RuntimeConfig.MetadataPtr);
#endif

#if NS_FFI_BACKEND_V8 && defined(TARGET_ENGINE_V8)
  {
    NativeApiConfig nativeApiV8Config;
    nativeApiV8Config.metadataPath = metadata_path;
    nativeApiV8Config.metadataPtr = RuntimeConfig.MetadataPtr;
    nativeApiV8Config.installGlobalSymbols = true;
    nativeApiV8Config.nativeCallbackInvoker =
        [env = env_](std::function<void()> task) {
          NapiScope scope(env);
          task();
        };
    nativeApiV8Config.jsThreadCallbackInvoker =
        [env = env_, runLoop = runtimeLoop_](std::function<void()> task) {
          ExecuteOnRunLoop(
              runLoop,
              [env, task = std::move(task)]() mutable {
                NapiScope scope(env);
                task();
              },
              false);
        };
    nativeApiV8Config.jsThreadAsyncCallbackInvoker =
        [env = env_, runLoop = runtimeLoop_](std::function<void()> task) {
          ExecuteOnRunLoop(
              runLoop,
              [env, task = std::move(task)]() mutable {
                NapiScope scope(env);
                task();
              },
              true);
        };
    InstallNativeApi(env_->isolate, env_->context(), nativeApiV8Config);
  }
#endif  // NS_FFI_BACKEND_V8 && TARGET_ENGINE_V8

#if NS_FFI_BACKEND_HERMES && defined(TARGET_ENGINE_HERMES)
  if (auto* jsiRuntime = js_get_jsi_runtime(env_)) {
    NativeApiJsiConfig nativeApiJsiConfig;
    nativeApiJsiConfig.metadataPath = metadata_path;
    nativeApiJsiConfig.metadataPtr = RuntimeConfig.MetadataPtr;
    nativeApiJsiConfig.installGlobalSymbols = true;
    nativeApiJsiConfig.invokeCallbacksOnNativeCallerThread = true;
    nativeApiJsiConfig.nativeInvocationInvoker =
        [env = env_](std::function<void()> task) {
          InvokeWithUnlockedHermesRuntime(env, task);
        };
    nativeApiJsiConfig.jsThreadCallbackInvoker =
        [env = env_, runLoop = runtimeLoop_](std::function<void()> task) {
          ExecuteOnRunLoop(
              runLoop,
              [env, task = std::move(task)]() mutable {
                NapiScope scope(env);
                task();
              },
              false);
        };
    nativeApiJsiConfig.jsThreadAsyncCallbackInvoker =
        [env = env_, runLoop = runtimeLoop_](std::function<void()> task) {
          ExecuteOnRunLoop(
              runLoop,
              [env, task = std::move(task)]() mutable {
                NapiScope scope(env);
                task();
              },
              true);
        };
    InstallNativeApiJSI(*jsiRuntime, nativeApiJsiConfig);
  }
#endif  // NS_FFI_BACKEND_HERMES && TARGET_ENGINE_HERMES

#if NS_FFI_BACKEND_JSC && defined(TARGET_ENGINE_JSC)
  {
    NativeApiConfig nativeApiJSCConfig;
    nativeApiJSCConfig.metadataPath = metadata_path;
    nativeApiJSCConfig.metadataPtr = RuntimeConfig.MetadataPtr;
    nativeApiJSCConfig.installGlobalSymbols = true;
    nativeApiJSCConfig.nativeCallbackInvoker =
        [env = env_](std::function<void()> task) {
          NapiScope scope(env);
          task();
        };
    nativeApiJSCConfig.jsThreadCallbackInvoker =
        [env = env_, runLoop = runtimeLoop_](std::function<void()> task) {
          ExecuteOnRunLoop(
              runLoop,
              [env, task = std::move(task)]() mutable {
                NapiScope scope(env);
                task();
              },
              false);
        };
    nativeApiJSCConfig.jsThreadAsyncCallbackInvoker =
        [env = env_, runLoop = runtimeLoop_](std::function<void()> task) {
          ExecuteOnRunLoop(
              runLoop,
              [env, task = std::move(task)]() mutable {
                NapiScope scope(env);
                task();
              },
              true);
        };
    InstallNativeApi(env_->context, nativeApiJSCConfig);
  }
#endif  // NS_FFI_BACKEND_JSC && TARGET_ENGINE_JSC

#if NS_FFI_BACKEND_QUICKJS && defined(TARGET_ENGINE_QUICKJS)
  {
    NativeApiConfig nativeApiQuickJSConfig;
    nativeApiQuickJSConfig.metadataPath = metadata_path;
    nativeApiQuickJSConfig.metadataPtr = RuntimeConfig.MetadataPtr;
    nativeApiQuickJSConfig.installGlobalSymbols = true;
    nativeApiQuickJSConfig.nativeCallbackInvoker =
        [env = env_](std::function<void()> task) {
          NapiScope scope(env);
          task();
        };
    nativeApiQuickJSConfig.jsThreadCallbackInvoker =
        [env = env_, runLoop = runtimeLoop_](std::function<void()> task) {
          ExecuteOnRunLoop(
              runLoop,
              [env, task = std::move(task)]() mutable {
                NapiScope scope(env);
                task();
              },
              false);
        };
    nativeApiQuickJSConfig.jsThreadAsyncCallbackInvoker =
        [env = env_, runLoop = runtimeLoop_](std::function<void()> task) {
          ExecuteOnRunLoop(
              runLoop,
              [env, task = std::move(task)]() mutable {
                NapiScope scope(env);
                task();
              },
              true);
        };
    InstallNativeApi(qjs_get_context(env_), nativeApiQuickJSConfig);
  }
#endif  // NS_FFI_BACKEND_QUICKJS && TARGET_ENGINE_QUICKJS

}

const int Runtime::WorkerId() { return this->workerId_; }

void Runtime::SetWorkerId(int workerId) { this->workerId_ = workerId; }

void Runtime::RunScript(std::string& scriptSrc, std::string file) {
  NapiScope scope(env_);

  napi_value script, result;
  napi_create_string_utf8(env_, scriptSrc.c_str(), scriptSrc.length(), &script);
  js_execute_script(env_, script, file.c_str(), &result);
}

napi_value Runtime::RunModule(std::string spec) {
  NapiScope scope(env_);
  return modules_.module.Require(env_, spec, RuntimeConfig.BaseDir);
}

void Runtime::RunMainModule() { napi_value result = RunModule("./"); }

void Runtime::RunLoop() {
  // Keep the runtime alive while asynchronous main-thread work is pending, but
  // still exit once the loop has been idle for a short period.
  constexpr CFTimeInterval kPollSeconds = 0.1;
  constexpr int kIdlePollsBeforeExit = 10;  // ~1s idle window

  int idlePolls = 0;
  while (true) {
    js_execute_pending_jobs(env_);

    const auto result =
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, kPollSeconds, true);

    if (result == kCFRunLoopRunHandledSource) {
      idlePolls = 0;
      continue;
    }

    if (result == kCFRunLoopRunTimedOut) {
#ifdef __APPLE__
      if (Timers::HasActiveTimers()) {
        idlePolls = 0;
        continue;
      }
#endif
      idlePolls++;
      if (idlePolls >= kIdlePollsBeforeExit) {
        break;
      }
      continue;
    }

    if (result == kCFRunLoopRunStopped || result == kCFRunLoopRunFinished) {
      break;
    }
  }
}

bool Runtime::IsAlive(napi_env env) {
  SpinLock lock(envsMutex_);
  auto it = runtimes_.find(env);
  return it != runtimes_.end();
}

thread_local Runtime* Runtime::currentRuntime_ = nullptr;
SpinMutex Runtime::envsMutex_;

}  // namespace nativescript

#endif  // ENABLE_JS_RUNTIME
