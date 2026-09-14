#ifndef NATIVESCRIPT_FFI_V8_NATIVE_API_V8_RUNTIME_H
#define NATIVESCRIPT_FFI_V8_NATIVE_API_V8_RUNTIME_H

#ifdef TARGET_ENGINE_V8

#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../shared/RuntimeCleanupRegistry.h"
#include "Metadata.h"
#include "MetadataReader.h"
#include "ffi.h"
#include "v8.h"

@protocol NativeApiClassBuilderProtocol
@end

#ifdef EMBED_METADATA_SIZE
extern const unsigned char embedded_metadata[EMBED_METADATA_SIZE];
#endif

namespace nativescript {
namespace engine {

class Runtime;
class Value;
class Object;
class Function;
class Array;
class String;
class BigInt;
class ArrayBuffer;

class JSError : public std::runtime_error {
 public:
  JSError(Runtime&, const std::string& message) : std::runtime_error(message) {}
  explicit JSError(const std::string& message) : std::runtime_error(message) {}
};

class StringBuffer {
 public:
  explicit StringBuffer(std::string value) : value_(std::move(value)) {}
  const char* data() const { return value_.data(); }
  size_t size() const { return value_.size(); }

 private:
  std::string value_;
};

class MutableBuffer {
 public:
  virtual ~MutableBuffer() = default;
  virtual size_t size() const = 0;
  virtual uint8_t* data() = 0;
};

class PropNameID {
 public:
  PropNameID() = default;
  explicit PropNameID(std::string value) : value_(std::move(value)) {}

  static PropNameID forAscii(Runtime&, const char* value) {
    return PropNameID(value != nullptr ? value : "");
  }

  static PropNameID forAscii(Runtime&, const std::string& value) { return PropNameID(value); }

  std::string utf8(Runtime&) const { return value_; }

 private:
  std::string value_;
};

class HostObject {
 public:
  virtual ~HostObject() = default;
  virtual Value get(Runtime& runtime, const PropNameID& name);
  virtual bool set(Runtime& runtime, const PropNameID& name, const Value& value);
  virtual std::vector<PropNameID> getPropertyNames(Runtime& runtime);
};

using HostFunctionType = std::function<Value(Runtime&, const Value&, const Value*, size_t)>;

namespace v8engine {

struct RuntimeState {
  RuntimeState(v8::Isolate* isolate, v8::Local<v8::Context> context)
      : isolate(isolate), context(isolate, context) {}

  ~RuntimeState() {
    nativeClassArgumentLast.value.Reset();
    nativeClassArgumentLast.nativeClass = Nil;
    for (auto& entry : nativeClassArgumentCache) {
      entry.value.Reset();
      entry.nativeClass = Nil;
    }
    nativeSelectorArgumentLast.value.Reset();
    nativeSelectorArgumentLast.selector = nullptr;
    for (auto& entry : nativeSelectorArgumentCache) {
      entry.value.Reset();
      entry.selector = nullptr;
    }
    context.Reset();
  }

  v8::Local<v8::Context> localContext() const {
    return context.Get(isolate);
  }

  v8::Isolate* isolate = nullptr;
  v8::Global<v8::Context> context;
  v8::Global<v8::ObjectTemplate> hostObjectTemplate;
  v8::Global<v8::ObjectTemplate> nativeObjectTemplate;  // kNonMasking for instances
  struct NativeClassArgumentCacheEntry {
    v8::Global<v8::Value> value;
    Class nativeClass = Nil;
  };
  NativeClassArgumentCacheEntry nativeClassArgumentLast;
  NativeClassArgumentCacheEntry nativeClassArgumentCache[4];
  size_t nativeClassArgumentCacheNext = 0;
  struct NativeSelectorArgumentCacheEntry {
    v8::Global<v8::Value> value;
    SEL selector = nullptr;
  };
  NativeSelectorArgumentCacheEntry nativeSelectorArgumentLast;
  NativeSelectorArgumentCacheEntry nativeSelectorArgumentCache[4];
  size_t nativeSelectorArgumentCacheNext = 0;
};

struct ValueStorage {
  enum class Kind : uint8_t {
    Undefined,
    Null,
    Bool,
    Number,
    V8,
    V8Borrowed,
  };

  explicit ValueStorage(Kind kind) : kind(kind) {}

