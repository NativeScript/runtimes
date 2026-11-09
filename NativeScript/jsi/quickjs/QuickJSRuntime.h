#ifndef NATIVESCRIPT_JSI_QUICKJS_QUICKJS_RUNTIME_H
#define NATIVESCRIPT_JSI_QUICKJS_QUICKJS_RUNTIME_H

// QuickJS behind the JSI-shaped `nativescript::engine` API.
//
// This header is platform-neutral: it depends on QuickJS and the C++ standard
// library, and on nothing from Foundation, the Objective-C runtime, or the
// Apple metadata format. Android compiles it as-is.
//
// The Apple bridge's extra baggage -- Foundation, <objc/*>, the Mach-O
// metadata section lookup, and the NativeApiClassBuilderProtocol forward
// declaration -- lives in ffi/objc/quickjs/NativeApiQuickJSRuntime.h, which
// includes this file. Apple sources keep including that path unchanged.

#ifdef TARGET_ENGINE_QUICKJS

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
#include "quickjs.h"

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
// This engine is not yet wired to the Node-API shim, so nothing populates the
// payload here and value() always reports null -- callers fall back to the
// message. The API exists so the shim stays engine-neutral.
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
  // QuickJS has no indexed hook, so this layer recognises an index-carrying
  // atom and routes it here -- but only for a host object that opted in, since
  // the named path also does the prototype handling.
  virtual Value getValueAtIndex(Runtime& runtime, uint32_t index);
  virtual bool setValueAtIndex(Runtime& runtime, uint32_t index, const Value& value);

  bool hasIndexedAccess() const { return indexedAccess_; }
  void setIndexedAccess(bool value) { indexedAccess_ = value; }

  // The JS object standing for this host object, valid ONLY for the duration of
  // the call the engine is currently dispatching. Mirrors the V8 and Hermes
  // layers; see V8Runtime.h for the full note.
  //
  // Handed in per call rather than stored, and deliberately non-owning. A
  // HostObject holding an owned handle to its own wrapper is a strong
  // self-cycle: the wrapper is then permanently reachable, its finalizer never
  // runs, and on Android ObjectManager never calls makeInstanceWeak -- every
  // Java instance stays pinned.
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

namespace quickjsengine {

template <typename T>
const void* hostObjectTypeToken() {
  static int token = 0;
  return &token;
}

struct RuntimeState : RuntimeCleanupRegistry {
  explicit RuntimeState(JSContext* context) : context(context) {}
  JSContext* context = nullptr;
  bool hostClassRegistered = false;
  bool functionClassRegistered = false;
  bool selectorGroupDataClassRegistered = false;
  bool nativeStateClassRegistered = false;
  // The atom every native-state slot hangs off, interned once per runtime.
  // Classic QuickJS has no JS_NewSymbol, so this is a string atom -- but the
  // property is defined non-enumerable and read with JS_GetOwnProperty, which
  // skips the prototype chain entirely.
  //
  // Freed by releaseStateForContext, not by ~RuntimeState: a HostObjectHolder
  // keeps a shared_ptr to this state and is itself released by a GC finaliser
  // during JS_FreeContext, so the state can outlive the context it names.
  JSAtom nativeStateAtom = JS_ATOM_NULL;
  // Whether an array index can be read straight out of the atom the exotic
  // handlers are handed. Established once per runtime in ensureClasses; see
  // atomAsArrayIndex in QuickJSHostObjects.cpp.
  bool indexAtomsAreTagged = false;
  bool indexAtomTaggingChecked = false;
};

extern JSClassID gHostClassId;
extern JSClassID gFunctionClassId;
// An ordinary object that happens to carry an opaque slot: no exotic table, so
// property access on it is exactly what it is on a plain object. Used for the
// receiver a host constructor builds, which is the object the Node-API shim
// wraps. See Object::setNativeState.
extern JSClassID gNativeStateClassId;

// JS_NewClassID gained a JSRuntime* parameter in quickjs-ng. Apple and Android's
// QUICKJS_NG build take the two-argument form, Android's classic QuickJS build
// the one-argument form; both are the same call. Resolved by overload rather
// than by #if, because there is no version macro that separates the two
// consistently across all three vendored copies.
// Both template parameters are deduced so that each overload's return type is
// dependent -- a non-dependent decltype would be a hard error, not a
// substitution failure, and the wrong arm would break the build outright.
template <typename R, typename C>
inline auto newClassIdImpl(R* runtime, C* classId, int)
    -> decltype(JS_NewClassID(runtime, classId)) {
  return JS_NewClassID(runtime, classId);
}
template <typename R, typename C>
inline auto newClassIdImpl(R*, C* classId, long) -> decltype(JS_NewClassID(classId)) {
  return JS_NewClassID(classId);
}
inline JSClassID newClassId(JSRuntime* runtime, JSClassID* classId) {
  return newClassIdImpl(runtime, classId, 0);
}

// JS_IsBigInt and JS_IsArray dropped their JSContext* in quickjs-ng. Android's
// QUICKJS_NG build takes the one-argument form; Android's classic QuickJS and
// the Apple copy take the two-argument one. Same overload trick as above.
template <typename C, typename V>
inline auto isBigIntImpl(C* context, const V& value, int)
    -> decltype(JS_IsBigInt(context, value)) {
  return JS_IsBigInt(context, value);
}
template <typename C, typename V>
inline auto isBigIntImpl(C*, const V& value, long) -> decltype(JS_IsBigInt(value)) {
  return JS_IsBigInt(value);
}
inline bool isBigInt(JSContext* context, JSValueConst value) {
  return isBigIntImpl(context, value, 0) != 0;
}

template <typename C, typename V>
inline auto isArrayImpl(C* context, const V& value, int)
    -> decltype(JS_IsArray(context, value)) {
  return JS_IsArray(context, value);
}
template <typename C, typename V>
inline auto isArrayImpl(C*, const V& value, long) -> decltype(JS_IsArray(value)) {
  return JS_IsArray(value);
}
inline bool isArray(JSContext* context, JSValueConst value) {
  return isArrayImpl(context, value, 0) != 0;
}

std::shared_ptr<RuntimeState> stateForContext(JSContext* context);

// Drops the RuntimeState cached for a context that is about to be freed.
//
// The cache is keyed by JSContext*, and the allocator hands the same address
// back for the next runtime -- workers create and destroy one each. A surviving
// entry would give the new context a state that already says
// hostClassRegistered, so JS_NewClass would never run for its JSRuntime and
// every host object created on it would carry an unregistered class id.
void releaseStateForContext(JSContext* context);

struct ValueStorage {
  enum class Kind {
    Undefined,
    Null,
    Bool,
    Number,
    QuickJS,
    QuickJSBorrowed,
  };

