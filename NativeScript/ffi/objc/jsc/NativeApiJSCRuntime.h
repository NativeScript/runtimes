#ifndef NATIVESCRIPT_FFI_JSC_NATIVE_API_JSC_RUNTIME_H
#define NATIVESCRIPT_FFI_JSC_NATIVE_API_JSC_RUNTIME_H

#ifdef TARGET_ENGINE_JSC

#import <Foundation/Foundation.h>
#include <JavaScriptCore/JSTypedArray.h>
#include <JavaScriptCore/JavaScript.h>
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

namespace jscengine {

inline std::string stringToUtf8(JSStringRef string) {
  if (string == nullptr) {
    return {};
  }
  size_t capacity = JSStringGetMaximumUTF8CStringSize(string);
  std::string result(capacity, '\0');
  size_t written = JSStringGetUTF8CString(string, result.data(), capacity);
  if (written == 0) {
    return {};
  }
  result.resize(written - 1);
  return result;
}

inline JSStringRef makeJSString(const std::string& value) {
  NSString* string = [[NSString alloc] initWithBytes:value.data()
                                              length:value.size()
                                            encoding:NSUTF8StringEncoding];
  if (string == nil) {
    return JSStringCreateWithUTF8CString(value.c_str());
  }

  NSUInteger length = [string length];
  std::vector<JSChar> characters(length);
  if (length > 0) {
    [string getCharacters:characters.data() range:NSMakeRange(0, length)];
  }
  [string release];
  return JSStringCreateWithCharacters(characters.data(), length);
}

inline JSStringRef makeJSString(const char* value) {
  return JSStringCreateWithUTF8CString(value != nullptr ? value : "");
}

inline std::string valueToUtf8(JSContextRef context, JSValueRef value) {
  if (value == nullptr) {
    return {};
  }
  JSValueRef exception = nullptr;
  JSStringRef string = JSValueToStringCopy(context, value, &exception);
  if (string == nullptr || exception != nullptr) {
    if (string != nullptr) {
      JSStringRelease(string);
    }
    return {};
  }
  std::string result = stringToUtf8(string);
  JSStringRelease(string);
  return result;
}

inline JSValueRef makeError(JSContextRef context, const std::string& message) {
  JSStringRef string = makeJSString(message);
  JSValueRef argument = JSValueMakeString(context, string);
  JSStringRelease(string);
  JSValueRef exception = nullptr;
  JSObjectRef error = JSObjectMakeError(context, 1, &argument, &exception);
  if (error != nullptr && exception == nullptr) {
    return error;
  }
  return argument;
}

inline void setException(JSContextRef context, JSValueRef* exception, const std::exception& error) {
  if (exception != nullptr) {
    *exception = makeError(context, error.what());
  }
}

struct RuntimeState : RuntimeCleanupRegistry {
  explicit RuntimeState(JSGlobalContextRef context) : context(context) {}

  ~RuntimeState() {
    if (hostClass != nullptr) {
      JSClassRelease(hostClass);
    }
    if (functionClass != nullptr) {
      JSClassRelease(functionClass);
    }
    if (selectorGroupFunctionClass != nullptr) {
      JSClassRelease(selectorGroupFunctionClass);
    }
  }

  JSGlobalContextRef context = nullptr;
  JSClassRef hostClass = nullptr;
  JSClassRef functionClass = nullptr;
  JSClassRef selectorGroupFunctionClass = nullptr;
};

std::shared_ptr<RuntimeState> stateForContext(JSGlobalContextRef context);
void releaseStateForContext(JSGlobalContextRef context);

struct ValueStorage {
  enum class Kind {
    Undefined,
    Null,
    Bool,
    Number,
    JSC,
    JSCBorrowed,
  };

  explicit ValueStorage(Kind kind) : kind(kind) {}

  ~ValueStorage() {
    if (kind == Kind::JSC && context != nullptr && value != nullptr) {
      JSValueUnprotect(context, value);
    }
  }

