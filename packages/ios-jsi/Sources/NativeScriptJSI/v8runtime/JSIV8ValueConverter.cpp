/*
 * Copyright (c) Kudo Chien.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSIV8ValueConverter.h"

#include "V8PointerValue.h"

namespace jsi = facebook::jsi;

namespace rnv8 {

// static
jsi::Value JSIV8ValueConverter::ToJSIValue(v8::Isolate* isolate,
                                           const v8::Local<v8::Value>& value) {
  if (value->IsNumber()) {
    return jsi::Value(value.As<v8::Number>()->Value());
  }
  if (value->IsUndefined()) {
    return jsi::Value::undefined();
  }
  if (value->IsNull()) {
    return jsi::Value::null();
  }
  if (value->IsBoolean()) {
    return jsi::Value(value->BooleanValue(isolate));
  }
  if (value->IsString()) {
    return V8Runtime::make<jsi::String>(new V8PointerValue(isolate, value));
  }
  if (value->IsObject()) {
    return V8Runtime::make<jsi::Object>(new V8PointerValue(isolate, value));
  }
  if (value->IsSymbol()) {
    return V8Runtime::make<jsi::Symbol>(new V8PointerValue(isolate, value));
  }

  return jsi::Value::undefined();
}

// static
v8::Local<v8::Value> JSIV8ValueConverter::ToV8Value(const V8Runtime& runtime,
                                                    const jsi::Value& value) {
  if (value.isNumber()) {
    return v8::Number::New(runtime.isolate_, value.getNumber());
  } else if (value.isUndefined()) {
    return v8::Undefined(runtime.isolate_);
  } else if (value.isNull()) {
    return v8::Null(runtime.isolate_);
  } else if (value.isBool()) {
    return v8::Boolean::New(runtime.isolate_, value.getBool());
  } else if (value.isString()) {
    return ToV8String(
        runtime, value.getString(const_cast<V8Runtime&>(runtime)));
  } else if (value.isObject()) {
    return ToV8Object(
        runtime, value.getObject(const_cast<V8Runtime&>(runtime)));
  } else {
    // What are you?
    std::abort();
  }
}

// static
v8::Local<v8::String> JSIV8ValueConverter::ToV8String(
    const V8Runtime& runtime, const jsi::String& string) {
  v8::EscapableHandleScope scopedHandle(runtime.isolate_);
  const V8PointerValue* v8PointerValue =
      static_cast<const V8PointerValue*>(runtime.getPointerValue(string));
  assert(v8PointerValue->Get(runtime.isolate_)->IsString());
  return scopedHandle.Escape(
      v8::Local<v8::String>::Cast(v8PointerValue->Get(runtime.isolate_)));
}

// static
v8::Local<v8::String> JSIV8ValueConverter::ToV8String(
    const V8Runtime& runtime, const jsi::PropNameID& propName) {
  v8::EscapableHandleScope scopedHandle(runtime.isolate_);
  const V8PointerValue* v8PointerValue =
      static_cast<const V8PointerValue*>(runtime.getPointerValue(propName));
  assert(v8PointerValue->Get(runtime.isolate_)->IsString());
  return scopedHandle.Escape(
      v8::Local<v8::String>::Cast(v8PointerValue->Get(runtime.isolate_)));
}

// static
v8::MaybeLocal<v8::String> JSIV8ValueConverter::ToV8String(
    const V8Runtime& runtime,
    const std::shared_ptr<const jsi::Buffer>& buffer) {
  v8::EscapableHandleScope scopedHandle(runtime.isolate_);
  v8::MaybeLocal<v8::String> ret = v8::String::NewFromUtf8(
      runtime.isolate_, reinterpret_cast<const char*>(buffer->data()),
      v8::NewStringType::kNormal, (int)buffer->size());
  return scopedHandle.EscapeMaybe(ret);
}

// static
v8::Local<v8::Symbol> JSIV8ValueConverter::ToV8Symbol(
    const V8Runtime& runtime, const jsi::Symbol& symbol) {
  v8::EscapableHandleScope scopedHandle(runtime.isolate_);
  const V8PointerValue* v8PointerValue =
      static_cast<const V8PointerValue*>(runtime.getPointerValue(symbol));
  assert(v8PointerValue->Get(runtime.isolate_)->IsSymbol());
  return scopedHandle.Escape(
      v8::Local<v8::Symbol>::Cast(v8PointerValue->Get(runtime.isolate_)));
}

// static
v8::Local<v8::Object> JSIV8ValueConverter::ToV8Object(
    const V8Runtime& runtime, const jsi::Object& object) {
  v8::EscapableHandleScope scopedHandle(runtime.isolate_);
  const V8PointerValue* v8PointerValue =
      static_cast<const V8PointerValue*>(runtime.getPointerValue(object));
  assert(v8PointerValue->Get(runtime.isolate_)->IsObject());
  return scopedHandle.Escape(
      v8::Local<v8::Object>::Cast(v8PointerValue->Get(runtime.isolate_)));
}

// static
v8::Local<v8::Array> JSIV8ValueConverter::ToV8Array(const V8Runtime& runtime,
                                                    const jsi::Array& array) {
  v8::EscapableHandleScope scopedHandle(runtime.isolate_);
  const V8PointerValue* v8PointerValue =
      static_cast<const V8PointerValue*>(runtime.getPointerValue(array));
  assert(v8PointerValue->Get(runtime.isolate_)->IsArray());
  return scopedHandle.Escape(
      v8::Local<v8::Array>::Cast(v8PointerValue->Get(runtime.isolate_)));
}

// static
v8::Local<v8::Function> JSIV8ValueConverter::ToV8Function(
    const V8Runtime& runtime, const jsi::Function& function) {
  v8::EscapableHandleScope scopedHandle(runtime.isolate_);
  const V8PointerValue* v8PointerValue =
      static_cast<const V8PointerValue*>(runtime.getPointerValue(function));
  assert(v8PointerValue->Get(runtime.isolate_)->IsFunction());
  return scopedHandle.Escape(
      v8::Local<v8::Function>::Cast(v8PointerValue->Get(runtime.isolate_)));
}

// static
jsi::PropNameID JSIV8ValueConverter::ToJSIPropNameID(
    const V8Runtime& runtime, const v8::Local<v8::Name>& property) {
  v8::HandleScope scopedHandle(runtime.isolate_);
  return runtime.make<jsi::PropNameID>(
      new V8PointerValue(runtime.isolate_, property));
}

// static
std::string JSIV8ValueConverter::ToSTLString(
    const v8::String::Utf8Value& string) {
  if (*string) {
    return std::string(*string, string.length());
  }
  return {};
}

// static
std::string JSIV8ValueConverter::ToSTLString(
    v8::Isolate* isolate, const v8::Local<v8::Value>& string) {
  v8::HandleScope scopedHandle(isolate);
  assert(string->IsString());
  v8::String::Utf8Value utf8(isolate, string);
  return ToSTLString(utf8);
}

}  // namespace rnv8