//
// Created by Ammar Ahmed on 01/03/2025.
//

#ifndef TEST_APP_ANDROID_RUNTIME_MODULES_H
#define TEST_APP_ANDROID_RUNTIME_MODULES_H

#include "js_native_api.h"
#include "url/URL.h"
#include "url/URLSearchParams.h"

namespace tns {
    class AndroidRuntimeModules {
    public:
        static void Init(napi_env env, napi_value global) {
            nativescript::URL::Init(env, global);
            nativescript::URLSearchParams::Init(env, global);
        }
    };
}

#endif //TEST_APP_ANDROID_RUNTIME_MODULES_H