  Kind kind = Kind::Undefined;
  bool boolValue = false;
  double numberValue = 0;
  JSGlobalContextRef context = nullptr;
  JSValueRef value = nullptr;
};

template <typename T>
const void* hostObjectTypeToken() {
  static int token = 0;
  return &token;
}

struct HostObjectHolder {
  HostObjectHolder(std::shared_ptr<RuntimeState> state, std::shared_ptr<HostObject> hostObject,
                   const void* typeToken, bool nativeInstance = false)
      : state(std::move(state)),
        hostObject(std::move(hostObject)),
        typeToken(typeToken),
        nativeInstance(nativeInstance) {}

  std::shared_ptr<RuntimeState> state;
  std::shared_ptr<HostObject> hostObject;
  const void* typeToken = nullptr;
  // Set for wrappers around an ObjC instance, whose JS prototype is allowed to
  // shadow a native property. Recorded as a flag rather than derived from
  // typeToken: the wrapper class is defined inside an anonymous namespace in
  // the engine translation unit, so a type token taken anywhere else names a
  // different type and never compares equal.
  bool nativeInstance = false;
};

struct FunctionHolder {
  FunctionHolder(std::shared_ptr<RuntimeState> state, HostFunctionType callback)
      : state(std::move(state)), callback(std::move(callback)) {}

  std::shared_ptr<RuntimeState> state;
  HostFunctionType callback;
};

struct ArrayBufferHolder {
  explicit ArrayBufferHolder(std::shared_ptr<MutableBuffer> buffer) : buffer(std::move(buffer)) {}

  std::shared_ptr<MutableBuffer> buffer;
};

void setFunctionPrototype(JSGlobalContextRef context, JSObjectRef function);

}  // namespace jscengine

class Runtime {
 public:
  explicit Runtime(JSGlobalContextRef context)
      : state_(jscengine::stateForContext(context)) {}

  explicit Runtime(std::shared_ptr<jscengine::RuntimeState> state) : state_(std::move(state)) {}

  JSGlobalContextRef context() const { return state_->context; }
  std::shared_ptr<jscengine::RuntimeState> state() const { return state_; }
  const void* identity() const { return state_.get(); }
  void detachState() { state_.reset(); }

  Object global();
  Value evaluateJavaScript(std::shared_ptr<StringBuffer> buffer, const std::string& sourceURL);
  void drainMicrotasks() {}

 private:
  std::shared_ptr<jscengine::RuntimeState> state_;
};

class String {
 public:
  String() = default;
  String(Runtime& runtime, JSStringRef string);

  static String createFromUtf8(Runtime& runtime, const char* value) {
    JSStringRef string = jscengine::makeJSString(value);
    String result(runtime, string);
    JSStringRelease(string);
    return result;
  }

  static String createFromUtf8(Runtime& runtime, const std::string& value) {
    JSStringRef string = jscengine::makeJSString(value);
    String result(runtime, string);
    JSStringRelease(string);
    return result;
  }

  static String createFromUtf8(Runtime& runtime, const uint8_t* value, size_t length) {
    std::string text(reinterpret_cast<const char*>(value), length);
    return createFromUtf8(runtime, text);
  }

  std::string utf8(Runtime& runtime) const;
  JSValueRef local(Runtime& runtime) const { return storage_->value; }
  operator Value() const;

 private:
  friend class Value;
  std::shared_ptr<jscengine::ValueStorage> storage_;
};

class Value {
 public:
  Value() : kind_(jscengine::ValueStorage::Kind::Undefined) {}

  Value(bool value) : kind_(jscengine::ValueStorage::Kind::Bool), boolValue_(value) {}

  Value(double value) : kind_(jscengine::ValueStorage::Kind::Number), numberValue_(value) {}

  Value(int value) : Value(static_cast<double>(value)) {}
  Value(uint32_t value) : Value(static_cast<double>(value)) {}

