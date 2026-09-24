#ifndef NATIVESCRIPT_JSI_JSC_JSC_RUNTIME_H
#define NATIVESCRIPT_JSI_JSC_JSC_RUNTIME_H

// JavaScriptCore behind the JSI-shaped `nativescript::engine` API.
//
// This header is platform-neutral: it depends on JavaScriptCore and the C++
// standard library, and on nothing from Foundation, the Objective-C runtime,
// or the Apple metadata format. Android compiles it as-is.
//
// The Apple bridge's extra baggage lives in
// ffi/objc/jsc/NativeApiJSCRuntime.h, which includes this file. Apple sources
// keep including that path unchanged.

#ifdef TARGET_ENGINE_JSC

#include <JavaScriptCore/JSTypedArray.h>
#include <JavaScriptCore/JavaScript.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jsi/shared/RuntimeCleanupRegistry.h"

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

// Mirrors jsi::JSError. See the V8 engine layer for why the thrown value
// matters: rebuilding an error from its message drops whatever the runtime
// attached to it (NativeScriptException's `nativeException`) and its stack.
//
// Throw sites in this layer go through jscengine::toJSError, which populates
// the payload. A JSError raised by any other constructor carries no value and
// value() reports null; callers fall back to the message.
class JSError : public std::runtime_error {
 public:
  JSError(Runtime&, const std::string& message) : std::runtime_error(message) {}
  explicit JSError(const std::string& message) : std::runtime_error(message) {}

  JSError(Runtime& runtime, const std::string& message, const Value& value,
          std::string stack);

  const Value* value() const { return value_.get(); }
  const std::string& stack() const { return stack_; }

 private:
  std::shared_ptr<Value> value_;
  std::string stack_;
};

namespace jscengine {
struct ValueStorage;
}  // namespace jscengine

// A handle that does not keep its referent alive.
//
// Mirrors the V8 and Hermes engine layers. The Node-API shim needs it for
// napi_ref: a ref is strong above refcount 0 and weak at 0, and the Android
// runtime's ObjectManager depends on the weak half -- it holds every
// host-object proxy through a refcount-0 ref precisely so that collection runs
// the finalizer that calls makeInstanceWeak.
//
// Backed by the JavaScript `WeakRef` built-in rather than JSWeakCreate. JSC's
// native weak APIs (JSWeakPrivate.h, JSWeakObjectMapRefPrivate.h) live in
// private headers that the Apple JavaScriptCore.framework does not ship, and
// this header is shared with the Apple runtime; `WeakRef` is public API on
// both. The cost is a JS call in lock(), against a cached
// WeakRef.prototype.deref.
class WeakObject {
 public:
  WeakObject() = default;
  WeakObject(Runtime& runtime, const Value& value);

  // The referent, or undefined if it has been collected (or was never an
  // object).
  Value lock(Runtime& runtime) const;

  bool empty() const { return storage_ == nullptr; }
  void reset() { storage_.reset(); }

 private:
  std::shared_ptr<jscengine::ValueStorage> storage_;
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

  // Indexed access, taking the index as an integer rather than as the decimal
  // string the named form would hand over. See V8Runtime.h for the full note.
  // JSC has no indexed hook, so this layer recognises an index-shaped property
  // name and routes it here -- but only for a host object that opted in, since
  // the named path also does the prototype handling.
  virtual Value getValueAtIndex(Runtime& runtime, uint32_t index);
  virtual bool setValueAtIndex(Runtime& runtime, uint32_t index, const Value& value);

  bool hasIndexedAccess() const { return indexedAccess_; }
  void setIndexedAccess(bool value) { indexedAccess_ = value; }

  // The JS object standing for this host object, valid ONLY for the duration of
  // the call the engine is currently dispatching.
  //
  // Handed in per call rather than stored, and deliberately non-owning. A
  // HostObject that holds an owned handle to its own wrapper is a strong
  // self-cycle: JSC only finalizes a JSObjectRef once nothing references it, so
  // the cycle would keep every host object permanently alive -- the class
  // finalizer would never run, ObjectManager would never call makeInstanceWeak
  // and every Java instance would stay strongly held.
  //
  // Mirrors the V8 and Hermes engine layers; the Apple bridge does not use it,
  // so this is purely additive there.
  const Value* receiver() const { return receiver_; }