  ~ValueStorage() { value.Reset(); }

  Kind kind = Kind::Undefined;
  bool boolValue = false;
  double numberValue = 0;
  v8::Global<v8::Value> value;
  v8::Local<v8::Value> borrowedValue;
};

template <typename T>
const void* hostObjectTypeToken() {
  static int token = 0;
  return &token;
}

struct HostObjectHolder {
  HostObjectHolder(std::shared_ptr<RuntimeState> state, std::shared_ptr<HostObject> hostObject,
                   const void* typeToken)
      : state(std::move(state)), hostObject(std::move(hostObject)), typeToken(typeToken) {}

  ~HostObjectHolder() { object.Reset(); }

  std::shared_ptr<RuntimeState> state;
  std::shared_ptr<HostObject> hostObject;
  const void* typeToken = nullptr;
  v8::Global<v8::Object> object;
};

struct FunctionHolder {
  FunctionHolder(std::shared_ptr<RuntimeState> state, HostFunctionType callback)
      : state(std::move(state)), callback(std::move(callback)) {}

  ~FunctionHolder() { function.Reset(); }

  std::shared_ptr<RuntimeState> state;
  HostFunctionType callback;
  v8::Global<v8::Function> function;
};

inline thread_local std::unordered_map<v8::Isolate*, RuntimeCleanupRegistry>
    runtimeAllocations;

template <typename T>
void trackRuntimeAllocation(v8::Isolate* isolate, T* allocation) {
  runtimeAllocations[isolate].track(allocation);
}

inline void untrackRuntimeAllocation(v8::Isolate* isolate,
                                     void* allocation) {
  auto runtime = runtimeAllocations.find(isolate);
  if (runtime == runtimeAllocations.end()) {
    return;
  }
  runtime->second.untrack(allocation);
  if (runtime->second.empty()) {
    runtimeAllocations.erase(runtime);
  }
}

inline void cleanupRuntimeAllocations(v8::Isolate* isolate) {
  auto runtime = runtimeAllocations.find(isolate);
  if (runtime == runtimeAllocations.end()) {
    return;
  }

  runtime->second.cleanup();
  runtimeAllocations.erase(runtime);
}

struct ArrayBufferHolder {
  explicit ArrayBufferHolder(std::shared_ptr<MutableBuffer> buffer) : buffer(std::move(buffer)) {}

  std::shared_ptr<MutableBuffer> buffer;
  v8::Global<v8::ArrayBuffer> object;
};

inline v8::Local<v8::String> makeV8String(v8::Isolate* isolate, const std::string& value) {
  return v8::String::NewFromUtf8(isolate, value.c_str(), v8::NewStringType::kNormal,
                                 static_cast<int>(value.size()))
      .ToLocalChecked();
}

inline std::string toUtf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  if (value.IsEmpty()) {
    return {};
  }
  v8::String::Utf8Value utf8(isolate, value);
  return *utf8 != nullptr ? std::string(*utf8, utf8.length()) : std::string();
}

inline std::string propertyNameToUtf8(v8::Isolate* isolate, v8::Local<v8::Name> property) {
  if (property->IsSymbol() &&
      property.As<v8::Value>()->StrictEquals(v8::Symbol::GetIterator(isolate))) {
    return "Symbol.iterator";
  }
  return toUtf8(isolate, property);
}

inline std::string currentExceptionMessage(v8::Isolate* isolate, v8::TryCatch& tryCatch) {
  if (tryCatch.HasCaught()) {
    return toUtf8(isolate, tryCatch.Exception());
  }
  return "NativeScript V8 engine operation failed.";
}

inline void throwV8Exception(v8::Isolate* isolate, const std::exception& exception) {
  isolate->ThrowException(v8::Exception::Error(makeV8String(isolate, exception.what())));
}

}  // namespace v8engine

class Runtime {
 public:
  Runtime(v8::Isolate* isolate, v8::Local<v8::Context> context)
      : state_(std::make_shared<v8engine::RuntimeState>(isolate, context)) {}

  explicit Runtime(std::shared_ptr<v8engine::RuntimeState> state) : state_(std::move(state)) {}

  v8::Isolate* isolate() const { return state_->isolate; }
  v8::Local<v8::Context> context() const { return state_->localContext(); }
  v8engine::RuntimeState* rawState() const { return state_.get(); }
  std::shared_ptr<v8engine::RuntimeState> state() const { return state_; }
  const void* identity() const { return state_.get(); }

