#include "jsi/jsc/JSCRuntime.h"

#ifdef TARGET_ENGINE_JSC

namespace nativescript {
namespace engine {

namespace jscengine {

JSError toJSError(Runtime& runtime, JSValueRef exception) {
  JSGlobalContextRef context = runtime.context();
  std::string message = valueToUtf8(context, exception);
  if (exception == nullptr || context == nullptr) {
    return JSError(runtime, message);
  }

  std::string stack;
  if (JSValueIsObject(context, exception)) {
    JSObjectRef object = JSValueToObject(context, exception, nullptr);
    if (object != nullptr) {
      // A thrown Error stringifies as "Name: message"; the shim splits on that
      // prefix when it has to rebuild one, so keep what() in that shape and
      // carry the object itself for everything else.
      JSStringRef stackName = makeJSString("stack");
      JSValueRef stackValue = JSObjectGetProperty(context, object, stackName, nullptr);
      JSStringRelease(stackName);
      if (stackValue != nullptr && JSValueIsString(context, stackValue)) {
        stack = valueToUtf8(context, stackValue);
      }
    }
  }

  return JSError(runtime, message, Value(runtime, exception), std::move(stack));
}

JSObjectRef weakRefConstructor(Runtime& runtime) {
  auto state = runtime.state();
  if (state->weakRefConstructor != nullptr) {
    return state->weakRefConstructor;
  }
  JSGlobalContextRef context = state->context;
  JSStringRef name = makeJSString("WeakRef");
  JSValueRef value =
      JSObjectGetProperty(context, JSContextGetGlobalObject(context), name, nullptr);
  JSStringRelease(name);
  if (value == nullptr || !JSValueIsObject(context, value)) {
    return nullptr;
  }
  JSObjectRef constructor = JSValueToObject(context, value, nullptr);
  if (constructor == nullptr || !JSObjectIsConstructor(context, constructor)) {
    return nullptr;
  }
  JSValueProtect(context, constructor);
  state->weakRefConstructor = constructor;
  return constructor;
}

JSObjectRef weakRefDeref(Runtime& runtime) {
  auto state = runtime.state();
  if (state->weakRefDeref != nullptr) {
    return state->weakRefDeref;
  }
  JSObjectRef constructor = weakRefConstructor(runtime);
  if (constructor == nullptr) {
    return nullptr;
  }
  JSGlobalContextRef context = state->context;
  JSStringRef prototypeName = makeJSString("prototype");
  JSValueRef prototype = JSObjectGetProperty(context, constructor, prototypeName, nullptr);
  JSStringRelease(prototypeName);
  if (prototype == nullptr || !JSValueIsObject(context, prototype)) {
    return nullptr;
  }
  JSObjectRef prototypeObject = JSValueToObject(context, prototype, nullptr);
  JSStringRef derefName = makeJSString("deref");
  JSValueRef deref = JSObjectGetProperty(context, prototypeObject, derefName, nullptr);
  JSStringRelease(derefName);
  if (deref == nullptr || !JSValueIsObject(context, deref)) {
    return nullptr;
  }
  JSObjectRef function = JSValueToObject(context, deref, nullptr);
  if (function == nullptr || !JSObjectIsFunction(context, function)) {
    return nullptr;
  }
  JSValueProtect(context, function);
  state->weakRefDeref = function;
  return function;
}

}  // namespace jscengine

WeakObject::WeakObject(Runtime& runtime, const Value& value) {
  if (!value.isObject()) {
    // Primitives cannot be weakly held, and the shim only ever weakens objects.
    // Staying empty makes lock() report undefined, i.e. "already gone".
    return;
  }
  JSObjectRef constructor = jscengine::weakRefConstructor(runtime);
  if (constructor == nullptr) {
    return;
  }
  JSGlobalContextRef context = runtime.context();
  JSValueRef target = value.local(runtime);
  JSValueRef exception = nullptr;
  JSObjectRef reference =
      JSObjectCallAsConstructor(context, constructor, 1, &target, &exception);
  if (reference == nullptr || exception != nullptr) {
    return;
  }
  storage_ = std::make_shared<jscengine::ValueStorage>(jscengine::ValueStorage::Kind::JSC);
  storage_->context = context;
  storage_->value = reference;
  JSValueProtect(context, reference);
}

Value WeakObject::lock(Runtime& runtime) const {
  if (storage_ == nullptr || storage_->value == nullptr) {
    return Value::undefined();
  }
  JSObjectRef deref = jscengine::weakRefDeref(runtime);
  if (deref == nullptr) {
    return Value::undefined();
  }
  JSGlobalContextRef context = runtime.context();
  JSValueRef exception = nullptr;
  JSValueRef result = JSObjectCallAsFunction(
      context, deref, reinterpret_cast<JSObjectRef>(const_cast<OpaqueJSValue*>(storage_->value)),
      0, nullptr, &exception);
  if (exception != nullptr || result == nullptr || JSValueIsUndefined(context, result)) {
    return Value::undefined();
  }
  return Value(runtime, result);
}

Value HostObject::get(Runtime&, const PropNameID&) { return Value::undefined(); }
bool HostObject::set(Runtime&, const PropNameID&, const Value&) { return true; }
std::vector<PropNameID> HostObject::getPropertyNames(Runtime&) { return {}; }

// The defaults reproduce what the engine used to do for an index: stringify it
// and take the named path. A host object that does not override these is
// therefore unaffected by the indexed routing.
Value HostObject::getValueAtIndex(Runtime& runtime, uint32_t index) {
  return get(runtime, PropNameID(std::to_string(index)));
}

bool HostObject::setValueAtIndex(Runtime& runtime, uint32_t index, const Value& value) {
  return set(runtime, PropNameID(std::to_string(index)), value);
}

String::String(Runtime& runtime, JSStringRef string)
    : storage_(std::make_shared<jscengine::ValueStorage>(jscengine::ValueStorage::Kind::JSC)) {
  storage_->context = runtime.context();
  storage_->value = JSValueMakeString(runtime.context(), string);
  JSValueProtect(runtime.context(), storage_->value);
}

std::string String::utf8(Runtime& runtime) const {
  return jscengine::valueToUtf8(runtime.context(), storage_->value);
}

String::operator Value() const { return Value::fromStorage(storage_); }

Value::Value(Runtime&, const String& value) {
  storage_ = value.storage_;
  kind_ = storage_ ? storage_->kind : jscengine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const Object& object) {
  storage_ = object.storage_;
  kind_ = storage_ ? storage_->kind : jscengine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const Function& function) {
  storage_ = function.storage_;
  kind_ = storage_ ? storage_->kind : jscengine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const Array& array) {
  storage_ = array.storage_;
  kind_ = storage_ ? storage_->kind : jscengine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const ArrayBuffer& arrayBuffer) {
  storage_ = arrayBuffer.storage_;
  kind_ = storage_ ? storage_->kind : jscengine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const BigInt& bigint) {
  storage_ = bigint.storage_;
  kind_ = storage_ ? storage_->kind : jscengine::ValueStorage::Kind::Undefined;
}

Object Value::asObject(Runtime& runtime) const {
  if (storage_) {
    return Object::fromValueStorage(storage_);
  }
  // Promote borrowed to owned storage for Object.
  auto s = std::make_shared<jscengine::ValueStorage>(jscengine::ValueStorage::Kind::JSC);
  s->context = runtime.context();
  s->value = borrowedValue_ != nullptr ? borrowedValue_ : JSValueMakeUndefined(runtime.context());
  JSValueProtect(runtime.context(), s->value);
  return Object::fromValueStorage(std::move(s));
}

Object Value::asObjectBorrowed(Runtime& runtime) const {
  return asObject(runtime);
}

String Value::asString(Runtime& runtime) const {
  JSValueRef exception = nullptr;
  JSStringRef string = JSValueToStringCopy(runtime.context(), local(runtime), &exception);
  if (string == nullptr || exception != nullptr) {
    throw jscengine::toJSError(runtime, exception);
  }
  String result(runtime, string);
  JSStringRelease(string);
  return result;
}

std::string Value::utf8(Runtime& runtime) const { return asString(runtime).utf8(runtime); }

Value Value::createStringFromUtf8(Runtime& runtime, const char* data, size_t length) {
  return Value(runtime, String::createFromUtf8(runtime,
                                               reinterpret_cast<const uint8_t*>(data), length));
}

BigInt Value::getBigInt(Runtime& runtime) const { return BigInt(runtime, local(runtime)); }

Function Object::getPropertyAsFunction(Runtime& runtime, const char* name) const {
  return getProperty(runtime, name).asObject(runtime).asFunction(runtime);
}
Function Object::asFunction(Runtime&) const { return Function(*this); }
Array Object::getArray(Runtime&) const { return Array(*this); }
ArrayBuffer Object::getArrayBuffer(Runtime&) const { return ArrayBuffer(*this); }

Array Object::getPropertyNames(Runtime& runtime) const {
  JSPropertyNameArrayRef propertyNames =
      JSObjectCopyPropertyNames(runtime.context(), local(runtime));
  size_t count = JSPropertyNameArrayGetCount(propertyNames);
  Array result(runtime, count);
  for (size_t i = 0; i < count; i++) {
    JSStringRef name = JSPropertyNameArrayGetNameAtIndex(propertyNames, i);
    result.setValueAtIndex(runtime, i, String(runtime, name));
  }
  JSPropertyNameArrayRelease(propertyNames);
  return result;
}

void Object::setProperty(Runtime& runtime, const char* name, const Function& value) {
  setProperty(runtime, name, Value(runtime, value));
}
void Object::setProperty(Runtime& runtime, const char* name, const Array& value) {
  setProperty(runtime, name, Value(runtime, value));
}
void Object::setProperty(Runtime& runtime, const char* name, const ArrayBuffer& value) {
  setProperty(runtime, name, Value(runtime, value));
}

}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_JSC
