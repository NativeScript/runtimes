#include "Timers.h"
#include "ArgConverter.h"
#include "Runtime.h"
#include "NativeScriptException.h"
#include "JEnv.h"
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include "Util.h"
#include "NativeScriptAssert.h"

/**
 * Timers ride the runtime thread's Java MessageQueue via a per-runtime
 * com.tns.TimerHandler bound to the isolate's Looper:
 *  - each scheduled timer enqueues one anonymous "due token" message via
 *    sendMessageAtTime, so timers share a single queue with Handler.post/
 *    postDelayed and fire in exact MessageQueue order;
 *  - a native list (sortedTimers_) sorted by exact (sub-millisecond) due time
 *    picks the earliest-due timer per token, preserving the relative ordering of
 *    JS timers despite the millisecond-quantized Java queue.
 *
 * Everything below runs on the runtime thread (Init, the setTimeout/clear
 * callbacks, and FireTimer via TimerHandler.handleMessage), so no locking is
 * needed — sortedTimers_/timerMap_ are only touched there.
 */

// Takes a value and transform into a positive number
// returns a negative number if the number is negative or invalid
//
// The napi version leaned on napi_coerce_to_number; there is no coercion
// primitive in the engine layer, so the two cases that reach here (a number, or
// a numeric string) are converted directly.
inline static double ToMaybePositiveValue(engine::Runtime &rt, const engine::Value &v) {
    if (v.isUndefined() || v.isNull()) {
        return -1;
    }
    if (v.isNumber()) {
        double value = v.getNumber();
        return isnan(value) ? -1 : value;
    }
    if (v.isBool()) {
        return v.getBool() ? 1 : 0;
    }
    if (v.isString()) {
        std::string text = v.asString(rt).utf8(rt);
        char *end = nullptr;
        double value = strtod(text.c_str(), &end);
        if (end == text.c_str() || isnan(value)) {
            return -1;
        }
        return value;
    }
    return -1;
}

static double now_ms() {
    struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return 1000.0 * res.tv_sec + (double) res.tv_nsec / 1e6;
}

using namespace tns;

jclass Timers::TIMER_HANDLER_CLASS = nullptr;
jmethodID Timers::TIMER_HANDLER_CTOR = nullptr;
jmethodID Timers::TIMER_HANDLER_POST = nullptr;
jmethodID Timers::TIMER_HANDLER_RELEASE = nullptr;

namespace {
    std::mutex s_timersMutex;
    // Keyed by engine::Runtime::identity(); &rt is not stable across callbacks.
    std::map<const void *, Timers *> s_timers;
}

void Timers::Init(engine::Runtime &rt, engine::Object &global) {
    rt_ = &rt;
    host_ = Runtime::GetRuntime(rt)->GetEngineHost();
    // TODO: remove the __ns__ prefix once this is validated
    engine_util::SetFunction(rt, global, "__ns__setTimeout",
                             [this](engine::Runtime &runtime, const engine::Value &thisVal,
                                    const engine::Value *args, size_t count) -> engine::Value {
                                 return SetTimer(runtime, thisVal, args, count, false);
                             });
    engine_util::SetFunction(rt, global, "__ns__setInterval",
                             [this](engine::Runtime &runtime, const engine::Value &thisVal,
                                    const engine::Value *args, size_t count) -> engine::Value {
                                 return SetTimer(runtime, thisVal, args, count, true);
                             });
    auto clearTimer = [this](engine::Runtime &runtime, const engine::Value &,
                             const engine::Value *args, size_t count) -> engine::Value {
        int id = -1;
        if (count > 0) {
            id = (int) ToMaybePositiveValue(runtime, args[0]);
        }
        // ids start at 1
        if (id > 0) {
            removeTask(id);
        }
        return engine::Value::undefined();
    };
    engine_util::SetFunction(rt, global, "__ns__clearTimeout", clearTimer);
    engine_util::SetFunction(rt, global, "__ns__clearInterval", clearTimer);

    {
        std::lock_guard<std::mutex> lock(s_timersMutex);
        s_timers[rt.identity()] = this;
    }

    JEnv jEnv;
    if (TIMER_HANDLER_CLASS == nullptr) {
        TIMER_HANDLER_CLASS = jEnv.FindClass("com/tns/TimerHandler");
        assert(TIMER_HANDLER_CLASS != nullptr);
        TIMER_HANDLER_CTOR = jEnv.GetMethodID(TIMER_HANDLER_CLASS, "<init>", "(J)V");
        TIMER_HANDLER_POST = jEnv.GetMethodID(TIMER_HANDLER_CLASS, "post", "(J)V");
        TIMER_HANDLER_RELEASE = jEnv.GetMethodID(TIMER_HANDLER_CLASS, "release", "()V");
    }

    // Bind a TimerHandler to the current (runtime) thread's Looper.
    jobject localHandler = jEnv.NewObject(TIMER_HANDLER_CLASS, TIMER_HANDLER_CTOR,
                                          reinterpret_cast<jlong>(this));
    handler_ = jEnv.NewGlobalRef(localHandler);
    stopped_ = false;
}

