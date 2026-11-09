#include "MessageLoopTimer.h"
#include <android/looper.h>
#include <unistd.h>
#include <cerrno>
#include <thread>
#include <fcntl.h>
#include <android/log.h>
#include "NativeScriptAssert.h"
#include "Runtime.h"
#include "native_api_util.h"

using namespace tns;

static const int SLEEP_INTERVAL_MS = 100;

MessageLoopTimer::MessageLoopTimer()
    : m_isRunning(false), m_fd{-1, -1}, m_looper(nullptr) {}

MessageLoopTimer::~MessageLoopTimer() {
    Stop();
}

void MessageLoopTimer::Init(napi_env env) {
    this->RegisterStartStopFunctions(env);
}

void MessageLoopTimer::RegisterStartStopFunctions(napi_env env) {

    napi_status status;
    napi_value timer_start;
    napi_value timer_stop;

    const char * timer_start_name = "__messageLoopTimerStart";
    const char * timer_stop_name = "__messageLoopTimerStop";

    NAPI_GUARD(napi_create_function(env, timer_start_name, strlen(timer_start_name), MessageLoopTimer::StartCallback,
                         this, &timer_start)) {
        return;
    }
    NAPI_GUARD(napi_create_function(env, timer_stop_name, strlen(timer_stop_name), MessageLoopTimer::StopCallback,
                         this, &timer_stop)) {
        return;
    }

    napi_value global;
    NAPI_GUARD(napi_get_global(env, &global)) {
        return;
    }
    NAPI_GUARD(napi_set_named_property(env, global, timer_start_name, timer_start)) {
        return;
    }
    NAPI_GUARD(napi_set_named_property(env, global, timer_stop_name, timer_stop)) {}
}

napi_value MessageLoopTimer::StartCallback(napi_env env, napi_callback_info info) {

    napi_status status;
    void * data = nullptr;
    NAPI_GUARD(napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data)) {
        return nullptr;
    }

    auto self = static_cast<MessageLoopTimer *>(data);

    self->Start(env);

    return nullptr;
}

napi_value MessageLoopTimer::StopCallback(napi_env env, napi_callback_info info) {
    napi_status status;
    void * data = nullptr;
    NAPI_GUARD(napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data)) {
        return nullptr;
    }
    auto self = static_cast<MessageLoopTimer *>(data);


    self->Stop();

    return nullptr;
}

int MessageLoopTimer::PumpMessageLoopCallback(int fd, int events, void* data) {
    uint8_t msg;
    read(fd, &msg, sizeof(uint8_t));
    auto env = (napi_env) data;

    js_execute_pending_jobs(env);

    return 1;
}

void MessageLoopTimer::WorkerThreadRun(MessageLoopTimer* timer) {
    while (timer->m_isRunning.load(std::memory_order_acquire)) {
        uint8_t msg = 1;
        write(timer->m_fd[1], &msg, sizeof(uint8_t));
        std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_INTERVAL_MS));
    }
}

bool MessageLoopTimer::Start(napi_env env) {
    bool expected = false;
    if (!m_isRunning.compare_exchange_strong(expected, true)) {
        return true;
    }

    m_looper = ALooper_forThread();
    if (m_looper == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NAPI", "Unable to get looper for the current thread");
        m_isRunning.store(false, std::memory_order_release);
        return false;
    }
    ALooper_acquire(m_looper);

    if (pipe2(m_fd, O_NONBLOCK | O_CLOEXEC) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NAPI", "Unable to create a pipe: %s", strerror(errno));
        ALooper_release(m_looper);
        m_looper = nullptr;
        m_isRunning.store(false, std::memory_order_release);
        return false;
    }

    if (ALooper_addFd(m_looper, m_fd[0], ALOOPER_POLL_CALLBACK,
                      ALOOPER_EVENT_INPUT, MessageLoopTimer::PumpMessageLoopCallback,
                      env) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "NAPI", "Unable to register message-loop pipe");
        close(m_fd[0]);
        close(m_fd[1]);
        m_fd[0] = m_fd[1] = -1;
        ALooper_release(m_looper);
        m_looper = nullptr;
        m_isRunning.store(false, std::memory_order_release);
        return false;
    }

    m_worker = std::thread(MessageLoopTimer::WorkerThreadRun, this);
    return true;
}

void MessageLoopTimer::Stop() {
    m_isRunning.store(false, std::memory_order_release);
    if (m_worker.joinable()) {
        m_worker.join();
    }

    if (m_looper != nullptr) {
        if (m_fd[0] != -1) {
            ALooper_removeFd(m_looper, m_fd[0]);
        }
        ALooper_release(m_looper);
        m_looper = nullptr;
    }

    if (m_fd[0] != -1) {
        close(m_fd[0]);
        m_fd[0] = -1;
    }
    if (m_fd[1] != -1) {
        close(m_fd[1]);
        m_fd[1] = -1;
    }
}