  // Sets the receiver for one dispatch and clears it on scope exit.
  class ReceiverScope {
   public:
    ReceiverScope(HostObject& host, const Value& receiver) : host_(host) {
      host_.receiver_ = &receiver;
    }
    ~ReceiverScope() { host_.receiver_ = nullptr; }
    ReceiverScope(const ReceiverScope&) = delete;
    ReceiverScope& operator=(const ReceiverScope&) = delete;

   private:
    HostObject& host_;
  };

 private:
  const Value* receiver_ = nullptr;
  bool indexedAccess_ = false;
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

// Decode strict UTF-8 into UTF-16, returning false on any malformed sequence.
//
// This used to go through NSString, which is why it was the last thing keeping
// the JSC engine layer tied to Foundation. JSStringCreateWithUTF8CString is
// not a substitute on its own: it takes a NUL-terminated C string, so it
// truncates at an embedded U+0000, which is a legal JavaScript character.
inline bool decodeUtf8ToUtf16(const std::string& value,
                              std::vector<JSChar>& out) {
  out.clear();
  out.reserve(value.size());

  const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
  const size_t size = value.size();

  for (size_t i = 0; i < size;) {
    const unsigned char lead = bytes[i];
    uint32_t codePoint = 0;
    size_t extra = 0;

    if (lead < 0x80) {
      codePoint = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      codePoint = lead & 0x1Fu;
      extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      codePoint = lead & 0x0Fu;
      extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      codePoint = lead & 0x07u;
      extra = 3;
    } else {
      return false;  // continuation byte or 5/6-byte form
    }

    if (i + extra >= size) {
      return false;  // truncated sequence (holds trivially when extra == 0)
    }

    for (size_t j = 1; j <= extra; j++) {
      const unsigned char continuation = bytes[i + j];
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }
      codePoint = (codePoint << 6) | (continuation & 0x3Fu);
    }

    // Reject overlong forms, surrogates encoded directly, and out-of-range.
    static constexpr uint32_t kMinForLength[4] = {0x0, 0x80, 0x800, 0x10000};
    if (codePoint < kMinForLength[extra] || codePoint > 0x10FFFF ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
      return false;
    }

    if (codePoint <= 0xFFFF) {
      out.push_back(static_cast<JSChar>(codePoint));
    } else {
      codePoint -= 0x10000;
      out.push_back(static_cast<JSChar>(0xD800 + (codePoint >> 10)));
      out.push_back(static_cast<JSChar>(0xDC00 + (codePoint & 0x3FF)));
    }

    i += extra + 1;
  }

  return true;
}

inline JSStringRef makeJSString(const std::string& value) {
  std::vector<JSChar> characters;
  if (!decodeUtf8ToUtf16(value, characters)) {
    // Malformed UTF-8; fall back to whatever JSC makes of it, matching the
    // behaviour of the previous NSString-returns-nil path.
    return JSStringCreateWithUTF8CString(value.c_str());
  }
  return JSStringCreateWithCharacters(characters.data(), characters.size());
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
    if (constructorClass != nullptr) {
      JSClassRelease(constructorClass);
    }
    if (nativeStateKey != nullptr) {
      JSStringRelease(nativeStateKey);
    }
    if (context != nullptr) {
      if (weakRefConstructor != nullptr) {
        JSValueUnprotect(context, weakRefConstructor);
      }
      if (weakRefDeref != nullptr) {
        JSValueUnprotect(context, weakRefDeref);
      }
    }
  }

  JSGlobalContextRef context = nullptr;
  JSClassRef hostClass = nullptr;
  JSClassRef functionClass = nullptr;
  JSClassRef selectorGroupFunctionClass = nullptr;
  // Backs Function::createFromHostConstructor. Separate from functionClass
  // because it additionally carries a callAsConstructor callback.
  JSClassRef constructorClass = nullptr;
  // `WeakRef` and `WeakRef.prototype.deref`, resolved once and protected.
  // WeakObject would otherwise do two global lookups and a prototype walk per
  // napi_get_reference_value on a weak ref, which ObjectManager runs often.
  JSObjectRef weakRefConstructor = nullptr;
  JSObjectRef weakRefDeref = nullptr;
  // The key every native-state slot hangs off, created once per runtime.
  // JSC's C API has no private-symbol or own-only property accessor, so this
  // is still a named property -- but a non-enumerable one under a cached
  // JSStringRef, and read with a single lookup instead of a has+get pair.
  JSStringRef nativeStateKey = nullptr;
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

  // Set only when this holder backs Object::setNativeState. See there: it
  // records which object the state belongs to, so a read can tell an own
  // payload from an inherited one. Never dereferenced -- compared only -- and
  // the holder is reachable solely through that object's own property, so it
  // cannot outlive it.
  JSObjectRef stateOwner = nullptr;
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

