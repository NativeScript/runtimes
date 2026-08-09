#ifndef NS_JSI_HERMES_HERMES_RUNTIME_H
#define NS_JSI_HERMES_HERMES_RUNTIME_H

// nativescript::engine for Hermes.
//
// nativescript::engine was modelled on facebook::jsi, and Hermes speaks the
// real thing, so this file is deliberately much smaller than
// jsi/v8/V8Runtime.h. It is not, however, a set of `using` declarations, and
// the reason is one property jsi does not have:
//
//   jsi::Value and jsi::Object are MOVE-ONLY. Copying one needs the Runtime
//   (Runtime::cloneObject and friends), because a jsi handle is an owned
//   PointerValue rather than a refcounted cell.
//
// nativescript::engine's Value is copyable -- V8's layer implements it as a
// shared_ptr<ValueStorage> -- and the shim leans on that everywhere: it stores
// Values in std::vector, assigns them into napi_ref__ and CallbackInfo, and
// converts Object -> Value implicitly. Aliasing the jsi types directly would
// require rewriting those ~40 call sites in shared code, which is exactly the
// code the V8 path also compiles.
//
// So the shape here is the same one V8Runtime.h uses: a refcounted storage cell
// holding the engine's own handle, with the engine::* types as thin copyable
// front ends over it. Everything that touches JS forwards straight to jsi.
//
// Two other differences are worth naming, because they are the only real logic
// in this file:
//
//   1. Exceptions. jsi throws facebook::jsi::JSError; the shim catches
//      nativescript::engine::JSError. Neither can catch the other, so every
//      operation that can throw is wrapped and translated (see `guard`), and
//      the host-function/host-object adapters translate back on the way out.
//
//   2. HostObject::receiver(). jsi's HostObject::get/set are handed no
//      receiver, so the adapter keeps a jsi::WeakObject to the wrapper it was
//      installed on and locks it for the duration of each dispatch. It must be
//      weak: an owned handle from a host object to its own JS wrapper is a
//      strong self-cycle, the weak finaliser never runs, and ObjectManager
//      never calls makeInstanceWeak.
//
// Borrowed values: jsi hands host functions `const jsi::Value*` that is valid
// for the duration of the call. A borrowed engine::Value points straight at it
// and allocates nothing, mirroring V8's Kind::V8Borrowed. Value(Runtime&,
// const Value&) promotes a borrowed value to an owned one, which is what makes
// napi_create_reference and friends safe on a callback argument.

#include <jsi/jsi.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nativescript {
namespace engine {

class Runtime;
class Value;
class Object;
class Function;
class Array;
class String;
class ArrayBuffer;
class PropNameID;
class HostObject;

using StringBuffer = ::facebook::jsi::StringBuffer;
using MutableBuffer = ::facebook::jsi::MutableBuffer;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

// Mirrors jsi/v8/V8Runtime.h's JSError exactly, including the optional carried
// value -- ShimTypes.h's exceptionFrom() reads value() and expects a pointer
// that may be null.
class JSError : public std::runtime_error {
 public:
  JSError(Runtime&, const std::string& message) : std::runtime_error(message) {}
  explicit JSError(const std::string& message) : std::runtime_error(message) {}

  // Defined after Value.
  JSError(Runtime& runtime, const std::string& message, const Value& value,
          std::string stack);

  const Value* value() const { return value_.get(); }
  const std::string& stack() const { return stack_; }

 private:
  std::shared_ptr<Value> value_;
  std::string stack_;
};

namespace hermesengine {

namespace jsi = ::facebook::jsi;

// One owned jsi handle, shared by every engine::Value/Object/String that refers
// to it. The typed views are materialised at most once: jsi's getObject() and
// getString() clone, and the shim asks for them repeatedly on the same value
// (asObject(env, x).getProperty(...) runs on every property read).
struct ValueStorage {
  jsi::Value value;

  explicit ValueStorage(jsi::Value v) : value(std::move(v)) {}

  jsi::Object& object(jsi::Runtime& rt) {
    if (!object_.has_value()) {
      object_.emplace(value.getObject(rt));
    }
    return *object_;
  }

  jsi::String& string(jsi::Runtime& rt) {
    if (!string_.has_value()) {
      string_.emplace(value.getString(rt));
    }
    return *string_;
  }

 private:
  std::optional<jsi::Object> object_;
  std::optional<jsi::String> string_;
};

using StoragePtr = std::shared_ptr<ValueStorage>;

inline StoragePtr makeStorage(jsi::Value value) {
  return std::make_shared<ValueStorage>(std::move(value));
}

// A distinct address per host-object type, so isHostObject<T> can answer
// without RTTI on the engine::HostObject hierarchy (the adapter below is the
// only jsi::HostObject there is, so dynamic_cast cannot distinguish them).
template <typename T>
inline const void* hostObjectTypeToken() {
  static const char token = 0;
  return &token;
}

}  // namespace hermesengine

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

class Runtime {
 public:
  explicit Runtime(std::unique_ptr<::facebook::jsi::Runtime> runtime)
      : owned_(std::move(runtime)), runtime_(owned_.get()) {}

  // Non-owning. Hermes is entered from Java threads (JNI callbacks, the worker
  // pool, the concurrency specs' five background threads) and its VM captures
  // per-thread stack bounds, so the runtime must be wrapped in a
  // jsi::ThreadSafeRuntime whose lock()/unlock() register the calling thread.
  // The wrapper lives in the JSR holder and does the locking at the js_lock_env
  // boundary; the work itself runs against getUnsafeRuntime(), which is exactly
  // what napi/hermes/jsr.cpp does. Decorating every call instead would lock
  // per jsi operation and make the benchmark measure the decorator.
  explicit Runtime(::facebook::jsi::Runtime& runtime) : runtime_(&runtime) {}

  ::facebook::jsi::Runtime& jsi() const { return *runtime_; }

  // A stable, per-runtime identity.
  //
  // engine::Runtime is a value wrapper around shared engine state, and the
  // host-function trampolines construct a fresh one on the stack for every
  // callback. So `&runtime` is NOT stable and must never be used as a map key;
  // this is. The pointer is opaque and only ever compared or hashed.
  const void* identity() const { return runtime_; }

  Object global();

  Value evaluateJavaScript(std::shared_ptr<StringBuffer> buffer,
                           const std::string& sourceURL);

  void drainMicrotasks() { runtime_->drainMicrotasks(); }

 private:
  std::unique_ptr<::facebook::jsi::Runtime> owned_;
  ::facebook::jsi::Runtime* runtime_ = nullptr;
};

namespace hermesengine {

// Translates a jsi exception into the engine's. Every entry point that can
// reach the VM goes through this; the shim's NS_LEAVE only knows
// engine::JSError, and a jsi::JSError escaping it would be reported as
// napi_generic_failure with the JS exception discarded.
//
// Defined after Value/Runtime are complete; see the bottom of this file.
template <typename F>
auto guard(Runtime& runtime, F&& body) -> decltype(body());

}  // namespace hermesengine

// ---------------------------------------------------------------------------
// PropNameID
// ---------------------------------------------------------------------------

// Borrows the engine's own name handle when the engine supplied one (a host
// object interception), and converts to UTF-8 only if someone asks. Names built
// by getPropertyNames carry a string instead and have no handle.
class PropNameID {
 public:
  PropNameID() = default;
  explicit PropNameID(std::string value)
      : value_(std::move(value)), hasUtf8_(true) {}

