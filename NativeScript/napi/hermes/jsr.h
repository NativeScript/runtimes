//
// Created by Ammar Ahmed on 16/11/2024.
//

#ifndef TEST_APP_JSR_H
#define TEST_APP_JSR_H

#include "hermes/hermes.h"
#include "jsi/threadsafe.h"
#include "jsr_common.h"

#include <mutex>
#include <unordered_map>

class JSR {
 public:
  JSR();
  std::unique_ptr<facebook::jsi::ThreadSafeRuntime> runtime;
  facebook::jsi::Runtime* rt;
  std::recursive_mutex js_mutex;
  void lock();
  void unlock();
  int currentLockDepth() const;

  static JSR* ForEnv(napi_env env);
  static void RegisterEnv(napi_env env, JSR* runtime);
  static void UnregisterEnv(napi_env env);

 private:
  static std::mutex env_cache_mutex;
  static std::unordered_map<napi_env, JSR*> env_to_jsr_cache;
};

int js_current_env_lock_depth(napi_env env);
facebook::jsi::Runtime* js_get_jsi_runtime(napi_env env);

typedef struct napi_runtime__ {
  JSR* hermes;
} napi_runtime__;

class NapiScope {
 public:
  explicit NapiScope(napi_env env, bool openHandle = true) : env_(env) {
    js_lock_env(env_);
    if (openHandle) {
      napi_open_handle_scope(env_, &napiHandleScope_);
    } else {
      napiHandleScope_ = nullptr;
    }
  }

  ~NapiScope() {
    if (napiHandleScope_) {
      napi_close_handle_scope(env_, napiHandleScope_);
    }
    js_unlock_env(env_);
  }

 private:
  napi_env env_;
  napi_handle_scope napiHandleScope_;
};

#define JSEnterScope

#endif  // TEST_APP_JSR_H