// Build a JSError that carries the thrown JS value, not just its text.
//
// Rebuilding an error from its message drops whatever was attached to it --
// NativeScriptException hangs the originating Java throwable off
// `nativeException` -- and drops its stack. Every throw site in this layer goes
// through here so a JS exception crosses the engine boundary intact.
//
// Additive: JSError already had the carrying constructor and value() simply
// reported null before. Callers that only read what() are unaffected.
JSError toJSError(Runtime& runtime, JSValueRef exception);

// Lazily resolved and cached on RuntimeState; null if the engine has no
// `WeakRef` (in which case WeakObject degrades to always-empty, which the shim
// reads as "already collected").
JSObjectRef weakRefConstructor(Runtime& runtime);
JSObjectRef weakRefDeref(Runtime& runtime);

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

  // See RuntimeState::nativeStateKey. Created on first use.
  JSStringRef nativeStateKey() const {
    if (state_->nativeStateKey == nullptr) {
      state_->nativeStateKey = JSStringCreateWithUTF8CString("__nsNativeState");
    }
    return state_->nativeStateKey;
  }

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
  // Borrowing is a V8-only capability; see jsi/v8/V8Runtime.h. Everywhere else
  // this is the owning conversion, and callers get the stronger guarantee.
  // Declared, not defined: Object is still incomplete here, so a body calling
  // asObject would not compile. Defined next to asObject in JSCValue.cpp.
  Object asObjectBorrowed(Runtime& runtime) const;
  String asString(Runtime& runtime) const;
  BigInt getBigInt(Runtime& runtime) const;

  // Portable spellings of "give me the UTF-8" and "make me a string value".
  // V8 and QuickJS implement these without materialising an owning String; JSC
  // values are protected rather than scope-rooted and its String is where the
  // protect lives, so here they are exactly the old two-step. Declared on every
  // engine so the shared bridge can call one name.
  std::string utf8(Runtime& runtime) const;
  static Value createStringFromUtf8(Runtime& runtime, const char* data, size_t length);

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