  PropNameID(::facebook::jsi::Runtime& runtime,
             const ::facebook::jsi::PropNameID& name)
      : runtime_(&runtime), name_(&name) {}

  static PropNameID forAscii(Runtime&, const char* value) {
    return PropNameID(value != nullptr ? std::string(value) : std::string());
  }

  static PropNameID forAscii(Runtime&, const std::string& value) {
    return PropNameID(value);
  }

  std::string utf8(Runtime&) const { return utf8(); }

  std::string utf8() const {
    if (!hasUtf8_) {
      if (runtime_ != nullptr && name_ != nullptr) {
        value_ = name_->utf8(*runtime_);
      }
      hasUtf8_ = true;
    }
    return value_;
  }

 private:
  ::facebook::jsi::Runtime* runtime_ = nullptr;
  const ::facebook::jsi::PropNameID* name_ = nullptr;
  mutable std::string value_;
  mutable bool hasUtf8_ = false;
};

// ---------------------------------------------------------------------------
// HostObject
// ---------------------------------------------------------------------------

class HostObject {
 public:
  virtual ~HostObject() = default;
  virtual Value get(Runtime& runtime, const PropNameID& name);
  virtual bool set(Runtime& runtime, const PropNameID& name, const Value& value);
  virtual std::vector<PropNameID> getPropertyNames(Runtime& runtime);

  // Indexed access, taking the index as an integer rather than as the decimal
  // string the named form would hand over. See V8Runtime.h for the full note.
  // jsi has no indexed hook and no way to read a PropNameID without building a
  // std::string, so nothing here calls these -- an index keeps arriving through
  // get/set as a name. They exist so a HostObject that overrides them still
  // compiles and behaves identically against Hermes.
  virtual Value getValueAtIndex(Runtime& runtime, uint32_t index);
  virtual bool setValueAtIndex(Runtime& runtime, uint32_t index, const Value& value);

  bool hasIndexedAccess() const { return indexedAccess_; }
  void setIndexedAccess(bool value) { indexedAccess_ = value; }

  // The JS object standing for this host object, valid ONLY for the duration of
  // the call currently being dispatched. Non-owning on purpose: an owned handle
  // to its own wrapper is a strong self-cycle that stops the finalizer running.
  const Value* receiver() const { return receiver_; }

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

using HostFunctionType =
    std::function<Value(Runtime&, const Value&, const Value*, size_t)>;

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

class Value {
 public:
  enum class Kind : uint8_t { Undefined, Null, Bool, Number, Owned, Borrowed };

  Value() = default;
  Value(bool value) : kind_(Kind::Bool), bool_(value) {}
  Value(double value) : kind_(Kind::Number), number_(value) {}
  Value(int value) : Value(static_cast<double>(value)) {}
  Value(uint32_t value) : Value(static_cast<double>(value)) {}

  // Value("foo") must not silently become a bool.
  template <typename T = void>
  Value(const char*) {
    static_assert(!std::is_same<void, T>::value,
                  "Value cannot be constructed from const char*");
  }

  static Value undefined() { return Value(); }
  static Value null() {
    Value v;
    v.kind_ = Kind::Null;
    return v;
  }

  // Explicit copy. Promotes a borrowed value to an owned one: a borrowed handle
  // is only valid for the dispatch that produced it, and this is the
  // constructor every call site that outlives the call already uses
  // (napi_create_reference, napi_throw, CallbackInfo storage).
  Value(Runtime& runtime, const Value& other) {
    if (other.kind_ == Kind::Borrowed) {
      kind_ = Kind::Owned;
      storage_ = hermesengine::makeStorage(
          ::facebook::jsi::Value(runtime.jsi(), *other.borrowed_));
      return;
    }
    kind_ = other.kind_;
    bool_ = other.bool_;
    number_ = other.number_;
    storage_ = other.storage_;
    borrowed_ = other.borrowed_;
  }

  Value(Runtime&, const Object& object);
  Value(Runtime&, const String& string);

  // Implicit copies share storage; they do NOT promote. Same contract as the
  // V8 layer.
  Value(const Value&) = default;
  Value& operator=(const Value&) = default;
  Value(Value&&) noexcept = default;
  Value& operator=(Value&&) noexcept = default;

  // Points at a jsi::Value the caller owns and outlives this. Allocates
  // nothing. Used for host-function arguments and host-object receivers.
  static Value borrowed(const ::facebook::jsi::Value& value) {
    Value result;
    result.kind_ = Kind::Borrowed;
    result.borrowed_ = &value;
    return result;
  }

  static Value fromStorage(hermesengine::StoragePtr storage) {
    Value result;
    if (storage == nullptr) return result;  // undefined
    result.kind_ = Kind::Owned;
    result.storage_ = std::move(storage);
    return result;
  }

  bool isUndefined() const {
    return kind_ == Kind::Undefined ||
           (isPointer() && jsiRef().isUndefined());
  }
  bool isNull() const {
    return kind_ == Kind::Null || (isPointer() && jsiRef().isNull());
  }
  bool isBool() const {
    return kind_ == Kind::Bool || (isPointer() && jsiRef().isBool());
  }
  bool isNumber() const {
    return kind_ == Kind::Number || (isPointer() && jsiRef().isNumber());
  }
  bool isString() const { return isPointer() && jsiRef().isString(); }
  bool isSymbol() const { return isPointer() && jsiRef().isSymbol(); }
  bool isBigInt() const { return isPointer() && jsiRef().isBigInt(); }
  bool isObject() const { return isPointer() && jsiRef().isObject(); }

  bool getBool() const {
    return kind_ == Kind::Bool ? bool_ : jsiRef().getBool();
  }
  double getNumber() const {
    return kind_ == Kind::Number ? number_ : jsiRef().getNumber();
  }

  Object asObject(Runtime& runtime) const;
  // Borrowing is a V8-only capability; see jsi/v8/V8Runtime.h. Everywhere else
  // this is the owning conversion, and callers get the stronger guarantee.
  // Declared, not defined: Object is still incomplete here, so a body calling
  // asObject would not compile. Defined next to asObject further down.
  Object asObjectBorrowed(Runtime& runtime) const;
  String asString(Runtime& runtime) const;

  // Portable spellings of "give me the UTF-8" and "make me a string value".
  // V8 and QuickJS implement these without materialising an owning String;
  // Hermes values are jsi handles all the way down and String is a view onto
  // the same storage, so here they are the old two-step. Declared on every
  // engine so the shared bridge can call one name.
  std::string utf8(Runtime& runtime) const;
  static Value createStringFromUtf8(Runtime& runtime, const char* data, size_t length);

