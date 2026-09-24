#include "jsi/jsc/JSCRuntime.h"

#ifdef TARGET_ENGINE_JSC

namespace nativescript {
namespace engine {

Object Runtime::global() {
  return Object::fromValueStorage(Value(*this, JSContextGetGlobalObject(context())).storage_);
}

Value Runtime::evaluateJavaScript(std::shared_ptr<StringBuffer> buffer,
                                  const std::string& sourceURL) {
  JSStringRef source = JSStringCreateWithUTF8CString(
      buffer != nullptr ? std::string(buffer->data(), buffer->size()).c_str() : "");
  JSStringRef url = jscengine::makeJSString(sourceURL);
  JSValueRef exception = nullptr;
  JSValueRef result = JSEvaluateScript(context(), source, nullptr, url, 1, &exception);
  JSStringRelease(source);
  JSStringRelease(url);
  if (exception != nullptr) {
    throw JSError(*this, jscengine::valueToUtf8(context(), exception));
  }
  return Value(*this, result);
}

}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_JSC
