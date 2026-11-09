#ifndef TEST_APP_FINALIZER_QUEUE_H
#define TEST_APP_FINALIZER_QUEUE_H

#include <jni.h>
#include <mutex>
#include <vector>
#include "Engine.h"

namespace tns {
    /**
     * Engine-agnostic deferral for finalizer cleanup that must touch the JS heap
     * (e.g. releasing an owned engine handle), which is illegal from inside a GC
     * finalizer on every engine (V8's InvokeFinalizerFromGC; a reentrant
     * JS_FreeValue during a QuickJS sweep corrupts the collector; etc.).
     *
     * A finalizer calls FinalizerQueue::Post — which only allocates and appends,
     * never touching the JS heap, so it is safe to run mid-GC on any thread. The
     * queued callbacks are drained on the runtime thread's Java message loop (see
     * com.tns.FinalizerHandler), a point guaranteed to be outside any GC sweep
     * with JS unwound to the host.
     *
     * Owned by Runtime; there is one per runtime. Post is thread-safe; Drain
     * and Destroy run on the runtime thread.
     */
    class FinalizerQueue {
    public:
        using Finalize = void (*)(JsRuntime &rt, void *data, void *hint);

        // Binds a com.tns.FinalizerHandler to the CURRENT thread's Looper, so this
        // MUST be constructed on the runtime thread.
        explicit FinalizerQueue(JsRuntime *rt);

        ~FinalizerQueue();

        // Schedules cb(rt, data, hint) to run at the next message-loop tick.
        // Thread-safe and safe to call from inside a GC finalizer (no JS-heap
        // interaction). A no-op cb is ignored.
        void Post(Finalize cb, void *data, void *hint);

        // Runs all currently-queued callbacks. Invoked from FinalizerHandler on the
        // runtime thread; the caller opens a JS scope first.
        void Drain();

        // Releases the Java handler and runs any still-queued callbacks inline.
        // Must run on the runtime thread while the runtime is still valid.
        // Idempotent.
        void Destroy();

        JsRuntime *Rt() const { return rt_; }

    private:
        struct Entry {
            Finalize cb;
            void *data;
            void *hint;
        };

        JsRuntime *rt_;
        std::mutex mutex_;
        std::vector<Entry> queue_;
        bool scheduled_ = false;
        bool stopped_ = false;
        jobject handler_ = nullptr; // global ref to com.tns.FinalizerHandler

        // Cached (process-wide) FinalizerHandler JNI ids.
        static jclass HANDLER_CLASS;
        static jmethodID HANDLER_CTOR;
        static jmethodID HANDLER_SCHEDULE;
        static jmethodID HANDLER_RELEASE;
    };

    // Convenience wrapper: defers cb(rt, data, hint) to the owning runtime's
    // post-GC finalizer drain. Safe to call from inside a GC finalizer. Falls back
    // to running inline if the runtime is unavailable/tearing down. Lets callers
    // (e.g. modules) defer without pulling in the heavy Runtime.h.
    void PostFinalizer(JsRuntime &rt, FinalizerQueue::Finalize cb, void *data, void *hint);
}

#endif //TEST_APP_FINALIZER_QUEUE_H
