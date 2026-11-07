//
// Created by Ammar Ahmed on 21/09/2024.
//

#ifndef TESTAPPNAPI_PERFORMANCE_H
#define TESTAPPNAPI_PERFORMANCE_H
#include <chrono>
#include "EngineHost.h"

inline engine::Value Now(engine::Runtime& rt, const engine::Value&, const engine::Value*, size_t) {
    auto now = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    return engine::Value(static_cast<double>(ms));
}

namespace tns {

    class Performance {
    public:
        static void createPerformance(engine::Runtime& rt, engine::Object& global) {
            bool isInstalled = !global.getProperty(rt, "performance").isUndefined();
            if (!isInstalled) {
                engine::Object performance(rt);
                engine_util::SetFunction(rt, performance, "now", Now);
                global.setProperty(rt, "performance", performance);
            }

        }
    };

} // tns

#endif //TESTAPPNAPI_PERFORMANCE_H