  // The jsi handle behind this value, materialising one for the inline scalar
  // kinds. Returned by value because the scalar kinds have no handle to refer
  // to.
  ::facebook::jsi::Value toJsi(Runtime& runtime) const {
    switch (kind_) {
      case Kind::Undefined:
        return ::facebook::jsi::Value::undefined();
      case Kind::Null:
        return ::facebook::jsi::Value::null();
      case Kind::Bool:
        return ::facebook::jsi::Value(bool_);
      case Kind::Number:
        return ::facebook::jsi::Value(number_);
      case Kind::Borrowed:
        return ::facebook::jsi::Value(runtime.jsi(), *borrowed_);
      case Kind::Owned:
        return storage_ != nullptr
                   ? ::facebook::jsi::Value(runtime.jsi(), storage_->value)
                   : ::facebook::jsi::Value::undefined();
    }
    return ::facebook::jsi::Value::undefined();
  }

  bool isPointer() const {
    return kind_ == Kind::Owned || kind_ == Kind::Borrowed;
  }

  // Only valid when isPointer().
  const ::facebook::jsi::Value& jsiRef() const {
    return kind_ == Kind::Borrowed ? *borrowed_ : storage_->value;
  }

  Kind kind() const { return kind_; }

 private:
  friend class Object;
  friend class String;

  // Storage for a pointer-kind value, materialising one for a borrowed value.
  // Sharing rather than cloning is what makes asObject cheap on repeat.
  hermesengine::StoragePtr sharedStorage(Runtime& runtime) const;

  Kind kind_ = Kind::Undefined;
  bool bool_ = false;
  double number_ = 0;
  const ::facebook::jsi::Value* borrowed_ = nullptr;
  // Mutable because sharedStorage() memoises the promotion of a borrowed value
  // (see its definition). Never read except through sharedStorage(): jsiRef()
  // answers a borrowed value from borrowed_ whether or not the cache is warm,
  // so the memo changes no observable behaviour.
  mutable hermesengine::StoragePtr storage_;
};

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------

class String {
 public:
  String() = default;

  static String createFromUtf8(Runtime& runtime, const char* value);
  static String createFromUtf8(Runtime& runtime, const std::string& value);
  static String createFromUtf8(Runtime& runtime, const uint8_t* value,
                               size_t length);

  std::string utf8(Runtime& runtime) const;

  operator Value() const { return Value::fromStorage(storage_); }

  static String fromStorage(hermesengine::StoragePtr storage) {
    String result;
    result.storage_ = std::move(storage);
    return result;
  }

 private:
  friend class Value;
  hermesengine::StoragePtr storage_;
};

// ---------------------------------------------------------------------------
// Object
// ---------------------------------------------------------------------------

class Object {
 public:
  Object() = default;
  explicit Object(Runtime& runtime);

  static Object fromStorage(hermesengine::StoragePtr storage) {
    Object result;
    result.storage_ = std::move(storage);
    return result;
  }

  template <typename T>
  static Object createFromHostObject(Runtime& runtime, std::shared_ptr<T> host) {
    auto base = std::static_pointer_cast<HostObject>(std::move(host));
    return createFromHostObjectWithToken(
        runtime, std::move(base), hermesengine::hostObjectTypeToken<T>());
  }

  // A native (Java/ObjC-backed) instance, as opposed to an opaque host object.
  //
  // Only V8 distinguishes a masking from a non-masking named interceptor, and
  // there the difference is large: a native instance carries the class
  // prototype where the field accessors live, so a masking interceptor would
  // divert every named read into the trap instead of letting V8 resolve it (and
  // form a load IC) on the prototype. This backend has no such distinction, so
  // a native instance is built exactly like any other host object; the separate
  // name exists so callers can express the intent once, for every engine.
  template <typename T>
  static Object createNativeInstanceHostObject(Runtime& runtime, std::shared_ptr<T> host) {
    return createFromHostObject<T>(runtime, std::move(host));
  }

  Value getProperty(Runtime& runtime, const char* name) const;
  Value getProperty(Runtime& runtime, const std::string& name) const {
    return getProperty(runtime, name.c_str());
  }
  Value getProperty(Runtime& runtime, const Value& key) const;

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

  Object getPropertyAsObject(Runtime& runtime, const char* name) const;
  Function getPropertyAsFunction(Runtime& runtime, const char* name) const;

  void setProperty(Runtime& runtime, const char* name, const Value& value);
  void setProperty(Runtime& runtime, const char* name, const Object& value);
  void setProperty(Runtime& runtime, const char* name, const String& value);
  void setProperty(Runtime& runtime, const char* name, bool value) {
    setProperty(runtime, name, Value(value));
  }
  void setProperty(Runtime& runtime, const char* name, double value) {
    setProperty(runtime, name, Value(value));
  }
  void setProperty(Runtime& runtime, const std::string& name,
                   const Value& value) {
    setProperty(runtime, name.c_str(), value);
  }
  void setProperty(Runtime& runtime, const Value& key, const Value& value);

  bool hasProperty(Runtime& runtime, const char* name) const;

  bool isFunction(Runtime& runtime) const;
  bool isArray(Runtime& runtime) const;
  bool isArrayBuffer(Runtime& runtime) const;

  Function asFunction(Runtime& runtime) const;
  Array getArray(Runtime& runtime) const;
  ArrayBuffer getArrayBuffer(Runtime& runtime) const;
  Array getPropertyNames(Runtime& runtime) const;

  template <typename T>
  bool isHostObject(Runtime& runtime) const {
    return hostObjectOf(runtime, hermesengine::hostObjectTypeToken<T>()) !=
           nullptr;
  }

  template <typename T>
  std::shared_ptr<T> getHostObject(Runtime& runtime) const {
    std::shared_ptr<HostObject> host =
        hostObjectOf(runtime, hermesengine::hostObjectTypeToken<T>());
    if (host == nullptr) return nullptr;
    return std::static_pointer_cast<T>(std::move(host));
  }

  // ---- Native state -------------------------------------------------------
  //
  // This is the one engine where the facility is native rather than emulated:
  // jsi::Object::setNativeState is an internal slot on the object, not a
  // property, so reading it back does no name lookup at all. See
  // jsi/v8/V8Runtime.h for why the Node-API shim needs it -- napi_unwrap runs
  // on every marshalled field access and used to cost two prototype-chain
  // walks for `__nsWrap`, which on Hermes could not be avoided the way V8's
  // non-masking host-object template avoids it (jsi's HostObject intercepts
  // every property with no fallback).
  template <typename T>
  void setNativeState(Runtime& runtime, std::shared_ptr<T> state) {
    setNativeStateWithToken(runtime,
                            std::static_pointer_cast<HostObject>(std::move(state)),
                            hermesengine::hostObjectTypeToken<T>());
  }

  template <typename T>
  std::shared_ptr<T> getNativeState(Runtime& runtime) const {
    std::shared_ptr<HostObject> host =
        nativeStateOf(runtime, hermesengine::hostObjectTypeToken<T>());
    if (host == nullptr) return nullptr;
    return std::static_pointer_cast<T>(std::move(host));
  }

  operator Value() const { return Value::fromStorage(storage_); }

  ::facebook::jsi::Object& jsiObject(Runtime& runtime) const;

  const hermesengine::StoragePtr& storage() const { return storage_; }

 protected:
  friend class Value;
  friend class Function;
  friend class Array;
  friend class ArrayBuffer;

