#include "Worker.h"
#include <Foundation/NSObjCRuntime.h>
#include <memory>
#include <string>
#include "runtime/apple/NativeScriptException.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "jsr.h"
#include "native_api_util.h"
#include "runtime/apple/Runtime.h"
#include "runtime/apple/Util.h"
#include "runtime/apple/modules/worker/WorkerImpl.h"

namespace nativescript {

void Worker::Init(napi_env env, napi_value global) { Init(env, global, true); }

void Worker::Init(napi_env env, napi_value global, bool isWorkerThread) {
  if (isWorkerThread) {
    napi_property_descriptor globalProperties[] = {
        napi_util::desc("postMessage", PostMessageToMain),
        napi_util::desc("close", CloseWorker),
    };

    napi_define_properties(env, global, 2, globalProperties);
  }

  napi_value Worker;
#ifdef TARGET_ENGINE_HERMES
  napi_create_function(env, "Worker", NAPI_AUTO_LENGTH, Constructor, nullptr,
                       &Worker);
#else
  napi_property_descriptor properties[] = {
      napi_util::desc("postMessage", PostMessage),
      napi_util::desc("terminate", Terminate),
  };
  napi_define_class(env, "Worker", NAPI_AUTO_LENGTH, Constructor, nullptr, 2, properties, &Worker);
#endif

  napi_set_named_property(env, global, "Worker", Worker);
}

JS_METHOD(Worker::Constructor) {
  try {
    size_t argc = 1;
    napi_value argv[1];
    napi_value jsThis;
    napi_get_cb_info(env, cbinfo, &argc, argv, &jsThis, nullptr);

#ifdef TARGET_ENGINE_HERMES
    if (!napi_util::is_of_type(env, jsThis, napi_object)) {
      napi_create_object(env, &jsThis);
    }

    napi_property_descriptor properties[] = {
        napi_util::desc("postMessage", PostMessage),
        napi_util::desc("terminate", Terminate),
    };
    napi_define_properties(env, jsThis, 2, properties);
#endif

    std::string workerPath = napi_util::get_cxx_string(env, argv[0]);

    WorkerImpl* worker = new WorkerImpl(env, Worker::OnMessage);
    napi_status wrapStatus =
        napi_wrap(env, jsThis, worker, Worker::Finalize, nullptr, nullptr);
    if (wrapStatus != napi_ok) {
      delete worker;
      throw NativeScriptException(env, "Unable to wrap Worker instance.");
    }

    std::shared_ptr<napi_util::PersistentObject> poWorker =
        std::make_shared<napi_util::PersistentObject>(env, jsThis);

    std::function<napi_env()> func([worker, workerPath]() {
      Runtime* runtime = new Runtime();
      try {
        runtime->Init(true);
        napi_env env = runtime->GetEnv();
        NapiScope scope(env);
        runtime->SetWorkerId(worker->WorkerId());
        runtime->RunModule(workerPath);
        return env;
      } catch (NativeScriptException& ex) {
        worker->PassUncaughtExceptionFromWorkerToMain(runtime->GetEnv(), ex, false);
        worker->Terminate();
        delete runtime;
        return static_cast<napi_env>(nullptr);
      }
    });

    worker->Start(poWorker, func);

    return jsThis;
  } catch (NativeScriptException& ex) {
    ex.ReThrowToJS(env);
    return nullptr;
  }
}

void Worker::Finalize(napi_env env, void* nativeObject, void* finalize_hint) {
  WorkerImpl* worker = reinterpret_cast<WorkerImpl*>(nativeObject);
  if (worker != nullptr) {
    delete worker;
  }
}

JS_METHOD(Worker::PostMessageToMain) {
  try {
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info(env, cbinfo, &argc, argv, nullptr, nullptr);

    if (argc < 1) {
      throw NativeScriptException("Not enough arguments.");
      return nullptr;
    }

    if (argc > 1) {
      throw NativeScriptException("Too many arguments passed.");
      return nullptr;
    }

    int workerId = Worker::GetWorkerId(env);
    WorkerImpl* worker = WorkerImpl::Workers->Get(workerId);

    if (worker == nullptr) {
      throw NativeScriptException("Worker is not initialized.");
      return nullptr;
    }

    if (!worker->IsRunning()) {
      return nullptr;
    }

    auto message = std::make_shared<worker::Message>();
    napi_value object;
    napi_create_object(env, &object);
    napi_set_named_property(env, object, "data", argv[0]);
    message->Serialize(env, object);

    napi_env mainEnv = worker->GetMainEnv();
    Runtime* mainRuntime = Runtime::GetRuntime(mainEnv);
    if (mainRuntime == nullptr || worker->GetWorkerHandle() == nullptr) {
      return nullptr;
    }

    CFRunLoopRef queue = mainRuntime->RuntimeLoop();
    if (queue == nullptr) {
      return nullptr;
    }

    ExecuteOnRunLoop(
        queue,
        [mainEnv, workerId, message]() mutable {
          WorkerImpl* activeWorker = WorkerImpl::Workers->Get(workerId);
          if (activeWorker == nullptr) {
            return;
          }
          auto workerHandle = activeWorker->GetWorkerHandle();
          if (workerHandle == nullptr) {
            return;
          }
          NapiScope scope(mainEnv);
          napi_value workerInstance = workerHandle->GetValue();
          Worker::OnMessage(mainEnv, workerInstance, message);
        },
        true);
  } catch (NativeScriptException& ex) {
    ex.ReThrowToJS(env);
  }

  return nullptr;
}

JS_METHOD(Worker::PostMessage) {
  try {
    napi_value argv[1];
    size_t argc = 1;
    napi_value jsThis;
    napi_get_cb_info(env, cbinfo, &argc, argv, &jsThis, nullptr);

    if (argc < 1) {
      throw NativeScriptException("Not enough arguments.");
      return nullptr;
    }

    if (argc > 1) {
      throw NativeScriptException("Too many arguments passed.");
      return nullptr;
    }

    WorkerImpl* worker = nullptr;
    napi_unwrap(env, jsThis, reinterpret_cast<void**>(&worker));

    if (worker == nullptr) {
      throw NativeScriptException("Worker is not initialized.");
      return nullptr;
    }

    if (!worker->IsRunning() || worker->IsClosing()) {
      return nullptr;
    }

    auto message = std::make_shared<worker::Message>();

    napi_value object;
    napi_create_object(env, &object);

    napi_set_named_property(env, object, "data", argv[0]);

    message->Serialize(env, object);
    worker->PostMessage(message);
  } catch (NativeScriptException& ex) {
    ex.ReThrowToJS(env);
  }

  return nullptr;
}

void Worker::OnMessage(napi_env env, napi_value receiver,
                       std::shared_ptr<worker::Message> message) {
  napi_value onMessageFunc = nullptr;
  napi_status status = napi_get_named_property(env, receiver, "onmessage", &onMessageFunc);

  if (!napi_util::is_of_type(env, onMessageFunc, napi_function)) {
    return;
  }

  napi_value result;
  napi_value arg = message->Deserialize(env);

  status = napi_call_function(env, receiver, onMessageFunc, 1, &arg, &result);

  if (status != napi_ok) {
    if (status == napi_pending_exception) {
      napi_value exception;
      napi_get_and_clear_last_exception(env, &exception);
      if (exception != nullptr && napi_util::is_of_type(env, exception, napi_object)) {
        napi_value stack = napi_util::get_property(env, exception, "stack");
        if (stack != nullptr) {
          std::string stackStr = napi_util::get_cxx_string(env, stack);
          NSLog(@"Worker::OnMessage - exception stack: %s", stackStr.c_str());
        }
      }
    }

    return;
  }
}

JS_METHOD(Worker::CloseWorker) {
  try {
    napi_value jsThis = napi_util::global(env);

    int workerId = Worker::GetWorkerId(env);
    WorkerImpl* worker = WorkerImpl::Workers->Get(workerId);

    if (worker == nullptr) {
      throw NativeScriptException("Worker is not initialized.");
    }

    if (!worker->IsRunning() || worker->IsClosing()) {
      return nullptr;
    }

    worker->Close();

    napi_value oncloseVal;
    napi_get_named_property(env, jsThis, "onclose", &oncloseVal);

    if (!napi_util::is_of_type(env, oncloseVal, napi_function)) {
      return nullptr;
    }

    napi_value result;
    napi_status status = napi_call_function(env, jsThis, oncloseVal, 0, nullptr, &result);
    if (status != napi_ok) {
      napi_value exception;
      napi_get_and_clear_last_exception(env, &exception);
      if (exception != nullptr && napi_util::is_of_type(env, exception, napi_object)) {
        worker->CallOnErrorHandlers(env, exception);
      }
    }
  } catch (NativeScriptException& ex) {
    ex.ReThrowToJS(env);
  }

  return nullptr;
}

JS_METHOD(Worker::Terminate) {
  try {
    napi_value jsThis;
    napi_get_cb_info(env, cbinfo, nullptr, nullptr, &jsThis, nullptr);

    WorkerImpl* worker = nullptr;
    napi_unwrap(env, jsThis, reinterpret_cast<void**>(&worker));

    if (worker == nullptr) {
      throw NativeScriptException("Worker is not initialized.");
    }

    worker->Terminate();
  } catch (NativeScriptException& ex) {
    ex.ReThrowToJS(env);
  }

  return nullptr;
}

int Worker::GetWorkerId(napi_env env) {
  Runtime* runtime = Runtime::GetRuntime(env);
  return runtime != nullptr ? runtime->WorkerId() : -1;
}

}  // namespace nativescript
