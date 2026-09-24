#ifndef MESSAGELOOPTIMER_H
#define MESSAGELOOPTIMER_H

#include <memory>
#include "EngineHost.h"

namespace tns {

class MessageLoopTimer {
public:
    // Takes the host rather than the runtime: the looper callback below has to
    // enter the engine to drain it, and it runs long after Init has returned.
    // The napi version handed the looper a raw napi_env, which the timer has no
    // way to keep alive; holding the host keeps the drain valid for as long as
    // the timer exists, and the timer is deleted in ~Runtime.
    void Init(std::shared_ptr<EngineHost> host);
private:
    bool m_isRunning;
    int m_fd[2];
    std::shared_ptr<EngineHost> m_host;

    void RegisterStartStopFunctions();
    static int PumpMessageLoopCallback(int fd, int events, void* data);
    static void WorkerThreadRun(MessageLoopTimer* timer);
};

}

#endif //MESSAGELOOPTIMER_H