  Value(Runtime& runtime, const Value& value) {
    if (value.kind_ == jscengine::ValueStorage::Kind::JSCBorrowed) {
      // Promote borrowed to owned
      storage_ = std::make_shared<jscengine::ValueStorage>(jscengine::ValueStorage::Kind::JSC);
      storage_->context = runtime.context();
      storage_->value = value.borrowedValue_ != nullptr ? value.borrowedValue_
                                                        : JSValueMakeUndefined(runtime.context());
      JSValueProtect(runtime.context(), storage_->value);
      kind_ = jscengine::ValueStorage::Kind::JSC;
      return;
    }
    kind_ = value.kind_;
    boolValue_ = value.boolValue_;
    numberValue_ = value.numberValue_;
    borrowedContext_ = value.borrowedContext_;
    borrowedValue_ = value.borrowedValue_;
    storage_ = value.storage_;
  }
  Value(Runtime& runtime, Value&& value)
      : kind_(value.kind_),
        boolValue_(value.boolValue_),
        numberValue_(value.numberValue_),
        borrowedContext_(value.borrowedContext_),
        borrowedValue_(value.borrowedValue_),
        storage_(std::move(value.storage_)) {}
  Value(Runtime& runtime, const String& value);
  Value(Runtime& runtime, const Object& object);
  Value(Runtime& runtime, const Function& function);
  Value(Runtime& runtime, const Array& array);
  Value(Runtime& runtime, const ArrayBuffer& arrayBuffer);
  Value(Runtime& runtime, const BigInt& bigint);
  Value(Runtime& runtime, JSValueRef value)
      : kind_(jscengine::ValueStorage::Kind::JSC),
        storage_(std::make_shared<jscengine::ValueStorage>(jscengine::ValueStorage::Kind::JSC)) {
    storage_->context = runtime.context();
    storage_->value = value != nullptr ? value : JSValueMakeUndefined(runtime.context());
    JSValueProtect(runtime.context(), storage_->value);
  }

  static Value borrowed(Runtime& runtime, JSValueRef value) {
    Value result;
    result.kind_ = jscengine::ValueStorage::Kind::JSCBorrowed;
    result.borrowedContext_ = runtime.context();
    result.borrowedValue_ = value != nullptr ? value : JSValueMakeUndefined(runtime.context());
    return result;
  }

  static Value undefined() { return Value(); }
  static Value null() {
    Value value;
    value.kind_ = jscengine::ValueStorage::Kind::Null;
    return value;
  }

  static bool strictEquals(Runtime& runtime, const Value& lhs,
                           const Value& rhs) {
    return JSValueIsStrictEqual(runtime.context(), lhs.local(runtime),
                                rhs.local(runtime));
  }

  bool isUndefined() const {
    return kind_ == jscengine::ValueStorage::Kind::Undefined ||
           (isJSC() && JSValueIsUndefined(jscContext(), jscValue()));
  }
  bool isNull() const {
    return kind_ == jscengine::ValueStorage::Kind::Null ||
           (isJSC() && JSValueIsNull(jscContext(), jscValue()));
  }
  bool isBool() const {
    return kind_ == jscengine::ValueStorage::Kind::Bool ||
           (isJSC() && JSValueIsBoolean(jscContext(), jscValue()));
  }
  bool getBool() const {
    if (kind_ == jscengine::ValueStorage::Kind::Bool) {
      return boolValue_;
    }
    return isJSC() && JSValueToBoolean(jscContext(), jscValue());
  }
  bool isNumber() const {
    return kind_ == jscengine::ValueStorage::Kind::Number ||
           (isJSC() && JSValueIsNumber(jscContext(), jscValue()));
  }
  double getNumber() const {
    if (kind_ == jscengine::ValueStorage::Kind::Number) {
      return numberValue_;
    }
    return isJSC() ? JSValueToNumber(jscContext(), jscValue(), nullptr) : 0;
  }

