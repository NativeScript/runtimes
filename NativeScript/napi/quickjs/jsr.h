//
// Created by Ammar Ahmed on 01/12/2024.
//

#ifndef TEST_APP_JSR_H
#define TEST_APP_JSR_H
#include "js_native_api.h"
#include "jsr_common.h"
#include "quicks-runtime.h"
#include "mutex"
#include <map>
#include "ConcurrentMap.h"

class JSR {
 public:
  JSR();
  std::recursive_mutex js_mutex;
  // Drain the pending-job queue once the outermost host scope unwinds.
  int jsEnterState = 0;
  void lock() { js_mutex.lock(); }
  void unlock() { js_mutex.unlock(); }

  static tns::ConcurrentMap<napi_env, JSR*> env_to_jsr_cache;
};

class NapiScope {
 public:
  explicit NapiScope(napi_env env, bool open_handle = true) : env_(env) {
    js_lock_env(env_);
    qjs_update_stack_top(env);
    jsr_ = JSR::env_to_jsr_cache.Get(env_);
    if (jsr_) {
      jsr_->jsEnterState++;
    }
    if (open_handle) {
      napi_open_handle_scope(env_, &napiHandleScope_);
    } else {
      napiHandleScope_ = nullptr;
    }
  }

  ~NapiScope() {
    if (jsr_ && --jsr_->jsEnterState <= 0) {
      jsr_->jsEnterState = 0;
      try {
        js_execute_pending_jobs(env_);
      } catch (...) {
      }
    }
    if (napiHandleScope_) {
      napi_close_handle_scope(env_, napiHandleScope_);
    }
    js_unlock_env(env_);
  }

 private:
  napi_env env_;
  napi_handle_scope napiHandleScope_;
  JSR* jsr_ = nullptr;
};

#define JSEnterScope

#endif //TEST_APP_JSR_H
