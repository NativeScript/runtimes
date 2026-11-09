//
// Created by pkanev on 12/8/2017.
//

#ifndef CONSOLE_H
#define CONSOLE_H

#include "EngineHost.h"
#include <map>
#include <string>
#include <vector>
#include <ArgConverter.h>
#include <android/log.h>

namespace tns {
    class Console {
    public:
        // The napi version also takes a ConsoleCallback that forwards every
        // console.* call to the V8 inspector's DevTools frontend. There is no
        // inspector on this binding layer, so there is nothing to forward to and
        // the parameter is gone rather than accepted and ignored.
        static void createConsole(engine::Runtime& rt, int maxLogcatObjectSize, bool forceLog);

        static void onDisposeRuntime(engine::Runtime& rt);

        // Public because the console.* implementations are free functions here
        // rather than static members: each one is a lambda/free function of
        // HostFunctionType shape, not a napi_callback taking `this` as data.
        static void sendToADBLogcat(const std::string& log, android_LogPriority logPriority);

        // Keyed by engine::Runtime::identity(); &rt is not stable across callbacks.
        static std::map<const void*, std::map<std::string, double>> s_rtToConsoleTimersMap;

    private:

        static int m_maxLogcatObjectSize;
        static const char* LOG_TAG;
    };

}

#endif //CONSOLE_H
