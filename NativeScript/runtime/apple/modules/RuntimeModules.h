#ifndef RUNTIMEMODULES_H
#define RUNTIMEMODULES_H

#include "console/Console.h"
#include "js_native_api_types.h"
#include "node/Node.h"
#include "performance/Performance.h"
#include "runtime/apple/RuntimeConfig.h"
#include "runtime/apple/modules/module/ModuleInternal.h"
#include "runtime/apple/modules/worker/Worker.h"
#include "runtime/modules/url/URL.h"
#include "runtime/modules/url/URLSearchParams.h"
#include "web/Web.h"
#ifdef __APPLE__
#include "app/App.h"
#include "timers/Timers.h"
#endif  // __APPLE__

namespace nativescript {

class RuntimeModules {
 public:
  inline RuntimeModules() {}

  inline void Init(napi_env env, napi_value global) {
    module.Init(env, RuntimeConfig.BaseDir);

    URL::Init(env, global);
    URLSearchParams::Init(env, global);

    Console::Init(env, global);
    Performance::Init(env, global);
    Node::Init(env, global);

#ifdef __APPLE__
    App::Init(env);
    Timers::Init(env, global);
    Web::Init(env, global);
#endif  // __APPLE__

    Worker::Init(env, global);
  }

  inline void DeInit() { module.DeInit(); }

  ModuleInternal module;
};

}  // namespace nativescript

#endif  // RUNTIMEMODULES_H