  static Object createFromHostObjectWithToken(Runtime& runtime,
                                              std::shared_ptr<HostObject> host,
                                              const void* typeToken);

  std::shared_ptr<HostObject> hostObjectOf(Runtime& runtime,
                                           const void* typeToken) const;

  void setNativeStateWithToken(Runtime& runtime,
                               std::shared_ptr<HostObject> host,
                               const void* typeToken);
  std::shared_ptr<HostObject> nativeStateOf(Runtime& runtime,
                                            const void* typeToken) const;

  hermesengine::StoragePtr storage_;
};

// ---------------------------------------------------------------------------
// Function
// ---------------------------------------------------------------------------

class Function : public Object {
 public:
  Function() = default;
  explicit Function(Object object) : Object(std::move(object)) {}

  static Function createFromHostFunction(Runtime& runtime,
                                         const PropNameID& name,
                                         unsigned int paramCount,
                                         HostFunctionType callback);

  // Like createFromHostFunction, but the result is usable with `new` and its
  // `prototype` is a writable own property. See the definition for what Hermes
  // requires here and why.
  static Function createFromHostConstructor(Runtime& runtime,
                                            const PropNameID& name,
                                            unsigned int paramCount,
                                            HostFunctionType callback);

  Value call(Runtime& runtime, const Value* args = nullptr,
             size_t count = 0) const;
  Value callWithThis(Runtime& runtime, const Object& thisObject,
                     const Value* args = nullptr, size_t count = 0) const;
  Value callAsConstructor(Runtime& runtime, const Value* args = nullptr,
                          size_t count = 0) const;

  operator Value() const { return Value::fromStorage(storage_); }
};

// ---------------------------------------------------------------------------
// Array / ArrayBuffer
// ---------------------------------------------------------------------------

class Array : public Object {
 public:
  Array() = default;
  Array(Runtime& runtime, size_t size);
  explicit Array(Object object) : Object(std::move(object)) {}

  size_t size(Runtime& runtime) const;
  Value getValueAtIndex(Runtime& runtime, size_t index) const;

  // See Object::getPropertyBorrowed.
  Value getValueAtIndexBorrowed(Runtime& runtime, size_t index) const {
    return getValueAtIndex(runtime, index);
  }

  void setValueAtIndex(Runtime& runtime, size_t index, const Value& value);

  operator Value() const { return Value::fromStorage(storage_); }
};

class ArrayBuffer : public Object {
 public:
  ArrayBuffer() = default;
  ArrayBuffer(Runtime& runtime, std::shared_ptr<MutableBuffer> buffer);
  explicit ArrayBuffer(Object object) : Object(std::move(object)) {}

  uint8_t* data(Runtime& runtime) const;
  size_t size(Runtime& runtime) const;

  operator Value() const { return Value::fromStorage(storage_); }
};

// ---------------------------------------------------------------------------
// WeakObject
// ---------------------------------------------------------------------------

class WeakObject {
 public:
  WeakObject() = default;
  WeakObject(Runtime& runtime, const Value& value);

  Value lock(Runtime& runtime) const;
  bool empty() const { return weak_ == nullptr && strong_ == nullptr; }
  void reset() {
    weak_.reset();
    strong_.reset();
  }

 private:
  std::shared_ptr<::facebook::jsi::WeakObject> weak_;
  // Non-object values have no weak form; keeping them strong matches what the
  // shim asks for (a reference that still reads back) and never pins a wrapper,
  // because only objects have finalizers.
  hermesengine::StoragePtr strong_;
};

// ===========================================================================
// Definitions
// ===========================================================================

namespace hermesengine {

// The jsi::HostObject actually installed on the JS object. Owns the
// engine::HostObject, carries its type token, and keeps a weak handle to the
// wrapper so receiver() can be answered per dispatch.
class HostObjectAdapter final : public jsi::HostObject {
 public:
  HostObjectAdapter(Runtime* runtime, std::shared_ptr<engine::HostObject> host,
                    const void* typeToken)
      : runtime_(runtime), host_(std::move(host)), typeToken_(typeToken) {}

  const void* typeToken() const { return typeToken_; }
  const std::shared_ptr<engine::HostObject>& host() const { return host_; }

  void attach(jsi::Runtime& rt, const jsi::Object& self) {
    weakSelf_.emplace(rt, self);
  }

  jsi::Value get(jsi::Runtime& rt, const jsi::PropNameID& name) override;
  void set(jsi::Runtime& rt, const jsi::PropNameID& name,
           const jsi::Value& value) override;
  std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime& rt) override;

 private:
  // Locks the wrapper for one dispatch. Empty (undefined) if it has already
  // been collected, which cannot normally happen while a property access on it
  // is in flight.
  jsi::Value self(jsi::Runtime& rt) const {
    if (!weakSelf_.has_value()) return jsi::Value::undefined();
    return weakSelf_->lock(rt);
  }

