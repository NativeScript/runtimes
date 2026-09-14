#include "Console.h"

#include <sstream>
#include <string>
#include <vector>

#include "runtime/apple/NativeScriptException.h"
#include "js_native_api.h"
#include "native_api_util.h"
#include "runtime/apple/RuntimeConfig.h"
#include "runtime/apple/Util.h"
#ifdef TARGET_ENGINE_V8
#include "v8-api.h"
#endif

#ifdef __APPLE__

#include <CoreFoundation/CFString.h>
extern "C" void NSLog(CFStringRef format, ...);

#if defined(__has_include)
#if __has_include(<os/log.h>)
#include <os/log.h>
#define TNS_HAVE_OS_LOG 1
#else
#define TNS_HAVE_OS_LOG 0
#endif
#else
#define TNS_HAVE_OS_LOG 0
#endif

#else
#include <iostream>
#endif

namespace nativescript {

namespace {

bool readStringValue(napi_env env, napi_value value, std::string& result) {
  if (value == nullptr) {
    return false;
  }

  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    return false;
  }

  std::vector<char> buffer(length + 1);
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(),
                                 &copied) != napi_ok) {
    return false;
  }

  result.assign(buffer.data(), copied);
  return true;
}

bool throwPendingException(napi_env env, const std::string& message) {
  bool isPending = false;
  if (napi_is_exception_pending(env, &isPending) != napi_ok || !isPending) {
    return false;
  }

  napi_value exception = nullptr;
  napi_get_and_clear_last_exception(env, &exception);

  if (exception != nullptr &&
      napi_util::is_of_type(env, exception, napi_object)) {
    throw NativeScriptException(env, exception, message);
  }

  throw NativeScriptException(env, message);
}

bool coerceToString(napi_env env, napi_value value, std::string& result) {
  napi_value stringValue = nullptr;
  if (napi_coerce_to_string(env, value, &stringValue) != napi_ok ||
      stringValue == nullptr) {
    throwPendingException(env, "Error converting console argument to string");
    return false;
  }

  if (!readStringValue(env, stringValue, result)) {
    throwPendingException(env, "Error reading console argument string");
    return false;
  }

  return true;
}

}  // namespace

JS_CLASS_INIT(Console::Init) {
  napi_value Console, console;

  const napi_property_descriptor properties[] = {
      napi_util::desc("log", Log, (void*)kConsoleLogTypeLog),
      napi_util::desc("error", Log, (void*)kConsoleLogTypeError),
      napi_util::desc("warn", Log, (void*)kConsoleLogTypeWarn),
      napi_util::desc("info", Log, (void*)kConsoleLogTypeInfo),
      napi_util::desc("assert", Log, (void*)kConsoleLogTypeAssert),
      napi_util::desc("time", Time, nullptr),
      napi_util::desc("timeEnd", TimeEnd, nullptr),
      napi_util::desc("dir", Dir, nullptr),
      napi_util::desc("trace", Trace, nullptr),
  };

  napi_define_class(env, "Console", NAPI_AUTO_LENGTH, Console::Constructor,
                    nullptr, 9, properties, &Console);

  napi_new_instance(env, Console, 0, nullptr, &console);

  napi_set_named_property(env, global, "Console", Console);
  napi_set_named_property(env, global, "console", console);
}

JS_METHOD(Console::Constructor) {
  napi_value thisArg;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &thisArg, nullptr);
  return thisArg;
}

std::string transformJSObject(napi_env env, napi_value object) {
  napi_value toStringFunc = nullptr;
  bool hasToString = false;

  // Check if the object has a toString method
  if (napi_has_named_property(env, object, "toString", &hasToString) !=
      napi_ok) {
    throwPendingException(env, "Error reading console object toString");
    return "[object Object]";
  }

  if (hasToString) {
    if (napi_get_named_property(env, object, "toString", &toStringFunc) !=
        napi_ok) {
      throwPendingException(env, "Error reading console object toString");
      return "[object Object]";
    }

    if (toStringFunc != nullptr &&
        napi_util::is_of_type(env, toStringFunc, napi_function)) {
      napi_value result = nullptr;
      if (napi_call_function(env, object, toStringFunc, 0, nullptr, &result) !=
              napi_ok ||
          result == nullptr) {
        throwPendingException(env, "Error converting console object to string");
        return "[object Object]";
      }

      std::string value;
      if (!coerceToString(env, result, value)) {
        return "[object Object]";
      }

      auto hasCustomToStringImplementation =
          value.find("[object Object]") == std::string::npos;
      if (hasCustomToStringImplementation) return value;
    }
  }
  // If no custom toString method, stringify the object
  return JsonStringifyObject(env, object, false);
}

