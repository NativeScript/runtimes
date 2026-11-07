#ifndef NATIVESCRIPT_JSI_V8_V8_RUNTIME_H
#define NATIVESCRIPT_JSI_V8_V8_RUNTIME_H

// V8 behind the JSI-shaped `nativescript::engine` API.
//
// This header is platform-neutral: it depends on V8 and the C++ standard
// library, and on nothing from Foundation, the Objective-C runtime, or the
// Apple metadata format. Android compiles it as-is.
//
// The Apple bridge's extra baggage lives in ffi/objc/v8/NativeApiV8Runtime.h,
// which includes this file. Apple sources keep including that path unchanged.

#ifdef TARGET_ENGINE_V8

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
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jsi/shared/RuntimeCleanupRegistry.h"
#include "v8.h"

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

// Mirrors jsi::JSError: it carries the thrown *value*, not just its text.
//
// The message alone is lossy in a way that matters. The Android runtime
// attaches properties to the errors it throws -- NativeScriptException hangs
// the originating Java throwable off `nativeException` -- and rebuilding an
// error from its stringified form silently drops them, along with the original
// stack. Anything that needs `e.nativeException.getStackTrace()` then sees
// undefined.
//
// The payload is optional: engine operations that fail without a JS exception
// (and the engines not yet wired to carry one) still construct message-only
// JSErrors, and callers check value() before using it.
//
// Value is incomplete here, which is fine: constructing the shared_ptr is the
// only operation that needs the complete type, so that constructor is defined
// further down. Copy and destruction type-erase through the deleter captured
// at construction, which is what makes this legal to throw.
class JSError : public std::runtime_error {
 public:
  JSError(Runtime&, const std::string& message) : std::runtime_error(message) {}
  explicit JSError(const std::string& message) : std::runtime_error(message) {}

  // Defined after Value; see above.
  JSError(Runtime& runtime, const std::string& message, const Value& value,
          std::string stack);

  // Null when this error was built from a message alone.
  const Value* value() const { return value_.get(); }

  // The engine's stack for the original throw, empty when unavailable.
  const std::string& stack() const { return stack_; }

 private:
  std::shared_ptr<Value> value_;
  std::string stack_;
};

namespace v8engine {
struct WeakStorage;
}  // namespace v8engine

// Mirrors jsi::WeakObject. lock() returns undefined once the referent has been
// collected.
class WeakObject {
 public:
  WeakObject() = default;
  WeakObject(Runtime& runtime, const Value& value);
  Value lock(Runtime& runtime) const;
  bool empty() const;  // defined after WeakStorage
  void reset() { storage_.reset(); }

 private:
  std::shared_ptr<v8engine::WeakStorage> storage_;
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

// A property name, which may be backed by the engine's own handle rather than
// by a string.
//
// This used to hold a std::string unconditionally, so a property interceptor --
// which receives a v8::Local<v8::Name> -- had to run v8::String::Utf8Value and
// build a std::string on the way in, on every intercepted access. Most
// consumers then either hand the name straight back to the engine or compare
// it, so that conversion was frequently pure waste: an instance field read
// converted the same name six times between V8 and the runtime.
//
// Holding the handle defers the conversion to whoever actually wants text, and
// lets a consumer that only forwards the name skip it entirely.
//
// Lifetime: a handle-backed PropNameID borrows a v8::Local and is therefore
// only valid inside the HandleScope that produced it. That suits the dispatch
// it exists for -- the engine builds one per interception and it dies with the
// call. Anything that must outlive the scope (getPropertyNames' return vector)
// is built from a string and has no handle at all.
class PropNameID {
 public:
  PropNameID() = default;
  explicit PropNameID(std::string value)
      : value_(std::move(value)), hasUtf8_(true) {}

  PropNameID(v8::Isolate* isolate, v8::Local<v8::Name> name)
      : isolate_(isolate), name_(name) {}

  static PropNameID forAscii(Runtime&, const char* value) {
    return PropNameID(value != nullptr ? std::string(value) : std::string());
  }

  static PropNameID forAscii(Runtime&, const std::string& value) { return PropNameID(value); }

  // Converted at most once, and only if asked. Still returns by value: callers
  // on the Apple side assign it straight into a std::string, and handing out a
  // reference into a temporary PropNameID would be a footgun for no gain.
  std::string utf8(Runtime&) const { return utf8(); }