  Object global();

  Value evaluateJavaScript(std::shared_ptr<StringBuffer> buffer, const std::string& sourceURL);

  void drainMicrotasks() { isolate()->PerformMicrotaskCheckpoint(); }

 private:
  std::shared_ptr<v8engine::RuntimeState> state_;
};

class String {
 public:
  String() = default;
  String(Runtime& runtime, v8::Local<v8::String> value);

  static String createFromUtf8(Runtime& runtime, const char* value) {
    return String(runtime,
                  v8engine::makeV8String(runtime.isolate(), value != nullptr ? value : ""));
  }

  static String createFromUtf8(Runtime& runtime, const std::string& value) {
    return String(runtime, v8engine::makeV8String(runtime.isolate(), value));
  }

  static String createFromUtf8(Runtime& runtime, const uint8_t* value, size_t length) {
    return String(runtime, v8::String::NewFromUtf8(
                               runtime.isolate(),
                               reinterpret_cast<const char*>(
                                   value != nullptr ? value : reinterpret_cast<const uint8_t*>("")),
                               v8::NewStringType::kNormal, static_cast<int>(length))
                               .ToLocalChecked());
  }

  std::string utf8(Runtime& runtime) const {
    return v8engine::toUtf8(runtime.isolate(), local(runtime));
  }

  v8::Local<v8::String> local(Runtime& runtime) const {
    return storage_->value.Get(runtime.isolate()).As<v8::String>();
  }

  operator Value() const;

 private:
  friend class Value;
  std::shared_ptr<v8engine::ValueStorage> storage_;
};

class Value {
 public:
  Value() : kind_(v8engine::ValueStorage::Kind::Undefined) {}

  Value(bool value) : kind_(v8engine::ValueStorage::Kind::Bool), boolValue_(value) {}

  Value(double value) : kind_(v8engine::ValueStorage::Kind::Number), numberValue_(value) {}

  Value(int value) : Value(static_cast<double>(value)) {}
  Value(uint32_t value) : Value(static_cast<double>(value)) {}

  Value(Runtime& runtime, const Value& value) {
    if (value.kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
      // Promote borrowed to owned
      storage_ =
          std::make_shared<v8engine::ValueStorage>(v8engine::ValueStorage::Kind::V8);
      storage_->value.Reset(runtime.isolate(), value.borrowedValue_);
      kind_ = v8engine::ValueStorage::Kind::V8;
      return;
    }
    kind_ = value.kind_;
    boolValue_ = value.boolValue_;
    numberValue_ = value.numberValue_;
    borrowedValue_ = value.borrowedValue_;
    storage_ = value.storage_;
  }
  Value(Runtime& runtime, Value&& value)
      : kind_(value.kind_),
        boolValue_(value.boolValue_),
        numberValue_(value.numberValue_),
        borrowedValue_(value.borrowedValue_),
        storage_(std::move(value.storage_)) {}
  Value(Runtime& runtime, const String& value);
  Value(Runtime& runtime, const Object& object);
  Value(Runtime& runtime, const Function& function);
  Value(Runtime& runtime, const Array& array);
  Value(Runtime& runtime, const ArrayBuffer& arrayBuffer);
  Value(Runtime& runtime, const BigInt& bigint);

  static Value undefined() { return Value(); }

  static Value null() {
    Value value;
    value.kind_ = v8engine::ValueStorage::Kind::Null;
    return value;
  }

  static bool strictEquals(Runtime& runtime, const Value& lhs,
                           const Value& rhs) {
    return lhs.local(runtime)->StrictEquals(rhs.local(runtime));
  }

  bool isUndefined() const;
  bool isNull() const;
  bool isBool() const;
  bool getBool() const;
  bool isNumber() const;
  double getNumber() const;

  bool isObject() const;
  bool isString() const;
  bool isBigInt() const;
  bool isSymbol() const;

  Object asObject(Runtime& runtime) const;
  String asString(Runtime& runtime) const;
  BigInt getBigInt(Runtime& runtime) const;

