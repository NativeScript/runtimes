#include "js_native_api.h"
#include "js_native_api_types.h"
#include "jsr.h"
#ifdef __APPLE__

#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <atomic>
#include <cmath>
#include "Timers.h"
#include "ffi/objc/napi/CallbackThreading.h"

static std::atomic<int> gActiveTimers{0};
struct TimerToken;

@interface NSTimerHandle : NSObject {
 @public
  NSTimer* timer;
  napi_env env;
  napi_ref callback;
  bool activeCounted;
  int64_t timerId;
  TimerToken* token;
}
@end

struct TimerToken {
  std::atomic<NSTimerHandle*> handle{nil};
  std::atomic<bool> externalAlive{true};
};

@implementation NSTimerHandle
- (void)dealloc {
  if (env != nullptr && callback != nullptr) {
    uint32_t remaining = 0;
    napi_reference_unref(env, callback, &remaining);
    napi_delete_reference(env, callback);
    callback = nullptr;
  }
  if (activeCounted) {
    gActiveTimers.fetch_sub(1, std::memory_order_relaxed);
    activeCounted = false;
  }
  if (token != nullptr) {
    token->handle.store(nil, std::memory_order_release);
    if (!token->externalAlive.load(std::memory_order_acquire)) {
      delete token;
    }
    token = nullptr;
  }
  timer = nil;
  [super dealloc];
}
@end

namespace {
const void* kTimerHandleAssociationKey = &kTimerHandleAssociationKey;
#ifdef TARGET_ENGINE_HERMES
const char* kInstallHermesTimerBridgeSource = R"(
  (function (global) {
    if (global.__nsHermesTimersInstalled) {
      return;
    }

    global.__nsHermesTimersInstalled = true;

    const callbacks = new Map();
    let nextTimerId = 1;

    function validateCallback(callback) {
      if (typeof callback !== "function") {
        throw new TypeError('The "callback" argument must be of type function');
      }
    }

    function allocateTimerId() {
      const id = nextTimerId++;
      if (nextTimerId > 0x7fffffff) {
        nextTimerId = 1;
      }
      return id;
    }

    function createTimer(nativeSetter, callback, ms) {
      validateCallback(callback);
      const timerId = allocateTimerId();
      callbacks.set(timerId, callback);
      return {
        __timerId: timerId,
        __nativeHandle: nativeSetter(timerId, ms),
      };
    }

    function clearTimer(token) {
      if (token == null) {
        return;
      }

      if (typeof token.__timerId === "number") {
        callbacks.delete(token.__timerId);
      }

      const nativeHandle =
        token && typeof token === "object" && "__nativeHandle" in token
          ? token.__nativeHandle
          : token;
      global.__ns__nativeClearTimer(nativeHandle);
    }

    global.__nsDispatchTimeout = function (timerId) {
      const callback = callbacks.get(timerId);
      callbacks.delete(timerId);
      if (typeof callback === "function") {
        callback();
      }
    };

    global.__nsDispatchInterval = function (timerId) {
      const callback = callbacks.get(timerId);
      if (typeof callback === "function") {
        callback();
      }
    };

    global.__nsReleaseTimer = function (timerId) {
      callbacks.delete(timerId);
    };

    global.__nsHermesTimerCallbackCount = function () {
      return callbacks.size;
    };

    global.__nsHermesHasTimerCallback = function (timerId) {
      return callbacks.has(timerId);
    };

    const setTimeoutImpl = function (callback, ms) {
      return createTimer(global.__ns__nativeSetTimeout, callback, ms);
    };

    const setIntervalImpl = function (callback, ms) {
      return createTimer(global.__ns__nativeSetInterval, callback, ms);
    };

    global.setTimeout = setTimeoutImpl;
    global.setInterval = setIntervalImpl;
    global.clearTimeout = clearTimer;
    global.clearInterval = clearTimer;
    global.__ns__setTimeout = setTimeoutImpl;
    global.__ns__setInterval = setIntervalImpl;
    global.__ns__clearTimeout = clearTimer;
    global.__ns__clearInterval = clearTimer;
  })(globalThis);
)";