  std::string utf8() const {
    if (!hasUtf8_) {
      if (isolate_ != nullptr && !name_.IsEmpty()) {
        v8::String::Utf8Value text(isolate_, name_);
        if (*text != nullptr) value_.assign(*text, text.length());
      }
      hasUtf8_ = true;
    }
    return value_;
  }

  // Empty unless this name came from the engine; see the lifetime note above.
  v8::Local<v8::Name> local() const { return name_; }
  bool hasLocal() const { return !name_.IsEmpty(); }

 private:
  v8::Isolate* isolate_ = nullptr;
  v8::Local<v8::Name> name_;
  mutable std::string value_;
  mutable bool hasUtf8_ = false;
};

class HostObject {
 public:
  virtual ~HostObject() = default;
  virtual Value get(Runtime& runtime, const PropNameID& name);
  virtual bool set(Runtime& runtime, const PropNameID& name, const Value& value);
  virtual std::vector<PropNameID> getPropertyNames(Runtime& runtime);

  // Indexed access, for a host object that is really an indexable collection.
  //
  // Reached through the named form, an index arrives as a decimal string that
  // the host object has to parse back into an integer -- a heap allocation and
  // a parse on every `obj[i]`. These take the integer. The defaults reproduce
  // the named form exactly, so overriding them is optional.
  //
  // An engine with a dedicated indexed hook (V8) always routes an index here.
  // An engine that delivers an index as a property name consults
  // hasIndexedAccess() first, because for it the named path also carries the
  // prototype handling that a non-indexable host object still needs.
  virtual Value getValueAtIndex(Runtime& runtime, uint32_t index);
  virtual bool setValueAtIndex(Runtime& runtime, uint32_t index, const Value& value);

  bool hasIndexedAccess() const { return indexedAccess_; }
  void setIndexedAccess(bool value) { indexedAccess_ = value; }

  // The JS object standing for this host object, valid ONLY for the duration of
  // the call the engine is currently dispatching.
  //
  // Handed in per call rather than stored, and deliberately non-owning. A
  // HostObject that holds an owned handle to its own wrapper is a strong
  // self-cycle: the engine keeps the wrapper weak so collection can fire the
  // finalizer, but the cycle makes the object permanently reachable and the
  // weak callback never runs. On Android that meant no host object was ever
  // collected, so ObjectManager never called makeInstanceWeak and every Java
  // instance stayed strongly held.
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

namespace v8engine {

struct RuntimeState {
  explicit RuntimeState(v8::Isolate* isolate, v8::Local<v8::Context> context) : isolate(isolate) {
    this->context.Reset(isolate, context);
  }

  ~RuntimeState() {
    nativeClassArgumentLast.value.Reset();
    nativeClassArgumentLast.nativeClass = nullptr;
    for (auto& entry : nativeClassArgumentCache) {
      entry.value.Reset();
      entry.nativeClass = nullptr;
    }
    nativeSelectorArgumentLast.value.Reset();
    nativeSelectorArgumentLast.selector = nullptr;
    for (auto& entry : nativeSelectorArgumentCache) {
      entry.value.Reset();
      entry.selector = nullptr;
    }
    nativeStateKey.Reset();
    context.Reset();
  }

  v8::Local<v8::Context> localContext() const {
    v8::Local<v8::Context> ctx = context.Get(isolate);
    return ctx.IsEmpty() ? isolate->GetCurrentContext() : ctx;
  }

  v8::Isolate* isolate = nullptr;
  v8::Global<v8::Context> context;
  // The private-symbol key every native-state slot hangs off. Private symbols
  // are looked up own-only (V8 configures the LookupIterator with
  // OWN_SKIP_INTERCEPTOR for them) and are invisible to JS, which is exactly
  // what a native payload slot wants: no prototype-chain walk on a miss, no
  // interceptor dispatch, and nothing observable from script.
  v8::Global<v8::Private> nativeStateKey;
  v8::Global<v8::ObjectTemplate> hostObjectTemplate;
  v8::Global<v8::ObjectTemplate> nativeObjectTemplate;  // kNonMasking for instances
  std::vector<std::shared_ptr<void>> retainedNativeData;
  // Argument-marshalling caches: they memoise "this JS value denotes that
  // native type / method" so a repeated call does not re-resolve it.
  //
  // Both native handles are held opaquely so this struct stays platform
  // neutral. They are Class and SEL on Apple, and the JNI equivalents
  // (jclass, jmethodID) on Android -- all four are pointers. The bridge that
  // populates and reads these casts back to its own types; see
  // ffi/objc/v8/NativeApiV8Marshalling.mm for the Apple side.
  struct NativeClassArgumentCacheEntry {
    v8::Global<v8::Value> value;
    const void* nativeClass = nullptr;
  };
  NativeClassArgumentCacheEntry nativeClassArgumentLast;
  NativeClassArgumentCacheEntry nativeClassArgumentCache[4];
  size_t nativeClassArgumentCacheNext = 0;
  struct NativeSelectorArgumentCacheEntry {
    v8::Global<v8::Value> value;
    const void* selector = nullptr;
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

