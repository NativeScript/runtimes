#include "NativeApiJSCRuntime.h"

#ifdef TARGET_ENGINE_JSC

namespace nativescript {
namespace engine {

Value HostObject::get(Runtime&, const PropNameID&) { return Value::undefined(); }
bool HostObject::set(Runtime&, const PropNameID&, const Value&) { return true; }
std::vector<PropNameID> HostObject::getPropertyNames(Runtime&) { return {}; }

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

String Value::asString(Runtime& runtime) const {
  JSValueRef exception = nullptr;
  JSStringRef string = JSValueToStringCopy(runtime.context(), local(runtime), &exception);
  if (string == nullptr || exception != nullptr) {
    throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
  }
  String result(runtime, string);
  JSStringRelease(string);
  return result;
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
