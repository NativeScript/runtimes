#include "FinalizerQueue.h"
#include "JEnv.h"
#include "NativeScriptException.h"
#include "NativeScriptAssert.h"
#include "Runtime.h"
#include <sstream>

using namespace tns;

jclass FinalizerQueue::HANDLER_CLASS = nullptr;
jmethodID FinalizerQueue::HANDLER_CTOR = nullptr;
jmethodID FinalizerQueue::HANDLER_SCHEDULE = nullptr;
jmethodID FinalizerQueue::HANDLER_RELEASE = nullptr;

FinalizerQueue::FinalizerQueue(JsRuntime *rt) : rt_(rt) {
    JEnv jEnv;
    if (HANDLER_CLASS == nullptr) {
        HANDLER_CLASS = jEnv.FindClass("com/tns/FinalizerHandler");
        assert(HANDLER_CLASS != nullptr);
        HANDLER_CTOR = jEnv.GetMethodID(HANDLER_CLASS, "<init>", "(J)V");
        HANDLER_SCHEDULE = jEnv.GetMethodID(HANDLER_CLASS, "schedule", "()V");
        HANDLER_RELEASE = jEnv.GetMethodID(HANDLER_CLASS, "release", "()V");
    }

    // Bind a FinalizerHandler to the current (runtime) thread's Looper.
    jobject localHandler = jEnv.NewObject(HANDLER_CLASS, HANDLER_CTOR,
                                          reinterpret_cast<jlong>(this));
    handler_ = jEnv.NewGlobalRef(localHandler);
}

FinalizerQueue::~FinalizerQueue() {
    Destroy();
}

void FinalizerQueue::Post(Finalize cb, void *data, void *hint) {
    if (cb == nullptr) {
        return;
    }

    bool runInline = false;
    jobject handler = nullptr; // captured under the lock so Destroy can't free it mid-use
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            // Teardown: the loop is no longer draining us. Fall back to running the
            // cleanup inline (matches the pre-deferral behavior for this edge).
            runInline = true;
        } else {
            queue_.push_back({cb, data, hint});
            // Only wake the loop on the empty -> non-empty transition; further posts
            // ride the already-scheduled drain.
            if (!scheduled_) {
                scheduled_ = true;
                handler = handler_;
            }
        }
    }

    if (runInline) {
        cb(*rt_, data, hint);
        return;
    }

    if (handler != nullptr) {
        JEnv jEnv;
        jEnv.CallVoidMethod(handler, HANDLER_SCHEDULE);
    }
}

void FinalizerQueue::Drain() {
    // Take the whole batch under the lock, then run callbacks outside it: a
    // callback may free objects whose GC finalizers Post() again, and that must
    // not deadlock on the queue mutex. Re-posted work sets scheduled_ = true and
    // wakes the loop for the next tick.
    std::vector<Entry> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduled_ = false;
        batch.swap(queue_);
    }

    // No per-callback handle scope here. An engine::Value owns its handle, so a
    // value a callback materialises is rooted by the Value itself rather than by
    // an enclosing scope; the engine entry (JSScope, opened in
    // nativeDrainFinalizers) supplies the isolate/context the engine calls need.
    for (auto &entry: batch) {
        if (entry.cb != nullptr) {
            entry.cb(*rt_, entry.data, entry.hint);
        }
    }
}

void FinalizerQueue::Destroy() {
    // Mark stopped and detach the handler under the lock, so a concurrent Post
    // (possible from a background JS thread's GC) either observes stopped_ and
    // runs inline, or has already captured the handler before we release it.
    std::vector<Entry> batch;
    jobject handler = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        handler = handler_;
        handler_ = nullptr;
        batch.swap(queue_);
    }

    if (handler != nullptr) {
        JEnv jEnv;
        jEnv.CallVoidMethod(handler, HANDLER_RELEASE);
        jEnv.DeleteGlobalRef(handler);
    }

    // Run whatever was still queued while the runtime is valid; finalizers that
    // fire during the subsequent teardown then run inline via PostFinalizer.
    for (auto &entry: batch) {
        if (entry.cb != nullptr) {
            entry.cb(*rt_, entry.data, entry.hint);
        }
    }
}

void tns::PostFinalizer(JsRuntime &rt, FinalizerQueue::Finalize cb, void *data, void *hint) {
    Runtime::PostFinalizer(rt, cb, data, hint);
}

// Reverse-native for com.tns.FinalizerHandler.nativeDrainFinalizers (bound by
// symbol name). Runs on the runtime thread at a safe, post-GC message-loop tick.
extern "C" JNIEXPORT void JNICALL
Java_com_tns_FinalizerHandler_nativeDrainFinalizers(JNIEnv *jniEnv, jclass clazz, jlong queuePtr) {
    auto *queue = reinterpret_cast<tns::FinalizerQueue *>(queuePtr);
    if (queue == nullptr) {
        return;
    }
    try {
        // Enter the JS scope (lock + isolate/context) before running any callback,
        // since they call into the engine.
        JSScope scope(*queue->Rt());
        queue->Drain();
    } catch (NativeScriptException &e) {
        e.ReThrowToJava(nullptr);
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJava(nullptr);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJava(nullptr);
    }
}