  // Reset the owned handle and remember which isolate it belongs to.
  //
  // The isolate is recorded because reading the handle back needs one, and the
  // only other way to obtain it -- v8::Isolate::GetCurrent() -- is a
  // thread-local read, which is not cheap here: V8 is linked as a prebuilt
  // archive compiled with *emulated* TLS, so GetCurrent() is a PLT call into
  // __emutls_get_address followed by pthread_getspecific. In the V8-13
  // marshalling profile that pair cost 4.07% + half of 2.13% of the benchmark
  // thread, almost all of it reached through Value's type predicates
  // (isString, isUndefined, ...), which called GetCurrent() once per question
  // asked. No build flag fixes it -- the emutls calls are inside the prebuilt
  // V8 -- so the fix is to stop asking.
  //
  // Storing it is also strictly more correct than GetCurrent(): a handle
  // belongs to the isolate that created it, not to whichever isolate happens
  // to be entered on this thread. Workers have their own.
  void reset(v8::Isolate* isolateIn, v8::Local<v8::Value> local) {
    isolate = isolateIn;
    value.Reset(isolateIn, local);
  }

  Kind kind = Kind::Undefined;
  bool boolValue = false;
  double numberValue = 0;
  // Null until an owned handle is stored; readers fall back to GetCurrent(),
  // which keeps any caller that Resets `value` directly working unchanged.
  v8::Isolate* isolate = nullptr;
  v8::Global<v8::Value> value;
  v8::Local<v8::Value> borrowedValue;

  // The isolate this storage's handle belongs to, or the current one if this
  // storage was filled without going through reset().
  v8::Isolate* isolateOrCurrent() const {
    return isolate != nullptr ? isolate : v8::Isolate::GetCurrent();
  }
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

// A weak handle: does not keep its referent alive.
//
// Node-API's napi_ref with refcount 0 is weak, and the Android runtime depends
// on it -- ObjectManager holds each host-object proxy through one so that
// dropping it from JS triggers the finalizer that calls makeInstanceWeak.
// Approximating it as strong pins every Java instance for the process
// lifetime.
struct WeakStorage {
  v8::Global<v8::Value> value;
  ~WeakStorage() { value.Reset(); }
};

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

// Property *names* want kInternalized, not kNormal.
//
// A non-internalized key forces V8 to hash and internalize the string inside
// every Get/Set/Has, and defeats the descriptor-array pointer-compare fast
// path. The Node-API implementation this replaces internalizes every property
// name; we did not, on ~250 named-property call sites in the runtime.
inline v8::Local<v8::String> makeV8Name(v8::Isolate* isolate, const char* value) {
  return v8::String::NewFromUtf8(isolate, value != nullptr ? value : "",
                                 v8::NewStringType::kInternalized)
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

// Rebuild a JS error from its stringified form, preserving its type.
//
// currentExceptionMessage stringifies the caught exception, which for a JS
// error yields "TypeError: msg" -- the type is in the text. Everything that
// crosses back into JS used to go through v8::Exception::Error, so a
// SyntaxError thrown by a module became a plain Error by the time JS caught it.
inline v8::Local<v8::Value> makeV8Error(v8::Isolate* isolate, const std::string& text) {
  // Direct calls rather than a table of function pointers: the v8::Exception
  // factories gained an optional second parameter in newer V8, so their
  // signatures differ across the versions this header serves.
  const auto rest = [&](size_t prefix) {
    return makeV8String(isolate, text.substr(prefix));
  };
  if (text.compare(0, 12, "RangeError: ") == 0) return v8::Exception::RangeError(rest(12));
  if (text.compare(0, 16, "ReferenceError: ") == 0) return v8::Exception::ReferenceError(rest(16));
  if (text.compare(0, 13, "SyntaxError: ") == 0) return v8::Exception::SyntaxError(rest(13));
  if (text.compare(0, 11, "TypeError: ") == 0) return v8::Exception::TypeError(rest(11));
  return v8::Exception::Error(makeV8String(isolate, text));
}

// Declared here, defined after Value: a JSError may carry the original thrown
// value, and rethrowing that verbatim is what preserves its identity and
// properties. Anything else (including every Apple-side JSError, which is
// message-only) falls back to rebuilding from the text.
inline void throwV8Exception(v8::Isolate* isolate, const JSError& error);
inline void throwV8Exception(v8::Isolate* isolate, const std::exception& exception);

}  // namespace v8engine

class Runtime {
 public:
  Runtime(v8::Isolate* isolate, v8::Local<v8::Context> context)
      : state_(std::make_shared<v8engine::RuntimeState>(isolate, context)) {}