// Defined here rather than with the class: constructing the shared_ptr needs
// Value to be complete.
inline JSError::JSError(Runtime& runtime, const std::string& message,
                        const Value& value, std::string stack)
    : std::runtime_error(message),
      value_(std::make_shared<Value>(runtime, value)),
      stack_(std::move(stack)) {}

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

  // A native (Java/ObjC-backed) instance, as opposed to an opaque host object.
  //
  // On V8 the distinction is about speed: a masking named interceptor would
  // divert every named read into the trap instead of letting V8 resolve it on
  // the class prototype and form a load IC.
  //
  // On JSC it is about correctness. The class's getProperty callback runs
  // before the prototype chain is consulted, so unless the holder is marked, a
  // JS property on the prototype can never shadow a native one of the same
  // name -- the native value always wins. The mark is what lets hostGetProperty
  // defer; see shouldDeferToNativeInstancePrototype.
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
      throw jscengine::toJSError(runtime, exception);
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
      throw jscengine::toJSError(runtime, exception);
    }
    return Value(runtime, result);
  }

  // Borrowing is a V8-only capability; see jsi/v8/V8Runtime.h.
  //
  // The name exists on every engine so the Node-API shim's read paths stay
  // engine-neutral, but this engine cannot honour it: a read here hands back a
  // handle the caller must keep alive (QuickJS returns a +1 refcount, JSC needs
  // JSValueProtect, Hermes owns its jsi::Value), and a bare handle rooted only
  // by an ambient scope has no equivalent. So these are the owning reads, and
  // callers get the stronger guarantee.
  Value getPropertyBorrowed(Runtime& runtime, const char* name) const {
    return getProperty(runtime, name);
  }
  Value getPropertyBorrowed(Runtime& runtime, const Value& key) const {
    return getProperty(runtime, key);
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
      throw jscengine::toJSError(runtime, exception);
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
      throw jscengine::toJSError(runtime, exception);
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

  // ---- Native state -------------------------------------------------------
  //
  // See jsi/v8/V8Runtime.h for what this is for. JSC's C API exposes neither
  // private symbols nor an own-property accessor, so the slot is a
  // non-enumerable named property under a cached key -- one lookup where the
  // old __nsWrap path did two, and invisible to Object.keys/for-in/JSON.
  //
  // The payload is still a HostObject, so finaliser timing is unchanged.
  template <typename T>
  void setNativeState(Runtime& runtime, std::shared_ptr<T> state) {
    Object holder = Object::createFromHostObject<T>(runtime, std::move(state));
    // Stamp the holder with the object it belongs to; getNativeState uses it to
    // reject an inherited payload. See the read below for why that is needed
    // here and on no other backend.
    if (auto* record = static_cast<jscengine::HostObjectHolder*>(
            JSObjectGetPrivate(holder.local(runtime)))) {
      record->stateOwner = local(runtime);
    }
    JSValueRef exception = nullptr;
    JSObjectSetProperty(runtime.context(), local(runtime), runtime.nativeStateKey(),
                        holder.local(runtime), kJSPropertyAttributeDontEnum, &exception);
    if (exception != nullptr) {
      throw jscengine::toJSError(runtime, exception);
    }
  }

  template <typename T>
  std::shared_ptr<T> getNativeState(Runtime& runtime) const {
    JSValueRef exception = nullptr;
    JSValueRef holder = JSObjectGetProperty(runtime.context(), local(runtime),
                                            runtime.nativeStateKey(), &exception);
    if (exception != nullptr || holder == nullptr ||
        !JSValueIsObject(runtime.context(), holder)) {
      return nullptr;
    }
    auto* record = static_cast<jscengine::HostObjectHolder*>(JSObjectGetPrivate(
        reinterpret_cast<JSObjectRef>(const_cast<OpaqueJSValue*>(holder))));
    // Own-property semantics, which this backend has to enforce by hand.
    //
    // V8 keeps native state in a private symbol and QuickJS in a class-backed
    // opaque slot, so on both a read can only ever see the object's own
    // payload. The JSC C API has neither, so the state is a named property --
    // and JSObjectGetProperty walks the prototype chain. Without this check an
    // object that merely *inherits* from something carrying state reads that
    // state back as its own: every Java wrapper chains to java.lang.Object's
    // prototype, so MethodCache::GetType named every argument "java/lang/Object"
    // and Java's overload resolution collapsed onto the Object overload.
    if (record != nullptr && record->stateOwner != nullptr &&
        record->stateOwner != local(runtime)) {
      return nullptr;
    }
    if (record == nullptr || record->typeToken != jscengine::hostObjectTypeToken<T>()) {
      return nullptr;
    }
    return std::static_pointer_cast<T>(record->hostObject);
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

  // Like createFromHostFunction, but the result is usable with `new` and has a
  // *writable* own `prototype`.
  //
  // A JSObjectMake'd object of a JSClass with only a callAsFunction callback is
  // callable but not constructible, and has no `prototype` property at all --
  // so `new Ctor()` throws and MetadataNode's plain `ctor.prototype = ...`
  // assignment (napi_util::set_prototype) has nothing to overwrite. Both are
  // what napi_define_class needs.
  //
  // JSC's JSObjectCallAsConstructorCallback is handed no `this` and must return
  // an object, so the trampoline synthesises the receiver the way
  // OrdinaryCreateFromConstructor does. Like Hermes, and unlike V8, that means
  // new.target cannot be observed. No JS wrapper function is involved, so stack
  // frames are unchanged -- the Worker constructor reads frames[2] to resolve
  // its module directory.
  static Function createFromHostConstructor(Runtime& runtime, const PropNameID& name,
                                            unsigned int paramCount, HostFunctionType callback);

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
      throw jscengine::toJSError(runtime, exception);
    }
    return Value(runtime, result);
  }

  Value call(Runtime& runtime) const {
    return call(runtime, static_cast<const Value*>(nullptr), 0);
  }
  Value call(Runtime& runtime, std::nullptr_t, size_t) const {
    return call(runtime, static_cast<const Value*>(nullptr), 0);
  }
  // `count` is deduced rather than fixed to size_t on purpose.
  //
  // With a `size_t` parameter, `fn.call(rt, args, 2)` needed an int -> size_t
  // conversion here while the variadic overload below matched exactly -- so the
  // variadic won, and silently reinterpreted (array, count) as a two-argument
  // JS call passing the array and the number. The array then decayed to a
  // pointer and converted to `bool`, so console.log(str) came out as "true".
  // Deducing the count makes this overload exact too, and partial ordering then
  // prefers it over the pack. V8's backend has carried this fix since the
  // Node-API shim work; it was never propagated here.
  template <size_t N, typename Count,
            typename = std::enable_if_t<std::is_integral_v<std::decay_t<Count>>>>
  Value call(Runtime& runtime, const Value (&args)[N], Count count) const {
    return call(runtime, static_cast<const Value*>(args), static_cast<size_t>(count));
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
      throw jscengine::toJSError(runtime, exception);
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
      throw jscengine::toJSError(runtime, exception);
    }
    return Value(runtime, result);
  }
  Value callAsConstructor(Runtime& runtime, std::nullptr_t, size_t) const {
    return callAsConstructor(runtime, static_cast<const Value*>(nullptr), 0);
  }
  // `count` is deduced rather than fixed to size_t on purpose.
  //
  // With a `size_t` parameter, `fn.call(rt, args, 2)` needed an int -> size_t
  // conversion here while the variadic overload below matched exactly -- so the
  // variadic won, and silently reinterpreted (array, count) as a two-argument
  // JS call passing the array and the number. The array then decayed to a
  // pointer and converted to `bool`, so console.log(str) came out as "true".
  // Deducing the count makes this overload exact too, and partial ordering then
  // prefers it over the pack. V8's backend has carried this fix since the
  // Node-API shim work; it was never propagated here.
  template <size_t N, typename Count,
            typename = std::enable_if_t<std::is_integral_v<std::decay_t<Count>>>>
  Value callAsConstructor(Runtime& runtime, const Value (&args)[N], Count count) const {
    return callAsConstructor(runtime, static_cast<const Value*>(args), static_cast<size_t>(count));
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
      throw jscengine::toJSError(runtime, exception);
    }
    JSValueProtect(runtime.context(), storage_->value);
  }

  explicit Array(Object object) : Object(std::move(object.storage_)) {}

  size_t size(Runtime& runtime) const {
    Value length = getProperty(runtime, "length");
    return length.isNumber() ? static_cast<size_t>(std::max<double>(0, length.getNumber())) : 0;
  }

  // See Object::getPropertyBorrowed.
  Value getValueAtIndexBorrowed(Runtime& runtime, size_t index) const {
    return getValueAtIndex(runtime, index);
  }

  Value getValueAtIndex(Runtime& runtime, size_t index) const {
    JSValueRef exception = nullptr;
    JSValueRef result = JSObjectGetPropertyAtIndex(runtime.context(), local(runtime),
                                                   static_cast<unsigned>(index), &exception);
    if (exception != nullptr) {
      throw jscengine::toJSError(runtime, exception);
    }
    return Value(runtime, result);
  }

  void setValueAtIndex(Runtime& runtime, size_t index, const Value& value) {
    JSValueRef exception = nullptr;
    JSObjectSetPropertyAtIndex(runtime.context(), local(runtime), static_cast<unsigned>(index),
                               value.local(runtime), &exception);
    if (exception != nullptr) {
      throw jscengine::toJSError(runtime, exception);
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
      throw jscengine::toJSError(runtime, exception);
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
      throw jscengine::toJSError(runtime, exception);
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

#endif  // NATIVESCRIPT_JSI_JSC_JSC_RUNTIME_H
