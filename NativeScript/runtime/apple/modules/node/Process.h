#ifndef NATIVE_NODE_PROCESS_MODULE_H
#define NATIVE_NODE_PROCESS_MODULE_H

#include "js_native_api_types.h"

namespace nativescript {

class Process {
 public:
  static void Init(napi_env env, napi_value global);
  static napi_value CreateModule(napi_env env);

 private:
  static napi_value EnsureGlobalProcess(napi_env env, napi_value global);
  static napi_value CreateProcessObject(napi_env env);

  static napi_value Cwd(napi_env env, napi_callback_info info);
  static napi_value Chdir(napi_env env, napi_callback_info info);
  static napi_value Uptime(napi_env env, napi_callback_info info);
  static napi_value Hrtime(napi_env env, napi_callback_info info);
  static napi_value HrtimeBigInt(napi_env env, napi_callback_info info);
  static napi_value MemoryUsage(napi_env env, napi_callback_info info);
  static napi_value MemoryUsageRss(napi_env env, napi_callback_info info);
  static napi_value Exit(napi_env env, napi_callback_info info);
  static napi_value StreamWrite(napi_env env, napi_callback_info info);

  static napi_value CreateWritableStream(napi_env env, int fd);
  static napi_value CreateReadableStream(napi_env env, int fd);
};

}  // namespace nativescript

#endif  // NATIVE_NODE_PROCESS_MODULE_H