std::string buildStringFromArg(napi_env env, napi_value val,
                               napi_value inspectSymbol) {
  napi_valuetype type;
  if (napi_typeof(env, val, &type) != napi_ok) {
    throwPendingException(env, "Error reading console argument type");
    return "<unknown>";
  }

  if (type == napi_function) {
    std::string funcString;
    if (coerceToString(env, val, funcString)) {
      return funcString;
    }
    return "<function>";
  } else if (napi_util::is_array(env, val)) {
    napi_value cachedSelf = val;

    // Get array length
    uint32_t arrayLength = 0;
    if (napi_get_array_length(env, val, &arrayLength) != napi_ok) {
      throwPendingException(env, "Error reading console array length");
      return "[]";
    }

    std::stringstream arrayStr;
    arrayStr << "[";

    for (uint32_t i = 0; i < arrayLength; i++) {
      napi_value propertyValue = nullptr;
      if (napi_get_element(env, val, i, &propertyValue) != napi_ok ||
          propertyValue == nullptr) {
        throwPendingException(env, "Error reading console array element");
        arrayStr << "<unknown>";
        if (i != arrayLength - 1) {
          arrayStr << ", ";
        }
        continue;
      }

      // Check for circular reference
      bool isStrictEqual = false;
      if (napi_strict_equals(env, propertyValue, cachedSelf, &isStrictEqual) !=
          napi_ok) {
        throwPendingException(env, "Error comparing console array element");
      }

      if (isStrictEqual) {
        arrayStr << "[Circular]";
      } else {
        std::string elementString =
            buildStringFromArg(env, propertyValue, inspectSymbol);
        arrayStr << elementString;
      }

      if (i != arrayLength - 1) {
        arrayStr << ", ";
      }
    }

    arrayStr << "]";
    return arrayStr.str();
  } else if (type == napi_object) {
    napi_value inspectFunc = nullptr;
    napi_status getInspectStatus =
        napi_get_property(env, val, inspectSymbol, &inspectFunc);
    if (getInspectStatus == napi_ok &&
        inspectFunc != nullptr &&
        napi_util::is_of_type(env, inspectFunc, napi_function)) {
      napi_value inspectedValue = nullptr;
      if (napi_call_function(env, val, inspectFunc, 0, nullptr,
                             &inspectedValue) != napi_ok ||
          inspectedValue == nullptr) {
        throwPendingException(env, "Error inspecting console object");
        return "[object Object]";
      }
      return buildStringFromArg(env, inspectedValue, inspectSymbol);
    } else if (getInspectStatus != napi_ok) {
      throwPendingException(env, "Error reading console inspect function");
    }
    return transformJSObject(env, val);
  } else if (type == napi_symbol) {
    std::string symString;
    if (coerceToString(env, val, symString)) {
      return symString;
    }
    return "Symbol()";
  } else {
    std::string defaultToString;
    if (coerceToString(env, val, defaultToString)) {
      return defaultToString;
    }
    return "<unknown>";
  }
}

std::string buildLogString(napi_env env, napi_callback_info info,
                           int startingIndex = 0) {
  NAPI_CALLBACK_BEGIN_VARGS()

  napi_value global, Symbol, SymbolFor, symbolDescription, inspectSymbol;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Symbol", &Symbol);
  napi_get_named_property(env, Symbol, "for", &SymbolFor);
  napi_create_string_utf8(env, "nodejs.util.inspect.custom", NAPI_AUTO_LENGTH,
                          &symbolDescription);
  napi_call_function(env, global, SymbolFor, 1, &symbolDescription,
                     &inspectSymbol);

  std::stringstream ss;

  if (argc) {
    for (size_t i = startingIndex; i < argc; i++) {
      // separate args with a space
      if (i != 0) {
        ss << " ";
      }

      std::string argString = buildStringFromArg(env, argv[i], inspectSymbol);
      ss << argString;
    }
  } else {
    ss << std::endl;
  }

  return ss.str();
}