  Runtime* runtime_;
  std::shared_ptr<engine::HostObject> host_;
  const void* typeToken_;
  std::optional<jsi::WeakObject> weakSelf_;
};

// Translates an engine::JSError back into a jsi::JSError, preserving the thrown
// value when there is one. Without this, a napi_callback that failed would
// surface in JS as a plain std::exception -> "Error: <what()>", losing the
// error's type and everything the runtime attached to it.
[[noreturn]] inline void rethrowAsJsi(Runtime& runtime,
                                      const engine::JSError& error) {
  if (const engine::Value* thrown = error.value()) {
    throw jsi::JSError(runtime.jsi(), thrown->toJsi(runtime));
  }
  throw jsi::JSError(runtime.jsi(), std::string(error.what()));
}

template <typename F>
auto guard(Runtime& runtime, F&& body) -> decltype(body()) {
  try {
    return body();
  } catch (const jsi::JSError& error) {
    // The thrown value is carried through, exactly as the V8 layer does, so
    // ShimTypes.h's exceptionFrom() can hand JS the original object rather than
    // a rebuilt one.
    engine::Value value = engine::Value::fromStorage(
        makeStorage(jsi::Value(runtime.jsi(), error.value())));
    throw engine::JSError(runtime, error.getMessage(), value, error.getStack());
  } catch (const jsi::JSIException& error) {
    throw engine::JSError(runtime, std::string(error.what()));
  }
}

}  // namespace hermesengine

// --- JSError ---------------------------------------------------------------

inline JSError::JSError(Runtime&, const std::string& message,
                        const Value& value, std::string stack)
    : std::runtime_error(message),
      value_(std::make_shared<Value>(value)),
      stack_(std::move(stack)) {}

// --- HostObject defaults ---------------------------------------------------

inline Value HostObject::get(Runtime&, const PropNameID&) {
  return Value::undefined();
}
inline bool HostObject::set(Runtime&, const PropNameID&, const Value&) {
  return false;
}
inline std::vector<PropNameID> HostObject::getPropertyNames(Runtime&) {
  return {};
}

// The defaults reproduce what an index does today on every engine: stringify
// it and take the named path. Nothing in the Hermes layer calls them.
inline Value HostObject::getValueAtIndex(Runtime& runtime, uint32_t index) {
  return get(runtime, PropNameID(std::to_string(index)));
}
inline bool HostObject::setValueAtIndex(Runtime& runtime, uint32_t index,
                                        const Value& value) {
  return set(runtime, PropNameID(std::to_string(index)), value);
}

// --- Value -----------------------------------------------------------------

inline Value::Value(Runtime&, const Object& object)
    : kind_(Kind::Owned), storage_(object.storage_) {}

inline Value::Value(Runtime&, const String& string)
    : kind_(Kind::Owned), storage_(string.storage_) {}

inline hermesengine::StoragePtr Value::sharedStorage(Runtime& runtime) const {
  if (kind_ == Kind::Owned) return storage_;
  if (kind_ == Kind::Borrowed) {
    // Memoised, because jsi has no way to borrow an Object out of a Value:
    // jsi::Value::getObject clones the PointerValue, so every promotion is a
    // make_shared plus a Hermes handle clone, and its destruction a matching
    // release. The Node-API shim asks the *same* arena slot who it is three
    // times per marshalled field read -- napi_get_host_object_data, the
    // napi_get_value_external it falls through to, and napi_unwrap -- and a
    // simpleperf profile of `javaObject.intField` in a loop attributes ~85% of
    // napi_get_host_object_data to that materialise/destroy churn and only
    // ~9% to the host-object token check it exists to perform.
    //
    // Caching is safe rather than merely faster: a borrowed value aliases a
    // jsi::Value the caller owns for the duration of the dispatch, so the
    // promoted handle can never outlive what it was cloned from, and every
    // other reader (jsiRef, toJsi, the scalar predicates) answers from
    // borrowed_ regardless. It also warms ValueStorage's cached jsi::Object,
    // so the three lookups now share one materialised object as well.
    if (storage_ == nullptr) {
      storage_ = hermesengine::makeStorage(
          ::facebook::jsi::Value(runtime.jsi(), *borrowed_));
    }
    return storage_;
  }
  return hermesengine::makeStorage(toJsi(runtime));
}

inline Object Value::asObject(Runtime& runtime) const {
  return Object::fromStorage(sharedStorage(runtime));
}

inline Object Value::asObjectBorrowed(Runtime& runtime) const {
  return asObject(runtime);
}

inline String Value::asString(Runtime& runtime) const {
  return String::fromStorage(sharedStorage(runtime));
}

// --- Runtime ---------------------------------------------------------------

inline Object Runtime::global() {
  return Object::fromStorage(
      hermesengine::makeStorage(::facebook::jsi::Value(runtime_->global())));
}

inline Value Runtime::evaluateJavaScript(std::shared_ptr<StringBuffer> buffer,
                                         const std::string& sourceURL) {
  try {
    return Value::fromStorage(hermesengine::makeStorage(
        runtime_->evaluateJavaScript(std::move(buffer), sourceURL)));
  } catch (const ::facebook::jsi::JSError& error) {
    // A real JS throw from the evaluated script: keep the thrown value.
    ::nativescript::engine::Value value = Value::fromStorage(
        hermesengine::makeStorage(::facebook::jsi::Value(*runtime_,
                                                         error.value())));
    throw JSError(*this, error.getMessage(), value, error.getStack());
  } catch (const ::facebook::jsi::JSIException& error) {
    // Not a JSError: Hermes reports a *compile* failure as a JSINativeException
    // reading "Compiling JS failed: 3:1:invalid expression". Every other engine
    // raises a real SyntaxError, and the module specs assert the type
    // ("main started SyntaxError main ended"), so name it -- ShimTypes.h's
    // errorFromMessage then rebuilds the right constructor from the prefix.
    // Only evaluate can fail this way; a runtime error thrown by the script is
    // a JSError and keeps its value above.
    throw JSError(*this, "SyntaxError: " + std::string(error.what()));
  }
}

// --- String ----------------------------------------------------------------

inline String String::createFromUtf8(Runtime& runtime, const char* value) {
  const char* text = value != nullptr ? value : "";
  return createFromUtf8(runtime, reinterpret_cast<const uint8_t*>(text),
                        std::strlen(text));
}

inline String String::createFromUtf8(Runtime& runtime,
                                     const std::string& value) {
  return createFromUtf8(runtime,
                        reinterpret_cast<const uint8_t*>(value.data()),
                        value.size());
}

inline String String::createFromUtf8(Runtime& runtime, const uint8_t* value,
                                     size_t length) {
  return hermesengine::guard(runtime, [&] {
    ::facebook::jsi::Runtime& rt = runtime.jsi();
    return String::fromStorage(hermesengine::makeStorage(
        ::facebook::jsi::Value(::facebook::jsi::String::createFromUtf8(
            rt,
            value != nullptr ? value : reinterpret_cast<const uint8_t*>(""),
            length))));
  });
}

inline std::string Value::utf8(Runtime& runtime) const {
  return asString(runtime).utf8(runtime);
}

inline Value Value::createStringFromUtf8(Runtime& runtime, const char* data, size_t length) {
  return Value(runtime,
               String::createFromUtf8(runtime, reinterpret_cast<const uint8_t*>(data), length));
}

inline std::string String::utf8(Runtime& runtime) const {
  if (storage_ == nullptr) return {};
  return hermesengine::guard(runtime, [&] {
    return storage_->string(runtime.jsi()).utf8(runtime.jsi());
  });
}

// --- Object ----------------------------------------------------------------

inline Object::Object(Runtime& runtime)
    : storage_(hermesengine::makeStorage(
          ::facebook::jsi::Value(::facebook::jsi::Object(runtime.jsi())))) {}

inline ::facebook::jsi::Object& Object::jsiObject(Runtime& runtime) const {
  return storage_->object(runtime.jsi());
}

inline Value Object::getProperty(Runtime& runtime, const char* name) const {
  return hermesengine::guard(runtime, [&] {
    return Value::fromStorage(hermesengine::makeStorage(
        jsiObject(runtime).getProperty(runtime.jsi(), name)));
  });
}

inline Value Object::getProperty(Runtime& runtime, const Value& key) const {
  return hermesengine::guard(runtime, [&] {
    if (key.isPointer()) {
      return Value::fromStorage(hermesengine::makeStorage(
          jsiObject(runtime).getProperty(runtime.jsi(), key.jsiRef())));
    }
    const ::facebook::jsi::Value materialised = key.toJsi(runtime);
    return Value::fromStorage(hermesengine::makeStorage(
        jsiObject(runtime).getProperty(runtime.jsi(), materialised)));
  });
}

inline Object Object::getPropertyAsObject(Runtime& runtime,
                                          const char* name) const {
  return getProperty(runtime, name).asObject(runtime);
}

inline Function Object::getPropertyAsFunction(Runtime& runtime,
                                              const char* name) const {
  return Function(getPropertyAsObject(runtime, name));
}

inline void Object::setProperty(Runtime& runtime, const char* name,
                                const Value& value) {
  hermesengine::guard(runtime, [&]() -> int {
    jsiObject(runtime).setProperty(runtime.jsi(), name, value.toJsi(runtime));
    return 0;
  });
}

inline void Object::setProperty(Runtime& runtime, const char* name,
                                const Object& value) {
  setProperty(runtime, name, Value::fromStorage(value.storage_));
}

inline void Object::setProperty(Runtime& runtime, const char* name,
                                const String& value) {
  setProperty(runtime, name, Value(runtime, value));
}

inline void Object::setProperty(Runtime& runtime, const Value& key,
                                const Value& value) {
  hermesengine::guard(runtime, [&]() -> int {
    const ::facebook::jsi::Value materialisedKey =
        key.isPointer() ? ::facebook::jsi::Value(runtime.jsi(), key.jsiRef())
                        : key.toJsi(runtime);
    jsiObject(runtime).setProperty(runtime.jsi(), materialisedKey,
                                   value.toJsi(runtime));
    return 0;
  });
}

inline bool Object::hasProperty(Runtime& runtime, const char* name) const {
  return hermesengine::guard(runtime, [&] {
    return jsiObject(runtime).hasProperty(runtime.jsi(), name);
  });
}

inline bool Object::isFunction(Runtime& runtime) const {
  return jsiObject(runtime).isFunction(runtime.jsi());
}

inline bool Object::isArray(Runtime& runtime) const {
  return jsiObject(runtime).isArray(runtime.jsi());
}

inline bool Object::isArrayBuffer(Runtime& runtime) const {
  return jsiObject(runtime).isArrayBuffer(runtime.jsi());
}

inline Function Object::asFunction(Runtime& runtime) const {
  return Function(*this);
}

inline Array Object::getArray(Runtime& runtime) const { return Array(*this); }

inline ArrayBuffer Object::getArrayBuffer(Runtime& runtime) const {
  return ArrayBuffer(*this);
}

inline Array Object::getPropertyNames(Runtime& runtime) const {
  return hermesengine::guard(runtime, [&] {
    return Array(Object::fromStorage(hermesengine::makeStorage(
        ::facebook::jsi::Value(
            jsiObject(runtime).getPropertyNames(runtime.jsi())))));
  });
}

inline Object Object::createFromHostObjectWithToken(
    Runtime& runtime, std::shared_ptr<HostObject> host, const void* typeToken) {
  return hermesengine::guard(runtime, [&] {
    ::facebook::jsi::Runtime& rt = runtime.jsi();
    auto adapter = std::make_shared<hermesengine::HostObjectAdapter>(
        &runtime, std::move(host), typeToken);
    ::facebook::jsi::Object created =
        ::facebook::jsi::Object::createFromHostObject(rt, adapter);
    // Weak, and set after creation: the adapter needs a handle on the wrapper
    // to answer receiver(), and a strong one would be a self-cycle.
    adapter->attach(rt, created);
    return Object::fromStorage(
        hermesengine::makeStorage(::facebook::jsi::Value(std::move(created))));
  });
}

inline std::shared_ptr<HostObject> Object::hostObjectOf(
    Runtime& runtime, const void* typeToken) const {
  ::facebook::jsi::Runtime& rt = runtime.jsi();
  ::facebook::jsi::Object& object = jsiObject(runtime);
  if (!object.isHostObject<hermesengine::HostObjectAdapter>(rt)) {
    return nullptr;
  }
  std::shared_ptr<hermesengine::HostObjectAdapter> adapter =
      object.getHostObject<hermesengine::HostObjectAdapter>(rt);
  if (adapter == nullptr || adapter->typeToken() != typeToken) return nullptr;
  return adapter->host();
}

namespace hermesengine {

// The payload jsi native state actually holds. jsi has no type tag of its own,
// so the token rides along and isHostObject-style identification stays a
// pointer compare rather than a dynamic_cast on a per-field-read path.
struct NativeStateBox : ::facebook::jsi::NativeState {
  NativeStateBox(std::shared_ptr<HostObject> host, const void* typeToken)
      : host(std::move(host)), typeToken(typeToken) {}
  std::shared_ptr<HostObject> host;
  const void* typeToken = nullptr;
};

}  // namespace hermesengine

inline void Object::setNativeStateWithToken(Runtime& runtime,
                                            std::shared_ptr<HostObject> host,
                                            const void* typeToken) {
  hermesengine::guard(runtime, [&]() -> int {
    jsiObject(runtime).setNativeState(
        runtime.jsi(), std::make_shared<hermesengine::NativeStateBox>(
                           std::move(host), typeToken));
    return 0;
  });
}

inline std::shared_ptr<HostObject> Object::nativeStateOf(
    Runtime& runtime, const void* typeToken) const {
  ::facebook::jsi::Runtime& rt = runtime.jsi();
  ::facebook::jsi::Object& object = jsiObject(runtime);
  if (!object.hasNativeState(rt)) return nullptr;
  // static, not dynamic: this shim is the only thing in the build that sets
  // jsi native state, and the token below is the identity check.
  auto* box = static_cast<hermesengine::NativeStateBox*>(
      object.getNativeState(rt).get());
  if (box == nullptr || box->typeToken != typeToken) return nullptr;
  return box->host;
}

// --- Function --------------------------------------------------------------

namespace hermesengine {

// Host-function arguments are borrowed, never cloned: jsi guarantees the array
// is valid for the duration of the call, which is exactly Node-API's rule for
// callback arguments. This is the marshalling hot path -- one clone per
// argument per call would be the single largest cost in the shim.
struct BorrowedArgs {
  static constexpr size_t kInline = 8;