  v8::Local<v8::Value> local(Runtime& runtime) const {
    v8::Isolate* isolate = runtime.isolate();
    switch (kind_) {
      case v8engine::ValueStorage::Kind::Undefined:
        return v8::Undefined(isolate);
      case v8engine::ValueStorage::Kind::Null:
        return v8::Null(isolate);
      case v8engine::ValueStorage::Kind::Bool:
        return v8::Boolean::New(isolate, boolValue_);
      case v8engine::ValueStorage::Kind::Number:
        return v8::Number::New(isolate, numberValue_);
      case v8engine::ValueStorage::Kind::V8:
        return storage_->value.Get(isolate);
      case v8engine::ValueStorage::Kind::V8Borrowed:
        return borrowedValue_;
    }
  }

  Value(Runtime& runtime, v8::Local<v8::Value> value)
      : kind_(v8engine::ValueStorage::Kind::V8),
        storage_(std::make_shared<v8engine::ValueStorage>(v8engine::ValueStorage::Kind::V8)) {
    storage_->value.Reset(runtime.isolate(), value);
  }

  static Value borrowed(Runtime&, v8::Local<v8::Value> value) {
    Value result;
    result.kind_ = v8engine::ValueStorage::Kind::V8Borrowed;
    result.borrowedValue_ = value;
    return result;
  }

  // Access the shared storage (for Object/Function/Array interop)
  std::shared_ptr<v8engine::ValueStorage> storage() const { return storage_; }

  static Value fromStorage(std::shared_ptr<v8engine::ValueStorage> s) {
    Value v;
    v.kind_ = s->kind;
    v.boolValue_ = s->boolValue;
    v.numberValue_ = s->numberValue;
    v.borrowedValue_ = s->borrowedValue;
    v.storage_ = std::move(s);
    return v;
  }

 private:
  friend class Runtime;
  friend class Object;
  friend class String;
  friend class BigInt;
  friend class ArrayBuffer;
  friend class Function;
  friend class Array;

  v8engine::ValueStorage::Kind kind_ = v8engine::ValueStorage::Kind::Undefined;
  bool boolValue_ = false;
  double numberValue_ = 0;
  v8::Local<v8::Value> borrowedValue_;
  std::shared_ptr<v8engine::ValueStorage> storage_;
};

class Object {
 public:
  Object() = default;
  explicit Object(Runtime& runtime)
      : storage_(std::make_shared<v8engine::ValueStorage>(v8engine::ValueStorage::Kind::V8)) {
    storage_->value.Reset(runtime.isolate(), v8::Object::New(runtime.isolate()));
  }

  static Object fromValueStorage(std::shared_ptr<v8engine::ValueStorage> storage) {
    Object object;
    object.storage_ = std::move(storage);
    return object;
  }

  template <typename T>
  static Object createFromHostObject(Runtime& runtime, std::shared_ptr<T> host) {
    auto baseHost = std::static_pointer_cast<HostObject>(std::move(host));
    return createFromHostObjectWithToken(runtime, std::move(baseHost),
                                         v8engine::hostObjectTypeToken<T>());
  }

  // Create a native object instance using kNonMasking template for fast
  // prototype-based property access.
  template <typename T>
  static Object createNativeInstanceHostObject(Runtime& runtime, std::shared_ptr<T> host) {
    auto baseHost = std::static_pointer_cast<HostObject>(std::move(host));
    return createNativeInstanceWithToken(runtime, std::move(baseHost),
                                         v8engine::hostObjectTypeToken<T>());
  }

  static Object createNativeInstanceWithToken(Runtime& runtime, std::shared_ptr<HostObject> host,
                                              const void* typeToken);

  Value getProperty(Runtime& runtime, const char* name) const {
    return getProperty(runtime,
                       v8engine::makeV8String(runtime.isolate(), name != nullptr ? name : ""));
  }

  Value getProperty(Runtime& runtime, const std::string& name) const {
    return getProperty(runtime, name.c_str());
  }

  Value getProperty(Runtime& runtime, const Value& key) const {
    return getProperty(runtime, key.local(runtime));
  }

  Value getProperty(Runtime& runtime, v8::Local<v8::Value> key) const {
    v8::TryCatch tryCatch(runtime.isolate());
    v8::Local<v8::Value> result;
    if (!local(runtime)->Get(runtime.context(), key).ToLocal(&result)) {
      throw JSError(runtime, v8engine::currentExceptionMessage(runtime.isolate(), tryCatch));
    }
    return Value(runtime, result);
  }

