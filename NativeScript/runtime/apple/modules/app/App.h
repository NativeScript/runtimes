#ifndef APP_H
#define APP_H

#include "js_native_api_types.h"

class App {
 public:
  static App* Init(napi_env env);
  static napi_value Run(napi_env env, napi_callback_info cbinfo);
};

#endif  // APP_H