  BorrowedArgs(const jsi::Value* args, size_t count) : count_(count) {
    engine::Value* target = inline_;
    if (count > kInline) {
      heap_.resize(count);
      target = heap_.data();
    }
    for (size_t i = 0; i < count; i++) {
      target[i] = engine::Value::borrowed(args[i]);
    }
    data_ = target;
  }

  const engine::Value* data() const { return data_; }
  size_t count() const { return count_; }

 private:
  engine::Value inline_[kInline];
  std::vector<engine::Value> heap_;
  const engine::Value* data_ = nullptr;
  size_t count_;
};

inline jsi::Function makeHostFunction(Runtime& runtime, const PropNameID& name,
                                      unsigned int paramCount,
                                      HostFunctionType callback) {
  jsi::Runtime& rt = runtime.jsi();
  Runtime* enginePtr = &runtime;
  const std::string text = name.utf8();
  return jsi::Function::createFromHostFunction(
      rt, jsi::PropNameID::forUtf8(rt, text), paramCount,
      [enginePtr, callback = std::move(callback)](
          jsi::Runtime& jsRuntime, const jsi::Value& thisVal,
          const jsi::Value* args, size_t count) -> jsi::Value {
        (void)jsRuntime;
        Runtime& engineRuntime = *enginePtr;
        BorrowedArgs borrowed(args, count);
        const engine::Value self = engine::Value::borrowed(thisVal);
        try {
          engine::Value outcome =
              callback(engineRuntime, self, borrowed.data(), borrowed.count());
          return outcome.toJsi(engineRuntime);
        } catch (const engine::JSError& error) {
          rethrowAsJsi(engineRuntime, error);
        }
      });
}

// Builds the object a construct call should run against: Object.create(
// Ctor.prototype), which is what the spec's OrdinaryCreateFromConstructor does
// and what every other engine hands a Node-API class constructor.
//
// Hermes does neither half of that for a native function. `new f()` passes the
// host function `undefined` as `this` and then *requires* it to return an
// object -- "FinalizableNativeFunction constructor must return an object" was
// 74 of the first run's 106 failures, and another 24 were `Worker should be
// called as a constructor!`, which is the Worker constructor finding no `this`.
//
// So the constructor trampoline synthesises the receiver and returns it. The
// function is held weakly: a strong handle from a function's own host data to
// its own prototype (which links back through `constructor`) is a cycle the GC
// cannot break.
inline jsi::Value makeConstructReceiver(jsi::Runtime& rt,
                                        std::optional<jsi::WeakObject>& weakSelf) {
  if (!weakSelf.has_value()) return jsi::Value(jsi::Object(rt));
  jsi::Value self = weakSelf->lock(rt);
  if (!self.isObject()) return jsi::Value(jsi::Object(rt));
  jsi::Value prototype = self.getObject(rt).getProperty(rt, "prototype");
  if (!prototype.isObject()) return jsi::Value(jsi::Object(rt));
  return jsi::Value(jsi::Object::create(rt, prototype));
}

// A JS function that wraps a host function so the result is constructible.
//
// Whether Hermes lets you `new` a native function is a property of the Hermes
// build, not of jsi: the vendored static-hermes allows it, the Hermes React
// Native ships throws "This function cannot be used as a constructor". Since
// @nativescript/react-native runs on whatever Hermes the app has, the
// constructor cannot be a native function at all. An ordinary JS function is
// constructible everywhere, gets a correct `this` from the language itself, and
// comes with a `prototype` whose `constructor` back-pointer is already right.
//
// Cached on the global because the factory is per-runtime and there is nowhere
// else to hang per-runtime state; the property is non-enumerable so it does not
// show up in application code.
inline jsi::Function hostConstructorFactory(jsi::Runtime& rt) {
  static constexpr const char* kFactoryKey = "__nsHostConstructorFactory";

  jsi::Value cached = rt.global().getProperty(rt, kFactoryKey);
  if (cached.isObject() && cached.getObject(rt).isFunction(rt)) {
    return cached.getObject(rt).getFunction(rt);
  }

  jsi::Value made = rt.evaluateJavaScript(
      std::make_shared<jsi::StringBuffer>(
          "(function (hostFn, name, length) {"
          "  function NativeScriptClass() {"
          "    return hostFn.apply(this, arguments);"
          "  }"
          "  Object.defineProperty(NativeScriptClass, 'name',"
          "    { value: name, configurable: true });"
          "  Object.defineProperty(NativeScriptClass, 'length',"
          "    { value: length, configurable: true });"
          "  return NativeScriptClass;"
          "})"),
      "nativescript:host-constructor");

  jsi::Function factory = made.getObject(rt).getFunction(rt);

  jsi::Object global = rt.global();
  jsi::Function defineProperty = global.getPropertyAsObject(rt, "Object")
                                     .getPropertyAsFunction(rt, "defineProperty");
  jsi::Object descriptor(rt);
  descriptor.setProperty(rt, "value", jsi::Value(rt, factory));
  descriptor.setProperty(rt, "enumerable", false);
  descriptor.setProperty(rt, "configurable", true);
  defineProperty.call(rt, jsi::Value(rt, global),
                      jsi::String::createFromAscii(rt, kFactoryKey),
                      jsi::Value(rt, descriptor));

  return factory;
}

inline jsi::Function makeHostConstructor(Runtime& runtime,
                                         const PropNameID& name,
                                         unsigned int paramCount,
                                         HostFunctionType callback) {
  jsi::Runtime& rt = runtime.jsi();
  Runtime* enginePtr = &runtime;
  const std::string text = name.utf8();
  auto weakSelf = std::make_shared<std::optional<jsi::WeakObject>>();

  jsi::Function fn = jsi::Function::createFromHostFunction(
      rt, jsi::PropNameID::forUtf8(rt, text), paramCount,
      [enginePtr, weakSelf, callback = std::move(callback)](
          jsi::Runtime& jsRuntime, const jsi::Value& thisVal,
          const jsi::Value* args, size_t count) -> jsi::Value {
        Runtime& engineRuntime = *enginePtr;
        const jsi::Value receiver =
            thisVal.isObject() ? jsi::Value(jsRuntime, thisVal)
                               : makeConstructReceiver(jsRuntime, *weakSelf);
        BorrowedArgs borrowed(args, count);
        const engine::Value self = engine::Value::borrowed(receiver);
        try {
          engine::Value outcome =
              callback(engineRuntime, self, borrowed.data(), borrowed.count());
          jsi::Value result = outcome.toJsi(engineRuntime);
          if (result.isObject()) return result;
          return jsi::Value(jsRuntime, receiver);
        } catch (const engine::JSError& error) {
          rethrowAsJsi(engineRuntime, error);
        }
      });

  // Wrap it in a real JS function; see hostConstructorFactory above. The
  // wrapper brings its own writable `prototype` carrying the `constructor`
  // back-pointer the spec requires, which is what stops `instance.constructor`
  // walking past the class prototype and reporting every native class as
  // "Object".
  jsi::Function factory = hostConstructorFactory(rt);
  jsi::Value made = factory.call(rt, jsi::Value(rt, fn),
                                 jsi::String::createFromUtf8(rt, text),
                                 jsi::Value(static_cast<double>(paramCount)));
  jsi::Function ctor = made.getObject(rt).getFunction(rt);

  // Weak, and pointing at the wrapper rather than the host function: it is the
  // wrapper that owns `prototype`, and it is what a direct (non-`new`) call has
  // to synthesise a receiver from. Strong would be a cycle through
  // prototype.constructor that the GC cannot break.
  weakSelf->emplace(rt, ctor);
  return ctor;
}

}  // namespace hermesengine

inline Function Function::createFromHostFunction(Runtime& runtime,
                                                 const PropNameID& name,
                                                 unsigned int paramCount,
                                                 HostFunctionType callback) {
  return hermesengine::guard(runtime, [&] {
    ::facebook::jsi::Runtime& rt = runtime.jsi();
    ::facebook::jsi::Function fn = hermesengine::makeHostFunction(
        runtime, name, paramCount, std::move(callback));
    return Function(Object::fromStorage(hermesengine::makeStorage(
        ::facebook::jsi::Value(std::move(fn)))));
  });
}

inline Function Function::createFromHostConstructor(Runtime& runtime,
                                                    const PropNameID& name,
                                                    unsigned int paramCount,
                                                    HostFunctionType callback) {
  return hermesengine::guard(runtime, [&] {
    ::facebook::jsi::Runtime& rt = runtime.jsi();
    ::facebook::jsi::Function fn = hermesengine::makeHostConstructor(
        runtime, name, paramCount, std::move(callback));
    return Function(Object::fromStorage(hermesengine::makeStorage(
        ::facebook::jsi::Value(std::move(fn)))));
  });
}

namespace hermesengine {

// Materialises engine Values into the contiguous jsi::Value array the jsi call
// API wants. jsi::Value is move-only, so this cannot alias the caller's array.
struct JsiArgs {
  JsiArgs(Runtime& runtime, const engine::Value* args, size_t count) {
    values_.reserve(count);
    for (size_t i = 0; i < count; i++) {
      values_.push_back(args[i].toJsi(runtime));
    }
  }

