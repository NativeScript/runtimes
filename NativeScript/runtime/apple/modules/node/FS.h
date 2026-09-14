#ifndef NATIVE_NODE_FS_MODULE_H
#define NATIVE_NODE_FS_MODULE_H

#include "js_native_api_types.h"

namespace nativescript {

class FS {
 public:
  static napi_value CreateModule(napi_env env);

 private:
  static napi_value ReadFileSync(napi_env env, napi_callback_info info);
  static napi_value WriteFileSync(napi_env env, napi_callback_info info);
  static napi_value ExistsSync(napi_env env, napi_callback_info info);
  static napi_value MkdirSync(napi_env env, napi_callback_info info);
  static napi_value ReaddirSync(napi_env env, napi_callback_info info);
  static napi_value StatSync(napi_env env, napi_callback_info info);
  static napi_value LstatSync(napi_env env, napi_callback_info info);
  static napi_value UnlinkSync(napi_env env, napi_callback_info info);
  static napi_value RmSync(napi_env env, napi_callback_info info);
};

}  // namespace nativescript

#endif  // NATIVE_NODE_FS_MODULE_H
