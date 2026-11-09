#ifndef NATIVE_NODE_PATH_MODULE_H
#define NATIVE_NODE_PATH_MODULE_H

#include "js_native_api_types.h"

namespace nativescript {

class Path {
 public:
  static napi_value CreateModule(napi_env env);

 private:
  static napi_value Basename(napi_env env, napi_callback_info info);
  static napi_value Dirname(napi_env env, napi_callback_info info);
  static napi_value Extname(napi_env env, napi_callback_info info);
  static napi_value IsAbsolute(napi_env env, napi_callback_info info);
  static napi_value Join(napi_env env, napi_callback_info info);
  static napi_value Normalize(napi_env env, napi_callback_info info);
  static napi_value Parse(napi_env env, napi_callback_info info);
  static napi_value Format(napi_env env, napi_callback_info info);
  static napi_value Relative(napi_env env, napi_callback_info info);
  static napi_value Resolve(napi_env env, napi_callback_info info);
  static napi_value ToNamespacedPath(napi_env env, napi_callback_info info);
};

}  // namespace nativescript

#endif  // NATIVE_NODE_PATH_MODULE_H
