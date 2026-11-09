#ifndef CALLBACK_THREADING_H
#define CALLBACK_THREADING_H

#include "js_native_api.h"

#include <functional>
#include <memory>

#if defined(ENABLE_JS_RUNTIME)
#include "jsr.h"
#endif

namespace nativescript {

namespace detail {

#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
inline thread_local int native_caller_thread_callback_depth = 0;
#endif

}  // namespace detail

inline bool isNativeCallerThreadCallbackActive() {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
  return detail::native_caller_thread_callback_depth > 0;
#else
  return false;
#endif
}

inline bool shouldInvokeCallbackOnNativeCallerThread() {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
  return isNativeCallerThreadCallbackActive();
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

    JSR* runtime = JSR::ForEnv(env);
    if (runtime == nullptr) {
      return;
    }

    jsr_ = runtime;
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
#else
    (void)env;
#endif
  }

  ~NativeCallRuntimeUnlockScope() {
#if defined(ENABLE_JS_RUNTIME) && defined(TARGET_ENGINE_HERMES)
    if (jsr_ != nullptr) {
      for (int i = 0; i < unlockedDepth_; i++) {
        jsr_->lock();
      }
    }
    if (unlockedRuntime_ && relockRuntime_) {
      relockRuntime_();
    }
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

    JSR* runtime = JSR::ForEnv(env_);
    if (runtime != nullptr) {
      jsr_ = runtime;
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