  bool isObject() const { return isJSC() && JSValueIsObject(jscContext(), jscValue()); }
  bool isString() const { return isJSC() && JSValueIsString(jscContext(), jscValue()); }
  bool isBigInt() const {
    if (!isJSC()) {
      return false;
    }
    if (__builtin_available(macOS 15.0, iOS 18.0, *)) {
      return JSValueIsBigInt(jscContext(), jscValue());
    }
    return false;
  }
  bool isSymbol() const { return isJSC() && JSValueIsSymbol(jscContext(), jscValue()); }

  Object asObject(Runtime& runtime) const;
  String asString(Runtime& runtime) const;
  BigInt getBigInt(Runtime& runtime) const;

  JSValueRef local(Runtime& runtime) const {
    switch (kind_) {
      case jscengine::ValueStorage::Kind::Undefined:
        return JSValueMakeUndefined(runtime.context());
      case jscengine::ValueStorage::Kind::Null:
        return JSValueMakeNull(runtime.context());
      case jscengine::ValueStorage::Kind::Bool:
        return JSValueMakeBoolean(runtime.context(), boolValue_);
      case jscengine::ValueStorage::Kind::Number:
        return JSValueMakeNumber(runtime.context(), numberValue_);
      case jscengine::ValueStorage::Kind::JSC:
        return storage_->value;
      case jscengine::ValueStorage::Kind::JSCBorrowed:
        return borrowedValue_;
    }
  }

  // Access the shared storage (for Object/Function/Array interop)
  std::shared_ptr<jscengine::ValueStorage> storage() const { return storage_; }

  static Value fromStorage(std::shared_ptr<jscengine::ValueStorage> s) {
    Value v;
    v.kind_ = s->kind;
    v.boolValue_ = s->boolValue;
    v.numberValue_ = s->numberValue;
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

  bool isJSC() const {
    return kind_ == jscengine::ValueStorage::Kind::JSC ||
           kind_ == jscengine::ValueStorage::Kind::JSCBorrowed;
  }
  JSContextRef jscContext() const {
    return kind_ == jscengine::ValueStorage::Kind::JSCBorrowed ? borrowedContext_
                                                               : storage_->context;
  }
  JSValueRef jscValue() const {
    return kind_ == jscengine::ValueStorage::Kind::JSCBorrowed ? borrowedValue_ : storage_->value;
  }

  jscengine::ValueStorage::Kind kind_ = jscengine::ValueStorage::Kind::Undefined;
  bool boolValue_ = false;
  double numberValue_ = 0;
  JSGlobalContextRef borrowedContext_ = nullptr;
  JSValueRef borrowedValue_ = nullptr;
  std::shared_ptr<jscengine::ValueStorage> storage_;
};

class Object {
 public:
  Object() = default;
  explicit Object(Runtime& runtime)
      : storage_(std::make_shared<jscengine::ValueStorage>(jscengine::ValueStorage::Kind::JSC)) {
    storage_->context = runtime.context();
    storage_->value = JSObjectMake(runtime.context(), nullptr, nullptr);
    JSValueProtect(runtime.context(), storage_->value);
  }

  static Object fromValueStorage(std::shared_ptr<jscengine::ValueStorage> storage) {
    Object object;
    object.storage_ = std::move(storage);
    return object;
  }

  template <typename T>
  static Object createFromHostObject(Runtime& runtime, std::shared_ptr<T> host) {
    auto baseHost = std::static_pointer_cast<HostObject>(std::move(host));
    return createFromHostObjectWithToken(runtime, std::move(baseHost),
                                         jscengine::hostObjectTypeToken<T>());
  }

  // The wrapper for an ObjC instance. Same object as createFromHostObject
  // builds, but marked so property reads defer to the JS prototype chain when
  // it carries the name -- V8 gets that from its kNonMasking template; JSC's
  // class callback runs before the prototype is consulted, so it has to ask.
  template <typename T>
  static Object createNativeInstanceHostObject(Runtime& runtime, std::shared_ptr<T> host) {
    auto baseHost = std::static_pointer_cast<HostObject>(std::move(host));
    return createFromHostObjectWithToken(runtime, std::move(baseHost),
                                         jscengine::hostObjectTypeToken<T>(),
                                         /* nativeInstance */ true);
  }

