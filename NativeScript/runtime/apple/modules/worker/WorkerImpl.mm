#include <Foundation/Foundation.h>
#include <memory>
#include <string>
#include "runtime/apple/NativeScriptException.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "jsr.h"
#include "jsr_common.h"
#include "native_api_util.h"

#include "WorkerImpl.h"
#include "runtime/apple/Runtime.h"
#include "runtime/apple/Util.h"
#include "runtime/apple/modules/worker/ConcurrentMap.h"

namespace nativescript {

WorkerImpl::WorkerImpl(
    napi_env env,
    std::function<void(napi_env, napi_value jsThis, std::shared_ptr<worker::Message>)> onMessage)
    : mainEnv_(env),
      workerEnv_(nullptr),
      isRunning_(false),
      isClosing_(false),
      isTerminating_(false),
      onMessage_(onMessage) {}

WorkerImpl::~WorkerImpl() {
  if (this->cleanupHookRegistered_ && this->mainEnv_ != nullptr) {
    js_remove_env_cleanup_hook(this->mainEnv_, CleanupMainEnv, this);
    this->cleanupHookRegistered_ = false;
  }
  this->Terminate();
  if (this->workerThread_.joinable()) {
    if (this->workerThread_.get_id() == std::this_thread::get_id()) {
      this->workerThread_.detach();
    } else {
      this->workerThread_.join();
    }
  }
}

bool WorkerImpl::IsRunning() const { return this->isRunning_.load(); }

bool WorkerImpl::IsClosing() const { return this->isClosing_.load(); }

int WorkerImpl::WorkerId() const { return this->workerId_; }

void WorkerImpl::PostMessage(std::shared_ptr<worker::Message> message) {
  if (!this->isTerminating_) {
    this->queue_.Push(message);
  }
}

void WorkerImpl::Start(std::shared_ptr<napi_util::PersistentObject> poWorker,
                       std::function<napi_env()> func) {
  this->poWorker_ = poWorker;
  this->workerId_ = nextId_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (js_add_env_cleanup_hook(this->mainEnv_, CleanupMainEnv, this) != napi_ok) {
    this->poWorker_.reset();
    throw NativeScriptException("Unable to register worker cleanup.");
  }
  this->cleanupHookRegistered_ = true;
  this->isRunning_.store(true);
  Workers->Insert(this->workerId_, this);

  try {
    this->workerThread_ = std::thread([this, func = std::move(func)] {
      @autoreleasepool {
        this->BackgroundLooper(func);
      }
    });
  } catch (...) {
    Workers->Remove(this->workerId_);
    this->isRunning_.store(false);
    js_remove_env_cleanup_hook(this->mainEnv_, CleanupMainEnv, this);
    this->cleanupHookRegistered_ = false;
    this->poWorker_.reset();
    throw NativeScriptException("Unable to start worker thread.");
  }
}

void WorkerImpl::DrainPendingTasks() {
  std::vector<std::shared_ptr<worker::Message>> messages = this->queue_.PopAll();

  NapiScope scope(this->workerEnv_);
  napi_value global = napi_util::global(this->workerEnv_);

  for (std::shared_ptr<worker::Message> message : messages) {
    if (this->isTerminating_) {
      break;
    }

    this->onMessage_(this->workerEnv_, global, message);

    napi_value exception = nullptr;
    napi_get_and_clear_last_exception(this->workerEnv_, &exception);
    if (exception != nullptr && napi_util::is_of_type(this->workerEnv_, exception, napi_object)) {
      this->CallOnErrorHandlers(this->workerEnv_, exception);
    }

  }
}

void WorkerImpl::BackgroundLooper(std::function<napi_env()> func) {
  if (!this->isTerminating_) {
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    this->queue_.Initialize(
        runLoop,
        [](void* info) {
          WorkerImpl* w = static_cast<WorkerImpl*>(info);
          w->DrainPendingTasks();
        },
        this);

    this->workerEnv_ = func();
    if (this->workerEnv_ != nullptr) {
      this->DrainPendingTasks();
    }

    // check again as it could terminate before this
    if (!this->isTerminating_) {
      CFRunLoopRun();
    }
  }

  Runtime* runtime = Runtime::GetCurrentRuntime();
  delete runtime;

  this->isRunning_.store(false);

  Runtime* mainRuntime = this->mainEnvClosing_.load()
                             ? nullptr
                             : Runtime::GetRuntime(this->mainEnv_);
  if (mainRuntime != nullptr && mainRuntime->RuntimeLoop() != nullptr) {
    int workerId = this->workerId_;
    ExecuteOnRunLoop(
        mainRuntime->RuntimeLoop(),
        [mainEnv = this->mainEnv_, workerId]() mutable {
          if (WorkerImpl::Workers->Get(workerId) == nullptr) {
            return;
          }
          NapiScope scope(mainEnv);
          WorkerImpl::FinishOnMainThread(workerId);
        },
        true);
  }
}

void WorkerImpl::FinishOnMainThread(int workerId) {
  WorkerImpl* worker = Workers->Get(workerId);
  if (worker == nullptr) {
    return;
  }
  auto workerHandle = std::move(worker->poWorker_);
  Workers->Remove(workerId);
  workerHandle.reset();
}

void WorkerImpl::CleanupMainEnv(void* data) {
  auto* worker = static_cast<WorkerImpl*>(data);
  worker->cleanupHookRegistered_ = false;
  worker->mainEnvClosing_.store(true);
  worker->Terminate();
  if (worker->workerThread_.joinable() &&
      worker->workerThread_.get_id() != std::this_thread::get_id()) {
    worker->workerThread_.join();
  }
  auto workerHandle = std::move(worker->poWorker_);
  Workers->Remove(worker->workerId_);
  workerHandle.reset();
}

void WorkerImpl::Close() {
  bool wasClosing = this->isClosing_.exchange(true);
  if (wasClosing || this->isTerminating_.load()) {
    return;
  }

  CFRunLoopRef runLoop = CFRunLoopGetCurrent();
  CFRunLoopPerformBlock(runLoop, kCFRunLoopCommonModes, ^{
    this->Terminate();
  });
  CFRunLoopWakeUp(runLoop);
}

void WorkerImpl::Terminate() {
  bool wasTerminating = this->isTerminating_.exchange(true);
  if (!wasTerminating) {
    this->queue_.Terminate();
    this->isRunning_ = false;
  }
}

void WorkerImpl::CallOnErrorHandlers(napi_env env, napi_value error) {
  if (this->isTerminating_) {
    return;
  }

  napi_value global = napi_util::global(env);

  napi_value onError = napi_util::get_property(env, global, "onerror");

  if (onError != nullptr && napi_util::is_of_type(env, onError, napi_function)) {
    napi_value args[1] = {error};
    napi_value result;
    napi_status status = napi_call_function(env, global, onError, 1, args, &result);
    if (status != napi_ok) {
      napi_get_and_clear_last_exception(env, &result);
      this->PassUncaughtExceptionFromWorkerToMain(env, error);
    }

    napi_value booleanValue;
    napi_coerce_to_bool(env, result, &booleanValue);
    bool handled = false;
    napi_get_value_bool(env, booleanValue, &handled);
    if (handled) {
      // Do nothing, exception is handled and does not need to be raised to the main thread's
      // onerror handler
      return;
    }

    this->PassUncaughtExceptionFromWorkerToMain(env, error);
  }
}

void WorkerImpl::PassUncaughtExceptionFromWorkerToMain(napi_env env, NativeScriptException& ex,
                                                       bool async) {
  Runtime* runtime = Runtime::GetRuntime(mainEnv_);
  if (runtime == nullptr) {
    return;
  }

  auto mainEnv = this->mainEnv_;
  int workerId = this->workerId_;
  if (this->poWorker_ == nullptr) {
    return;
  }
  auto name = ex.Name();
  auto description = ex.Description();

  ExecuteOnRunLoop(
      runtime->RuntimeLoop(),
      [mainEnv, workerId, name, description]() mutable {
        WorkerImpl* worker = Workers->Get(workerId);
        if (worker == nullptr) {
          return;
        }
        auto workerHandle = worker->GetWorkerHandle();
        if (workerHandle == nullptr) {
          return;
        }
        napi_env env = mainEnv;
        NapiScope scope(env);
        napi_value workerObj = workerHandle->GetValue();
        napi_value onError = napi_util::get_property(env, workerObj, "onerror");

        if (onError != nullptr && napi_util::is_of_type(env, onError, napi_function)) {
          napi_value arg;
          napi_create_error(env, napi_util::to_js_string(env, name),
                            napi_util::to_js_string(env, description), &arg);
          napi_value result;
          napi_call_function(env, workerObj, onError, 1, &arg, &result);
        } else {
          NSLog(@"Uncaught exception in worker: %s", description.c_str());
        }
      },
      async);
}

void WorkerImpl::PassUncaughtExceptionFromWorkerToMain(napi_env env, napi_value error, bool async) {
  napi_value message = napi_util::get_property(env, error, "message");
  napi_value stackTrace = napi_util::get_property(env, error, "stack");

  std::string messageStr = napi_util::get_cxx_string(env, message);
  std::string stackTraceStr = napi_util::get_cxx_string(env, stackTrace);

  Runtime* runtime = Runtime::GetRuntime(mainEnv_);
  if (runtime == nullptr) {
    return;
  }

  auto mainEnv = this->mainEnv_;
  int workerId = this->workerId_;
  if (this->poWorker_ == nullptr) {
    return;
  }

  ExecuteOnRunLoop(
      runtime->RuntimeLoop(),
      [mainEnv, workerId, messageStr, stackTraceStr]() mutable {
        WorkerImpl* worker = Workers->Get(workerId);
        if (worker == nullptr) {
          return;
        }
        auto workerHandle = worker->GetWorkerHandle();
        if (workerHandle == nullptr) {
          return;
        }
        napi_env env = mainEnv;
        NapiScope scope(env);
        napi_value workerObj = workerHandle->GetValue();
        napi_value onError = napi_util::get_property(env, workerObj, "onerror");

        if (onError != nullptr && napi_util::is_of_type(env, onError, napi_function)) {
          napi_value arg;
          napi_create_object(env, &arg);
          napi_set_named_property(env, arg, "message",
                                  napi_util::to_js_string(env, messageStr));
          napi_set_named_property(env, arg, "stack",
                                  napi_util::to_js_string(env, stackTraceStr));
          napi_value result;
          napi_call_function(env, workerObj, onError, 1, &arg, &result);
        } else {
          NSLog(@"Uncaught exception in worker: %s\n  at\n%s", messageStr.c_str(),
                stackTraceStr.c_str());
        }
      },
      async);
}

std::atomic<int> WorkerImpl::nextId_(0);

std::shared_ptr<ConcurrentMap<int, WorkerImpl*>> WorkerImpl::Workers =
    std::make_shared<ConcurrentMap<int, WorkerImpl*>>();

}  // namespace nativescript
