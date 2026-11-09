#ifndef NATIVE_WEB_MODULE_H
#define NATIVE_WEB_MODULE_H

#include <string>

#include "js_native_api_types.h"

namespace nativescript {

class Web {
 public:
  static void Init(napi_env env, napi_value global);
  static napi_value LoadInternalModule(napi_env env,
                                       const std::string& moduleName);
};

}  // namespace nativescript

#endif  // NATIVE_WEB_MODULE_H
