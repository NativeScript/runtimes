#include "NativeApiQuickJSRuntime.h"

#ifdef TARGET_ENGINE_QUICKJS

namespace nativescript {
namespace engine {

Value HostObject::get(Runtime&, const PropNameID&) { return Value::undefined(); }
bool HostObject::set(Runtime&, const PropNameID&, const Value&) { return true; }
std::vector<PropNameID> HostObject::getPropertyNames(Runtime&) { return {}; }
String::String(Runtime& runtime, JSValue value)
    : storage_(std::make_shared<quickjsengine::ValueStorage>(
          quickjsengine::ValueStorage::Kind::QuickJS)) {
  storage_->context = runtime.context();
  storage_->value = JS_DupValue(runtime.context(), value);
}
std::string String::utf8(Runtime& runtime) const {
  JSValue value = local(runtime);
  std::string result = quickjsengine::valueToUtf8(runtime.context(), value);
  JS_FreeValue(runtime.context(), value);
  return result;
}
JSValue String::local(Runtime& runtime) const {
  return JS_DupValue(runtime.context(), storage_->value);
}
String::operator Value() const { return Value::fromStorage(storage_); }
Value::Value(Runtime&, const Object& object) {
  storage_ = object.storage_;
  kind_ = storage_ ? storage_->kind : quickjsengine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const Function& function) {
  storage_ = function.storage_;
  kind_ = storage_ ? storage_->kind : quickjsengine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const Array& array) {
  storage_ = array.storage_;
  kind_ = storage_ ? storage_->kind : quickjsengine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const ArrayBuffer& arrayBuffer) {
  storage_ = arrayBuffer.storage_;
  kind_ = storage_ ? storage_->kind : quickjsengine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const BigInt& bigint) {
  storage_ = bigint.storage_;
  kind_ = storage_ ? storage_->kind : quickjsengine::ValueStorage::Kind::Undefined;
}
Object Value::asObject(Runtime& runtime) const {
  if (storage_) {
    return Object::fromValueStorage(storage_);
  }
  // Promote to owned storage for Object.
  auto s = std::make_shared<quickjsengine::ValueStorage>(kind_);
  if (kind_ == quickjsengine::ValueStorage::Kind::QuickJSBorrowed) {
    s->kind = quickjsengine::ValueStorage::Kind::QuickJS;
    s->context = borrowedContext_;
    s->value = JS_DupValue(borrowedContext_, borrowedValue_);
  }
  return Object::fromValueStorage(std::move(s));
}
String Value::asString(Runtime& runtime) const {
  JSValue value = local(runtime);
  String result(runtime, value);
  JS_FreeValue(runtime.context(), value);
  return result;
}
BigInt Value::getBigInt(Runtime& runtime) const {
  JSValue value = local(runtime);
  BigInt result(runtime, value);
  JS_FreeValue(runtime.context(), value);
  return result;
}
Function Object::getPropertyAsFunction(Runtime& runtime, const char* name) const {
  return getProperty(runtime, name).asObject(runtime).asFunction(runtime);
}
Function Object::asFunction(Runtime&) const { return Function(*this); }
Array Object::getArray(Runtime&) const { return Array(*this); }
ArrayBuffer Object::getArrayBuffer(Runtime&) const { return ArrayBuffer(*this); }
Array Object::getPropertyNames(Runtime& runtime) const {
  JSValue object = local(runtime);
  JSPropertyEnum* properties = nullptr;
  uint32_t count = 0;
  int status = JS_GetOwnPropertyNames(runtime.context(), &properties, &count, object,
                                      JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_ENUM_ONLY);
  JS_FreeValue(runtime.context(), object);
  if (status < 0) {
    throw JSError(runtime, "QuickJS property names failed.");
  }
  Array result(runtime, count);
  for (uint32_t i = 0; i < count; i++) {
    JSValue nameValue = JS_AtomToValue(runtime.context(), properties[i].atom);
    result.setValueAtIndex(runtime, i, Value(runtime, nameValue));
    JS_FreeValue(runtime.context(), nameValue);
    JS_FreeAtom(runtime.context(), properties[i].atom);
  }
  js_free(runtime.context(), properties);
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

#endif  // TARGET_ENGINE_QUICKJS
