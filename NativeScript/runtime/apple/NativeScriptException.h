#ifndef NATIVESCRIPT_RUNTIME_NATIVE_SCRIPT_EXCEPTION_H
#define NATIVESCRIPT_RUNTIME_NATIVE_SCRIPT_EXCEPTION_H

#include <string>

#include "js_native_api.h"
#include "js_native_api_types.h"

namespace nativescript {

class NativeScriptException {
 public:
  NativeScriptException(const std::string& message);
  NativeScriptException(const std::string& message,
                        const std::string& stackTrace);
  NativeScriptException(napi_env env, napi_value error,
                        const std::string& message,
                        const std::string& name = "NativeScriptException");
  NativeScriptException(napi_env env, const std::string& message,
                        const std::string& name = "NativeScriptException");

  void ReThrowToJS(napi_env env, napi_value* errorOut = nullptr);

  static void OnUncaughtError(napi_env env, napi_value error);

  inline std::string Name() const {
    if (name_.empty()) {
      return "NativeScriptException";
    }
    return name_;
  }

  std::string Description() const;

 private:
  std::string name_;
  std::string message_;
  std::string stackTrace_;
  std::string fullMessage_;

  static std::string GetErrorStackTrace(napi_env env, napi_value stackTrace);
  static std::string GetErrorMessage(napi_env env, napi_value error,
                                     const std::string& prependMessage = "");
  std::string GetFullMessage(napi_env env, napi_value error,
                             const std::string& jsExceptionMessage);

  static void PrintErrorMessage(const std::string& errorMessage);
};

}  // namespace nativescript

#endif  // NATIVESCRIPT_RUNTIME_NATIVE_SCRIPT_EXCEPTION_H