  Value getProperty(Runtime& runtime, const char* name) const {
    JSStringRef property = jscengine::makeJSString(name);
    JSValueRef exception = nullptr;
    JSValueRef result =
        JSObjectGetProperty(runtime.context(), local(runtime), property, &exception);
    JSStringRelease(property);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
    return Value(runtime, result);
  }

  Value getProperty(Runtime& runtime, const std::string& name) const {
    return getProperty(runtime, name.c_str());
  }

  Value getProperty(Runtime& runtime, const Value& key) const {
    JSValueRef exception = nullptr;
    JSValueRef result = JSObjectGetPropertyForKey(runtime.context(), local(runtime),
                                                  key.local(runtime), &exception);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
    return Value(runtime, result);
  }

  Object getPropertyAsObject(Runtime& runtime, const char* name) const {
    return getProperty(runtime, name).asObject(runtime);
  }

  Function getPropertyAsFunction(Runtime& runtime, const char* name) const;

  void setProperty(Runtime& runtime, const char* name, const Value& value) {
    JSStringRef property = jscengine::makeJSString(name);
    JSValueRef exception = nullptr;
    JSObjectSetProperty(runtime.context(), local(runtime), property, value.local(runtime),
                        kJSPropertyAttributeNone, &exception);
    JSStringRelease(property);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
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
    JSValueRef exception = nullptr;
    JSObjectSetPropertyForKey(runtime.context(), local(runtime), key.local(runtime),
                              value.local(runtime), kJSPropertyAttributeNone, &exception);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
  }

  bool hasProperty(Runtime& runtime, const char* name) const {
    JSStringRef property = jscengine::makeJSString(name);
    bool result = JSObjectHasProperty(runtime.context(), local(runtime), property);
    JSStringRelease(property);
    return result;
  }

  bool isFunction(Runtime& runtime) const {
    return JSObjectIsFunction(runtime.context(), local(runtime));
  }

  bool isArray(Runtime& runtime) const {
    JSStringRef name = jscengine::makeJSString("Array");
    JSValueRef constructorValue = JSObjectGetProperty(
        runtime.context(), JSContextGetGlobalObject(runtime.context()), name, nullptr);
    JSStringRelease(name);
    if (constructorValue == nullptr || !JSValueIsObject(runtime.context(), constructorValue)) {
      return false;
    }
    JSObjectRef constructor = JSValueToObject(runtime.context(), constructorValue, nullptr);
    JSValueRef exception = nullptr;
    bool result =
        JSValueIsInstanceOfConstructor(runtime.context(), local(runtime), constructor, &exception);
    return exception == nullptr && result;
  }

  bool isArrayBuffer(Runtime& runtime) const {
    JSValueRef exception = nullptr;
    JSTypedArrayType type =
        JSValueGetTypedArrayType(runtime.context(), storage_->value, &exception);
    return exception == nullptr && type == kJSTypedArrayTypeArrayBuffer;
  }

  Function asFunction(Runtime& runtime) const;
  Array getArray(Runtime& runtime) const;
  ArrayBuffer getArrayBuffer(Runtime& runtime) const;
  Array getPropertyNames(Runtime& runtime) const;

  template <typename T>
  bool isHostObject(Runtime& runtime) const {
    auto holder = hostObjectHolder(runtime);
    return holder != nullptr && holder->typeToken == jscengine::hostObjectTypeToken<T>();
  }

  template <typename T>
  std::shared_ptr<T> getHostObject(Runtime& runtime) const {
    auto holder = hostObjectHolder(runtime);
    if (holder == nullptr || holder->typeToken != jscengine::hostObjectTypeToken<T>()) {
      return nullptr;
    }
    return std::static_pointer_cast<T>(holder->hostObject);
  }