void Timers::postTimer(const std::shared_ptr<TimerTask> &task, double now) {
    // Due-now timers post at (long)now so they tie (FIFO) with a same-ms
    // postDelayed(0); future timers post at ceil(dueTime) so they never fire early.
    jlong when = task->dueTime_ <= now ? (jlong) now : (jlong) std::ceil(task->dueTime_);
    JEnv jEnv;
    jEnv.CallVoidMethod(handler_, TIMER_HANDLER_POST, when);
}

void Timers::addTask(const std::shared_ptr<TimerTask>& task) {
    if (task->queued_) {
        return;
    }
    auto now = now_ms();
    task->nestingLevel_ = nesting + 1;
    task->queued_ = true;
    // theoretically this should be >5 on the spec, but we're following chromium behavior here again
    if (task->nestingLevel_ >= 5 && task->frequency_ < 4) {
        task->frequency_ = 4;
        task->startTime_ = now;
    }
    timerMap_.emplace(task->id_, task);
    task->dueTime_ = task->NextTime(now);

    auto it = std::upper_bound(sortedTimers_.begin(), sortedTimers_.end(), task->dueTime_,
                               [](const double &value, const TimerReference &ref) {
                                   return ref.dueTime > value;
                               });
    sortedTimers_.insert(it, TimerReference{task->id_, task->dueTime_});

    postTimer(task, now);
}

void Timers::removeTask(const std::shared_ptr<TimerTask> &task) {
    removeTask(task->id_);
}

void Timers::removeTask(const int &taskId) {
    auto it = timerMap_.find(taskId);
    if (it == timerMap_.end()) {
        return;
    }
    auto task = it->second;
    if (task->queued_) {
        // Remove the pending due-token reference (matched by id at its dueTime).
        auto lo = std::lower_bound(sortedTimers_.begin(), sortedTimers_.end(), task->dueTime_,
                                   [](const TimerReference &ref, const double &value) {
                                       return ref.dueTime < value;
                                   });
        for (auto ref = lo; ref != sortedTimers_.end() && ref->dueTime == task->dueTime_; ++ref) {
            if (ref->id == taskId) {
                sortedTimers_.erase(ref);
                break;
            }
        }
        // A token already enqueued in the Java queue is left to no-op in FireTimer.
    }
    task->Unschedule();
    timerMap_.erase(it);
}

