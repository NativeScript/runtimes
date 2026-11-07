#ifndef MESSAGELOOPTIMER_H
#define MESSAGELOOPTIMER_H

#include <android/looper.h>
#include <atomic>
#include <thread>

#include "js_native_api.h"

namespace tns {

class MessageLoopTimer {
public:
    MessageLoopTimer();
    ~MessageLoopTimer();

    void Init(napi_env env);
private:
    std::atomic_bool m_isRunning;
    int m_fd[2];
    ALooper* m_looper;
    std::thread m_worker;

    void RegisterStartStopFunctions(napi_env env);
    bool Start(napi_env env);
    void Stop();
    static napi_value StartCallback(napi_env, napi_callback_info info);
    static napi_value StopCallback(napi_env env, napi_callback_info info);
    static int PumpMessageLoopCallback(int fd, int events, void* data);
    static void WorkerThreadRun(MessageLoopTimer* timer);
};

}

#endif //MESSAGELOOPTIMER_H