  explicit ValueStorage(Kind kind) : kind(kind) {}
  ~ValueStorage() {
    if (kind == Kind::QuickJS && context != nullptr && !JS_IsUninitialized(value)) {
      JS_FreeValue(context, value);
    }
  }

  Kind kind = Kind::Undefined;
  bool boolValue = false;
  double numberValue = 0;
  JSContext* context = nullptr;
  JSValue value = JS_UNINITIALIZED;
};

struct HostObjectHolder {
  HostObjectHolder(std::shared_ptr<RuntimeState> state, std::shared_ptr<HostObject> hostObject,
                   const void* typeToken)
      : state(std::move(state)), hostObject(std::move(hostObject)), typeToken(typeToken) {}
  std::shared_ptr<RuntimeState> state;
  std::shared_ptr<HostObject> hostObject;
  const void* typeToken = nullptr;
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

inline std::string valueToUtf8(JSContext* context, JSValueConst value) {
  size_t length = 0;
  const char* cString = JS_ToCStringLen(context, &length, value);
  if (cString == nullptr) {
    return {};
  }
  std::string result(cString, length);
  JS_FreeCString(context, cString);
  return result;
}

inline std::string currentExceptionMessage(JSContext* context) {
  JSValue exception = JS_GetException(context);
  std::string message = valueToUtf8(context, exception);
  JS_FreeValue(context, exception);
  return message.empty() ? std::string("QuickJS function call failed.")
                         : message;
}

inline std::string atomToUtf8(JSContext* context, JSAtom atom) {
  const char* cString = JS_AtomToCString(context, atom);
  if (cString == nullptr) {
    return {};
  }
  std::string result(cString);
  JS_FreeCString(context, cString);
  return result;
}

inline JSValue throwError(JSContext* context, const std::exception& error) {
  return JS_ThrowTypeError(context, "%s", error.what());
}

void ensureClasses(Runtime& runtime);

}  // namespace quickjsengine

// Mirrors jsi::WeakObject. lock() returns undefined once the referent has been
// collected.
//
// Backed by a JS `WeakRef`, which is what QuickJS exposes -- it has no
// C-level weak handle. Same mechanism the Node-API binding uses
// (vendor/quickjs/quickjs-api.c's napi_create_reference), so the two
// binding layers have the same collection behaviour.
//
// A value WeakRef cannot hold -- a primitive, or a string -- is kept strongly
// instead. Those are not garbage in the sense a weak reference cares about, and
// reporting them as collected would be wrong.
class WeakObject {
 public:
  WeakObject() = default;
  WeakObject(Runtime& runtime, const Value& value);
  Value lock(Runtime& runtime) const;
  bool empty() const { return storage_ == nullptr; }
  void reset() {
    storage_.reset();
    isWeakRef_ = false;
  }

