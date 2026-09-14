#ifndef RUNTIME_H
#define RUNTIME_H

#include <CoreFoundation/CFRunLoop.h>

#include <unordered_map>

#include "js_native_api_types.h"
#include "runtime/apple/SpinLock.h"
#include "runtime/apple/modules/RuntimeModules.h"

typedef napi_value (*napi_module_init)(napi_env env, napi_value exports);

extern "C" {
void node_module_register(const char* name, napi_module_init init);
}

namespace nativescript {

extern std::unordered_map<std::string, napi_module_init> napiModuleRegistry;

class Runtime {
 public:
  Runtime();
  ~Runtime();

  void Init(bool isWorker = false);

  inline napi_env GetEnv() { return env_; }
  static Runtime* GetRuntime(napi_env env);

  void RunScript(std::string& script, std::string file = "<anonymous>");
  napi_value RunModule(std::string spec);
  void RunMainModule();

  const int WorkerId();
  void SetWorkerId(int workerId);
  inline bool IsRuntimeWorker() { return workerId_ > 0; }

  static bool IsWorker() {
    if (currentRuntime_ == nullptr) {
      return false;
    }

    return currentRuntime_->IsRuntimeWorker();
  }

  inline CFRunLoopRef RuntimeLoop() { return runtimeLoop_; }

  void RunLoop();

  static Runtime* GetCurrentRuntime() { return currentRuntime_; }

  static bool IsAlive(napi_env env);

 private:
  int workerId_;
  CFRunLoopRef runtimeLoop_;
  CFRunLoopObserverRef microtaskObserver_;
  double startTime_;
  double realtimeOrigin_;

  napi_runtime runtime_ = nullptr;
  napi_env env_ = nullptr;
  napi_handle_scope globalScope_ = nullptr;
  RuntimeModules modules_ = RuntimeModules();

  static thread_local Runtime* currentRuntime_;
  static SpinMutex envsMutex_;
  static std::atomic<int> nextIsolateId;

  // std::shared_ptr<ConcurrentMap<int, std::shared_ptr<Caches::WorkerState>>>
  //     workerCache_;
};

}  // namespace nativescript

#endif  // RUNTIME_H
