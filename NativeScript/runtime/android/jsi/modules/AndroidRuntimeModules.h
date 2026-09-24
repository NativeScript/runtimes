//
// Created by Ammar Ahmed on 01/03/2025.
//

#ifndef TEST_APP_ANDROID_RUNTIME_MODULES_H
#define TEST_APP_ANDROID_RUNTIME_MODULES_H

#include "EngineHost.h"

namespace tns {
    class AndroidRuntimeModules {
    public:
        static void Init(engine::Runtime& rt, engine::Object& global) {
            // The napi version installs URL, URLSearchParams and URLPattern here.
            // Those live in runtime/modules/url and are shared verbatim with the
            // Apple build, which means they are Node-API programs
            // (nativescript::URL::Init takes a napi_env). There is no Node-API on
            // this path and those files must not be forked, so this binding layer
            // has no URL globals until they gain an engine:: front end.
            (void) rt;
            (void) global;
        }
    };
}

#endif //TEST_APP_ANDROID_RUNTIME_MODULES_H