 private:
  std::shared_ptr<quickjsengine::ValueStorage> storage_;
  bool isWeakRef_ = false;
};

class Runtime {
 public:
  explicit Runtime(JSContext* context) : state_(quickjsengine::stateForContext(context)) {}
  explicit Runtime(std::shared_ptr<quickjsengine::RuntimeState> state) : state_(std::move(state)) {}
  JSContext* context() const { return state_->context; }
  std::shared_ptr<quickjsengine::RuntimeState> state() const { return state_; }
  const void* identity() const { return state_.get(); }
  void detachState() { state_.reset(); }
  // See RuntimeState::nativeStateAtom. Interned on first use.
  JSAtom nativeStateAtom() const {
    if (state_->nativeStateAtom == JS_ATOM_NULL) {
      state_->nativeStateAtom = JS_NewAtom(state_->context, "__nsNativeState");
    }
    return state_->nativeStateAtom;
  }
  Object global();
  Value evaluateJavaScript(std::shared_ptr<StringBuffer> buffer, const std::string& sourceURL);
  void drainMicrotasks() {
    JSContext* ctx = context();
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSContext* jobCtx = nullptr;
    while (JS_ExecutePendingJob(rt, &jobCtx) > 0) {
    }
  }

 private:
  std::shared_ptr<quickjsengine::RuntimeState> state_;
};

class String {
 public:
  String() = default;
  String(Runtime& runtime, JSValue value);

  // The three factories below adopt the reference JS_New*String returns instead
  // of going through String(Runtime&, JSValue), which *duplicates* it. That
  // constructor's contract is "the caller keeps its own reference and frees it"
  // -- correct for Value::asString, which does free -- but a freshly created
  // string has no other owner, so duplicating it left the refcount permanently
  // one too high. Every JS string built through this layer on QuickJS leaked.
  static String adopt(Runtime& runtime, JSValue value) {
    String result;
    result.storage_ = std::make_shared<quickjsengine::ValueStorage>(
        quickjsengine::ValueStorage::Kind::QuickJS);
    result.storage_->context = runtime.context();
    result.storage_->value = value;
    return result;
  }
  static String createFromUtf8(Runtime& runtime, const char* value) {
    return adopt(runtime, JS_NewString(runtime.context(), value != nullptr ? value : ""));
  }
  static String createFromUtf8(Runtime& runtime, const std::string& value) {
    return adopt(runtime, JS_NewStringLen(runtime.context(), value.data(), value.size()));
  }
  static String createFromUtf8(Runtime& runtime, const uint8_t* value, size_t length) {
    return adopt(runtime,
                 JS_NewStringLen(runtime.context(), reinterpret_cast<const char*>(value), length));
  }
  std::string utf8(Runtime& runtime) const;
  JSValue local(Runtime& runtime) const;
  operator Value() const;

 private:
  friend class Value;
  std::shared_ptr<quickjsengine::ValueStorage> storage_;
};

class Value {
 public:
  Value() : kind_(quickjsengine::ValueStorage::Kind::Undefined) {}

  Value(bool value) : kind_(quickjsengine::ValueStorage::Kind::Bool), boolValue_(value) {}

  Value(double value) : kind_(quickjsengine::ValueStorage::Kind::Number), numberValue_(value) {}

  Value(int value) : Value(static_cast<double>(value)) {}
  Value(uint32_t value) : Value(static_cast<double>(value)) {}