void InstallHermesTimerBridge(napi_env env) {
  napi_value script = nullptr;
  napi_create_string_utf8(env, kInstallHermesTimerBridgeSource, NAPI_AUTO_LENGTH, &script);
  napi_value result = nullptr;
  napi_run_script(env, script, &result);
}

void DispatchHermesTimerCallback(napi_env env, const char* dispatcherName, int64_t timerId) {
  if (env == nullptr || dispatcherName == nullptr) {
    return;
  }

  napi_value global = nullptr;
  napi_value dispatcher = nullptr;
  napi_get_global(env, &global);
  if (napi_get_named_property(env, global, dispatcherName, &dispatcher) != napi_ok ||
      dispatcher == nullptr) {
    return;
  }

  napi_value timerIdValue = nullptr;
  napi_create_int64(env, timerId, &timerIdValue);
  napi_call_function(env, global, dispatcher, 1, &timerIdValue, nullptr);
}
#endif

void MarkTimerActive(NSTimerHandle* handle) {
  if (handle == nil) {
    return;
  }

  @synchronized(handle) {
    if (!handle->activeCounted) {
      handle->activeCounted = true;
      gActiveTimers.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

bool MarkTimerInactive(NSTimerHandle* handle) {
  if (handle == nil) {
    return false;
  }

  bool didDeactivate = false;
  @synchronized(handle) {
    if (handle->activeCounted) {
      handle->activeCounted = false;
      didDeactivate = true;
    }
  }

  if (didDeactivate) {
    gActiveTimers.fetch_sub(1, std::memory_order_relaxed);
  }

  return didDeactivate;
}

bool shouldAvoidMainQueueSyncWhileHoldingHermesLock(napi_env env) {
#ifdef TARGET_ENGINE_HERMES
  if ([NSThread isMainThread]) {
    return false;
  }

  // A native-caller-thread callback already owns the Hermes JS lock. A
  // synchronous hop to main can deadlock if the main run loop is draining jobs.
  return nativescript::isNativeCallerThreadCallbackActive() ||
         (env != nullptr && js_current_env_lock_depth(env) > 0);
#else
  (void)env;
  return false;
#endif
}

void DrainPendingJobs(napi_env env) {
  if (env == nullptr) {
    return;
  }

#ifdef ENABLE_JS_RUNTIME
  js_execute_pending_jobs(env);
#else
  (void)env;
#endif
}

void AddTimerToMainRunLoop(napi_env env, NSTimer* timer) {
  if (timer == nil) {
    return;
  }

  auto addTimer = ^{
    if ([timer isValid]) {
      [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSDefaultRunLoopMode];
    }
  };

  if ([NSThread isMainThread]) {
    addTimer();
    return;
  }

  if (shouldAvoidMainQueueSyncWhileHoldingHermesLock(env)) {
    [timer retain];
    dispatch_async(dispatch_get_main_queue(), ^{
      addTimer();
      [timer release];
    });
    return;
  }

  dispatch_sync(dispatch_get_main_queue(), addTimer);
}

void DisposeTimerHandle(napi_env callEnv, NSTimerHandle* handle, bool invalidateTimer = true) {
  if (handle == nil) {
    return;
  }

  auto disposeTimer = ^{
    @synchronized(handle) {
      if (handle->timer != nil) {
        NSTimer* rawTimer = handle->timer;
        if (invalidateTimer) {
          objc_setAssociatedObject(rawTimer, kTimerHandleAssociationKey, nil,
                                   OBJC_ASSOCIATION_ASSIGN);
          if ([rawTimer isValid]) {
            [rawTimer invalidate];
          }
        }
        handle->timer = nil;
      }
    }
  };

  if ([NSThread isMainThread]) {
    disposeTimer();
  } else if (shouldAvoidMainQueueSyncWhileHoldingHermesLock(
                 callEnv != nullptr ? callEnv : handle->env)) {
    [handle retain];
    dispatch_async(dispatch_get_main_queue(), ^{
      disposeTimer();
      [handle release];
    });
  } else {
    dispatch_sync(dispatch_get_main_queue(), disposeTimer);
  }

  napi_ref callback = nullptr;
  @synchronized(handle) {
    callback = handle->callback;
    handle->callback = nullptr;
  }

  MarkTimerInactive(handle);

  napi_env cleanupEnv = callEnv != nullptr ? callEnv : handle->env;
#ifdef TARGET_ENGINE_HERMES
  if (cleanupEnv != nullptr && handle->timerId != 0) {
    DispatchHermesTimerCallback(cleanupEnv, "__nsReleaseTimer", handle->timerId);
    handle->timerId = 0;
  }
#endif
  if (cleanupEnv != nullptr && callback != nullptr) {
    uint32_t remaining = 0;
    napi_reference_unref(cleanupEnv, callback, &remaining);
    napi_delete_reference(cleanupEnv, callback);
    DrainPendingJobs(cleanupEnv);
  }
}

void ScheduleOneShotTimerCleanup(napi_env env, NSTimerHandle* handle) {
  if (handle == nil) {
    return;
  }

  if ([NSThread isMainThread]) {
    DisposeTimerHandle(env, handle, false);
    return;
  }

  [handle retain];
  dispatch_async(dispatch_get_main_queue(), ^{
    DisposeTimerHandle(env, handle, false);
    [handle release];
  });
}
}  // namespace

namespace nativescript {

JS_CLASS_INIT(Timers::Init) {
  const napi_property_descriptor properties[] = {
#ifdef TARGET_ENGINE_HERMES
      napi_util::desc("__ns__nativeSetTimeout", SetTimeout),
      napi_util::desc("__ns__nativeSetInterval", SetInterval),
      napi_util::desc("__ns__nativeClearTimer", ClearTimer),
      napi_util::desc("queueMicrotask", QueueMicrotask),
      napi_util::desc("__ns__queueMicrotask", QueueMicrotask),
#else
      napi_util::desc("setTimeout", SetTimeout),
      napi_util::desc("setInterval", SetInterval),
      napi_util::desc("clearTimeout", ClearTimer),
      napi_util::desc("clearInterval", ClearTimer),
      napi_util::desc("__nsActiveTimerCount", ActiveTimerCount),
      napi_util::desc("queueMicrotask", QueueMicrotask),
      napi_util::desc("__ns__setTimeout", SetTimeout),
      napi_util::desc("__ns__setInterval", SetInterval),
      napi_util::desc("__ns__clearTimeout", ClearTimer),
      napi_util::desc("__ns__clearInterval", ClearTimer),
      napi_util::desc("__ns__queueMicrotask", QueueMicrotask),
#endif
  };

  napi_define_properties(env, global, sizeof(properties) / sizeof(properties[0]), properties);
#ifdef TARGET_ENGINE_HERMES
  InstallHermesTimerBridge(env);
#endif
}

JS_METHOD(Timers::SetTimeout) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, cbinfo, &argc, argv, nullptr, nullptr);

  double ms = 0;
  if (argc > 1) {
    napi_get_value_double(env, argv[1], &ms);
  }
  if (ms < 0 || !std::isfinite(ms)) {
    ms = 0;
  }

  NSTimeInterval interval = ms / 1000;

  NSTimerHandle* handle = [[NSTimerHandle alloc] init];
  handle->env = env;
  handle->callback = nullptr;
  handle->activeCounted = false;
  handle->timerId = 0;
  handle->token = new TimerToken();
#ifdef TARGET_ENGINE_HERMES
  if (argc > 0) {
    napi_get_value_int64(env, argv[0], &handle->timerId);
  }
#else
  napi_create_reference(env, argv[0], 1, &handle->callback);
#endif
  MarkTimerActive(handle);

  NSTimer* timer = [NSTimer
      timerWithTimeInterval:interval
                    repeats:NO
                      block:^(NSTimer* timer) {
                        NSTimerHandle* handle = (NSTimerHandle*)objc_getAssociatedObject(
                            timer, kTimerHandleAssociationKey);
                        if (handle == nil) {
                          return;
                        }
                        [handle retain];

                        napi_env callbackEnv = nullptr;
                        int64_t timerId = 0;
                        napi_ref callbackRef = nullptr;
                        @synchronized(handle) {
                          callbackEnv = handle->env;
                          timerId = handle->timerId;
                          callbackRef = handle->callback;
                        }

                        if (callbackEnv == nullptr) {
                          [handle release];
                          return;
                        }

                        NapiScope scope(callbackEnv);
                        MarkTimerInactive(handle);
#ifdef TARGET_ENGINE_HERMES
                        DispatchHermesTimerCallback(callbackEnv, "__nsDispatchTimeout", timerId);
#else
        if (callbackRef == nullptr) {
          [handle release];
          return;
        }
        napi_value global, callbackValue;
        napi_get_global(callbackEnv, &global);
        napi_get_reference_value(callbackEnv, callbackRef, &callbackValue);
        napi_call_function(callbackEnv, global, callbackValue, 0, nullptr, nullptr);
#endif
                        DrainPendingJobs(callbackEnv);

                        ScheduleOneShotTimerCleanup(callbackEnv, handle);
                        [handle release];
                      }];

  handle->timer = timer;
  handle->token->handle.store(handle, std::memory_order_release);
  objc_setAssociatedObject(timer, kTimerHandleAssociationKey, handle,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);

  napi_value result;
  napi_create_external(
      env, handle->token,
      [](napi_env env, void* data, void* /*hint*/) {
        TimerToken* token = static_cast<TimerToken*>(data);
        if (token == nullptr) {
          return;
        }
        token->externalAlive.store(false, std::memory_order_release);
        NSTimerHandle* handle = token->handle.load(std::memory_order_acquire);
        if (handle == nil) {
          delete token;
        }
      },
      nullptr, &result);
  // Drop creator ownership. Remaining ownership is the timer association.
  [handle release];

  AddTimerToMainRunLoop(env, timer);

  return result;
}

JS_METHOD(Timers::SetInterval) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, cbinfo, &argc, argv, nullptr, nullptr);

  double ms = 0;
  if (argc > 1) {
    napi_get_value_double(env, argv[1], &ms);
  }
  if (ms < 0 || !std::isfinite(ms)) {
    ms = 0;
  }

  NSTimeInterval interval = ms / 1000;

  NSTimerHandle* handle = [[NSTimerHandle alloc] init];
  handle->env = env;
  handle->callback = nullptr;
  handle->activeCounted = false;
  handle->timerId = 0;
  handle->token = new TimerToken();