  explicit Runtime(std::shared_ptr<v8engine::RuntimeState> state) : state_(std::move(state)) {}

  v8::Isolate* isolate() const { return state_->isolate; }
  v8::Local<v8::Context> context() const { return state_->localContext(); }
  v8engine::RuntimeState* rawState() const { return state_.get(); }

  // A stable, per-runtime identity.
  //
  // engine::Runtime is a value wrapper around shared engine state, and the
  // host-function trampolines construct a fresh one on the stack for every
  // callback. So `&runtime` is NOT stable and must never be used as a map key;
  // this is. The pointer is opaque and only ever compared or hashed.
  const void* identity() const { return state_.get(); }

  // See RuntimeState::nativeStateKey. Created on first use so a runtime that
  // never wraps anything never allocates it.
  v8::Local<v8::Private> nativeStateKey() const {
    if (state_->nativeStateKey.IsEmpty()) {
      state_->nativeStateKey.Reset(
          state_->isolate,
          v8::Private::New(state_->isolate,
                           v8engine::makeV8Name(state_->isolate, "__nsNativeState")));
    }
    return state_->nativeStateKey.Get(state_->isolate);
  }
  std::shared_ptr<v8engine::RuntimeState> state() const { return state_; }

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
    if (storage_->kind == v8engine::ValueStorage::Kind::V8Borrowed) {
      return storage_->borrowedValue.As<v8::String>();
    }
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
      storage_->reset(runtime.isolate(), value.borrowedValue_);
      kind_ = v8engine::ValueStorage::Kind::V8;
      return;
    }
    kind_ = value.kind_;
    boolValue_ = value.boolValue_;
    numberValue_ = value.numberValue_;
    isolate_ = value.isolate_;
    borrowedValue_ = value.borrowedValue_;
    storage_ = value.storage_;
  }
  // Must promote exactly as the copy constructor above does. Both spell
  // "make a Value owned by this Runtime", and that has to hold whatever the
  // argument's value category is: a borrowed Value is a bare v8::Local, alive
  // only until its HandleScope closes, so carrying the tag through unchanged
  // produces a handle that dangles the moment the scope unwinds.
  //
  // This was invisible while every caller passed an lvalue -- overload
  // resolution picked the copy constructor and promoted. The first caller to
  // pass a temporary (the napi shim's refValue, once napi_value became a v8
  // handle slot) got this one instead, stored the borrowed handle in a
  // napi_ref, and crashed in GlobalizeReference on the next read.
  Value(Runtime& runtime, Value&& value)
      : kind_(value.kind_),
        boolValue_(value.boolValue_),
        numberValue_(value.numberValue_),
        isolate_(value.isolate_),
        borrowedValue_(value.borrowedValue_),
        storage_(std::move(value.storage_)) {
    if (kind_ == v8engine::ValueStorage::Kind::V8Borrowed) {
      storage_ = std::make_shared<v8engine::ValueStorage>(
          v8engine::ValueStorage::Kind::V8);
      storage_->reset(runtime.isolate(), borrowedValue_);
      kind_ = v8engine::ValueStorage::Kind::V8;
    }
  }
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
  // Like asObject, but a borrowed Value yields a borrowed Object; see
  // Object::getPropertyBorrowed for the lifetime contract.
  Object asObjectBorrowed(Runtime& runtime) const;
  String asString(Runtime& runtime) const;
  BigInt getBigInt(Runtime& runtime) const;