  Value(Runtime& runtime, const Value& value) {
    if (value.kind_ == quickjsengine::ValueStorage::Kind::QuickJSBorrowed) {
      // Promote borrowed to owned
      storage_ = std::make_shared<quickjsengine::ValueStorage>(
          quickjsengine::ValueStorage::Kind::QuickJS);
      storage_->context = runtime.context();
      storage_->value = JS_DupValue(runtime.context(), value.borrowedValue_);
      kind_ = quickjsengine::ValueStorage::Kind::QuickJS;
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
  Value(Runtime& runtime, const String& value) : storage_(value.storage_) {
    kind_ = storage_ ? storage_->kind : quickjsengine::ValueStorage::Kind::Undefined;
  }
  Value(Runtime& runtime, const Object& object);
  Value(Runtime& runtime, const Function& function);
  Value(Runtime& runtime, const Array& array);
  Value(Runtime& runtime, const ArrayBuffer& arrayBuffer);
  Value(Runtime& runtime, const BigInt& bigint);
  Value(Runtime& runtime, JSValue value)
      : kind_(quickjsengine::ValueStorage::Kind::QuickJS),
        storage_(std::make_shared<quickjsengine::ValueStorage>(
            quickjsengine::ValueStorage::Kind::QuickJS)) {
    storage_->context = runtime.context();
    storage_->value = JS_DupValue(runtime.context(), value);
  }

  static Value borrowed(Runtime& runtime, JSValueConst value) {
    Value result;
    result.kind_ = quickjsengine::ValueStorage::Kind::QuickJSBorrowed;
    result.borrowedContext_ = runtime.context();
    result.borrowedValue_ = value;
    return result;
  }

  static Value undefined() { return Value(); }
  static Value null() {
    Value value;
    value.kind_ = quickjsengine::ValueStorage::Kind::Null;
    return value;
  }
  static bool strictEquals(Runtime& runtime, const Value& lhs,
                           const Value& rhs) {
    JSValue lhsValue = lhs.local(runtime);
    JSValue rhsValue = rhs.local(runtime);
    bool equal = JS_IsStrictEqual(runtime.context(), lhsValue, rhsValue);
    JS_FreeValue(runtime.context(), lhsValue);
    JS_FreeValue(runtime.context(), rhsValue);
    return equal;
  }
  bool isUndefined() const {
    if (kind_ == quickjsengine::ValueStorage::Kind::Undefined) {
      return true;
    }
    return isQuickJS() && JS_IsUndefined(jsValue());
  }
  bool isNull() const {
    if (kind_ == quickjsengine::ValueStorage::Kind::Null) {
      return true;
    }
    return isQuickJS() && JS_IsNull(jsValue());
  }
  bool isBool() const {
    if (kind_ == quickjsengine::ValueStorage::Kind::Bool) {
      return true;
    }
    return isQuickJS() && JS_IsBool(jsValue());
  }
  bool getBool() const {
    if (kind_ == quickjsengine::ValueStorage::Kind::Bool) {
      return boolValue_;
    }
    if (isQuickJS()) {
      return JS_ToBool(jsContext(), jsValue()) != 0;
    }
    return false;
  }
  bool isNumber() const {
    if (kind_ == quickjsengine::ValueStorage::Kind::Number) {
      return true;
    }
    return isQuickJS() && JS_IsNumber(jsValue());
  }
  double getNumber() const {
    if (kind_ == quickjsengine::ValueStorage::Kind::Number) {
      return numberValue_;
    }
    if (isQuickJS()) {
      double value = 0;
      JS_ToFloat64(jsContext(), &value, jsValue());
      return value;
    }
    return 0;
  }
  bool isObject() const { return isQuickJS() && JS_IsObject(jsValue()); }
  bool isString() const { return isQuickJS() && JS_IsString(jsValue()); }
  bool isBigInt() const {
    return isQuickJS() && quickjsengine::isBigInt(jsContext(), jsValue());
  }
  bool isSymbol() const { return isQuickJS() && JS_IsSymbol(jsValue()); }

  Object asObject(Runtime& runtime) const;
  // Borrowing is a V8-only capability; see jsi/v8/V8Runtime.h. Everywhere else
  // this is the owning conversion, and callers get the stronger guarantee.
  // Declared, not defined: Object is still incomplete here, so a body calling
  // asObject would not compile. Defined next to asObject in QuickJSValue.cpp.
  Object asObjectBorrowed(Runtime& runtime) const;
  String asString(Runtime& runtime) const;
  BigInt getBigInt(Runtime& runtime) const;

  // Read the UTF-8 of a string value without materialising a String. See the
  // comment on the V8 declaration: String is an owning type, and building one
  // costs a make_shared plus a JS_DupValue/JS_FreeValue pair for a handle that
  // dies two statements later.
  std::string utf8(Runtime& runtime) const;

  // Create a string value, adopting the reference JS_NewStringLen returns
  // instead of duplicating it. QuickJS strings are refcounted rather than
  // scope-rooted, so unlike V8 this still needs owning storage -- but it does
  // not need a second reference.
  static Value createStringFromUtf8(Runtime& runtime, const char* data, size_t length);

  JSValue local(Runtime& runtime) const {
    switch (kind_) {
      case quickjsengine::ValueStorage::Kind::Undefined:
        return JS_UNDEFINED;
      case quickjsengine::ValueStorage::Kind::Null:
        return JS_NULL;
      case quickjsengine::ValueStorage::Kind::Bool:
        return JS_NewBool(runtime.context(), boolValue_);
      case quickjsengine::ValueStorage::Kind::Number:
        return JS_NewFloat64(runtime.context(), numberValue_);
      case quickjsengine::ValueStorage::Kind::QuickJS:
      case quickjsengine::ValueStorage::Kind::QuickJSBorrowed:
        return JS_DupValue(runtime.context(), jsValue());
    }
  }

  // Access the shared storage (for Object/Function/Array interop)
  std::shared_ptr<quickjsengine::ValueStorage> storage() const { return storage_; }

  static Value fromStorage(std::shared_ptr<quickjsengine::ValueStorage> s) {
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

  bool isQuickJS() const {
    return kind_ == quickjsengine::ValueStorage::Kind::QuickJS ||
           kind_ == quickjsengine::ValueStorage::Kind::QuickJSBorrowed;
  }
  JSContext* jsContext() const {
    return kind_ == quickjsengine::ValueStorage::Kind::QuickJSBorrowed ? borrowedContext_
                                                                       : storage_->context;
  }
  JSValue jsValue() const {
    return kind_ == quickjsengine::ValueStorage::Kind::QuickJSBorrowed ? borrowedValue_
                                                                       : storage_->value;
  }

  quickjsengine::ValueStorage::Kind kind_ = quickjsengine::ValueStorage::Kind::Undefined;
  bool boolValue_ = false;
  double numberValue_ = 0;
  JSContext* borrowedContext_ = nullptr;
  JSValue borrowedValue_ = JS_UNINITIALIZED;
  std::shared_ptr<quickjsengine::ValueStorage> storage_;
};

// Defined here rather than with the class: constructing the shared_ptr needs
// Value to be complete.
inline WeakObject::WeakObject(Runtime& runtime, const Value& value) {
  JSContext* ctx = runtime.context();
  JSValue target = value.local(runtime);
  storage_ =
      std::make_shared<quickjsengine::ValueStorage>(quickjsengine::ValueStorage::Kind::QuickJS);
  storage_->context = ctx;

  if (!JS_IsObject(target) && !JS_IsSymbol(target)) {
    storage_->value = target;
    return;
  }

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue weakRefCtor = JS_GetPropertyStr(ctx, global, "WeakRef");
  JS_FreeValue(ctx, global);
  if (!JS_IsFunction(ctx, weakRefCtor)) {
    JS_FreeValue(ctx, weakRefCtor);
    storage_->value = target;
    return;
  }
  JSValue args[1] = {target};
  JSValue weakRef = JS_CallConstructor(ctx, weakRefCtor, 1, args);
  JS_FreeValue(ctx, weakRefCtor);
  if (JS_IsException(weakRef)) {
    // Nothing may throw out of here: this runs from napi_create_reference,
    // which has no way to report it. Falling back to a strong handle keeps the
    // reference usable; it merely stops being collectable.
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, weakRef);
    storage_->value = target;
    return;
  }
  JS_FreeValue(ctx, target);
  storage_->value = weakRef;
  isWeakRef_ = true;
}

inline Value WeakObject::lock(Runtime& runtime) const {
  if (storage_ == nullptr) {
    return Value::undefined();
  }
  if (!isWeakRef_) {
    return Value::fromStorage(storage_);
  }
  JSValue target = JS_WeakRef_Deref(runtime.context(), storage_->value);
  Value result(runtime, target);
  JS_FreeValue(runtime.context(), target);
  return result;
}

inline JSError::JSError(Runtime& runtime, const std::string& message,
                        const Value& value, std::string stack)
    : std::runtime_error(message),
      value_(std::make_shared<Value>(runtime, value)),
      stack_(std::move(stack)) {}

namespace quickjsengine {

// Take the context's pending exception and wrap it in a JSError that CARRIES
// the thrown value, not just its text.
//
// Rebuilding an error from its message loses two things the Node-API shim
// needs: the error's type (a SyntaxError arrives as the string
// "SyntaxError: ..." and reaches JS as a plain Error) and anything the runtime
// attached to it -- NativeScriptException hangs the originating Java throwable
// off `nativeException`, and dropping it makes `e.nativeException.getStackTrace()`
// undefined. This mirrors what the V8 and Hermes layers already do.
//
// Additive: JSError's payload was previously never populated by this engine and
// every existing caller reads only what(), which is unchanged.
inline JSError caughtError(Runtime& runtime, const char* fallback) {
  JSContext* context = runtime.context();
  JSValue exception = JS_GetException(context);
  // JS_GetException yields null when nothing is pending, which some failure
  // paths (a bad atom, an out-of-range index) can reach. Reporting the string
  // "null" as the message would be worse than the caller's fallback.
  if (JS_IsNull(exception) || JS_IsUninitialized(exception)) {
    JS_FreeValue(context, exception);
    return JSError(runtime, fallback != nullptr ? fallback : "QuickJS call failed.");
  }
  std::string message = valueToUtf8(context, exception);
  std::string stack;
  if (JS_IsObject(exception)) {
    JSValue stackValue = JS_GetPropertyStr(context, exception, "stack");
    if (JS_IsString(stackValue)) {
      stack = valueToUtf8(context, stackValue);
    }
    JS_FreeValue(context, stackValue);
  }
  Value value(runtime, exception);
  JS_FreeValue(context, exception);
  if (message.empty()) {
    message = fallback != nullptr ? fallback : "QuickJS call failed.";
  }
  return JSError(runtime, message, value, std::move(stack));
}

// Re-throw a JSError into JS, preserving the original thrown value when the
// error carries one. Without this every error crossing the host boundary was
// flattened into a TypeError built from its message.
inline JSValue throwJSError(Runtime& runtime, const JSError& error) {
  JSContext* context = runtime.context();
  if (const Value* thrown = error.value()) {
    if (!thrown->isUndefined() && !thrown->isNull()) {
      return JS_Throw(context, thrown->local(runtime));
    }
  }
  return throwError(context, error);
}

}  // namespace quickjsengine

class Object {
 public:
  Object() = default;
  explicit Object(Runtime& runtime)
      : storage_(std::make_shared<quickjsengine::ValueStorage>(
            quickjsengine::ValueStorage::Kind::QuickJS)) {
    storage_->context = runtime.context();
    storage_->value = JS_NewObject(runtime.context());
  }
  static Object fromValueStorage(std::shared_ptr<quickjsengine::ValueStorage> storage) {
    Object object;
    object.storage_ = std::move(storage);
    return object;
  }
  template <typename T>
  static Object createFromHostObject(Runtime& runtime, std::shared_ptr<T> host) {
    auto baseHost = std::static_pointer_cast<HostObject>(std::move(host));
    return createFromHostObjectWithToken(runtime, std::move(baseHost),
                                         quickjsengine::hostObjectTypeToken<T>());
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

  Value getProperty(Runtime& runtime, const char* name) const {
    JSValue object = local(runtime);
    JSValue result = JS_GetPropertyStr(runtime.context(), object, name != nullptr ? name : "");
    JS_FreeValue(runtime.context(), object);
    if (JS_IsException(result)) {
      throw quickjsengine::caughtError(runtime, "QuickJS property get failed.");
    }
    Value value(runtime, result);
    JS_FreeValue(runtime.context(), result);
    return value;
  }
  Value getProperty(Runtime& runtime, const std::string& name) const {
    return getProperty(runtime, name.c_str());
  }
  Value getProperty(Runtime& runtime, const Value& key) const {
    JSValue object = local(runtime);
    JSValue keyValue = key.local(runtime);
    JSAtom atom = JS_ValueToAtom(runtime.context(), keyValue);
    JS_FreeValue(runtime.context(), keyValue);
    JSValue result =
        atom == JS_ATOM_NULL ? JS_UNDEFINED : JS_GetProperty(runtime.context(), object, atom);
    if (atom != JS_ATOM_NULL) {
      JS_FreeAtom(runtime.context(), atom);
    }
    JS_FreeValue(runtime.context(), object);
    if (JS_IsException(result)) {
      throw quickjsengine::caughtError(runtime, "QuickJS property get failed.");
    }
    Value value(runtime, result);
    JS_FreeValue(runtime.context(), result);
    return value;
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
    JSValue object = local(runtime);
    JSValue localValue = value.local(runtime);
    int status =
        JS_SetPropertyStr(runtime.context(), object, name != nullptr ? name : "", localValue);
    JS_FreeValue(runtime.context(), object);
    if (status < 0) {
      throw quickjsengine::caughtError(runtime, "QuickJS property set failed.");
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
    JSValue object = local(runtime);
    JSValue keyValue = key.local(runtime);
    JSAtom atom = JS_ValueToAtom(runtime.context(), keyValue);
    JS_FreeValue(runtime.context(), keyValue);
    JSValue localValue = value.local(runtime);
    int status =
        atom == JS_ATOM_NULL ? -1 : JS_SetProperty(runtime.context(), object, atom, localValue);
    if (atom != JS_ATOM_NULL) {
      JS_FreeAtom(runtime.context(), atom);
    }
    JS_FreeValue(runtime.context(), object);
    if (status < 0) {
      throw quickjsengine::caughtError(runtime, "QuickJS property set failed.");
    }
  }
  bool hasProperty(Runtime& runtime, const char* name) const {
    JSValue object = local(runtime);
    JSAtom atom = JS_NewAtom(runtime.context(), name != nullptr ? name : "");
    int result = JS_HasProperty(runtime.context(), object, atom);
    JS_FreeAtom(runtime.context(), atom);
    JS_FreeValue(runtime.context(), object);
    return result > 0;
  }
  bool isFunction(Runtime& runtime) const {
    JSValue object = local(runtime);
    bool result = JS_IsFunction(runtime.context(), object);
    JS_FreeValue(runtime.context(), object);
    return result;
  }
  bool isArray(Runtime& runtime) const {
    JSValue object = local(runtime);
    bool result = quickjsengine::isArray(runtime.context(), object);
    JS_FreeValue(runtime.context(), object);
    return result;
  }
  bool isArrayBuffer(Runtime& runtime) const {
    JSValue object = local(runtime);
    // JS_IsArrayBuffer2 rather than JS_IsArrayBuffer: the same predicate, but
    // it is the spelling present in every vendored QuickJS here. Classic
    // QuickJS (Android's non-NG build) has no JS_IsArrayBuffer at all.
    bool result = JS_IsArrayBuffer2(runtime.context(), object) != 0;
    JS_FreeValue(runtime.context(), object);
    return result;
  }
  Function asFunction(Runtime& runtime) const;
  Array getArray(Runtime& runtime) const;
  ArrayBuffer getArrayBuffer(Runtime& runtime) const;
  Array getPropertyNames(Runtime& runtime) const;

  template <typename T>
  bool isHostObject(Runtime& runtime) const {
    auto holder = hostObjectHolder(runtime);
    return holder != nullptr && holder->typeToken == quickjsengine::hostObjectTypeToken<T>();
  }
  template <typename T>
  std::shared_ptr<T> getHostObject(Runtime& runtime) const {
    auto holder = hostObjectHolder(runtime);
    if (holder == nullptr || holder->typeToken != quickjsengine::hostObjectTypeToken<T>()) {
      return nullptr;
    }
    return std::static_pointer_cast<T>(holder->hostObject);
  }
  // ---- Native state -------------------------------------------------------
  //
  // See jsi/v8/V8Runtime.h for what this is for. QuickJS gets it from its own
  // per-object opaque slot: objects created for a host constructor's `this`
  // are built with JS_NewObjectProtoClass and a dedicated class that carries
  // no exotic table (so ordinary property access on them is unchanged) but
  // does carry an opaque pointer and a finalizer. Reading the payload back is
  // then a field load plus a class-id compare, where the old __nsWrap path did
  // two full prototype-chain walks.
  //
  // Node-API allows napi_wrap on any object, including a plain `{}` JS made
  // itself, which is not class-backed. Those fall back to a non-enumerable
  // property under an interned atom, read own-only with JS_GetOwnProperty.
  //
  // In both cases the payload is a HostObject released when the object is
  // collected, so finalizer timing is what it was.
  template <typename T>
  void setNativeState(Runtime& runtime, std::shared_ptr<T> state) {
    setNativeStateWithToken(runtime, std::static_pointer_cast<HostObject>(std::move(state)),
                            quickjsengine::hostObjectTypeToken<T>());
  }

  template <typename T>
  std::shared_ptr<T> getNativeState(Runtime& runtime) const {
    std::shared_ptr<HostObject> host =
        nativeStateOf(runtime, quickjsengine::hostObjectTypeToken<T>());
    if (host == nullptr) return nullptr;
    return std::static_pointer_cast<T>(std::move(host));
  }

  JSValue local(Runtime& runtime) const { return JS_DupValue(runtime.context(), storage_->value); }
  operator Value() const { return Value::fromStorage(storage_); }

 protected:
  friend class Value;
  friend class Runtime;
  friend class Function;
  friend class Array;
  friend class ArrayBuffer;
  explicit Object(std::shared_ptr<quickjsengine::ValueStorage> storage)
      : storage_(std::move(storage)) {}
  static Object createFromHostObjectWithToken(Runtime& runtime, std::shared_ptr<HostObject> host,
                                              const void* typeToken);
  void setNativeStateWithToken(Runtime& runtime, std::shared_ptr<HostObject> host,
                               const void* typeToken);
  std::shared_ptr<HostObject> nativeStateOf(Runtime& runtime, const void* typeToken) const;
  quickjsengine::HostObjectHolder* hostObjectHolder(Runtime& runtime) const;
  std::shared_ptr<quickjsengine::ValueStorage> storage_;
};

class Function : public Object {
 public:
  Function() = default;
  explicit Function(Object object) : Object(std::move(object.storage_)) {}
  static Function createFromHostFunction(Runtime& runtime, const PropNameID& name, unsigned int,
                                         HostFunctionType callback);

  // Like createFromHostFunction, but the result may be used with `new`, and its
  // `prototype` property is a writable object.
  //
  // createFromHostFunction builds the function with JS_NewCFunctionData, which
  // produces an object whose constructor bit is clear and which has no
  // `prototype` property at all -- so `new Ctor()` throws "not a constructor"
  // and napi_define_class cannot reach, let alone reassign, `Ctor.prototype`.
  // The Android runtime's MetadataNode chains class prototypes with a plain
  // `ctor.prototype = ...` assignment, so a read-only (or absent) prototype
  // silently drops the whole inheritance chain.
  //
  // The function is built from the engine's own JSClass instead: its JSClassCall
  // receives QuickJS' call flags, so the trampoline can tell `new` from a plain
  // call and synthesise the receiver the way OrdinaryCreateFromConstructor does.
  // No JS wrapper is involved, so no extra frame appears in stack traces --
  // which matters, because the runtime resolves a Worker's module directory by
  // reading `frames[2]`.
  static Function createFromHostConstructor(Runtime& runtime, const PropNameID& name,
                                            unsigned int paramCount,
                                            HostFunctionType callback);
  Value call(Runtime& runtime, const Value* args, size_t count) const {
    JSValue function = local(runtime);
    JSValue global = JS_GetGlobalObject(runtime.context());
    std::vector<JSValue> argv;
    argv.reserve(count);
    for (size_t i = 0; i < count; i++) {
      argv.push_back(args[i].local(runtime));
    }
    JSValue result = JS_Call(runtime.context(), function, global, static_cast<int>(argv.size()),
                             argv.empty() ? nullptr : argv.data());
    for (auto& arg : argv) {
      JS_FreeValue(runtime.context(), arg);
    }
    JS_FreeValue(runtime.context(), global);
    JS_FreeValue(runtime.context(), function);
    if (JS_IsException(result)) {
      throw quickjsengine::caughtError(runtime, "QuickJS function call failed.");
    }
    Value value(runtime, result);
    JS_FreeValue(runtime.context(), result);
    return value;
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
    JSValue function = local(runtime);
    JSValue thisValue = thisObject.local(runtime);
    std::vector<JSValue> argv;
    argv.reserve(count);
    for (size_t i = 0; i < count; i++) {
      argv.push_back(args[i].local(runtime));
    }
    JSValue result = JS_Call(runtime.context(), function, thisValue, static_cast<int>(argv.size()),
                             argv.empty() ? nullptr : argv.data());
    for (auto& arg : argv) {
      JS_FreeValue(runtime.context(), arg);
    }
    JS_FreeValue(runtime.context(), thisValue);
    JS_FreeValue(runtime.context(), function);
    if (JS_IsException(result)) {
      throw quickjsengine::caughtError(runtime, "QuickJS function call failed.");
    }
    Value value(runtime, result);
    JS_FreeValue(runtime.context(), result);
    return value;
  }
  Value callAsConstructor(Runtime& runtime, const Value* args, size_t count) const {
    JSValue function = local(runtime);
    std::vector<JSValue> argv;
    argv.reserve(count);
    for (size_t i = 0; i < count; i++) {
      argv.push_back(args[i].local(runtime));
    }
    JSValue result = JS_CallConstructor(runtime.context(), function, static_cast<int>(argv.size()),
                                        argv.empty() ? nullptr : argv.data());
    for (auto& arg : argv) {
      JS_FreeValue(runtime.context(), arg);
    }
    JS_FreeValue(runtime.context(), function);
    if (JS_IsException(result)) {
      throw quickjsengine::caughtError(runtime, "QuickJS constructor call failed.");
    }
    Value value(runtime, result);
    JS_FreeValue(runtime.context(), result);
    return value;
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
      : Object(std::make_shared<quickjsengine::ValueStorage>(
            quickjsengine::ValueStorage::Kind::QuickJS)) {
    storage_->context = runtime.context();
    storage_->value = JS_NewArray(runtime.context());
    JS_SetPropertyStr(runtime.context(), storage_->value, "length",
                      JS_NewUint32(runtime.context(), static_cast<uint32_t>(size)));
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
    JSValue object = local(runtime);
    JSValue result = JS_GetPropertyUint32(runtime.context(), object, static_cast<uint32_t>(index));
    JS_FreeValue(runtime.context(), object);
    if (JS_IsException(result)) {
      throw quickjsengine::caughtError(runtime, "QuickJS array get failed.");
    }
    Value value(runtime, result);
    JS_FreeValue(runtime.context(), result);
    return value;
  }
  void setValueAtIndex(Runtime& runtime, size_t index, const Value& value) {
    JSValue object = local(runtime);
    JSValue localValue = value.local(runtime);
    int status =
        JS_SetPropertyUint32(runtime.context(), object, static_cast<uint32_t>(index), localValue);
    JS_FreeValue(runtime.context(), object);
    if (status < 0) {
      throw quickjsengine::caughtError(runtime, "QuickJS array set failed.");
    }
  }
  void setValueAtIndex(Runtime& runtime, size_t index, const String& value) {
    setValueAtIndex(runtime, index, Value(runtime, value));
  }
};

class BigInt {
 public:
  BigInt() = default;
  BigInt(Runtime& runtime, JSValue value)
      : storage_(std::make_shared<quickjsengine::ValueStorage>(
            quickjsengine::ValueStorage::Kind::QuickJS)) {
    storage_->context = runtime.context();
    storage_->value = JS_DupValue(runtime.context(), value);
  }
  static BigInt fromInt64(Runtime& runtime, int64_t value) {
    JSValue result = JS_NewBigInt64(runtime.context(), value);
    BigInt bigint(runtime, result);
    JS_FreeValue(runtime.context(), result);
    return bigint;
  }
  static BigInt fromUint64(Runtime& runtime, uint64_t value) {
    JSValue result = JS_NewBigUint64(runtime.context(), value);
    BigInt bigint(runtime, result);
    JS_FreeValue(runtime.context(), result);
    return bigint;
  }
  String toString(Runtime& runtime, int) const;
  JSValue local(Runtime& runtime) const { return JS_DupValue(runtime.context(), storage_->value); }
  operator Value() const { return Value::fromStorage(storage_); }

 private:
  friend class Value;
  std::shared_ptr<quickjsengine::ValueStorage> storage_;
};

class ArrayBuffer : public Object {
 public:
  ArrayBuffer(Runtime& runtime, std::shared_ptr<MutableBuffer> buffer)
      : Object(std::make_shared<quickjsengine::ValueStorage>(
            quickjsengine::ValueStorage::Kind::QuickJS)) {
    auto* holder = new quickjsengine::ArrayBufferHolder(std::move(buffer));
    storage_->context = runtime.context();
    storage_->value = JS_NewArrayBuffer(
        runtime.context(), holder->buffer->data(), holder->buffer->size(),
        [](JSRuntime*, void* opaque, void*) {
          delete static_cast<quickjsengine::ArrayBufferHolder*>(opaque);
        },
        holder, false);
  }
  explicit ArrayBuffer(Object object) : Object(std::move(object.storage_)) {}
  size_t size(Runtime& runtime) const {
    JSValue object = local(runtime);
    size_t size = 0;
    JS_GetArrayBuffer(runtime.context(), &size, object);
    JS_FreeValue(runtime.context(), object);
    return size;
  }
  uint8_t* data(Runtime& runtime) const {
    JSValue object = local(runtime);
    size_t size = 0;
    uint8_t* data = JS_GetArrayBuffer(runtime.context(), &size, object);
    JS_FreeValue(runtime.context(), object);
    return data;
  }
};
}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_QUICKJS

#endif  // NATIVESCRIPT_JSI_QUICKJS_QUICKJS_RUNTIME_H
