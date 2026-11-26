#include "jsi/quickjs/QuickJSRuntime.h"

#ifdef TARGET_ENGINE_QUICKJS

namespace nativescript {
namespace engine {

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
Object Value::asObjectBorrowed(Runtime& runtime) const {
  return asObject(runtime);
}
String Value::asString(Runtime& runtime) const {
  JSValue value = local(runtime);
  String result(runtime, value);
  JS_FreeValue(runtime.context(), value);
  return result;
}

std::string Value::utf8(Runtime& runtime) const {
  // JS_ToCStringLen does not consume its argument, so a refcounted value can be
  // read in place -- no local(), which dups, and no matching free.
  if (kind_ == quickjsengine::ValueStorage::Kind::QuickJS ||
      kind_ == quickjsengine::ValueStorage::Kind::QuickJSBorrowed) {
    return quickjsengine::valueToUtf8(runtime.context(), jsValue());
  }
  JSValue value = local(runtime);
  std::string result = quickjsengine::valueToUtf8(runtime.context(), value);
  JS_FreeValue(runtime.context(), value);
  return result;
}

Value Value::createStringFromUtf8(Runtime& runtime, const char* data, size_t length) {
  Value result;
  result.kind_ = quickjsengine::ValueStorage::Kind::QuickJS;
  result.storage_ =
      std::make_shared<quickjsengine::ValueStorage>(quickjsengine::ValueStorage::Kind::QuickJS);
  result.storage_->context = runtime.context();
  result.storage_->value =
      JS_NewStringLen(runtime.context(), data != nullptr ? data : "", length);
  return result;
}

Value Value::createStringFromUtf16(Runtime& runtime, const char16_t* data, size_t length) {
  Value result;
  result.kind_ = quickjsengine::ValueStorage::Kind::QuickJS;
  result.storage_ =
      std::make_shared<quickjsengine::ValueStorage>(quickjsengine::ValueStorage::Kind::QuickJS);
  result.storage_->context = runtime.context();
  result.storage_->value = JS_NewStringUTF16(
      runtime.context(), reinterpret_cast<const uint16_t*>(data != nullptr ? data : u""), length);
  return result;
}

size_t Value::copyUtf16(Runtime& runtime, char16_t* buffer, size_t capacity) const {
  JSContext* context = runtime.context();
  JSValue value = local(runtime);
  size_t length = 0;
  const uint16_t* units = JS_ToCStringLenUTF16(context, &length, value);
  JS_FreeValue(context, value);
  if (units == nullptr) return 0;
  std::memcpy(buffer, units, std::min(length, capacity) * sizeof(char16_t));
  JS_FreeCStringUTF16(context, units);
  return length;
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
    throw quickjsengine::caughtError(runtime, "QuickJS property names failed.");
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
