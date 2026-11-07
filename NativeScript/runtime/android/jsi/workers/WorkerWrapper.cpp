#include "WorkerWrapper.h"

#include <android/looper.h>
#include <pthread.h>
#include <unistd.h>

#include <thread>
#include <vector>

#include "ArgConverter.h"
#include "CallbackHandlers.h"
#include "GlobalHelpers.h"
#include "JEnv.h"
#include "JniLocalRef.h"
#include "LooperTasks.h"
#include "NativeScriptAssert.h"
#include "NativeScriptException.h"
#include "Runtime.h"

namespace tns {

WorkerWrapper::WorkerWrapper(engine::Runtime& parentRt, int workerId, std::string workerPath,
                             std::string callingDir, int priority,
                             const engine::Value& workerObject)
        : parentRt_(&Runtime::GetRuntime(parentRt)->GetJSRuntime()),
          // runs on the parent's thread, where the parent runtime is alive
          parentHost_(Runtime::GetRuntime(parentRt)->GetEngineHost()),
          parentTasks_(Runtime::GetRuntime(parentRt)->GetLooperTasks()),
          workerRt_(nullptr),
          runtime_(nullptr),
          workerId_(workerId),
          workerPath_(std::move(workerPath)),
          callingDir_(std::move(callingDir)),
          // workerPath_ (not workerPath) - the parameter was just moved from
          threadName_("W" + std::to_string(workerId) + ": " + workerPath_),
          priority_(priority),
          // Owned: it has to survive the handle scope the constructor was
          // called in, and every later use is on the parent's thread.
          poWorker_(parentRt, workerObject),
          isClosing_(false),
          isTerminating_(false),
          isDisposed_(false),
          javaLooperRef_(nullptr) {
}

void WorkerWrapper::Start() {
    auto self = shared_from_this();
    std::thread thread([self]() {
        self->BackgroundLooper(self);
    });
    thread.detach();
}

void WorkerWrapper::PostMessage(std::shared_ptr<worker::Message> message) {
    if (!isTerminating_ && !isClosing_) {
        queue_.Push(message);
    }
}

void WorkerWrapper::PostMessageToParent(std::shared_ptr<worker::Message> message) {
    if (isTerminating_) {
        return;
    }

    auto parentTasks = parentTasks_.lock();
    if (parentTasks == nullptr) {
        // the parent runtime is gone (e.g. a parent worker that shut down)
        return;
    }

    int workerId = workerId_;
    parentTasks->Post([workerId, message]() {
        WorkerWrapper::FireMessageOnParentWorkerObject(workerId, message);
    });
}

void WorkerWrapper::Terminate() {
    if (isClosing_ || isDisposed_) {
        // The worker is already shutting down on its own; nothing to do.
        return;
    }

    bool wasTerminating = isTerminating_.exchange(true);
    if (wasTerminating) {
        return;
    }

    // Cooperative: there is no cross-thread "interrupt running JS" primitive
    // here (v8::TerminateExecution has no engine:: equivalent), so we simply
    // quit the worker's Java looper. Any JS already running finishes its
    // current turn; once the callback unwinds, Looper.loop() returns and the
    // thread shuts down. A worker stuck in a synchronous busy-loop will NOT be
    // preempted.
    QuitLooper();
}

void WorkerWrapper::Close() {
    bool wasClosing = isClosing_.exchange(true);
    if (wasClosing) {
        return;
    }

    // Once the current callback unwinds, Looper.loop() returns and the
    // thread proceeds to cleanup. Pending messages are dropped, matching the
    // previous front-of-queue TerminateAndCloseThread behavior.
    QuitLooper();
}

void WorkerWrapper::QuitLooper() {
    std::lock_guard<std::mutex> lock(looperMutex_);
    if (javaLooperRef_ != nullptr) {
        JEnv env;
        env.CallVoidMethod(javaLooperRef_, LOOPER_QUIT_METHOD_ID);
    }
}

int WorkerWrapper::DrainCallback(int fd, int events, void* data) {
    uint64_t value;
    read(fd, &value, sizeof(value));

    auto wrapper = static_cast<WorkerWrapper*>(data);
    wrapper->DrainPendingTasks();
    return 1;
}

void WorkerWrapper::DrainPendingTasks() {
    engine::Runtime* rtPtr = workerRt_.load();
    if (rtPtr == nullptr || isTerminating_) {
        return;
    }

    auto messages = queue_.PopAll();
    if (messages.empty()) {
        return;
    }

    JSScope scope(workerHost_);
    engine::Runtime& rt = *rtPtr;
    engine::Object globalObject = rt.global();

    for (auto& message : messages) {
        if (isTerminating_ || isClosing_) {
            break;
        }

        engine::Value callback = globalObject.getProperty(rt, "onmessage");
        if (!callback.isObject() || !callback.asObjectBorrowed(rt).isFunction(rt)) {
            DEBUG_WRITE(
                    "WORKER: couldn't fire a worker's `onmessage` callback because it isn't implemented!");
            continue;
        }

        engine::Object event(rt);
        engine::Value data = tns::JsonParseString(rt, message->data);
        if (!data.isUndefined() && !data.isNull()) {
            event.setProperty(rt, "data", data);
        }

        engine::Value args[1] = {engine::Value(rt, event)};
        try {
            callback.asObject(rt).asFunction(rt).callWithThis(rt, globalObject, args, 1);
        } catch (engine::JSError& error) {
            if (!isTerminating_) {
                CallbackHandlers::CallWorkerScopeOnErrorHandle(rt, error);
            }
        }
    }
}

void WorkerWrapper::FireMessageOnParentWorkerObject(int workerId,
                                                    std::shared_ptr<worker::Message> message) {
    auto wrapper = WorkerWrapper::GetById(workerId);
    if (wrapper == nullptr) {
        DEBUG_WRITE("MAIN: no worker instance was found with workerId=%d.", workerId);
        return;
    }

    JSScope scope(wrapper->parentHost_);
    engine::Runtime& rt = *wrapper->parentRt_;

    if (wrapper->poWorker_.isUndefined() || wrapper->poWorker_.isNull()) {
        DEBUG_WRITE(
                "MAIN: couldn't fire a worker(id=%d) object's `onmessage` callback because the worker has been cleared.",
                workerId);
        return;
    }

    engine::Object worker = wrapper->poWorker_.asObject(rt);

    engine::Value callback = worker.getProperty(rt, "onmessage");
    if (!callback.isObject() || !callback.asObjectBorrowed(rt).isFunction(rt)) {
        DEBUG_WRITE(
                "MAIN: couldn't fire a worker(id=%d) object's `onmessage` callback because it isn't implemented.",
                workerId);
        return;
    }

    engine::Object event(rt);
    event.setProperty(rt, "data", tns::JsonParseString(rt, message->data));

    engine::Value args[1] = {engine::Value(rt, event)};
    try {
        callback.asObject(rt).asFunction(rt).callWithThis(rt, worker, args, 1);
    } catch (engine::JSError& error) {
        // Surface to Java; LooperTasks::Drain wraps this in a try/catch.
        throw NativeScriptException(rt, error, "Error calling onmessage on Worker object");
    }
}

void WorkerWrapper::PassUncaughtExceptionFromWorkerToParent(const std::string& message,
                                                            const std::string& filename,
                                                            const std::string& stackTrace,
                                                            int lineno) {
    auto parentTasks = parentTasks_.lock();
    if (parentTasks == nullptr) {
        // the parent runtime is gone (e.g. a parent worker that shut down)
        return;
    }

    int workerId = workerId_;
    std::string threadName = threadName_;

    parentTasks->Post([workerId, message, filename, stackTrace, lineno, threadName]() {
        WorkerWrapper::FireErrorOnParentWorkerObject(workerId, message, stackTrace, filename,
                                                     lineno, threadName);
    });
}

void WorkerWrapper::FireErrorOnParentWorkerObject(int workerId, const std::string& message,
                                                  const std::string& stackTrace,
                                                  const std::string& filename, int lineno,
                                                  const std::string& threadName) {
    auto wrapper = WorkerWrapper::GetById(workerId);
    if (wrapper == nullptr) {
        DEBUG_WRITE("MAIN: no worker instance was found with workerId=%d.", workerId);
        return;
    }

    JSScope scope(wrapper->parentHost_);
    engine::Runtime& rt = *wrapper->parentRt_;

    if (wrapper->poWorker_.isUndefined() || wrapper->poWorker_.isNull()) {
        DEBUG_WRITE(
                "MAIN: couldn't fire a worker(id=%d) object's `onerror` callback because the worker has been cleared.",
                workerId);
        return;
    }

    engine::Object worker = wrapper->poWorker_.asObject(rt);

    engine::Value callback = worker.getProperty(rt, "onerror");

    if (callback.isObject() && callback.asObjectBorrowed(rt).isFunction(rt)) {
        engine::Object errEvent = GlobalHelpers::CreateError(rt, message);

        // Combine the worker-side stack trace with the Worker object's captured
        // construction stack (main thread), mirroring the old fork behavior.
        engine::Value mainStackValue = worker.getProperty(rt, "__stack__");
        std::string fullStack = stackTrace;
        if (mainStackValue.isString()) {
            std::string mainStack = mainStackValue.asString(rt).utf8(rt);
            auto nl = mainStack.find_first_of("\n");
            if (nl != std::string::npos) {
                fullStack = stackTrace + "\n" + mainStack.substr(nl + 1);
            }
        }

        errEvent.setProperty(rt, "stack", engine::String::createFromUtf8(rt, fullStack));

        engine::Value args[1] = {engine::Value(rt, errEvent)};
        engine::Value result;
        try {
            result = callback.asObject(rt).asFunction(rt).callWithThis(rt, worker, args, 1);
        } catch (engine::JSError& error) {
            throw NativeScriptException(rt, error, "Error calling onerror on Worker object");
        }

        // If the handler returns a truthy value, the exception is handled.
        if (!result.isUndefined() && !result.isNull()) {
            if (result.isBool() && result.getBool()) {
                return;
            }
        }
    }

    DEBUG_WRITE(
            "Unhandled exception in '%s' thread. file: %s, line %d, message: %s\nStackTrace: %s",
            threadName.c_str(), filename.c_str(), lineno, message.c_str(), stackTrace.c_str());
}

void WorkerWrapper::BackgroundLooper(std::shared_ptr<WorkerWrapper> self) {
    JavaVM* jvm = Runtime::GetJVM();
    JNIEnv* jniEnv = nullptr;

    JavaVMAttachArgs attachArgs;
    attachArgs.version = JNI_VERSION_1_6;
    attachArgs.name = const_cast<char*>(threadName_.c_str());
    attachArgs.group = nullptr;
    jvm->AttachCurrentThread(&jniEnv, &attachArgs);

    // pthread names are limited to 15 chars
    pthread_setname_np(pthread_self(), threadName_.substr(0, 15).c_str());

    int runtimeId = -1;

    try {
        JEnv env;

        // Performs the cgroup/scheduling-policy move in addition to the nice
        // value, exactly like the previous Java-side
        // Process.setThreadPriority(THREAD_PRIORITY_BACKGROUND) call.
        env.CallStaticVoidMethod(PROCESS_CLASS, SET_THREAD_PRIORITY_METHOD_ID, priority_);

        if (!isTerminating_ && !isClosing_) {
            // Prepares the Java Looper for this thread and creates the
            // per-worker com.tns.Runtime (which creates the worker runtime on
            // this thread via initNativeScript).
            JniLocalRef callingDir(env.NewStringUTF(callingDir_.c_str()));
            runtimeId = env.CallStaticIntMethod(RUNTIME_CLASS, INIT_WORKER_RUNTIME_METHOD_ID,
                                                workerId_, (jstring) callingDir);
            runtime_ = Runtime::GetRuntime(runtimeId);

            {
                std::lock_guard<std::mutex> lock(looperMutex_);
                JniLocalRef looper(env.CallStaticObjectMethod(LOOPER_CLASS, MY_LOOPER_METHOD_ID));
                javaLooperRef_ = env.NewGlobalRef(looper);
            }

            // Looper.prepare() ran above, so ALooper_forThread() returns the
            // native looper backing the Java one - fds added here are pumped by
            // Looper.loop().
            queue_.Initialize(ALooper_forThread(), WorkerWrapper::DrainCallback, this);

            workerHost_ = runtime_->GetEngineHost();
            engine::Runtime* workerRt = &runtime_->GetJSRuntime();
            workerRt_.store(workerRt);
            {
                std::lock_guard<std::mutex> lock(registryMutex_);
                rtRegistry_[workerRt->identity()] = this;
            }

            if (!isTerminating_) {
                JSScope scope(workerHost_);

                // A worker script that throws arrives as a thrown JSError
                // rather than a pending-exception flag, so the message and
                // stack come off the error object the engine layer already
                // captured. JSError::stack() is read eagerly at throw time,
                // which is why it is still readable here.
                try {
                    runtime_->RunWorker(workerPath_);
                } catch (engine::JSError& error) {
                    if (!isTerminating_) {
                        PassUncaughtExceptionFromWorkerToParent(std::string(error.what()),
                                                                workerPath_, error.stack(), 0);
                    }
                }
            }

            // Deliver messages that were posted before the worker was ready.
            DrainPendingTasks();

            if (!isTerminating_ && !isClosing_) {
                // Blocks, pumping Java Handler messages (cross-thread Java->JS
                // calls), timers and the worker inbox until quit() is called.
                env.CallStaticVoidMethod(RUNTIME_CLASS, RUN_WORKER_LOOP_METHOD_ID);
            }
        }
    } catch (NativeScriptException& ex) {
        if (jniEnv->ExceptionCheck()) {
            jniEnv->ExceptionClear();
        }
        if (!isTerminating_) {
            PassUncaughtExceptionFromWorkerToParent(std::string(ex.what()), workerPath_, "", 0);
        }
    } catch (std::exception& ex) {
        DEBUG_WRITE_FORCE("Worker(id=%d) error: c++ exception: %s", workerId_, ex.what());
    } catch (...) {
        DEBUG_WRITE_FORCE("Worker(id=%d) error: unknown c++ exception!", workerId_);
    }

    // ----- Shutdown (close, terminate or bootstrap failure) -----

    isTerminating_ = true;

    // Terminate any workers this worker created (nested workers). Their Worker
    // object handles live in this runtime, so they must be released before it is
    // disposed below. Each child cascades to its own children during shutdown.
    {
        engine::Runtime* workerRt = workerRt_.load();
        if (workerRt != nullptr) {
            TerminateChildren(*workerRt);
        }
    }

    // On this thread: safe to unregister the inbox fd from the looper.
    queue_.Terminate();

    if (runtime_ != nullptr) {
        engine::Runtime* workerRt = workerRt_.load();

        try {
            // Java-side detach (GcListener.unsubscribe + runtimeCache.remove)
            // must happen before the runtime is disposed.
            JEnv env;
            env.CallStaticVoidMethod(RUNTIME_CLASS, DETACH_WORKER_RUNTIME_METHOD_ID, runtimeId);
        } catch (NativeScriptException& ex) {
            if (jniEnv->ExceptionCheck()) {
                jniEnv->ExceptionClear();
            }
            DEBUG_WRITE_FORCE("Worker(id=%d) error while detaching Java runtime: %s", workerId_,
                              ex.what());
        }

        {
            std::lock_guard<std::mutex> lock(registryMutex_);
            if (workerRt != nullptr) {
                rtRegistry_.erase(workerRt->identity());
            }
        }

        workerRt_.store(nullptr);

        // Enters the engine scope, tears the runtime down and deletes it.
        Runtime::DisposeWorkerRuntime(runtime_);
        runtime_ = nullptr;
        // The last reference this thread holds to the worker VM, and it is
        // dropped only after DisposeWorkerRuntime's scope has fully unwound.
        workerHost_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(looperMutex_);
        if (javaLooperRef_ != nullptr) {
            jniEnv->DeleteGlobalRef(javaLooperRef_);
            javaLooperRef_ = nullptr;
        }
    }

    isDisposed_ = true;

    // Notify the parent thread so the Worker object handle and the registry
    // entry are released (no-op if terminate() or the parent's own shutdown
    // already cleared them).
    if (auto parentTasks = parentTasks_.lock()) {
        int workerId = workerId_;
        parentTasks->Post([workerId]() {
            WorkerWrapper::ClearWorkerOnParent(workerId);
        });
    }

    // ART aborts if a native thread exits while still attached. This must be
    // the very last JNI-touching action on this thread.
    jvm->DetachCurrentThread();
}

int WorkerWrapper::NextWorkerId() {
    return nextWorkerId_.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::shared_ptr<WorkerWrapper> WorkerWrapper::GetById(int workerId) {
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto it = registry_.find(workerId);
    return it != registry_.end() ? it->second : nullptr;
}

void WorkerWrapper::Insert(int workerId, std::shared_ptr<WorkerWrapper> wrapper) {
    std::lock_guard<std::mutex> lock(registryMutex_);
    registry_.emplace(workerId, std::move(wrapper));
}

void WorkerWrapper::ClearWorkerOnParent(int workerId) {
    std::shared_ptr<WorkerWrapper> wrapper;
    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        auto it = registry_.find(workerId);
        if (it == registry_.end()) {
            return;
        }
        wrapper = it->second;
        registry_.erase(it);
    }

    // The scope has to cover the *test* as well as the assignment. Asking an
    // owned engine::Value what it holds reads its handle back, which needs a
    // HandleScope just as much as releasing it does -- and this runs from
    // LooperTasks::Drain on the parent's looper, where no scope is open.
    JSScope scope(wrapper->parentHost_);
    if (!wrapper->poWorker_.isUndefined()) {
        wrapper->poWorker_ = engine::Value::undefined();
    }
}

void WorkerWrapper::TerminateChildren(engine::Runtime& parentRt) {
    std::vector<std::shared_ptr<WorkerWrapper>> children;
    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        for (auto& entry : registry_) {
            if (entry.second->parentRt_->identity() == parentRt.identity()) {
                children.push_back(entry.second);
            }
        }
    }