  JSObjectRef local(Runtime& runtime) const {
    return reinterpret_cast<JSObjectRef>(const_cast<OpaqueJSValue*>(storage_->value));
  }

  operator Value() const { return Value::fromStorage(storage_); }

 protected:
  friend class Value;
  friend class Runtime;
  friend class Function;
  friend class Array;
  friend class ArrayBuffer;

  explicit Object(std::shared_ptr<jscengine::ValueStorage> storage)
      : storage_(std::move(storage)) {}

  static Object createFromHostObjectWithToken(Runtime& runtime, std::shared_ptr<HostObject> host,
                                              const void* typeToken,
                                              bool nativeInstance = false);

  jscengine::HostObjectHolder* hostObjectHolder(Runtime& runtime) const {
    return static_cast<jscengine::HostObjectHolder*>(JSObjectGetPrivate(local(runtime)));
  }

  std::shared_ptr<jscengine::ValueStorage> storage_;
};

class Function : public Object {
 public:
  Function() = default;
  explicit Function(Object object) : Object(std::move(object.storage_)) {}

  static Function createFromHostFunction(Runtime& runtime, const PropNameID& name, unsigned int,
                                         HostFunctionType callback);

  Value call(Runtime& runtime, const Value* args, size_t count) const {
    std::vector<JSValueRef> argv;
    argv.reserve(count);
    for (size_t i = 0; i < count; i++) {
      argv.push_back(args[i].local(runtime));
    }
    JSValueRef exception = nullptr;
    JSValueRef result = JSObjectCallAsFunction(
        runtime.context(), local(runtime), JSContextGetGlobalObject(runtime.context()), argv.size(),
        argv.empty() ? nullptr : argv.data(), &exception);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
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
    std::vector<JSValueRef> argv;
    argv.reserve(count);
    for (size_t i = 0; i < count; i++) {
      argv.push_back(args[i].local(runtime));
    }
    JSValueRef exception = nullptr;
    JSValueRef result =
        JSObjectCallAsFunction(runtime.context(), local(runtime), thisObject.local(runtime),
                               argv.size(), argv.empty() ? nullptr : argv.data(), &exception);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
    return Value(runtime, result);
  }

  Value callAsConstructor(Runtime& runtime, const Value* args, size_t count) const {
    std::vector<JSValueRef> argv;
    argv.reserve(count);
    for (size_t i = 0; i < count; i++) {
      argv.push_back(args[i].local(runtime));
    }
    JSValueRef exception = nullptr;
    JSValueRef result = JSObjectCallAsConstructor(runtime.context(), local(runtime), argv.size(),
                                                  argv.empty() ? nullptr : argv.data(), &exception);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
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
};

class Array : public Object {
 public:
  explicit Array(Runtime& runtime, size_t size)
      : Object(std::make_shared<jscengine::ValueStorage>(jscengine::ValueStorage::Kind::JSC)) {
    std::vector<JSValueRef> initial(size, JSValueMakeUndefined(runtime.context()));
    JSValueRef exception = nullptr;
    storage_->context = runtime.context();
    storage_->value =
        JSObjectMakeArray(runtime.context(), initial.size(), initial.data(), &exception);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
    JSValueProtect(runtime.context(), storage_->value);
  }

  explicit Array(Object object) : Object(std::move(object.storage_)) {}

  size_t size(Runtime& runtime) const {
    Value length = getProperty(runtime, "length");
    return length.isNumber() ? static_cast<size_t>(std::max<double>(0, length.getNumber())) : 0;
  }

  Value getValueAtIndex(Runtime& runtime, size_t index) const {
    JSValueRef exception = nullptr;
    JSValueRef result = JSObjectGetPropertyAtIndex(runtime.context(), local(runtime),
                                                   static_cast<unsigned>(index), &exception);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
    return Value(runtime, result);
  }

