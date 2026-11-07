#include "jsi/v8/V8Runtime.h"

#ifdef TARGET_ENGINE_V8

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
  // Value(runtime, value) promotes: the indexed setter hands over a borrowed
  // value, and a host object reached through the named setter has always been
  // given an owned one.
  return set(runtime, PropNameID(std::to_string(index)), Value(runtime, value));
}

String::String(Runtime& runtime, v8::Local<v8::String> value)
    : storage_(std::make_shared<v8engine::ValueStorage>(v8engine::ValueStorage::Kind::V8)) {
  storage_->reset(runtime.isolate(), value);
}

String::operator Value() const {
  return Value::fromStorage(storage_);
}

// Every one of these takes a Runtime, which is this layer's spelling of "give
// me a value I own". Since asObject stopped globalizing a borrowed handle, the
// storage they adopt may be borrowed -- so they route through
// Value(Runtime&, const Value&), whose whole job is to promote exactly that
// case. Copying the tag through unchanged would produce a Value that dangles
// when the enclosing HandleScope unwinds, and whose inline borrowedValue_ was
// never filled at all (that member lives on Value, not on the storage).
namespace {
Value adopt(Runtime& runtime, const std::shared_ptr<v8engine::ValueStorage>& storage) {
  if (!storage) {
    return Value::undefined();
  }
  return Value(runtime, Value::fromStorage(storage));
}
}  // namespace

Value::Value(Runtime& runtime, const String& value)
    : Value(adopt(runtime, value.storage_)) {}
Value::Value(Runtime& runtime, const Object& object)
    : Value(adopt(runtime, object.storage_)) {}
Value::Value(Runtime& runtime, const Function& function)
    : Value(adopt(runtime, function.storage_)) {}
Value::Value(Runtime& runtime, const Array& array)
    : Value(adopt(runtime, array.storage_)) {}
Value::Value(Runtime& runtime, const ArrayBuffer& arrayBuffer)
    : Value(adopt(runtime, arrayBuffer.storage_)) {}
Value::Value(Runtime& runtime, const BigInt& bigint)
    : Value(adopt(runtime, bigint.storage_)) {}

bool Value::isObject() const {
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsObject();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = storage_->isolateOrCurrent();
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
  v8::Isolate* isolate = storage_->isolateOrCurrent();
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
  v8::Isolate* isolate = storage_->isolateOrCurrent();
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
  v8::Isolate* isolate = storage_->isolateOrCurrent();
  return isolate != nullptr && storage_->value.Get(isolate)->IsBoolean();
}

bool Value::getBool() const {
  if (kind_ == v8engine::ValueStorage::Kind::Bool) {
    return boolValue_;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    // A borrowed Value has no storage_ -- it is a bare Local -- so the isolate
    // comes from the one recorded when it was tagged, falling back to the
    // thread only for the handful of callers that had none.
    v8::Isolate* isolate = borrowedIsolate();
    return isolate != nullptr && !borrowedValue_.IsEmpty()
               ? borrowedValue_->BooleanValue(isolate)
               : false;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8 && storage_ && !storage_->value.IsEmpty()) {
    v8::Isolate* isolate = storage_->isolateOrCurrent();
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
  v8::Isolate* isolate = storage_->isolateOrCurrent();
  return isolate != nullptr && storage_->value.Get(isolate)->IsNumber();
}

double Value::getNumber() const {
  if (kind_ == v8engine::ValueStorage::Kind::Number) {
    return numberValue_;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    // No storage_ on a borrowed Value; see getBool.
    v8::Isolate* isolate = borrowedIsolate();
    if (isolate != nullptr && !borrowedValue_.IsEmpty()) {
      return borrowedValue_->NumberValue(isolate->GetCurrentContext()).FromMaybe(0);
    }
    return 0;
  }
  if (kind_ == v8engine::ValueStorage::Kind::V8 && storage_ && !storage_->value.IsEmpty()) {
    v8::Isolate* isolate = storage_->isolateOrCurrent();
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
  v8::Isolate* isolate = storage_->isolateOrCurrent();
  return storage_->value.Get(isolate)->IsString();
}

bool Value::isBigInt() const {
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsBigInt();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = storage_->isolateOrCurrent();
  return storage_->value.Get(isolate)->IsBigInt();
}

bool Value::isSymbol() const {
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    return !borrowedValue_.IsEmpty() && borrowedValue_->IsSymbol();
  }
  if (kind_ != v8engine::ValueStorage::Kind::V8 || !storage_ || storage_->value.IsEmpty()) {
    return false;
  }
  v8::Isolate* isolate = storage_->isolateOrCurrent();
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
    s->reset(runtime.isolate(), borrowedValue_);
  }
  return Object::fromValueStorage(std::move(s));
}

Object Value::asObjectBorrowed(Runtime& runtime) const {
  if (storage_) {
    return Object::fromValueStorage(storage_);
  }
  // Unlike asObject, a borrowed Value stays borrowed.
  //
  // asObject globalizes the handle -- make_shared plus a
  // GlobalHandles::Create, released again a few statements later -- to give
  // Object a persistent handle. Object reads its handle through local(), which
  // already understands the borrowed kind, so for a caller that does not
  // outlive the enclosing HandleScope that Global is pure cost: an allocation
  // plus a GC root that every scavenge scans, on every property access made
  // through a borrowed value.
  //
  // Kept as a separate entry point rather than folded into asObject because
  // asObject has ~140 call sites in the Apple bridge whose lifetimes have not
  // been audited, and this weakens the guarantee. The Node-API shim is the one
  // caller that provably qualifies: a napi_value is scope-bounded by
  // definition, and every napi handle scope is nested inside the v8::HandleScope
  // that NapiScope opens.
  auto s = std::make_shared<v8engine::ValueStorage>(kind_);
  if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
    s->isolate = isolate_ != nullptr ? isolate_ : runtime.isolate();
    s->borrowedValue = borrowedValue_;
  }
  return Object::fromValueStorage(std::move(s));
}

String Value::asString(Runtime& runtime) const {
  return String(runtime, local(runtime).As<v8::String>());
}

std::string Value::utf8(Runtime& runtime) const {
  return v8engine::toUtf8(runtime.isolate(), local(runtime));
}

Value Value::createStringFromUtf8(Runtime& runtime, const char* data, size_t length) {
  v8::Isolate* isolate = runtime.isolate();
  return Value::borrowed(
      isolate, v8::String::NewFromUtf8(isolate, data != nullptr ? data : "",
                                       v8::NewStringType::kNormal, static_cast<int>(length))
                   .ToLocalChecked());
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
    throw v8engine::caughtError(runtime, tryCatch);
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
