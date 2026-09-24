#include "jsi/v8/V8Runtime.h"

#ifdef TARGET_ENGINE_V8

namespace nativescript {
namespace engine {

Value HostObject::get(Runtime&, const PropNameID&) { return Value::undefined(); }

bool HostObject::set(Runtime&, const PropNameID&, const Value&) { return true; }

std::vector<PropNameID> HostObject::getPropertyNames(Runtime&) { return {}; }

String::String(Runtime& runtime, v8::Local<v8::String> value)
    : storage_(std::make_shared<v8engine::ValueStorage>(v8engine::ValueStorage::Kind::V8)) {
  storage_->value.Reset(runtime.isolate(), value);
}

String::operator Value() const {
  return Value::fromStorage(storage_);
}

Value::Value(Runtime&, const String& value) {
  storage_ = value.storage_;
  kind_ = storage_->kind;
}
Value::Value(Runtime&, const Object& object) {
  storage_ = object.storage_;
  kind_ = storage_ ? storage_->kind : v8engine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const Function& function) {
  storage_ = function.storage_;
  kind_ = storage_ ? storage_->kind : v8engine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const Array& array) {
  storage_ = array.storage_;
  kind_ = storage_ ? storage_->kind : v8engine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const ArrayBuffer& arrayBuffer) {
  storage_ = arrayBuffer.storage_;
  kind_ = storage_ ? storage_->kind : v8engine::ValueStorage::Kind::Undefined;
}
Value::Value(Runtime&, const BigInt& bigint) {
  storage_ = bigint.storage_;
  kind_ = storage_ ? storage_->kind : v8engine::ValueStorage::Kind::Undefined;
}

bool Value::isObject() const {
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsObject();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  return isolate != nullptr && storage_->value.Get(isolate)->IsObject();
}

bool Value::isUndefined() const {
  if (kind_ == v8engine::ValueStorage::Kind::Undefined) {
    return true;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return borrowedValue_.IsEmpty() || borrowedValue_->IsUndefined();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  return isolate != nullptr && storage_->value.Get(isolate)->IsUndefined();
}

bool Value::isNull() const {
  if (kind_ == v8engine::ValueStorage::Kind::Null) {
    return true;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsNull();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  return isolate != nullptr && storage_->value.Get(isolate)->IsNull();
}

bool Value::isBool() const {
  if (kind_ == v8engine::ValueStorage::Kind::Bool) {
    return true;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsBoolean();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  return isolate != nullptr && storage_->value.Get(isolate)->IsBoolean();
}

bool Value::getBool() const {
  if (kind_ == v8engine::ValueStorage::Kind::Bool) {
    return boolValue_;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    return isolate != nullptr && !borrowedValue_.IsEmpty()
               ? borrowedValue_->BooleanValue(isolate)
               : false;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8 && storage_ && !storage_->value.IsEmpty()) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    if (isolate != nullptr) {
      return storage_->value.Get(isolate)->BooleanValue(isolate);
    }
  }
  return false;
}

bool Value::isNumber() const {
  if (kind_ == v8engine::ValueStorage::Kind::Number) {
    return true;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsNumber();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  return isolate != nullptr && storage_->value.Get(isolate)->IsNumber();
}

double Value::getNumber() const {
  if (kind_ == v8engine::ValueStorage::Kind::Number) {
    return numberValue_;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    if (isolate != nullptr && !borrowedValue_.IsEmpty()) {
      return borrowedValue_->NumberValue(isolate->GetCurrentContext()).FromMaybe(0);
    }
    return 0;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8 && storage_ && !storage_->value.IsEmpty()) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    if (isolate != nullptr) {
      return storage_->value.Get(isolate)->NumberValue(isolate->GetCurrentContext()).FromMaybe(0);
    }
  }
  return 0;
}

bool Value::isString() const {
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsString();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  return storage_->value.Get(isolate)->IsString();
}

bool Value::isBigInt() const {
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsBigInt();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  return storage_->value.Get(isolate)->IsBigInt();
}

bool Value::isSymbol() const {
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsSymbol();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  return storage_->value.Get(isolate)->IsSymbol();
}

Object Value::asObject(Runtime& runtime) const {
  if (storage_) {
    return Object::fromValueStorage(storage_);
  }
  // Need to promote to storage for Object
  auto s = std::make_shared<v8engine::ValueStorage>(kind_);
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    s->kind = v8engine::ValueStorage::Kind::V8;
    s->value.Reset(runtime.isolate(), borrowedValue_);
  }
  return Object::fromValueStorage(std::move(s));
}

String Value::asString(Runtime& runtime) const {
  return String(runtime, local(runtime).As<v8::String>());
}

BigInt Value::getBigInt(Runtime& runtime) const {
  return BigInt(runtime, local(runtime).As<v8::BigInt>());
}

Function Object::getPropertyAsFunction(Runtime& runtime, const char* name) const {
  return getProperty(runtime, name).asObject(runtime).asFunction(runtime);
}

Function Object::asFunction(Runtime& runtime) const { return Function(*this); }

Array Object::getArray(Runtime& runtime) const { return Array(*this); }

ArrayBuffer Object::getArrayBuffer(Runtime& runtime) const { return ArrayBuffer(*this); }

Array Object::getPropertyNames(Runtime& runtime) const {
  v8::TryCatch tryCatch(runtime.isolate());
  v8::Local<v8::Array> result;
  if (!local(runtime)->GetPropertyNames(runtime.context()).ToLocal(&result)) {
    throw JSError(runtime, v8engine::currentExceptionMessage(runtime.isolate(), tryCatch));
  }
  return Array(Object::fromValueStorage(Value(runtime, result).storage_));
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

#endif  // TARGET_ENGINE_V8
