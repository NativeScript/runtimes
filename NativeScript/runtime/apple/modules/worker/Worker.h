#ifndef WORKER_H
#define WORKER_H

#include <memory>
#include <string>
#include <vector>

#include "native_api_util.h"
#include "runtime/apple/modules/worker/Message.h"

namespace nativescript {

class Worker {
 public:
  static void Init(napi_env env, napi_value global, bool isWorkerThread);
  static void Init(napi_env env, napi_value global);

 private:
  static JS_METHOD(Constructor);
  static void Finalize(napi_env env, void* nativeObject,
                        void* finalize_hint);
  static JS_METHOD(PostMessage);
  static JS_METHOD(Terminate);
  static void OnMessage(napi_env env, napi_value jsThis,
                        std::shared_ptr<worker::Message>);
  static JS_METHOD(PostMessageToMain);
  static JS_METHOD(CloseWorker);

  static int GetWorkerId(napi_env env);
};

}  // namespace nativescript

#endif  // WORKER_H
