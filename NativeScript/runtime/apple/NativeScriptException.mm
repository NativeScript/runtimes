#include "runtime/apple/NativeScriptException.h"
#import <Foundation/Foundation.h>
#include <sstream>
#include "js_native_api.h"
#include "js_native_api_types.h"
#ifdef ENABLE_JS_RUNTIME
#include "jsr.h"
#endif
#include "native_api_util.h"
#include "runtime/apple/Runtime.h"

namespace nativescript {

NativeScriptException::NativeScriptException(const std::string& message) {
  this->message_ = message;
  this->name_ = "NativeScriptException";
}

NativeScriptException::NativeScriptException(const std::string& message,
                                             const std::string& stackTrace) {
  this->message_ = message;
  this->stackTrace_ = stackTrace;
  this->name_ = "NativeScriptException";
}

NativeScriptException::NativeScriptException(napi_env env, const std::string& message,
                                             const std::string& name) {
  this->message_ = message;
  this->stackTrace_ = "";
  this->fullMessage_ = message;
  this->name_ = name;
}

NativeScriptException::NativeScriptException(napi_env env, napi_value error,
                                             const std::string& message, const std::string& name) {
  this->message_ = GetErrorMessage(env, error, message);
  this->stackTrace_ = GetErrorStackTrace(env, error);
  this->fullMessage_ = GetFullMessage(env, error, this->message_);
  this->name_ = name;
  napi_set_named_property(env, error, "name", napi_util::to_js_string(env, name));
  NSLog(@"NativeScriptException::NativeScriptException: %s", this->fullMessage_.c_str());
}

std::string NativeScriptException::Description() const {
  if (this->fullMessage_.empty()) {
    return this->message_;
  } else {
    return this->fullMessage_;
  }
}

void NativeScriptException::OnUncaughtError(napi_env env, napi_value error) {
#ifdef ENABLE_JS_RUNTIME
  NapiScope scope(env);
#endif

  std::stringstream fullMessageStream;

  if (napi_util::has_property(env, error, "fullMessage")) {
    fullMessageStream << napi_util::get_cxx_string(
        env, napi_util::get_property(env, error, "fullMessage"));
  } else {
#ifdef TARGET_ENGINE_V8
    fullMessageStream << GetErrorStackTrace(env, error);
#else
    fullMessageStream << GetErrorMessage(env, error);
    fullMessageStream << "\n at \n" << GetErrorStackTrace(env, error);
#endif
  }

  // TODO: discardUncaughtJsExceptions

  napi_value global;
  napi_get_global(env, &global);

  napi_value handler;
  napi_get_named_property(env, global, "__onUncaughtError", &handler);

  if (napi_util::is_of_type(env, handler, napi_function)) {
    napi_value args[] = {error};
    napi_value result;
    napi_status status = napi_call_function(env, global, handler, 1, args, &result);

    if (status != napi_ok) {
      napi_get_and_clear_last_exception(env, &result);

      if (napi_util::is_of_type(env, result, napi_object)) {
        fullMessageStream << "\n\nError handler threw an error too:\n";
#ifdef TARGET_ENGINE_V8
        fullMessageStream << GetErrorStackTrace(env, result);
#else
        fullMessageStream << GetErrorMessage(env, result);
        fullMessageStream << "\n at \n" << GetErrorStackTrace(env, result);
#endif
      } else {
        fullMessageStream
            << "\n\nError handler threw an error too, but we couldn't get the error object";
      }
    }
  }

  std::string fullMessage = fullMessageStream.str();

  NSLog(@"NativeScriptException::OnUncaughtError: %s", fullMessage.c_str());

  NSException* objcException = [NSException exceptionWithName:@"NativeScriptException"
                                                       reason:@(fullMessage.c_str())
                                                     userInfo:@{@"sender" : @"onUncaughtError"}];

  // dispatch_async(dispatch_get_main_queue(), ^(void) {
  @throw objcException;
  // });
}

void NativeScriptException::ReThrowToJS(napi_env env, napi_value* errorOut) {
  napi_value errObj;

  if (!fullMessage_.empty()) {
    napi_create_error(env, napi_util::to_js_string(env, name_),
                      napi_util::to_js_string(env, fullMessage_), &errObj);
  } else if (!message_.empty()) {
    napi_create_error(env, napi_util::to_js_string(env, name_),
                      napi_util::to_js_string(env, message_), &errObj);
  } else {
    napi_create_error(env, napi_util::to_js_string(env, name_),
                      napi_util::to_js_string(env, "No javascript exception or message provided."),
                      &errObj);
  }

  if (errorOut != nullptr) {
    *errorOut = errObj;
  } else {
    napi_throw(env, errObj);
  }
}

void NativeScriptException::PrintErrorMessage(const std::string& errorMessage) {
  std::stringstream ss(errorMessage);
  std::string line;
  while (std::getline(ss, line, '\n')) {
    NSLog(@"%s", line.c_str());
  }
}

std::string NativeScriptException::GetErrorMessage(napi_env env, napi_value error,
                                                   const std::string& prependMessage) {
  bool isError;
  napi_is_error(env, error, &isError);

  if (!isError) {
    napi_value err;
    if (napi_coerce_to_string(env, error, &err) != napi_ok || err == nullptr) {
      return prependMessage;
    }
    return napi_util::get_string_value(env, err);
  }

  napi_value message = nullptr;
  std::string mes;
  if (napi_get_named_property(env, error, "message", &message) == napi_ok &&
      message != nullptr && napi_util::is_of_type(env, message, napi_string)) {
    mes = napi_util::get_string_value(env, message);
  }

  std::stringstream ss;

  if (!prependMessage.empty()) {
    ss << prependMessage << std::endl;
  }

  std::string errMessage;
  bool hasFullErrorMessage = false;
  napi_value fullMessage = nullptr;
  if (napi_get_named_property(env, error, "fullMessage", &fullMessage) == napi_ok &&
      fullMessage != nullptr && napi_util::is_of_type(env, fullMessage, napi_string)) {
    hasFullErrorMessage = true;
    errMessage = napi_util::get_string_value(env, fullMessage);
    ss << errMessage;
  }

  if (!mes.empty()) {
    if (hasFullErrorMessage) {
      ss << std::endl;
    }
    ss << mes;
  }

  return ss.str();
}

std::string NativeScriptException::GetErrorStackTrace(napi_env env, napi_value error) {
  std::stringstream ss;

  bool isError;
  napi_is_error(env, error, &isError);
  if (!isError) return "";

  napi_value stack = nullptr;
  if (napi_get_named_property(env, error, "stack", &stack) != napi_ok || stack == nullptr ||
      !napi_util::is_of_type(env, stack, napi_string)) {
    return "";
  }

  std::string stackStr = napi_util::get_string_value(env, stack);
  ss << stackStr;

  return ss.str();
}

std::string NativeScriptException::GetFullMessage(napi_env env, napi_value error,
                                                  const std::string& jsExceptionMessage) {
  bool isError;
  napi_is_error(env, error, &isError);
  if (!isError) {
    return jsExceptionMessage;
  }

  std::string stackTraceMessage = GetErrorStackTrace(env, error);

  std::stringstream ss;

#ifdef TARGET_ENGINE_V8
  ss << stackTraceMessage;
#else
  ss << jsExceptionMessage;

  ss << std::endl << "StackTrace: " << std::endl << stackTraceMessage;
#endif

  std::string loggedMessage = ss.str();

  // PrintErrorMessage(loggedMessage);

  return loggedMessage;
}

}  // namespace nativescript