  // Read the UTF-8 of a string value without materialising a String.
  //
  // asString(rt).utf8(rt) is the obvious spelling and it is what every caller
  // used, but String is an *owning* type: constructing one costs a
  // make_shared<ValueStorage> plus a GlobalHandles::Create, and destroying it
  // costs a NodeSpace::Release plus the free -- for a handle that is dead two
  // statements later. Reading straight off the Local skips all four. The Local
  // is already rooted in the enclosing HandleScope, which is what makes this
  // safe rather than merely cheaper.
  std::string utf8(Runtime& runtime) const;

  // Create a string value without materialising an owning handle.
  //
  // The mirror image of utf8(): String::createFromUtf8 globalizes the fresh
  // v8::Local so the String can outlive the scope, but a value that is handed
  // straight back to the engine (a marshalled Java string, a property name)
  // never leaves the HandleScope it was made in. Borrowing the Local instead
  // removes the make_shared and the global handle entirely. On V8-13 that pair
  // was 81% of all operator new traffic on the marshalling thread.
  //
  // Contract: the result must not outlive the current HandleScope. Callers that
  // need an owning value keep using String::createFromUtf8.
  static Value createStringFromUtf8(Runtime& runtime, const char* data, size_t length);

  v8::Local<v8::Value> local(Runtime& runtime) const;

  // Only the isolate is ever needed, and some callers (rethrowing a caught
  // JSError from a host-function trampoline) have one without a Runtime.
  v8::Local<v8::Value> local(v8::Isolate* isolate) const {
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
    storage_->reset(runtime.isolate(), value);
  }

  static Value borrowed(Runtime& runtime, v8::Local<v8::Value> value) {
    return borrowed(runtime.isolate(), value);
  }

  // Records the isolate the handle belongs to, for the same reason
  // ValueStorage::reset does: getNumber()/getBool() on a borrowed value need
  // one, and the only other source is v8::Isolate::GetCurrent(), which lands in
  // __emutls_get_address inside the prebuilt V8. Callers that have an isolate
  // should hand it over rather than make the read pay for it.
  static Value borrowed(v8::Isolate* isolate, v8::Local<v8::Value> value) {
    Value result;
    result.kind_ = v8engine::ValueStorage::Kind::V8Borrowed;
    result.isolate_ = isolate;
    result.borrowedValue_ = value;
    return result;
  }

  // The Runtime is not used -- borrowing is just tagging a Local -- and callers
  // that have only a handle should not have to invent one. The napi shim's
  // napi_value -> Value conversion is on the hottest path there is and has no
  // Runtime in scope.
  static Value borrowed(v8::Local<v8::Value> value) {
    return borrowed(static_cast<v8::Isolate*>(nullptr), value);
  }

  // Access the shared storage (for Object/Function/Array interop)
  std::shared_ptr<v8engine::ValueStorage> storage() const { return storage_; }