void Timers::FireTimer() {
    if (stopped_ || rt_ == nullptr) {
        return;
    }

    JSScope scope(host_);
    engine::Runtime &rt = *rt_;

    if (sortedTimers_.empty()) {
        return; // leftover token
    }
    auto ref = sortedTimers_.front();
    if (ref.dueTime > now_ms()) {
        return; // front not due yet → leftover token
    }
    sortedTimers_.erase(sortedTimers_.begin());

    auto it = timerMap_.find(ref.id);
    if (it == timerMap_.end()) {
        return;
    }
    auto task = it->second;
    task->queued_ = false;
    nesting = task->nestingLevel_;

    if (task->repeats_) {
        // Follow chromium's non-drifting interval scheduling: anchor the next
        // fire to the ideal dueTime rather than "now".
        task->startTime_ = task->dueTime_;
        addTask(task);
    }

    engine::Function cb = task->callback_.asObject(rt).asFunction(rt);
    engine::Object recv = task->thisArg.isObject() ? task->thisArg.asObject(rt) : rt.global();
    size_t argc = task->args_ == nullptr ? 0 : task->args_->size();

    // The callback throwing must not skip the cleanup below: the napi version
    // left the exception pending on the env and cleared it after removing the
    // task, and every subsequent napi call would have failed until it did.
    std::unique_ptr<engine::JSError> thrown;
    try {
        if (argc > 0) {
            cb.callWithThis(rt, recv, task->args_->data(), argc);
        } else {
            cb.callWithThis(rt, recv);
        }
    } catch (engine::JSError &error) {
        thrown = std::make_unique<engine::JSError>(error);
    }

    // task is not queued, so it's either a setTimeout or a cleared setInterval:
    // remove it (which releases its JS references via Unschedule).
    if (!task->queued_) {
        removeTask(task);
    }

    nesting = 0;

    if (thrown != nullptr) {
        throw NativeScriptException(rt, *thrown, "Error in timer callback");
    }
}

void Timers::Destroy() {
    if (stopped_) {
        return;
    }
    stopped_ = true;

    if (handler_ != nullptr) {
        JEnv jEnv;
        jEnv.CallVoidMethod(handler_, TIMER_HANDLER_RELEASE);
        jEnv.DeleteGlobalRef(handler_);
        handler_ = nullptr;
    }

    // Release any references still held by pending tasks.
    for (auto &entry: timerMap_) {
        entry.second->Unschedule();
    }
    timerMap_.clear();
    sortedTimers_.clear();
    rt_ = nullptr;
    host_.reset();
}

Timers::~Timers() {
    Destroy();
}

void Timers::onDisposeRuntime(engine::Runtime &rt) {
    Timers *timers = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_timersMutex);
        auto it = s_timers.find(rt.identity());
        if (it == s_timers.end()) {
            return;
        }
        timers = it->second;
        s_timers.erase(it);
    }
    delete timers;
}

engine::Value Timers::SetTimer(engine::Runtime &rt, const engine::Value &thisVal,
                               const engine::Value *args, size_t count, bool repeatable) {
    int id = ++currentTimerId;
    if (count >= 1) {
        if (!args[0].isObject() || !args[0].asObjectBorrowed(rt).isFunction(rt)) {
            return engine::Value::undefined();
        }

        long timeout = 0;
        if (count >= 2) {
            timeout = (long) ToMaybePositiveValue(rt, args[1]);
            if (timeout < 0) {
                timeout = 0;
            }
        }

        std::shared_ptr<std::vector<engine::Value>> argArray;
        if (count >= 3) {
            auto otherArgLength = count - 2;
            argArray = std::make_shared<std::vector<engine::Value>>();
            argArray->reserve(otherArgLength);
            for (size_t i = 0; i < otherArgLength; i++) {
                argArray->emplace_back(rt, args[i + 2]);
            }
        }

        auto task = std::make_shared<TimerTask>(engine::Value(rt, args[0]), timeout,
                                                repeatable, argArray,
                                                engine::Value(rt, thisVal), id, now_ms());
        addTask(task);
    }
    return engine::Value(id);
}

void Timers::InitStatic(engine::Runtime &rt, engine::Object &global) {
    auto timers = new Timers();
    timers->Init(rt, global);
}

// Reverse native for com.tns.TimerHandler.nativeFireTimer (bound by symbol name).
extern "C" JNIEXPORT void JNICALL
Java_com_tns_TimerHandler_nativeFireTimer(JNIEnv* env, jclass clazz, jlong timersPtr) {
    try {
        reinterpret_cast<tns::Timers *>(timersPtr)->FireTimer();
    } catch (NativeScriptException& e) {
        e.ReThrowToJava(nullptr);
    } catch (std::exception& e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJava(nullptr);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJava(nullptr);
    }
}