  Object getPropertyAsObject(Runtime& runtime, const char* name) const {
    return getProperty(runtime, name).asObject(runtime);
  }

  Function getPropertyAsFunction(Runtime& runtime, const char* name) const;

  void setProperty(Runtime& runtime, const char* name, const Value& value) {
    setProperty(runtime, v8engine::makeV8String(runtime.isolate(), name != nullptr ? name : ""),
                value);
  }

  void setProperty(Runtime& runtime, const char* name, const String& value) {
    setProperty(runtime, name, Value(runtime, value));
  }

  void setProperty(Runtime& runtime, const char* name, const Object& value) {
    setProperty(runtime, name, Value(runtime, value));
  }

  void setProperty(Runtime& runtime, const char* name, const Function& value);
  void setProperty(Runtime& runtime, const char* name, const Array& value);
  void setProperty(Runtime& runtime, const char* name, const ArrayBuffer& value);
  void setProperty(Runtime& runtime, const char* name, bool value) {
    setProperty(runtime, name, Value(value));
  }
  void setProperty(Runtime& runtime, const char* name, double value) {
    setProperty(runtime, name, Value(value));
  }

  void setProperty(Runtime& runtime, const std::string& name, const Value& value) {
    setProperty(runtime, name.c_str(), value);
  }

  void setProperty(Runtime& runtime, const Value& key, const Value& value) {
    setProperty(runtime, key.local(runtime), value);
  }

  void setProperty(Runtime& runtime, v8::Local<v8::Value> key, const Value& value) {
    v8::TryCatch tryCatch(runtime.isolate());
    if (!local(runtime)->Set(runtime.context(), key, value.local(runtime)).FromMaybe(false)) {
      throw JSError(runtime, v8engine::currentExceptionMessage(runtime.isolate(), tryCatch));
    }
  }

  bool hasProperty(Runtime& runtime, const char* name) const {
    v8::TryCatch tryCatch(runtime.isolate());
    return local(runtime)
        ->Has(runtime.context(),
              v8engine::makeV8String(runtime.isolate(), name != nullptr ? name : ""))
        .FromMaybe(false);
  }

  bool isFunction(Runtime& runtime) const { return local(runtime)->IsFunction(); }
  bool isArray(Runtime& runtime) const { return local(runtime)->IsArray(); }
  bool isArrayBuffer(Runtime& runtime) const { return local(runtime)->IsArrayBuffer(); }

  Function asFunction(Runtime& runtime) const;
  Array getArray(Runtime& runtime) const;
  ArrayBuffer getArrayBuffer(Runtime& runtime) const;
  Array getPropertyNames(Runtime& runtime) const;

  template <typename T>
  bool isHostObject(Runtime& runtime) const {
    auto holder = hostObjectHolder(runtime);
    return holder != nullptr && holder->typeToken == v8engine::hostObjectTypeToken<T>();
  }

  template <typename T>
  std::shared_ptr<T> getHostObject(Runtime& runtime) const {
    auto holder = hostObjectHolder(runtime);
    if (holder == nullptr || holder->typeToken != v8engine::hostObjectTypeToken<T>()) {
      return nullptr;
    }
    return std::static_pointer_cast<T>(holder->hostObject);
  }

  v8::Local<v8::Object> local(Runtime& runtime) const {
    if (storage_->kind == v8engine::ValueStorage::Kind::V8Borrowed) {
      return storage_->borrowedValue.As<v8::Object>();
    }
    return storage_->value.Get(runtime.isolate()).As<v8::Object>();
  }

  operator Value() const {
    return Value::fromStorage(storage_);
  }

 protected:
  friend class Value;
  friend class Runtime;
  friend class Function;
  friend class Array;
  friend class ArrayBuffer;

  explicit Object(std::shared_ptr<v8engine::ValueStorage> storage) : storage_(std::move(storage)) {}

  static Object createFromHostObjectWithToken(Runtime& runtime, std::shared_ptr<HostObject> host,
                                              const void* typeToken);

  v8engine::HostObjectHolder* hostObjectHolder(Runtime& runtime) const {
    v8::Local<v8::Object> object = local(runtime);
    if (object->InternalFieldCount() < 1) {
      return nullptr;
    }
    return static_cast<v8engine::HostObjectHolder*>(object->GetAlignedPointerFromInternalField(0));
  }

