#include "NativeApiQuickJSRuntime.h"

#ifdef TARGET_ENGINE_QUICKJS

namespace nativescript {
namespace engine {

String BigInt::toString(Runtime& runtime, int) const {
  JSValue value = local(runtime);
  JSValue stringValue = JS_ToString(runtime.context(), value);
  JS_FreeValue(runtime.context(), value);
  String result(runtime, stringValue);
  JS_FreeValue(runtime.context(), stringValue);
  return result;
}

Object Runtime::global() {
  JSValue global = JS_GetGlobalObject(context());
  Object result = Object::fromValueStorage(Value(*this, global).storage_);
  JS_FreeValue(context(), global);
  return result;
}

Value Runtime::evaluateJavaScript(std::shared_ptr<StringBuffer> buffer,
                                  const std::string& sourceURL) {
  JSValue result =
      JS_Eval(context(), buffer != nullptr ? buffer->data() : "",
              buffer != nullptr ? buffer->size() : 0, sourceURL.c_str(), JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result)) {
    throw JSError(*this, "QuickJS script evaluation failed.");
  }
  Value value(*this, result);
  JS_FreeValue(context(), result);
  return value;
}

}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_QUICKJS