    for (auto& child : children) {
        DEBUG_WRITE("Terminating nested worker(id=%d) because its parent is shutting down",
                    child->workerId_);
        child->Terminate();
        ClearWorkerOnParent(child->workerId_);
    }
}

WorkerWrapper* WorkerWrapper::FromRuntime(engine::Runtime& rt) {
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto it = rtRegistry_.find(rt.identity());
    return it != rtRegistry_.end() ? it->second : nullptr;
}

void WorkerWrapper::EnsureJniCached() {
    if (RUNTIME_CLASS != nullptr) {
        return;
    }

    JEnv env;

    RUNTIME_CLASS = env.FindClass("com/tns/Runtime");
    assert(RUNTIME_CLASS != nullptr);
    INIT_WORKER_RUNTIME_METHOD_ID =
            env.GetStaticMethodID(RUNTIME_CLASS, "initWorkerRuntime", "(ILjava/lang/String;)I");
    RUN_WORKER_LOOP_METHOD_ID = env.GetStaticMethodID(RUNTIME_CLASS, "runWorkerLoop", "()V");
    DETACH_WORKER_RUNTIME_METHOD_ID =
            env.GetStaticMethodID(RUNTIME_CLASS, "detachWorkerRuntime", "(I)V");

    LOOPER_CLASS = env.FindClass("android/os/Looper");
    assert(LOOPER_CLASS != nullptr);
    MY_LOOPER_METHOD_ID = env.GetStaticMethodID(LOOPER_CLASS, "myLooper", "()Landroid/os/Looper;");
    LOOPER_QUIT_METHOD_ID = env.GetMethodID(LOOPER_CLASS, "quit", "()V");

    PROCESS_CLASS = env.FindClass("android/os/Process");
    assert(PROCESS_CLASS != nullptr);
    SET_THREAD_PRIORITY_METHOD_ID =
            env.GetStaticMethodID(PROCESS_CLASS, "setThreadPriority", "(I)V");
}

std::mutex WorkerWrapper::registryMutex_;
std::map<int, std::shared_ptr<WorkerWrapper>> WorkerWrapper::registry_;
std::map<const void*, WorkerWrapper*> WorkerWrapper::rtRegistry_;
std::atomic_int WorkerWrapper::nextWorkerId_(0);

jclass WorkerWrapper::RUNTIME_CLASS = nullptr;
jclass WorkerWrapper::LOOPER_CLASS = nullptr;
jclass WorkerWrapper::PROCESS_CLASS = nullptr;
jmethodID WorkerWrapper::INIT_WORKER_RUNTIME_METHOD_ID = nullptr;
jmethodID WorkerWrapper::RUN_WORKER_LOOP_METHOD_ID = nullptr;
jmethodID WorkerWrapper::DETACH_WORKER_RUNTIME_METHOD_ID = nullptr;
jmethodID WorkerWrapper::MY_LOOPER_METHOD_ID = nullptr;
jmethodID WorkerWrapper::LOOPER_QUIT_METHOD_ID = nullptr;
jmethodID WorkerWrapper::SET_THREAD_PRIORITY_METHOD_ID = nullptr;

}  // namespace tns
