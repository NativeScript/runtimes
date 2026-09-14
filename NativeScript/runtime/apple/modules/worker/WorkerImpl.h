#ifndef WORKER_IMPL_H
#define WORKER_IMPL_H

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "runtime/apple/NativeScriptException.h"
#include "js_native_api_types.h"
#include "native_api_util.h"
#include "runtime/apple/modules/worker/ConcurrentMap.h"
#include "runtime/apple/modules/worker/ConcurrentQueue.h"
#include "runtime/apple/modules/worker/Message.h"

namespace nativescript {

class WorkerImpl {
 public:
  WorkerImpl(napi_env env, std::function<void(napi_env, napi_value jsThis,
                                              std::shared_ptr<worker::Message>)>
                               onMessage);
  ~WorkerImpl();

  void Start(std::shared_ptr<napi_util::PersistentObject> worker,
             std::function<napi_env()> func);

  void CallOnErrorHandlers(napi_env env, napi_value error);
  void PassUncaughtExceptionFromWorkerToMain(napi_env env,
                                             NativeScriptException& ex,
                                             bool async = true);
  void PassUncaughtExceptionFromWorkerToMain(napi_env env, napi_value error,
                                             bool async = true);
  void PostMessage(std::shared_ptr<worker::Message> message);
  void Close();
  void Terminate();

  bool IsRunning() const;
  bool IsClosing() const;
  int WorkerId() const;
  inline napi_env GetMainEnv() { return mainEnv_; }
  inline std::shared_ptr<napi_util::PersistentObject> GetWorkerHandle() {
    return poWorker_;
  }

  static std::shared_ptr<ConcurrentMap<int, WorkerImpl*>> Workers;

 private:
  napi_env mainEnv_;
  napi_env workerEnv_;
  std::atomic<bool> isRunning_;
  std::atomic<bool> isClosing_;
  std::atomic<bool> isTerminating_;
  std::atomic<bool> mainEnvClosing_{false};
  bool cleanupHookRegistered_ = false;
  std::function<void(napi_env, napi_value jsThis,
                     std::shared_ptr<worker::Message>)>
      onMessage_;
  std::shared_ptr<napi_util::PersistentObject> poWorker_;
  ConcurrentQueue queue_;
  std::thread workerThread_;
  static std::atomic<int> nextId_;
  int workerId_;

  void BackgroundLooper(std::function<napi_env()> func);
  void DrainPendingTasks();
  static void CleanupMainEnv(void* data);
  static void FinishOnMainThread(int workerId);
};

}  // namespace nativescript

#endif  // WORKER_IMPL_H
