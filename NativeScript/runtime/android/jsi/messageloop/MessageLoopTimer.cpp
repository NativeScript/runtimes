#include "MessageLoopTimer.h"
#include <android/looper.h>
#include <unistd.h>
#include <cerrno>
#include <thread>
#include <android/log.h>
#include "NativeScriptAssert.h"
#include "Runtime.h"

using namespace tns;

static const int SLEEP_INTERVAL_MS = 100;

void MessageLoopTimer::Init(std::shared_ptr<EngineHost> host) {
    m_host = std::move(host);
    this->RegisterStartStopFunctions();
}

void MessageLoopTimer::RegisterStartStopFunctions() {
    engine::Runtime &rt = m_host->GetRuntime();
    engine::Object global = rt.global();

    const char *timer_start_name = "__messageLoopTimerStart";
    const char *timer_stop_name = "__messageLoopTimerStop";

    // The napi version routes `this` through the callback's data pointer; a
    // capture is the engine:: equivalent, so there are no static trampolines.
    engine_util::SetFunction(rt, global, timer_start_name,
                             [this](engine::Runtime &, const engine::Value &,
                                    const engine::Value *, size_t) -> engine::Value {
                                 if (m_isRunning) {
                                     return engine::Value::undefined();
                                 }

                                 m_isRunning = true;

                                 auto looper = ALooper_forThread();
                                 if (looper == nullptr) {
                                     __android_log_print(ANDROID_LOG_ERROR, "JSI",
                                                         "Unable to get looper for the current thread");
                                     return engine::Value::undefined();
                                 }

                                 int pipeStatus = pipe(m_fd);
                                 if (pipeStatus != 0) {
                                     __android_log_print(ANDROID_LOG_ERROR, "JSI",
                                                         "Unable to create a pipe: %s",
                                                         strerror(errno));
                                     return engine::Value::undefined();
                                 }

                                 ALooper_addFd(looper, m_fd[0], 0, ALOOPER_EVENT_INPUT,
                                               MessageLoopTimer::PumpMessageLoopCallback, this);

                                 std::thread worker(MessageLoopTimer::WorkerThreadRun, this);

                                 worker.detach();

                                 return engine::Value::undefined();
                             });

    engine_util::SetFunction(rt, global, timer_stop_name,
                             [this](engine::Runtime &, const engine::Value &,
                                    const engine::Value *, size_t) -> engine::Value {
                                 if (!m_isRunning) {
                                     return engine::Value::undefined();
                                 }

                                 m_isRunning = false;

                                 return engine::Value::undefined();
                             });
}

int MessageLoopTimer::PumpMessageLoopCallback(int fd, int events, void *data) {
    uint8_t msg;
    read(fd, &msg, sizeof(uint8_t));
    auto self = static_cast<MessageLoopTimer *>(data);

    // Draining runs JS, so it has to hold the engine the way any other entry
    // from the host does; the napi version got the lock from NapiScope higher up.
    JSScope scope(self->m_host);
    self->m_host->ExecutePendingJobs();

    return 1;
}

void MessageLoopTimer::WorkerThreadRun(MessageLoopTimer *timer) {
    while (timer->m_isRunning) {
        uint8_t msg = 1;
        write(timer->m_fd[1], &msg, sizeof(uint8_t));
        std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_INTERVAL_MS));
    }

    uint8_t msg = 0;
    write(timer->m_fd[1], &msg, sizeof(uint8_t));
}