#ifdef TARGET_ENGINE_HERMES
  if (argc > 0) {
    napi_get_value_int64(env, argv[0], &handle->timerId);
  }
#else
  napi_create_reference(env, argv[0], 1, &handle->callback);
#endif
  MarkTimerActive(handle);

  NSTimer* timer = [NSTimer
      timerWithTimeInterval:interval
                    repeats:YES
                      block:^(NSTimer* timer) {
                        NSTimerHandle* handle = (NSTimerHandle*)objc_getAssociatedObject(
                            timer, kTimerHandleAssociationKey);
                        if (handle == nil) {
                          return;
                        }
                        [handle retain];

                        napi_env callbackEnv = nullptr;
                        int64_t timerId = 0;
                        napi_ref callbackRef = nullptr;
                        @synchronized(handle) {
                          callbackEnv = handle->env;
                          timerId = handle->timerId;
                          callbackRef = handle->callback;
                        }

                        if (callbackEnv == nullptr) {
                          [handle release];
                          return;
                        }

                        NapiScope scope(callbackEnv);
#ifdef TARGET_ENGINE_HERMES
                        DispatchHermesTimerCallback(callbackEnv, "__nsDispatchInterval", timerId);
#else
        if (callbackRef == nullptr) {
          [handle release];
          return;
        }
        napi_value global, callbackValue;
        napi_get_global(callbackEnv, &global);
        napi_get_reference_value(callbackEnv, callbackRef, &callbackValue);
        napi_call_function(callbackEnv, global, callbackValue, 0, nullptr, nullptr);
#endif
                        DrainPendingJobs(callbackEnv);
                        [handle release];
                      }];

  handle->timer = timer;
  handle->token->handle.store(handle, std::memory_order_release);
  objc_setAssociatedObject(timer, kTimerHandleAssociationKey, handle,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);

  napi_value result;
  napi_create_external(
      env, handle->token,
      [](napi_env env, void* data, void* /*hint*/) {
        TimerToken* token = static_cast<TimerToken*>(data);
        if (token == nullptr) {
          return;
        }
        token->externalAlive.store(false, std::memory_order_release);
        NSTimerHandle* handle = token->handle.load(std::memory_order_acquire);
        if (handle == nil) {
          delete token;
        }
      },
      nullptr, &result);
  // Drop creator ownership. Remaining ownership is the timer association.
  [handle release];

  AddTimerToMainRunLoop(env, timer);

  return result;
}

