#ifndef TEST_APP_TIMERS_H
#define TEST_APP_TIMERS_H

#include <jni.h>
#include <memory>
#include <vector>
#include "EngineHost.h"
#include "robin_hood.h"

namespace tns {
    /**
     * A Timer Task
     * this class is used to store the persistent values and context
     * once Unschedule is called everything is released
     */
    class TimerTask {
    public:
        inline TimerTask(engine::Value callback, double frequency,
                         bool repeats,
                         const std::shared_ptr<std::vector<engine::Value>> &args,
                         engine::Value _thisArg,
                         int id, double startTime)
                : callback_(std::move(callback)), thisArg(std::move(_thisArg)),
                  frequency_(frequency), repeats_(repeats), args_(args), id_(id),
                  startTime_(startTime) {

        }

        inline double NextTime(double targetTime) {
            if (frequency_ <= 0) {
                return targetTime;
            }
            auto timeDiff = targetTime - startTime_;
            auto div = std::div((long) timeDiff, (long) frequency_);
            return startTime_ + frequency_ * (div.quot + 1);
        }

        // Releases the JS values held by this task. Dropping an owned handle
        // touches the engine, so it is called from the runtime thread
        // (FireTimer / removeTask) exactly as the napi_ref version was.
        inline void Unschedule() {
            callback_ = engine::Value::undefined();
            thisArg = engine::Value::undefined();
            args_.reset();
            queued_ = false;
        }

        int nestingLevel_ = 0;
        engine::Value callback_;
        std::shared_ptr<std::vector<engine::Value>> args_;
        engine::Value thisArg;
        bool repeats_ = false;
        /**
         * this helper parameter is used in the following way:
         * task scheduled means queued_ = true
         * this is set to false right before the callback is executed
         */
        bool queued_ = false;
        double frequency_ = 0;
        double dueTime_ = -1;
        double startTime_ = -1;
        int id_;
    };

    struct TimerReference {
        int id;
        double dueTime;
    };

    class Timers {
    public:
        /**
         * Initializes the global functions setTimeout, setInterval, clearTimeout and clearInterval
         * and binds a Java TimerHandler to the executing thread's Looper.
         * @param rt target runtime
         * @param global global object
         */
        void Init(engine::Runtime &rt, engine::Object &global);

        static void InitStatic(engine::Runtime &rt, engine::Object &global);

        /**
         * Fires the earliest-due timer. Invoked from Java (TimerHandler.handleMessage)
         * on the runtime thread, once per posted "due token".
         */
        void FireTimer();

        /**
         * Disposes the timers, releasing all references and the Java handler.
         * MUST be called on the thread Init was called on. Idempotent.
         */
        void Destroy();

        // The napi version hangs the Timers instance off a finalizer on the
        // global object, which fires when the env is freed. There is no
        // equivalent for an arbitrary engine object, so the instance is kept in
        // a per-runtime registry and destroyed from Runtime::DestroyRuntime,
        // like every other onDisposeRuntime in this tree.
        static void onDisposeRuntime(engine::Runtime &rt);

        ~Timers();

    private:
        void addTask(const std::shared_ptr<TimerTask>& task);

        void removeTask(const std::shared_ptr<TimerTask> &task);

        void removeTask(const int &taskId);

        // Enqueues one "due token" on the Java MessageQueue for this task. Due-now
        // timers post at (long)now so they tie (FIFO) with a same-ms postDelayed(0);
        // future timers post at ceil(dueTime) so they never fire early.
        void postTimer(const std::shared_ptr<TimerTask> &task, double now);

        engine::Value SetTimer(engine::Runtime &rt, const engine::Value &thisVal,
                               const engine::Value *args, size_t count, bool repeatable);

        engine::Runtime *rt_ = nullptr;
        std::shared_ptr<EngineHost> host_;
        int currentTimerId = 0;
        int nesting = 0;
        // stores the map of timer tasks
        robin_hood::unordered_map<int, std::shared_ptr<TimerTask>> timerMap_;
        // sorted by exact (sub-millisecond) dueTime; touched only on the runtime thread
        std::vector<TimerReference> sortedTimers_;
        // global ref to the com.tns.TimerHandler bound to this thread's Looper
        jobject handler_ = nullptr;
        bool stopped_ = false;

        // Cached (process-wide) TimerHandler JNI ids.
        static jclass TIMER_HANDLER_CLASS;
        static jmethodID TIMER_HANDLER_CTOR;
        static jmethodID TIMER_HANDLER_POST;
        static jmethodID TIMER_HANDLER_RELEASE;
    };

}

#endif //TEST_APP_TIMERS_H