  static Value fromStorage(std::shared_ptr<v8engine::ValueStorage> s) {
    Value v;
    v.kind_ = s->kind;
    v.boolValue_ = s->boolValue;
    v.numberValue_ = s->numberValue;
    v.isolate_ = s->isolate;
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
  // Only meaningful for the borrowed kind; the owned kind keeps its isolate in
  // ValueStorage. Null means "ask the thread", which is the old behaviour.
  v8::Isolate* isolate_ = nullptr;
  v8::Local<v8::Value> borrowedValue_;
  std::shared_ptr<v8engine::ValueStorage> storage_;

  v8::Isolate* borrowedIsolate() const {
    return isolate_ != nullptr ? isolate_ : v8::Isolate::GetCurrent();
  }
};

inline v8::Local<v8::Value> Value::local(Runtime& runtime) const {
  return local(runtime.isolate());
}

// Defined after Value for the same reason as JSError's payload ctor.
inline WeakObject::WeakObject(Runtime& runtime, const Value& value)
    : storage_(std::make_shared<v8engine::WeakStorage>()) {
  storage_->value.Reset(runtime.isolate(), value.local(runtime));
  // Parameterless SetWeak: no finalizer callback, the Global simply empties
  // when the referent is collected. That is exactly napi_ref's weak contract --
  // napi_get_reference_value then reports undefined.
  storage_->value.SetWeak();
}

inline bool WeakObject::empty() const {
  return storage_ == nullptr || storage_->value.IsEmpty();
}

inline Value WeakObject::lock(Runtime& runtime) const {
  if (storage_ == nullptr || storage_->value.IsEmpty()) {
    return Value::undefined();
  }
  return Value(runtime, storage_->value.Get(runtime.isolate()));
}

// Defined here rather than with the class: constructing the shared_ptr needs
// Value to be complete.
inline JSError::JSError(Runtime& runtime, const std::string& message,
                        const Value& value, std::string stack)
    : std::runtime_error(message),
      value_(std::make_shared<Value>(runtime, value)),
      stack_(std::move(stack)) {}

namespace v8engine {

// Turn a caught V8 exception into a JSError that carries the thrown value.
//
// Everything the engine layer throws on a failed operation should come through
// here, so the value survives to whoever catches it. The Node-API shim stores
// it as the pending exception verbatim, which is what keeps runtime-attached
// properties (NativeScriptException's `nativeException`) and the original stack
// intact -- rebuilding an error from its message drops both.
inline JSError caughtError(Runtime& runtime, v8::TryCatch& tryCatch) {
  const std::string message = currentExceptionMessage(runtime.isolate(), tryCatch);
  if (!tryCatch.HasCaught()) {
    return JSError(runtime, message);
  }
  v8::Local<v8::Value> caught = tryCatch.Exception();
  if (caught.IsEmpty()) {
    return JSError(runtime, message);
  }
  // The stack is read eagerly: `error.stack` is a lazy accessor, and the
  // TryCatch that makes it resolvable is gone by the time anyone catches this.
  std::string stack;
  if (caught->IsObject()) {
    v8::Local<v8::Value> stackValue;
    if (caught.As<v8::Object>()
            ->Get(runtime.context(), makeV8String(runtime.isolate(), "stack"))
            .ToLocal(&stackValue) &&
        stackValue->IsString()) {
      stack = toUtf8(runtime.isolate(), stackValue);
    }
  }
  // Owned, not borrowed: this outlives the handle scope it was caught in.
  return JSError(runtime, message, Value(runtime, caught), std::move(stack));
}

}  // namespace v8engine

namespace v8engine {

// Overloaded rather than dispatched with dynamic_cast: this tree builds with
// -fno-rtti. Callers catch JSError explicitly before std::exception, so the
// value-carrying overload is chosen at the catch site.
inline void throwV8Exception(v8::Isolate* isolate, const JSError& error) {
  if (const Value* thrown = error.value()) {
    // Only if it is a real value; see exceptionFrom in the Node-API shim for
    // why an undefined payload must not displace the message.
    if (!thrown->isUndefined() && !thrown->isNull()) {
      // Rethrow the original object, so its identity and any properties the
      // runtime attached to it survive.
      isolate->ThrowException(thrown->local(isolate));
      return;
    }
  }
  isolate->ThrowException(makeV8Error(isolate, error.what()));
}

inline void throwV8Exception(v8::Isolate* isolate, const std::exception& exception) {
  isolate->ThrowException(makeV8Error(isolate, exception.what()));
}

}  // namespace v8engine

class Object {
 public:
  Object() = default;
  explicit Object(Runtime& runtime)
      : storage_(std::make_shared<v8engine::ValueStorage>(v8engine::ValueStorage::Kind::V8)) {
    storage_->reset(runtime.isolate(), v8::Object::New(runtime.isolate()));
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
                       v8engine::makeV8Name(runtime.isolate(), name));
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
      throw v8engine::caughtError(runtime, tryCatch);
    }
    return Value(runtime, result);
  }