JS_METHOD(Console::Log) {
  try {
    if (!RuntimeConfig.LogToSystemConsole) {
      return UNDEFINED;
    }

    size_t argc = 0;
    ConsoleLogType stream;
    void* data = nullptr;

    napi_get_cb_info(env, cbinfo, &argc, nullptr, nullptr, &data);

    stream = ConsoleLogType((unsigned long)data);

    size_t initialArg = 0;

    if (stream == kConsoleLogTypeAssert) {
      bool passes = false;

      if (argc > 0) {
        napi_value firstArg = nullptr;
        napi_get_cb_info(env, cbinfo, &argc, &firstArg, nullptr, nullptr);
        napi_coerce_to_bool(env, firstArg, &firstArg);
        napi_get_value_bool(env, firstArg, &passes);
      }

      if (!passes) {
        initialArg = 1;
      } else {
        return UNDEFINED;
      }
    }

    napi_value argv[argc];
    napi_get_cb_info(env, cbinfo, &argc, argv, nullptr, nullptr);

    std::stringstream log;

    // TODO(dj): what if we made this pretty?

    log << "CONSOLE";
    switch (stream) {
      case kConsoleLogTypeLog:
        log << " LOG";
        break;
      case kConsoleLogTypeError:
        log << " ERROR";
        break;
      case kConsoleLogTypeWarn:
        log << " WARN";
        break;
      case kConsoleLogTypeInfo:
        log << " INFO";
        break;
      case kConsoleLogTypeAssert:
        log << " ASSERT FAILED";
        break;
    }
    log << ": ";

    log << buildLogString(env, cbinfo, initialArg);

    log << "\n";

    std::string logString = log.str();

#ifdef __APPLE__
#if TARGET_OS_OSX
    NSLog(CFSTR("%s"), logString.c_str());
#elif TNS_HAVE_OS_LOG
    os_log(OS_LOG_DEFAULT, "%{public}s", logString.c_str());
#else
    NSLog(CFSTR("%s"), logString.c_str());
#endif
#else
    switch (stream) {
      case kConsoleLogTypeLog:
      case kConsoleLogTypeInfo:
        std::cout << logString;
        break;
      case kConsoleLogTypeError:
      case kConsoleLogTypeWarn:
      case kConsoleLogTypeAssert:
        std::cerr << logString;
        break;
    }
#endif

    if (RuntimeConfig.CustomLogCallback) {
      RuntimeConfig.CustomLogCallback(logString.c_str());
    }

#ifdef TARGET_ENGINE_V8
    v8_inspector::ConsoleAPIType method;
    switch (stream) {
      case kConsoleLogTypeLog:
        method = v8_inspector::ConsoleAPIType::kLog;
        break;
      case kConsoleLogTypeError:
        method = v8_inspector::ConsoleAPIType::kError;
        break;
      case kConsoleLogTypeWarn:
        method = v8_inspector::ConsoleAPIType::kWarning;
        break;
      case kConsoleLogTypeInfo:
        method = v8_inspector::ConsoleAPIType::kInfo;
        break;
      case kConsoleLogTypeAssert:
        method = v8_inspector::ConsoleAPIType::kAssert;
        break;
      default:
        break;
    }

    sendToDevToolsFrontEnd(env, method, logString);
#endif
  } catch (NativeScriptException& e) {
    e.ReThrowToJS(env);
  }

  return UNDEFINED;
}

JS_METHOD(Console::Time) {
  napi_value thisArg;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &thisArg, nullptr);
  return thisArg;
}

JS_METHOD(Console::TimeEnd) {
  napi_value thisArg;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &thisArg, nullptr);
  return thisArg;
}

JS_METHOD(Console::Dir) {
  napi_value thisArg;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &thisArg, nullptr);
  return thisArg;
}

JS_METHOD(Console::Trace) {
  napi_value thisArg;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &thisArg, nullptr);
  return thisArg;
}

#ifdef TARGET_ENGINE_V8
int Console::sendToDevToolsFrontEnd(napi_env env,
                                    v8_inspector::ConsoleAPIType method,
                                    napi_callback_info info) {
  if (!m_callback) {
    return 0;
  }
  NAPI_CALLBACK_BEGIN_VARGS()

  v8::Local<v8::Value>* v8_args = reinterpret_cast<v8::Local<v8::Value>*>(
      const_cast<napi_value*>(argv.data()));

  std::vector<v8::Local<v8::Value>> arg_vector;
  unsigned nargs = argc;
  arg_vector.reserve(nargs);
  for (unsigned ix = 0; ix < nargs; ix++) arg_vector.push_back(v8_args[ix]);

  m_callback(env, method, arg_vector);
  return 0;
}

void Console::sendToDevToolsFrontEnd(napi_env env,
                                     v8_inspector::ConsoleAPIType method,
                                     const std::string& message) {
  if (!m_callback) {
    return;
  }

  v8::Local<v8::Value> v8_message =
      v8::String::NewFromUtf8(env->isolate, message.c_str(),
                              v8::NewStringType::kNormal, message.length())
          .ToLocalChecked();

  std::vector<v8::Local<v8::Value>> args{
      v8_message,
  };
  m_callback(env, method, args);
}
#endif

ConsoleCallback Console::m_callback = nullptr;

}  // namespace nativescript