  void setValueAtIndex(Runtime& runtime, size_t index, const Value& value) {
    JSValueRef exception = nullptr;
    JSObjectSetPropertyAtIndex(runtime.context(), local(runtime), static_cast<unsigned>(index),
                               value.local(runtime), &exception);
    if (exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
  }
  void setValueAtIndex(Runtime& runtime, size_t index, const String& value) {
    setValueAtIndex(runtime, index, Value(runtime, value));
  }
};

class BigInt {
 public:
  BigInt() = default;
  BigInt(Runtime& runtime, JSValueRef value)
      : storage_(std::make_shared<jscengine::ValueStorage>(jscengine::ValueStorage::Kind::JSC)) {
    storage_->context = runtime.context();
    storage_->value = value;
    JSValueProtect(runtime.context(), storage_->value);
  }

  static BigInt fromInt64(Runtime& runtime, int64_t value) {
    JSValueRef exception = nullptr;
    JSValueRef result = nullptr;
    if (__builtin_available(macOS 15.0, iOS 18.0, *)) {
      result = JSBigIntCreateWithInt64(runtime.context(), value, &exception);
    }
    if (result == nullptr || exception != nullptr) {
      result = JSValueMakeNumber(runtime.context(), static_cast<double>(value));
    }
    return BigInt(runtime, result);
  }

  static BigInt fromUint64(Runtime& runtime, uint64_t value) {
    JSValueRef exception = nullptr;
    JSValueRef result = nullptr;
    if (__builtin_available(macOS 15.0, iOS 18.0, *)) {
      result = JSBigIntCreateWithUInt64(runtime.context(), value, &exception);
    }
    if (result == nullptr || exception != nullptr) {
      result = JSValueMakeNumber(runtime.context(), static_cast<double>(value));
    }
    return BigInt(runtime, result);
  }

  String toString(Runtime& runtime, int) const {
    JSValueRef exception = nullptr;
    JSStringRef string = JSValueToStringCopy(runtime.context(), local(runtime), &exception);
    if (string == nullptr || exception != nullptr) {
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
    String result(runtime, string);
    JSStringRelease(string);
    return result;
  }

  JSValueRef local(Runtime& runtime) const { return storage_->value; }

  operator Value() const { return Value::fromStorage(storage_); }

 private:
  friend class Value;
  std::shared_ptr<jscengine::ValueStorage> storage_;
};

class ArrayBuffer : public Object {
 public:
  ArrayBuffer(Runtime& runtime, std::shared_ptr<MutableBuffer> buffer)
      : Object(std::make_shared<jscengine::ValueStorage>(jscengine::ValueStorage::Kind::JSC)) {
    auto* holder = new jscengine::ArrayBufferHolder(std::move(buffer));
    JSValueRef exception = nullptr;
    storage_->context = runtime.context();
    storage_->value = JSObjectMakeArrayBufferWithBytesNoCopy(
        runtime.context(), holder->buffer->data(), holder->buffer->size(),
        [](void*, void* deallocatorContext) {
          delete static_cast<jscengine::ArrayBufferHolder*>(deallocatorContext);
        },
        holder, &exception);
    if (exception != nullptr) {
      delete holder;
      throw JSError(runtime, jscengine::valueToUtf8(runtime.context(), exception));
    }
    JSValueProtect(runtime.context(), storage_->value);
  }

  explicit ArrayBuffer(Object object) : Object(std::move(object.storage_)) {}

  size_t size(Runtime& runtime) const {
    JSValueRef exception = nullptr;
    return JSObjectGetArrayBufferByteLength(runtime.context(), local(runtime), &exception);
  }

  uint8_t* data(Runtime& runtime) const {
    JSValueRef exception = nullptr;
    return static_cast<uint8_t*>(
        JSObjectGetArrayBufferBytesPtr(runtime.context(), local(runtime), &exception));
  }
};
}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_JSC

#endif  // NATIVESCRIPT_FFI_JSC_NATIVE_API_JSC_RUNTIME_H