JS_METHOD(Timers::ClearTimer) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, cbinfo, &argc, argv, nullptr, nullptr);

  if (argc < 1 || argv[0] == nullptr) {
    return nullptr;
  }

  napi_valuetype type;
  napi_typeof(env, argv[0], &type);
  if (type != napi_external) {
    return nullptr;
  }

  void* rawToken = nullptr;
  napi_get_value_external(env, argv[0], &rawToken);
  TimerToken* token = static_cast<TimerToken*>(rawToken);
  if (token == nullptr) {
    return nullptr;
  }
  NSTimerHandle* handle = token->handle.load(std::memory_order_acquire);
  if (handle == nil) {
    return nullptr;
  }
  [handle retain];
  DisposeTimerHandle(env, handle);
  [handle release];

  return nullptr;
}

JS_METHOD(Timers::QueueMicrotask) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, cbinfo, &argc, argv, nullptr, nullptr);

  if (argc < 1 || argv[0] == nullptr) {
    napi_throw_type_error(env, nullptr, "The \"callback\" argument must be of type function");
    return nullptr;
  }

  napi_valuetype callbackType;
  if (napi_typeof(env, argv[0], &callbackType) != napi_ok || callbackType != napi_function) {
    napi_throw_type_error(env, nullptr, "The \"callback\" argument must be of type function");
    return nullptr;
  }

  napi_value global;
  napi_get_global(env, &global);

  napi_value promiseCtor;
  if (napi_get_named_property(env, global, "Promise", &promiseCtor) != napi_ok) {
    napi_throw_error(env, nullptr, "Promise is not available");
    return nullptr;
  }

  napi_value resolveFn;
  if (napi_get_named_property(env, promiseCtor, "resolve", &resolveFn) != napi_ok) {
    napi_throw_error(env, nullptr, "Promise.resolve is not available");
    return nullptr;
  }

  napi_value undefinedValue;
  napi_get_undefined(env, &undefinedValue);

  napi_value promise;
  napi_value resolveArgs[1] = {undefinedValue};
  if (napi_call_function(env, promiseCtor, resolveFn, 1, resolveArgs, &promise) != napi_ok) {
    return nullptr;
  }

  napi_value thenFn;
  if (napi_get_named_property(env, promise, "then", &thenFn) != napi_ok) {
    napi_throw_error(env, nullptr, "Promise.then is not available");
    return nullptr;
  }

  if (napi_call_function(env, promise, thenFn, 1, argv, nullptr) != napi_ok) {
    return nullptr;
  }

  return napi_util::undefined(env);
}

JS_METHOD(Timers::ActiveTimerCount) {
  napi_value result = nullptr;
  napi_create_int32(env, gActiveTimers.load(std::memory_order_relaxed), &result);
  return result;
}

bool Timers::HasActiveTimers() { return gActiveTimers.load(std::memory_order_relaxed) > 0; }

}  // namespace nativescript

#endif  // __APPLE__