  std::shared_ptr<v8engine::ValueStorage> storage_;
};

class Function : public Object {
 public:
  Function() = default;
  explicit Function(Object object) : Object(std::move(object.storage_)) {}

  static Function createFromHostFunction(Runtime& runtime, const PropNameID& name, unsigned int,
                                         HostFunctionType callback);

  Value call(Runtime& runtime, const Value* args, size_t count) const {
    v8::TryCatch tryCatch(runtime.isolate());
    std::vector<v8::Local<v8::Value>> argv;
    argv.reserve(count);
    for (size_t i = 0; i < count; i++) {
      argv.push_back(args[i].local(runtime));
    }
    v8::Local<v8::Value> result;
    if (!local(runtime)
             .As<v8::Function>()
             ->Call(runtime.context(), runtime.context()->Global(), static_cast<int>(argv.size()),
                    argv.data())
             .ToLocal(&result)) {
      throw JSError(runtime, v8engine::currentExceptionMessage(runtime.isolate(), tryCatch));
    }
    return Value(runtime, result);
  }

  Value call(Runtime& runtime) const {
    return call(runtime, static_cast<const Value*>(nullptr), 0);
  }

  Value call(Runtime& runtime, std::nullptr_t, size_t) const {
    return call(runtime, static_cast<const Value*>(nullptr), 0);
  }

  template <size_t N>
  Value call(Runtime& runtime, const Value (&args)[N], size_t count) const {
    return call(runtime, static_cast<const Value*>(args), count);
  }

  template <typename... Args>
  Value call(Runtime& runtime, Args&&... args) const {
    Value argv[] = {Value(runtime, std::forward<Args>(args))...};
    return call(runtime, static_cast<const Value*>(argv), sizeof...(Args));
  }

  Value callWithThis(Runtime& runtime, const Object& thisObject, const Value* args = nullptr,
                     size_t count = 0) const {
    v8::TryCatch tryCatch(runtime.isolate());
    std::vector<v8::Local<v8::Value>> argv;
    argv.reserve(count);
    for (size_t i = 0; i < count; i++) {
      argv.push_back(args[i].local(runtime));
    }
    v8::Local<v8::Value> result;
    if (!local(runtime)
             .As<v8::Function>()
             ->Call(runtime.context(), thisObject.local(runtime), static_cast<int>(argv.size()),
                    argv.data())
             .ToLocal(&result)) {
      throw JSError(runtime, v8engine::currentExceptionMessage(runtime.isolate(), tryCatch));
    }
    return Value(runtime, result);
  }

  Value callAsConstructor(Runtime& runtime, const Value* args, size_t count) const {
    v8::TryCatch tryCatch(runtime.isolate());
    std::vector<v8::Local<v8::Value>> argv;
    argv.reserve(count);
    for (size_t i = 0; i < count; i++) {
      argv.push_back(args[i].local(runtime));
    }
    v8::Local<v8::Value> result;
    if (!local(runtime)
             .As<v8::Function>()
             ->NewInstance(runtime.context(), static_cast<int>(argv.size()), argv.data())
             .ToLocal(&result)) {
      throw JSError(runtime, v8engine::currentExceptionMessage(runtime.isolate(), tryCatch));
    }
    return Value(runtime, result);
  }

  Value callAsConstructor(Runtime& runtime, std::nullptr_t, size_t) const {
    return callAsConstructor(runtime, static_cast<const Value*>(nullptr), 0);
  }

  template <size_t N>
  Value callAsConstructor(Runtime& runtime, const Value (&args)[N], size_t count) const {
    return callAsConstructor(runtime, static_cast<const Value*>(args), count);
  }

  template <typename... Args>
  Value callAsConstructor(Runtime& runtime, Args&&... args) const {
    Value argv[] = {Value(runtime, std::forward<Args>(args))...};
    return callAsConstructor(runtime, static_cast<const Value*>(argv), sizeof...(Args));
  }

  operator Value() const {
    return Value::fromStorage(storage_);
  }
};

class Array : public Object {
 public:
  explicit Array(Runtime& runtime, size_t size)
      : Object(std::make_shared<v8engine::ValueStorage>(v8engine::ValueStorage::Kind::V8)) {
    storage_->value.Reset(runtime.isolate(),
                          v8::Array::New(runtime.isolate(), static_cast<int>(size)));
  }