  // Reads that hand back a borrowed handle rather than an owned one.
  //
  // getProperty returns an *owned* Value: a shared_ptr<ValueStorage> plus a
  // v8::Global, created and then released again as soon as the caller is done.
  // For a caller whose result is already rooted by the enclosing HandleScope
  // that is pure overhead -- and it is measurable overhead, because a v8::Global
  // is a GC root that every scavenge has to scan. In the V8-13 marshalling
  // profile GlobalHandles::Create, GlobalizeReference and NodeSpace::Release
  // together were 11.3% of the benchmark thread.
  //
  // The Node-API shim is exactly such a caller: a napi_value is defined to stay
  // valid only until its handle scope closes, and every napi handle scope is
  // strictly nested inside the v8::HandleScope that NapiScope opens on entry
  // from the host. So the shim's read paths use these.
  //
  // The result is valid only inside the HandleScope that produced it. Anything
  // that must outlive it goes through Value(Runtime&, const Value&), which
  // promotes -- napi_create_reference and napi_throw already do.
  Value getPropertyBorrowed(Runtime& runtime, v8::Local<v8::Value> key) const {
    v8::TryCatch tryCatch(runtime.isolate());
    v8::Local<v8::Value> result;
    if (!local(runtime)->Get(runtime.context(), key).ToLocal(&result)) {
      throw v8engine::caughtError(runtime, tryCatch);
    }
    return Value::borrowed(runtime.isolate(), result);
  }

  Value getPropertyBorrowed(Runtime& runtime, const char* name) const {
    return getPropertyBorrowed(runtime, v8engine::makeV8Name(runtime.isolate(), name));
  }

  Value getPropertyBorrowed(Runtime& runtime, const Value& key) const {
    return getPropertyBorrowed(runtime, key.local(runtime));
  }

  Object getPropertyAsObject(Runtime& runtime, const char* name) const {
    return getProperty(runtime, name).asObject(runtime);
  }

  Function getPropertyAsFunction(Runtime& runtime, const char* name) const;

  void setProperty(Runtime& runtime, const char* name, const Value& value) {
    setProperty(runtime, v8engine::makeV8Name(runtime.isolate(), name),
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
      throw v8engine::caughtError(runtime, tryCatch);
    }
  }

