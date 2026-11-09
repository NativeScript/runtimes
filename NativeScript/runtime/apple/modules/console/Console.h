#ifndef CONSOLE_H
#define CONSOLE_H

#include "native_api_util.h"

#ifdef TARGET_ENGINE_V8
#include <src/inspector/v8-console-message.h>
#endif

#ifdef TARGET_ENGINE_V8
#include <src/inspector/v8-console-message.h>
#endif

#ifdef TARGET_ENGINE_V8
typedef void (*ConsoleCallback)(napi_env env,
                                v8_inspector::ConsoleAPIType method,
                                const std::vector<v8::Local<v8::Value>>& args);
#else
typedef void (*ConsoleCallback)(napi_env env, napi_callback_info info);
#endif

namespace nativescript {

enum ConsoleLogType {
  kConsoleLogTypeLog,
  kConsoleLogTypeError,
  kConsoleLogTypeWarn,
  kConsoleLogTypeInfo,
  kConsoleLogTypeAssert,
};

class Console {
 public:
  static JS_CLASS_INIT(Init);

  static JS_METHOD(Constructor);

  static JS_METHOD(Log);
  static JS_METHOD(Time);
  static JS_METHOD(TimeEnd);
  static JS_METHOD(Dir);
  static JS_METHOD(Trace);

#ifdef TARGET_ENGINE_V8
  static int sendToDevToolsFrontEnd(napi_env env,
                                    v8_inspector::ConsoleAPIType method,
                                    napi_callback_info info);
  static void sendToDevToolsFrontEnd(napi_env env,
                                     v8_inspector::ConsoleAPIType method,
                                     const std::string& args);
#endif

  static ConsoleCallback m_callback;
};

}  // namespace nativescript

#endif  // CONSOLE_H