  const jsi::Value* data() const { return values_.data(); }
  size_t count() const { return values_.size(); }

 private:
  std::vector<jsi::Value> values_;
};

}  // namespace hermesengine

inline Value Function::call(Runtime& runtime, const Value* args,
                            size_t count) const {
  return hermesengine::guard(runtime, [&] {
    ::facebook::jsi::Runtime& rt = runtime.jsi();
    hermesengine::JsiArgs converted(runtime, args, count);
    ::facebook::jsi::Function fn = jsiObject(runtime).getFunction(rt);
    return Value::fromStorage(hermesengine::makeStorage(
        fn.call(rt, converted.data(), converted.count())));
  });
}

inline Value Function::callWithThis(Runtime& runtime, const Object& thisObject,
                                    const Value* args, size_t count) const {
  return hermesengine::guard(runtime, [&] {
    ::facebook::jsi::Runtime& rt = runtime.jsi();
    hermesengine::JsiArgs converted(runtime, args, count);
    ::facebook::jsi::Function fn = jsiObject(runtime).getFunction(rt);
    return Value::fromStorage(hermesengine::makeStorage(fn.callWithThis(
        rt, thisObject.jsiObject(runtime), converted.data(),
        converted.count())));
  });
}

inline Value Function::callAsConstructor(Runtime& runtime, const Value* args,
                                         size_t count) const {
  return hermesengine::guard(runtime, [&] {
    ::facebook::jsi::Runtime& rt = runtime.jsi();
    hermesengine::JsiArgs converted(runtime, args, count);
    ::facebook::jsi::Function fn = jsiObject(runtime).getFunction(rt);
    return Value::fromStorage(hermesengine::makeStorage(
        fn.callAsConstructor(rt, converted.data(), converted.count())));
  });
}

// --- Array / ArrayBuffer ---------------------------------------------------

inline Array::Array(Runtime& runtime, size_t size)
    : Object(Object::fromStorage(hermesengine::makeStorage(
          ::facebook::jsi::Value(::facebook::jsi::Array(runtime.jsi(), size))))) {}

inline size_t Array::size(Runtime& runtime) const {
  return hermesengine::guard(runtime, [&] {
    return jsiObject(runtime).getArray(runtime.jsi()).size(runtime.jsi());
  });
}

inline Value Array::getValueAtIndex(Runtime& runtime, size_t index) const {
  return hermesengine::guard(runtime, [&] {
    return Value::fromStorage(hermesengine::makeStorage(
        jsiObject(runtime)
            .getArray(runtime.jsi())
            .getValueAtIndex(runtime.jsi(), index)));
  });
}

inline void Array::setValueAtIndex(Runtime& runtime, size_t index,
                                   const Value& value) {
  hermesengine::guard(runtime, [&]() -> int {
    jsiObject(runtime)
        .getArray(runtime.jsi())
        .setValueAtIndex(runtime.jsi(), index, value.toJsi(runtime));
    return 0;
  });
}

inline ArrayBuffer::ArrayBuffer(Runtime& runtime,
                                std::shared_ptr<MutableBuffer> buffer)
    : Object(Object::fromStorage(hermesengine::makeStorage(
          ::facebook::jsi::Value(::facebook::jsi::ArrayBuffer(
              runtime.jsi(), std::move(buffer)))))) {}

inline uint8_t* ArrayBuffer::data(Runtime& runtime) const {
  return hermesengine::guard(runtime, [&] {
    return jsiObject(runtime).getArrayBuffer(runtime.jsi()).data(runtime.jsi());
  });
}

inline size_t ArrayBuffer::size(Runtime& runtime) const {
  return hermesengine::guard(runtime, [&] {
    return jsiObject(runtime).getArrayBuffer(runtime.jsi()).size(runtime.jsi());
  });
}

// --- WeakObject ------------------------------------------------------------

inline WeakObject::WeakObject(Runtime& runtime, const Value& value) {
  if (!value.isObject()) {
    // Nothing weak to make; keep it alive so the reference still reads back.
    strong_ = hermesengine::makeStorage(value.toJsi(runtime));
    return;
  }
  Object object = value.asObject(runtime);
  weak_ = std::make_shared<::facebook::jsi::WeakObject>(
      runtime.jsi(), object.jsiObject(runtime));
}

inline Value WeakObject::lock(Runtime& runtime) const {
  if (strong_ != nullptr) return Value::fromStorage(strong_);
  if (weak_ == nullptr) return Value::undefined();
  return Value::fromStorage(
      hermesengine::makeStorage(weak_->lock(runtime.jsi())));
}

// --- HostObjectAdapter -----------------------------------------------------

namespace hermesengine {

inline jsi::Value HostObjectAdapter::get(jsi::Runtime& rt,
                                         const jsi::PropNameID& name) {
  engine::Runtime& runtime = *runtime_;
  const jsi::Value receiver = self(rt);
  const engine::Value engineReceiver = engine::Value::borrowed(receiver);
  engine::PropNameID engineName(rt, name);
  engine::HostObject::ReceiverScope scope(*host_, engineReceiver);
  try {
    return host_->get(runtime, engineName).toJsi(runtime);
  } catch (const engine::JSError& error) {
    rethrowAsJsi(runtime, error);
  }
}

inline void HostObjectAdapter::set(jsi::Runtime& rt,
                                   const jsi::PropNameID& name,
                                   const jsi::Value& value) {
  engine::Runtime& runtime = *runtime_;
  const jsi::Value receiver = self(rt);
  const engine::Value engineReceiver = engine::Value::borrowed(receiver);
  const engine::Value engineValue = engine::Value::borrowed(value);
  engine::PropNameID engineName(rt, name);
  engine::HostObject::ReceiverScope scope(*host_, engineReceiver);
  try {
    host_->set(runtime, engineName, engineValue);
  } catch (const engine::JSError& error) {
    rethrowAsJsi(runtime, error);
  }
}

inline std::vector<jsi::PropNameID> HostObjectAdapter::getPropertyNames(
    jsi::Runtime& rt) {
  engine::Runtime& runtime = *runtime_;
  const jsi::Value receiver = self(rt);
  const engine::Value engineReceiver = engine::Value::borrowed(receiver);
  engine::HostObject::ReceiverScope scope(*host_, engineReceiver);
  std::vector<jsi::PropNameID> out;
  try {
    std::vector<engine::PropNameID> names = host_->getPropertyNames(runtime);
    out.reserve(names.size());
    for (const engine::PropNameID& name : names) {
      out.push_back(jsi::PropNameID::forUtf8(rt, name.utf8()));
    }
  } catch (const engine::JSError& error) {
    rethrowAsJsi(runtime, error);
  }
  return out;
}

}  // namespace hermesengine

}  // namespace engine
}  // namespace nativescript

#endif  // NS_JSI_HERMES_HERMES_RUNTIME_H