  bool hasProperty(Runtime& runtime, const char* name) const {
    v8::TryCatch tryCatch(runtime.isolate());
    return local(runtime)
        ->Has(runtime.context(),
              v8engine::makeV8Name(runtime.isolate(), name))
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

  // ---- Native state -------------------------------------------------------
  //
  // An opaque native payload attached to a JS object, mirroring
  // jsi::Object::setNativeState/getNativeState. The point is that reading it
  // back is *not* a JS property lookup: napi_unwrap runs on every marshalled
  // field access, and doing it as `hasProperty("__nsWrap") +
  // getProperty("__nsWrap")` cost two full prototype-chain walks per call.
  //
  // The payload is still held by a HostObject, so its lifetime and finaliser
  // timing are byte-for-byte what they were when the slot was a named
  // property: the engine's weak callback releases the holder, the shared_ptr
  // drops, and ~T runs the napi_finalize. Only the *key* changed.
  template <typename T>
  void setNativeState(Runtime& runtime, std::shared_ptr<T> state) {
    Object holder = Object::createFromHostObject<T>(runtime, std::move(state));
    v8::TryCatch tryCatch(runtime.isolate());
    if (!local(runtime)
             ->SetPrivate(runtime.context(), runtime.nativeStateKey(), holder.local(runtime))
             .FromMaybe(false)) {
      throw v8engine::caughtError(runtime, tryCatch);
    }
  }

  template <typename T>
  std::shared_ptr<T> getNativeState(Runtime& runtime) const {
    v8::Local<v8::Value> holder;
    if (!local(runtime)
             ->GetPrivate(runtime.context(), runtime.nativeStateKey())
             .ToLocal(&holder) ||
        !holder->IsObject()) {
      return nullptr;
    }
    v8::Local<v8::Object> holderObject = holder.As<v8::Object>();
    if (holderObject->InternalFieldCount() < 1) return nullptr;
    auto* record = static_cast<v8engine::HostObjectHolder*>(
        holderObject->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
    if (record == nullptr || record->typeToken != v8engine::hostObjectTypeToken<T>()) {
      return nullptr;
    }
    return std::static_pointer_cast<T>(record->hostObject);
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
    return static_cast<v8engine::HostObjectHolder*>(object->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
  }

  std::shared_ptr<v8engine::ValueStorage> storage_;
};

class Function : public Object {
 public:
  Function() = default;
  explicit Function(Object object) : Object(std::move(object.storage_)) {}

  static Function createFromHostFunction(Runtime& runtime, const PropNameID& name, unsigned int,
                                         HostFunctionType callback);

  // Like createFromHostFunction, but the result is a constructor whose
  // `prototype` property is writable.
  //
  // createFromHostFunction builds the function from a v8::FunctionTemplate, and
  // V8 makes such a function's `prototype` non-writable and non-configurable.
  // Node-API callers expect otherwise -- V8's own Node-API implementation uses
  // v8::Function::New, whose `prototype` behaves like a normal function's --
  // and the Android runtime relies on it: MetadataNode chains class prototypes
  // with a plain `ctor.prototype = ...` assignment, which against a template
  // function fails *silently* in sloppy mode and drops the whole inheritance
  // chain.
  static Function createFromHostConstructor(Runtime& runtime, const PropNameID& name,
                                            unsigned int paramCount,
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
      throw v8engine::caughtError(runtime, tryCatch);
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
  // JS call passing the array and the number. Every such call site in the
  // Node-API shim was quietly broken, which surfaced as "Object.defineProperty
  // called on non-object" and poisoned the env with a latched pending
  // exception. Deducing the count makes this overload exact too, and partial
  // ordering then prefers it over the pack.
  template <size_t N, typename Count,
            typename = std::enable_if_t<std::is_integral_v<std::decay_t<Count>>>>
  Value call(Runtime& runtime, const Value (&args)[N], Count count) const {
    return call(runtime, static_cast<const Value*>(args),
                static_cast<size_t>(count));
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
      throw v8engine::caughtError(runtime, tryCatch);
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
      throw v8engine::caughtError(runtime, tryCatch);
    }
    return Value(runtime, result);
  }

  Value callAsConstructor(Runtime& runtime, std::nullptr_t, size_t) const {
    return callAsConstructor(runtime, static_cast<const Value*>(nullptr), 0);
  }

  // Count deduced, matching call() above: with a fixed size_t an int literal
  // made the variadic pack win overload resolution and silently constructed
  // with (array, count) as two JS arguments.
  template <size_t N, typename Count,
            typename = std::enable_if_t<std::is_integral_v<std::decay_t<Count>>>>
  Value callAsConstructor(Runtime& runtime, const Value (&args)[N], Count count) const {
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
    storage_->reset(runtime.isolate(),
                    v8::Array::New(runtime.isolate(), static_cast<int>(size)));
  }

  explicit Array(Object object) : Object(std::move(object.storage_)) {}

  size_t size(Runtime& runtime) const { return local(runtime).As<v8::Array>()->Length(); }

  Value getValueAtIndex(Runtime& runtime, size_t index) const {
    v8::TryCatch tryCatch(runtime.isolate());
    v8::Local<v8::Value> result;
    if (!local(runtime)->Get(runtime.context(), static_cast<uint32_t>(index)).ToLocal(&result)) {
      throw v8engine::caughtError(runtime, tryCatch);
    }
    return Value(runtime, result);
  }

  // See Object::getPropertyBorrowed for the lifetime contract.
  Value getValueAtIndexBorrowed(Runtime& runtime, size_t index) const {
    v8::TryCatch tryCatch(runtime.isolate());
    v8::Local<v8::Value> result;
    if (!local(runtime)->Get(runtime.context(), static_cast<uint32_t>(index)).ToLocal(&result)) {
      throw v8engine::caughtError(runtime, tryCatch);
    }
    return Value::borrowed(runtime.isolate(), result);
  }

  void setValueAtIndex(Runtime& runtime, size_t index, const Value& value) {
    v8::TryCatch tryCatch(runtime.isolate());
    if (!local(runtime)
             ->Set(runtime.context(), static_cast<uint32_t>(index), value.local(runtime))
             .FromMaybe(false)) {
      throw v8engine::caughtError(runtime, tryCatch);
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
    storage_->reset(runtime.isolate(), value);
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
      throw v8engine::caughtError(runtime, tryCatch);
    }
    return String(runtime, result);
  }

  v8::Local<v8::BigInt> local(Runtime& runtime) const {
    if (storage_->kind == v8engine::ValueStorage::Kind::V8Borrowed) {
      return storage_->borrowedValue.As<v8::BigInt>();
    }
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
    storage_->reset(runtime.isolate(), arrayBuffer);
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

#endif  // NATIVESCRIPT_JSI_V8_V8_RUNTIME_H