  explicit Array(Object object) : Object(std::move(object.storage_)) {}

  size_t size(Runtime& runtime) const { return local(runtime).As<v8::Array>()->Length(); }

  Value getValueAtIndex(Runtime& runtime, size_t index) const {
    v8::TryCatch tryCatch(runtime.isolate());
    v8::Local<v8::Value> result;
    if (!local(runtime)->Get(runtime.context(), static_cast<uint32_t>(index)).ToLocal(&result)) {
      throw JSError(runtime, v8engine::currentExceptionMessage(runtime.isolate(), tryCatch));
    }
    return Value(runtime, result);
  }

  void setValueAtIndex(Runtime& runtime, size_t index, const Value& value) {
    v8::TryCatch tryCatch(runtime.isolate());
    if (!local(runtime)
             ->Set(runtime.context(), static_cast<uint32_t>(index), value.local(runtime))
             .FromMaybe(false)) {
      throw JSError(runtime, v8engine::currentExceptionMessage(runtime.isolate(), tryCatch));
    }
  }

  void setValueAtIndex(Runtime& runtime, size_t index, const String& value) {
    setValueAtIndex(runtime, index, Value(runtime, value));
  }

  operator Value() const {
    return Value::fromStorage(storage_);
  }
};

class BigInt {
 public:
  BigInt() = default;
  BigInt(Runtime& runtime, v8::Local<v8::BigInt> value)
      : storage_(std::make_shared<v8engine::ValueStorage>(v8engine::ValueStorage::Kind::V8)) {
    storage_->value.Reset(runtime.isolate(), value);
  }

  static BigInt fromInt64(Runtime& runtime, int64_t value) {
    return BigInt(runtime, v8::BigInt::New(runtime.isolate(), value));
  }

  static BigInt fromUint64(Runtime& runtime, uint64_t value) {
    return BigInt(runtime, v8::BigInt::NewFromUnsigned(runtime.isolate(), value));
  }

  String toString(Runtime& runtime, int radix) const {
    v8::TryCatch tryCatch(runtime.isolate());
    v8::Local<v8::String> result;
    (void)radix;
    if (!local(runtime)->ToString(runtime.context()).ToLocal(&result)) {
      throw JSError(runtime, v8engine::currentExceptionMessage(runtime.isolate(), tryCatch));
    }
    return String(runtime, result);
  }

  v8::Local<v8::BigInt> local(Runtime& runtime) const {
    return storage_->value.Get(runtime.isolate()).As<v8::BigInt>();
  }

  operator Value() const {
    return Value::fromStorage(storage_);
  }

 private:
  friend class Value;
  std::shared_ptr<v8engine::ValueStorage> storage_;
};

class ArrayBuffer : public Object {
 public:
  ArrayBuffer(Runtime& runtime, std::shared_ptr<MutableBuffer> buffer)
      : Object(std::make_shared<v8engine::ValueStorage>(v8engine::ValueStorage::Kind::V8)) {
    auto holder = new v8engine::ArrayBufferHolder(std::move(buffer));
    auto backingStore = v8::ArrayBuffer::NewBackingStore(
        holder->buffer->data(), holder->buffer->size(),
        [](void*, size_t, void* deleterData) {
          auto* holder = static_cast<v8engine::ArrayBufferHolder*>(deleterData);
          holder->object.Reset();
          delete holder;
        },
        holder);
    v8::Local<v8::ArrayBuffer> arrayBuffer =
        v8::ArrayBuffer::New(runtime.isolate(), std::move(backingStore));
    storage_->value.Reset(runtime.isolate(), arrayBuffer);
    holder->object.Reset(runtime.isolate(), arrayBuffer);
  }

  explicit ArrayBuffer(Object object) : Object(std::move(object.storage_)) {}

  size_t size(Runtime& runtime) const { return local(runtime).As<v8::ArrayBuffer>()->ByteLength(); }

  uint8_t* data(Runtime& runtime) const {
    auto backingStore = local(runtime).As<v8::ArrayBuffer>()->GetBackingStore();
    return static_cast<uint8_t*>(backingStore->Data());
  }

  operator Value() const {
    return Value::fromStorage(storage_);
  }
};
}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_V8

#endif  // NATIVESCRIPT_FFI_V8_NATIVE_API_V8_RUNTIME_H
