#ifndef WORKERWRAPPER_H_
#define WORKERWRAPPER_H_

#include <jni.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "ConcurrentQueue.h"
#include "WorkerMessage.h"
#include "EngineHost.h"

namespace tns {

class LooperTasks;
class Runtime;

/*
 * Owns a worker's native thread and its lifecycle, mirroring the iOS
 * runtime's WorkerWrapper. The thread is a std::thread that attaches to the
 * JVM, prepares a Java Looper (so worker/plugin code can keep using Android
 * Handlers) and then drives that single looper, which pumps both Java
 * messages and the worker's C++ inbox (ConcurrentQueue + eventfd).
 *
 * It uses:
 *  - engine::Runtime instead of v8::Isolate*
 *  - JSON string payloads (worker::Message) instead of V8 ValueSerializer
 *  - a cooperative Terminate() (quit the looper) instead of
 *    v8::TerminateExecution (which has no engine:: equivalent)
 *
 * Messaging:
 *  - parent -> worker: queue_ + eventfd wakeup on the worker looper
 *  - worker -> parent: the parent runtime's LooperTasks queue
 *
 * The parent may be the main thread or another worker (nested workers); a
 * worker's children are terminated when the worker itself shuts down.
 */
class WorkerWrapper : public std::enable_shared_from_this<WorkerWrapper> {
public:
    WorkerWrapper(engine::Runtime& parentRt, int workerId, std::string workerPath,
                  std::string callingDir, int priority, const engine::Value& workerObject);

    int WorkerId() const { return workerId_; }
    bool IsTerminating() const { return isTerminating_; }
    bool IsClosing() const { return isClosing_; }
    bool IsDisposed() const { return isDisposed_; }

    /*
     * Spawns the (detached) worker thread. The thread holds a shared_ptr to
     * this wrapper, keeping it alive until the thread fully shuts down.
     */
    void Start();

    /*
     * parent -> worker. Queues a JSON message and wakes the worker looper.
     * Messages posted before the worker finishes bootstrapping are drained
     * right after the worker script runs.
     */
    void PostMessage(std::shared_ptr<worker::Message> message);

    /*
     * worker -> parent. Posts a task that fires the Worker object's
     * `onmessage` on the parent runtime's thread.
     */
    void PostMessageToParent(std::shared_ptr<worker::Message> message);

    /*
     * Called on the parent's thread. Cooperatively interrupts the worker by
     * quitting its Java looper (no v8::TerminateExecution equivalent exists
     * here, so a runaway busy-loop in the worker is NOT preempted; the loop
     * stops once the current JS callback unwinds).
     */
    void Terminate();

    /*
     * Called on the worker thread (self.close()). Lets the current callback
     * unwind, then the looper quits and the thread shuts down gracefully.
     */
    void Close();

    /*
     * Posts the worker object's `onerror` invocation to the parent's thread.
     * Strings only - must not hold any engine handles from the worker runtime.
     */
    void PassUncaughtExceptionFromWorkerToParent(const std::string& message,
                                                 const std::string& filename,
                                                 const std::string& stackTrace,
                                                 int lineno);

    /*
     * Registry of live workers, keyed by workerId. Replaces the old
     * CallbackHandlers::id2WorkerMap. Guarded by a mutex because the worker
     * shutdown path posts cleanup from the worker thread.
     */
    static int NextWorkerId();
    static std::shared_ptr<WorkerWrapper> GetById(int workerId);
    static void Insert(int workerId, std::shared_ptr<WorkerWrapper> wrapper);

    /*
     * Parent thread only: drops the Worker object handle and removes the
     * wrapper from the registry. Idempotent.
     */
    static void ClearWorkerOnParent(int workerId);

    /*
     * Terminates and clears all workers whose parent is the given runtime.
     * Must run on the parent's thread, before the parent runtime is disposed
     * (the children's Worker object handles live in that runtime).
     * Cascades: each child terminates its own children during shutdown.
     */
    static void TerminateChildren(engine::Runtime& parentRt);

    /*
     * Resolves the wrapper of a worker runtime via the runtime registry.
     * Returns nullptr on the main runtime.
     */
    static WorkerWrapper* FromRuntime(engine::Runtime& rt);

    /*
     * Resolves the jclass/jmethodID handles used by the worker thread
     * bootstrap, before the first worker starts. The first call is always on
     * the main thread (nested workers require a main-thread worker first),
     * where class loading is safe.
     */
    static void EnsureJniCached();

private:
    void BackgroundLooper(std::shared_ptr<WorkerWrapper> self);
    void DrainPendingTasks();
    void QuitLooper();
    static int DrainCallback(int fd, int events, void* data);
    static void FireMessageOnParentWorkerObject(int workerId,
                                                std::shared_ptr<worker::Message> message);
    static void FireErrorOnParentWorkerObject(int workerId, const std::string& message,
                                              const std::string& stackTrace,
                                              const std::string& filename, int lineno,
                                              const std::string& threadName);

    // The parent runtime's own engine::Runtime, taken from its EngineHost --
    // NOT the engine::Runtime the constructor was handed. That one is the
    // stack temporary the engine builds for a host call (see
    // engine::Runtime::identity()) and would dangle on the first use here.
    engine::Runtime* parentRt_;
    // Held rather than looked up: every parent-side callback below has to enter
    // the parent engine, and the napi version's raw napi_env gave it no way to
    // know the parent was still there.
    std::shared_ptr<EngineHost> parentHost_;
    // The parent runtime's task queue; weak so a child outliving its parent
    // just drops its posts instead of touching a dead runtime.
    std::weak_ptr<LooperTasks> parentTasks_;
    // Readiness flag and registry key. Written on the worker thread only.
    std::atomic<engine::Runtime*> workerRt_;
    // Worker thread only, alongside workerRt_.
    std::shared_ptr<EngineHost> workerHost_;
    Runtime* runtime_;

    const int workerId_;
    const std::string workerPath_;
    const std::string callingDir_;
    const std::string threadName_;
    const int priority_;

    // Owned handle to the JS Worker object in the *parent* runtime. Cleared on
    // the parent's thread inside a scope; see ClearWorkerOnParent.
    engine::Value poWorker_;

    std::atomic_bool isClosing_;
    std::atomic_bool isTerminating_;
    std::atomic_bool isDisposed_;

    ConcurrentQueue queue_;

    std::mutex looperMutex_;
    jobject javaLooperRef_;

    static std::mutex registryMutex_;
    static std::map<int, std::shared_ptr<WorkerWrapper>> registry_;
    // Keyed by engine::Runtime::identity(); &rt is not stable across callbacks,
    // and every lookup here comes from one (postMessage/close/onerror).
    static std::map<const void*, WorkerWrapper*> rtRegistry_;
    static std::atomic_int nextWorkerId_;

    static jclass RUNTIME_CLASS;
    static jclass LOOPER_CLASS;
    static jclass PROCESS_CLASS;
    static jmethodID INIT_WORKER_RUNTIME_METHOD_ID;
    static jmethodID RUN_WORKER_LOOP_METHOD_ID;
    static jmethodID DETACH_WORKER_RUNTIME_METHOD_ID;
    static jmethodID MY_LOOPER_METHOD_ID;
    static jmethodID LOOPER_QUIT_METHOD_ID;
    static jmethodID SET_THREAD_PRIORITY_METHOD_ID;
};

}  // namespace tns

#endif /* WORKERWRAPPER_H_ */
