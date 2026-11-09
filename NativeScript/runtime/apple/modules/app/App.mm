#ifdef __APPLE__

#include "App.h"
#include "js_native_api.h"

#ifdef TARGET_PLATFORM_MACOS

#import <AppKit/AppKit.h>

#endif

App* App::Init(napi_env env) {
  App* appInst = new App();

  napi_value global, App, app;

  napi_get_global(env, &global);

  napi_create_object(env, &App);
  napi_set_named_property(env, global, "App", App);

  napi_create_function(env, "run", NAPI_AUTO_LENGTH, Run, appInst, &app);
  napi_set_named_property(env, App, "run", app);

  return appInst;
}

napi_value App::Run(napi_env env, napi_callback_info cbinfo) {
  App* appInst = nullptr;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, nullptr, (void**)&appInst);

#ifdef TARGET_PLATFORM_MACOS

  NSApplication* app = [NSApplication sharedApplication];

  [app finishLaunching];

  NSEvent* event;

  while (true) {
    event = [app nextEventMatchingMask:NSEventMaskAny
                             untilDate:[NSDate distantFuture]
                                inMode:NSDefaultRunLoopMode
                               dequeue:YES];
    if (event) {
      [app sendEvent:event];
      // appInst->runtime->drainMicrotasks();
    }
  }

#endif

  return nullptr;
}

#endif  // __APPLE__
