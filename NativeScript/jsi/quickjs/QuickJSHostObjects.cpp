#include "jsi/quickjs/QuickJSRuntime.h"
#include "../shared/NativeApiStackValueArray.h"

#ifdef TARGET_ENGINE_QUICKJS

namespace nativescript {
class NativeApiObjectHostObject;
}

namespace nativescript {
namespace engine {

namespace quickjsengine {

JSClassID gHostClassId = 0;
JSClassID gFunctionClassId = 0;
JSClassID gNativeStateClassId = 0;

namespace {
std::mutex& runtimeStatesMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::unordered_map<JSContext*, std::shared_ptr<RuntimeState>>& runtimeStates() {
  static auto* states = new std::unordered_map<JSContext*, std::shared_ptr<RuntimeState>>();
  return *states;
}
}  // namespace

std::shared_ptr<RuntimeState> stateForContext(JSContext* context) {
  std::lock_guard<std::mutex> lock(runtimeStatesMutex());
  auto& states = runtimeStates();
  auto it = states.find(context);
  if (it != states.end()) {
    return it->second;
  }
  auto state = std::make_shared<RuntimeState>(context);
  states[context] = state;
  return state;
}

void releaseStateForContext(JSContext* context) {
  std::shared_ptr<RuntimeState> state;
  {
    std::lock_guard<std::mutex> lock(runtimeStatesMutex());
    auto& states = runtimeStates();
    auto it = states.find(context);
    if (it == states.end()) {
      return;
    }
    state = std::move(it->second);
    states.erase(it);
  }
  state->cleanup();
  // The interned native-state atom must be released here, while the context is
  // still alive: a HostObjectHolder holds a shared_ptr to this state and is
  // freed by a GC finaliser during JS_FreeContext, so ~RuntimeState can run
  // after the context is gone. Leaving the atom would also trip QuickJS'
  // atom-leak check at JS_FreeRuntime.
  if (state != nullptr && state->nativeStateAtom != JS_ATOM_NULL) {
    JS_FreeAtom(context, state->nativeStateAtom);
    state->nativeStateAtom = JS_ATOM_NULL;
  }
}

// QuickJS interns a canonical array index as a *tagged integer* atom rather
// than as a string: JS_ValueToAtom turns an int JSValue straight into
// `index | JS_ATOM_TAG_INT`, and that is the atom the exotic get/set handlers
// receive for `a[0]`. So the index can be recovered with a mask, where
// JS_AtomToCString would allocate a C string that the host object then has to
// parse back into an integer on every element access.
//
// JS_ATOM_TAG_INT is engine-internal -- it appears in quickjs.c, not quickjs.h
// -- and is the same value in bellard QuickJS and quickjs-ng. Rather than trust
// that, each runtime verifies it once (verifyIndexAtomTagging) by asking the
// engine for the atom of a known integer through the same public API the
// property paths use. If the check fails, indexAtomsAreTagged stays false and
// every access takes the string path it took before, which is still correct.
static constexpr JSAtom kAtomTagInt = 1U << 31;

static inline bool atomAsArrayIndex(const RuntimeState& state, JSAtom atom, uint32_t* index) {
  if (!state.indexAtomsAreTagged || (atom & kAtomTagInt) == 0) {
    return false;
  }
  // A tagged-int atom holds at most JS_ATOM_MAX_INT (2^31 - 1), which is inside
  // the valid array-index range, so no bound check is needed here.
  *index = atom & ~kAtomTagInt;
  return true;
}

static void verifyIndexAtomTagging(JSContext* ctx, RuntimeState* state) {
  const uint32_t probe = 1234;
  JSAtom atom = JS_ValueToAtom(ctx, JS_NewInt32(ctx, static_cast<int32_t>(probe)));
  state->indexAtomsAreTagged = (atom & kAtomTagInt) != 0 && (atom & ~kAtomTagInt) == probe;
  JS_FreeAtom(ctx, atom);
}

static bool isNativeInstancePrototypeBypassExcluded(JSContext* ctx,
                                                    JSAtom atom) {
  const char* name = JS_AtomToCString(ctx, atom);
  if (name == nullptr) {
    return true;
  }
  bool excluded =
      std::strcmp(name, "kind") == 0 ||
      std::strcmp(name, "className") == 0 ||
      std::strcmp(name, "nativeAddress") == 0 ||
      std::strcmp(name, "class") == 0 ||
      std::strcmp(name, "constructor") == 0 ||
      std::strcmp(name, "super") == 0 ||
      std::strcmp(name, "invoke") == 0 ||
      std::strcmp(name, "send") == 0 ||
      std::strcmp(name, "takeRetainedValue") == 0 ||
      std::strcmp(name, "takeUnretainedValue") == 0 ||
      std::strcmp(name, "toString") == 0;
  JS_FreeCString(ctx, name);
  return excluded;
}

static void freePropertyDescriptor(JSContext* ctx,
                                   JSPropertyDescriptor& desc) {
  JS_FreeValue(ctx, desc.getter);
  JS_FreeValue(ctx, desc.setter);
  JS_FreeValue(ctx, desc.value);
}

static JSValue nativePrototypeProperty(JSContext* ctx, JSValueConst obj,
                                       JSAtom atom, JSValueConst receiver,
                                       HostObjectHolder* holder,
                                       bool* handled) {
  *handled = false;
  if (holder == nullptr ||
      holder->typeToken != hostObjectTypeToken<NativeApiObjectHostObject>()) {
    return JS_UNDEFINED;
  }

  JSValue prototype = JS_GetPrototype(ctx, obj);
  if (JS_IsException(prototype)) {
    *handled = true;
    return prototype;
  }

  for (size_t depth = 0; depth < 64 && JS_IsObject(prototype); depth++) {
    JSPropertyDescriptor desc = {};
    int found = JS_GetOwnProperty(ctx, &desc, prototype, atom);
    if (found < 0) {
      JS_FreeValue(ctx, prototype);
      *handled = true;
      return JS_EXCEPTION;
    }
    if (found > 0) {
      if (isNativeInstancePrototypeBypassExcluded(ctx, atom)) {
        freePropertyDescriptor(ctx, desc);
        JS_FreeValue(ctx, prototype);
        return JS_UNDEFINED;
      }

      *handled = true;
      JS_FreeValue(ctx, prototype);
      if ((desc.flags & JS_PROP_GETSET) != 0) {
        JSValue getter = desc.getter;
        JS_FreeValue(ctx, desc.setter);
        JS_FreeValue(ctx, desc.value);
        if (JS_IsUndefined(getter)) {
          JS_FreeValue(ctx, getter);
          return JS_UNDEFINED;
        }
        JSValue result = JS_Call(ctx, getter, receiver, 0, nullptr);
        JS_FreeValue(ctx, getter);
        return result;
      }

      JS_FreeValue(ctx, desc.getter);
      JS_FreeValue(ctx, desc.setter);
      return desc.value;
    }

    JSValue nextPrototype = JS_GetPrototype(ctx, prototype);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(nextPrototype)) {
      *handled = true;
      return nextPrototype;
    }
    prototype = nextPrototype;
  }

  JS_FreeValue(ctx, prototype);
  return JS_UNDEFINED;
}

static JSValue nativeHostGet(JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst receiver) {
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(obj, gHostClassId));
  if (holder == nullptr || holder->hostObject == nullptr) {
    return JS_UNDEFINED;
  }
  // The holder already owns the RuntimeState, so take it from there rather than
  // from stateForContext(), which takes a process-wide mutex and hashes the
  // JSContext* on every property access. On an 8-entry marshalling profile that
  // lookup was 600 ms of self time plus 360 ms inside pthread_mutex.
  Runtime runtime(holder->state);
  try {
    // Ahead of the prototype walk: an index is never a named property of a
    // native instance, so that walk can only fail for one.
    uint32_t index = 0;
    if (holder->hostObject->hasIndexedAccess() &&
        atomAsArrayIndex(*holder->state, atom, &index)) {
      Value self = Value::borrowed(runtime, obj);
      HostObject::ReceiverScope receiverScope(*holder->hostObject, self);
      return holder->hostObject->getValueAtIndex(runtime, index).local(runtime);
    }

    bool handledByPrototype = false;
    JSValue prototypeResult =
        nativePrototypeProperty(ctx, obj, atom, receiver, holder,
                                &handledByPrototype);
    if (handledByPrototype) {
      return prototypeResult;
    }

    // The receiver for this dispatch is the host object itself, matching what
    // the Node-API binding passes as `host_object` (quickjs-api.c's
    // host_object_get dups `obj`, not `receiver`). Non-owning: see
    // HostObject::receiver.
    Value self = Value::borrowed(runtime, obj);
    HostObject::ReceiverScope receiverScope(*holder->hostObject, self);
    Value result = holder->hostObject->get(runtime, PropNameID(atomToUtf8(ctx, atom)));
    if (!result.isUndefined()) {
      return result.local(runtime);
    }
    return JS_UNDEFINED;
  } catch (const JSError& error) {
    // Re-throw the original value, not a TypeError built from its text: the
    // Node-API shim carries the thrown object (and NativeScriptException's
    // `nativeException` with it) on JSError.
    return throwJSError(runtime, error);
  } catch (const std::exception& error) {
    return throwError(ctx, error);
  }
}

static int nativeHostSet(JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst value,
                         JSValueConst, int) {
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(obj, gHostClassId));
  if (holder == nullptr || holder->hostObject == nullptr) {
    return 0;
  }
  // The holder already owns the RuntimeState, so take it from there rather than
  // from stateForContext(), which takes a process-wide mutex and hashes the
  // JSContext* on every property access. On an 8-entry marshalling profile that
  // lookup was 600 ms of self time plus 360 ms inside pthread_mutex.
  Runtime runtime(holder->state);
  try {
    Value self = Value::borrowed(runtime, obj);
    HostObject::ReceiverScope receiverScope(*holder->hostObject, self);
    uint32_t index = 0;
    if (holder->hostObject->hasIndexedAccess() &&
        atomAsArrayIndex(*holder->state, atom, &index)) {
      return holder->hostObject->setValueAtIndex(runtime, index,
                                                 Value::borrowed(runtime, value))
                 ? 1
                 : 0;
    }
    bool handled = holder->hostObject->set(
        runtime, PropNameID(atomToUtf8(ctx, atom)),
        Value::borrowed(runtime, value));
    return handled ? 1 : 0;
  } catch (const JSError& error) {
    throwJSError(runtime, error);
    return -1;
  } catch (const std::exception& error) {
    throwError(ctx, error);
    return -1;
  }
}

static int nativeHostHas(JSContext* ctx, JSValueConst obj, JSAtom atom) {
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(obj, gHostClassId));
  if (holder == nullptr || holder->hostObject == nullptr) {
    return 0;
  }
  // The holder already owns the RuntimeState, so take it from there rather than
  // from stateForContext(), which takes a process-wide mutex and hashes the
  // JSContext* on every property access. On an 8-entry marshalling profile that
  // lookup was 600 ms of self time plus 360 ms inside pthread_mutex.
  Runtime runtime(holder->state);
  try {
    Value self = Value::borrowed(runtime, obj);
    HostObject::ReceiverScope receiverScope(*holder->hostObject, self);
    auto names = holder->hostObject->getPropertyNames(runtime);
    std::string requested = atomToUtf8(ctx, atom);
    for (const auto& name : names) {
      if (name.utf8(runtime) == requested) {
        return 1;
      }
    }
  } catch (const std::exception&) {
  }
  return 0;
}

static int nativeHostOwnNames(JSContext* ctx, JSPropertyEnum** ptab, uint32_t* plen,
                              JSValueConst obj) {
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(obj, gHostClassId));
  if (holder == nullptr || holder->hostObject == nullptr) {
    *ptab = nullptr;
    *plen = 0;
    return 0;
  }
  // See nativeHostGet: the RuntimeState comes off the holder, not out of the
  // mutex-guarded context map.
  Runtime runtime(holder->state);
  Value self = Value::borrowed(runtime, obj);
  HostObject::ReceiverScope receiverScope(*holder->hostObject, self);
  auto names = holder->hostObject->getPropertyNames(runtime);
  *plen = static_cast<uint32_t>(names.size());
  // A host object that reports no names must not reach the allocator:
  // quickjs-ng asserts count != 0 && size != 0 in js_calloc_rt, where bellard
  // QuickJS returns a valid empty block. JSON.stringify on such an object goes
  // Object.keys -> JS_GetOwnPropertyNamesInternal -> here, and aborted the
  // whole runtime on QUICKJS_NG.
  if (names.empty()) {
    *ptab = nullptr;
    return 0;
  }
  *ptab = static_cast<JSPropertyEnum*>(js_mallocz(ctx, sizeof(JSPropertyEnum) * names.size()));
  for (uint32_t i = 0; i < *plen; i++) {
    (*ptab)[i].is_enumerable = true;
    (*ptab)[i].atom = JS_NewAtom(ctx, names[i].utf8(runtime).c_str());
  }
  return 0;
}

static void nativeHostFinalize(JSRuntime*, JSValue value) {
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(value, gHostClassId));
  if (holder != nullptr && holder->state != nullptr) {
    holder->state->untrack(holder);
  }
  delete holder;
}

static JSValue invokeFunctionHolder(JSContext* ctx, FunctionHolder* holder, JSValueConst thisValue,
                                    int argc, JSValueConst* argv) {
  if (holder == nullptr || !holder->callback) {
    return JS_UNDEFINED;
  }
  // See nativeHostGet: the RuntimeState comes off the holder, not out of the
  // mutex-guarded context map.
  Runtime runtime(holder->state);
  StackValueArray<Value, 8> args(static_cast<size_t>(argc));
  for (int i = 0; i < argc; i++) {
    args.emplace(static_cast<size_t>(i), Value::borrowed(runtime, argv[i]));
  }
  try {
    Value self = Value::borrowed(runtime, thisValue);
    Value result =
        holder->callback(runtime, self, args.size() == 0 ? nullptr : args.data(), args.size());
    return result.local(runtime);
  } catch (const JSError& error) {
    return throwJSError(runtime, error);
  } catch (const std::exception& error) {
    return throwError(ctx, error);
  }
}

static JSValue nativeFunctionCall(JSContext* ctx, JSValue function, JSValue thisValue, int argc,
                                  JSValue* argv, int flags) {
  auto* holder = static_cast<FunctionHolder*>(JS_GetOpaque(function, gFunctionClassId));
  if ((flags & JS_CALL_FLAG_CONSTRUCTOR) == 0) {
    return invokeFunctionHolder(ctx, holder, thisValue, argc, argv);
  }

  // Called through `new`. QuickJS hands a JSClassCall the *new target* as
  // `thisValue` and takes whatever comes back as the construction result -- it
  // does not create the receiver for a non-bytecode callee. So synthesise it
  // here the way OrdinaryCreateFromConstructor does, and fall back to it when
  // the callback returns a non-object, which is what a JS constructor body
  // does.
  JSValue prototype = JS_GetPropertyStr(ctx, thisValue, "prototype");
  if (JS_IsException(prototype)) {
    return prototype;
  }
  // gNativeStateClassId, not a plain object: the receiver a host constructor
  // builds is exactly the object the Node-API shim napi_wraps
  // (ObjectManager::Link runs on `this`), and a class-backed object carries an
  // opaque slot that napi_unwrap can read in a field load instead of two
  // prototype-chain property lookups. The class has no exotic table, so this
  // object behaves as an ordinary object in every other respect.
  JSValue self = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, gNativeStateClassId)
                                        : JS_NewObjectClass(ctx, gNativeStateClassId);
  JS_FreeValue(ctx, prototype);
  if (JS_IsException(self)) {
    return self;
  }
  JSValue result = invokeFunctionHolder(ctx, holder, self, argc, argv);
  if (JS_IsException(result) || JS_IsObject(result)) {
    JS_FreeValue(ctx, self);
    return result;
  }
  JS_FreeValue(ctx, result);
  return self;
}

static JSValue nativeFunctionCallData(JSContext* ctx, JSValue thisValue, int argc, JSValue* argv,
                                      int, JSValue* data) {
  auto* holder = static_cast<FunctionHolder*>(JS_GetOpaque(data[0], gFunctionClassId));
  return invokeFunctionHolder(ctx, holder, thisValue, argc, argv);
}

static void nativeStateFinalize(JSRuntime*, JSValue value) {
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(value, gNativeStateClassId));
  delete holder;
}

static void nativeFunctionFinalize(JSRuntime*, JSValue value) {
  auto* holder = static_cast<FunctionHolder*>(JS_GetOpaque(value, gFunctionClassId));
  if (holder != nullptr && holder->state != nullptr) {
    holder->state->untrack(holder);
  }
  delete holder;
}

// The exotic table for host objects.
//
// On Android the vendored QuickJS is patched (see
// platforms/android/tools/patches/quickjs*/): JS_GetPropertyInternal compares
// the class's exotic table against `NapiHostObjectExoticMethods` *by address*
// and, when they match, treats get_property as a NON-masking fallback -- own
// properties and the whole prototype chain are consulted first, and the host is
// asked only if nothing was found. Every other exotic stays authoritative.
//
// The Node-API binding (vendor/quickjs/quickjs-api.c) owns that symbol on
// its build. On the jsi build that file is not linked, so this layer must both
// define it -- otherwise quickjs.c has an undefined reference -- and register
// its class with it, or host objects would mask their own prototypes and every
// native method on a Java instance would read back undefined.
//
// Off Android the symbol does not exist and the table stays file-local, so the
// Apple runtime is unaffected.
#if defined(USE_HOST_OBJECT) && defined(__ANDROID__)
extern "C" JSClassExoticMethods NapiHostObjectExoticMethods = {
#else
static JSClassExoticMethods NapiHostObjectExoticMethods = {
#endif
    .get_own_property = nullptr,
    .get_own_property_names = nativeHostOwnNames,
    .delete_property = nullptr,
    .define_own_property = nullptr,
    .has_property = nativeHostHas,
    .get_property = nativeHostGet,
    .set_property = nativeHostSet,
};

void ensureClasses(Runtime& runtime) {
  auto state = runtime.state();
  JSRuntime* rt = JS_GetRuntime(runtime.context());
  if (!state->indexAtomTaggingChecked) {
    verifyIndexAtomTagging(runtime.context(), state.get());
    state->indexAtomTaggingChecked = true;
  }
  if (gHostClassId == 0) {
    newClassId(rt, &gHostClassId);
  }
  if (!state->hostClassRegistered) {
    JSClassDef def = {};
    def.class_name = "NativeScriptEngineHostObject";
    def.exotic = &NapiHostObjectExoticMethods;
    def.finalizer = nativeHostFinalize;
    JS_NewClass(rt, gHostClassId, &def);
    JS_SetClassProto(runtime.context(), gHostClassId, JS_NewObject(runtime.context()));
    state->hostClassRegistered = true;
  }
  if (gFunctionClassId == 0) {
    newClassId(rt, &gFunctionClassId);
  }
  if (!state->functionClassRegistered) {
    JSClassDef def = {};
    def.class_name = "NativeScriptEngineFunction";
    def.call = nativeFunctionCall;
    def.finalizer = nativeFunctionFinalize;
    JS_NewClass(rt, gFunctionClassId, &def);
    JS_SetClassProto(runtime.context(), gFunctionClassId, JS_NewObject(runtime.context()));
    state->functionClassRegistered = true;
  }
  if (gNativeStateClassId == 0) {
    newClassId(rt, &gNativeStateClassId);
  }
  if (!state->nativeStateClassRegistered) {
    JSClassDef def = {};
    def.class_name = "Object";
    // No exotic table on purpose. This class exists only to give an ordinary
    // object an opaque slot; an exotic table here would put an interception
    // hook on every property access of every instance, which is the cost this
    // whole change exists to remove.
    def.finalizer = nativeStateFinalize;
    JS_NewClass(rt, gNativeStateClassId, &def);
    // Object.prototype, so an instance built by JS_NewObjectClass (the branch
    // taken when a constructor has no object `prototype`) is a normal object
    // rather than a null-prototype one.
    JSContext* ctx = runtime.context();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue objectCtor = JS_GetPropertyStr(ctx, global, "Object");
    JSValue objectProto = JS_GetPropertyStr(ctx, objectCtor, "prototype");
    JS_FreeValue(ctx, objectCtor);
    JS_FreeValue(ctx, global);
    if (JS_IsObject(objectProto)) {
      JS_SetClassProto(ctx, gNativeStateClassId, objectProto);
    } else {
      JS_FreeValue(ctx, objectProto);
      JS_SetClassProto(ctx, gNativeStateClassId, JS_NewObject(ctx));
    }
    state->nativeStateClassRegistered = true;
  }
}

}  // namespace quickjsengine

quickjsengine::HostObjectHolder* Object::hostObjectHolder(Runtime& runtime) const {
  quickjsengine::ensureClasses(runtime);
  JSValue object = local(runtime);
  auto* holder = static_cast<quickjsengine::HostObjectHolder*>(
      JS_GetOpaque(object, quickjsengine::gHostClassId));
  JS_FreeValue(runtime.context(), object);
  return holder;
}

void Object::setNativeStateWithToken(Runtime& runtime, std::shared_ptr<HostObject> host,
                                     const void* typeToken) {
  quickjsengine::ensureClasses(runtime);
  JSContext* ctx = runtime.context();
  JSValue self = local(runtime);

  // Fast path: the object carries an opaque slot of its own.
  if (JS_GetClassID(self) == quickjsengine::gNativeStateClassId) {
    // Replacing an existing payload must free the old one; nothing else will,
    // and QuickJS reports leaks at JS_FreeRuntime.
    delete static_cast<quickjsengine::HostObjectHolder*>(
        JS_GetOpaque(self, quickjsengine::gNativeStateClassId));
    JS_SetOpaque(self, new quickjsengine::HostObjectHolder(runtime.state(), std::move(host),
                                                           typeToken));
    JS_FreeValue(ctx, self);
    return;
  }

  // Fallback for an object JS created itself, which Node-API still allows
  // napi_wrap on. A non-enumerable property under an interned atom, holding
  // the payload in a host object exactly as the old __nsWrap slot did.
  Object holder = Object::createFromHostObjectWithToken(runtime, std::move(host), typeToken);
  // JS_DefinePropertyValue takes ownership of the value it is handed. Writable
  // and configurable so a second wrap replaces the first, as assigning did.
  int rc = JS_DefinePropertyValue(ctx, self, runtime.nativeStateAtom(), holder.local(runtime),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, self);
  if (rc < 0) {
    throw quickjsengine::caughtError(runtime, "QuickJS native state set failed.");
  }
}

std::shared_ptr<HostObject> Object::nativeStateOf(Runtime& runtime, const void* typeToken) const {
  JSContext* ctx = runtime.context();
  JSValue self = local(runtime);

  // JS_GetAnyOpaque returns the object's opaque word without a class check, so
  // the class id it hands back must be validated before the pointer is used --
  // on an object of another class that word is a different union member.
  JSClassID classId = 0;
  void* opaque = JS_GetAnyOpaque(self, &classId);
  if (quickjsengine::gNativeStateClassId != 0 && classId == quickjsengine::gNativeStateClassId) {
    JS_FreeValue(ctx, self);
    auto* holder = static_cast<quickjsengine::HostObjectHolder*>(opaque);
    if (holder == nullptr || holder->typeToken != typeToken) return nullptr;
    return holder->hostObject;
  }

  // Property fallback. Own-only: the miss is the common case (every receiver
  // the runtime probes), and walking a prototype chain to answer it was the
  // cost this change removes.
  if (runtime.state()->nativeStateAtom == JS_ATOM_NULL) {
    JS_FreeValue(ctx, self);
    return nullptr;
  }
  JSPropertyDescriptor descriptor;
  int rc = JS_GetOwnProperty(ctx, &descriptor, self, runtime.nativeStateAtom());
  JS_FreeValue(ctx, self);
  if (rc <= 0) return nullptr;
  JS_FreeValue(ctx, descriptor.getter);
  JS_FreeValue(ctx, descriptor.setter);
  std::shared_ptr<HostObject> result;
  auto* holder = static_cast<quickjsengine::HostObjectHolder*>(
      JS_GetOpaque(descriptor.value, quickjsengine::gHostClassId));
  if (holder != nullptr && holder->typeToken == typeToken) {
    result = holder->hostObject;
  }
  JS_FreeValue(ctx, descriptor.value);
  return result;
}

Object Object::createFromHostObjectWithToken(Runtime& runtime, std::shared_ptr<HostObject> host,
                                             const void* typeToken) {
  quickjsengine::ensureClasses(runtime);
  auto* holder = new quickjsengine::HostObjectHolder(runtime.state(), std::move(host), typeToken);
  JSValue object = JS_NewObjectClass(runtime.context(), quickjsengine::gHostClassId);
  JS_SetOpaque(object, holder);
  runtime.state()->track(holder, [](void* pointer) {
    auto* tracked = static_cast<quickjsengine::HostObjectHolder*>(pointer);
    tracked->state.reset();
    tracked->hostObject.reset();
  });
  Object result = Object::fromValueStorage(Value(runtime, object).storage_);
  JS_FreeValue(runtime.context(), object);
  return result;
}

Function Function::createFromHostFunction(Runtime& runtime, const PropNameID& name,
                                          unsigned int parameterCount, HostFunctionType callback) {
  quickjsengine::ensureClasses(runtime);
  auto* holder = new quickjsengine::FunctionHolder(runtime.state(), std::move(callback));
  JSValue data = JS_NewObjectClass(runtime.context(), quickjsengine::gFunctionClassId);
  if (JS_IsException(data)) {
    delete holder;
    throw JSError(runtime, "QuickJS host function data allocation failed.");
  }
  JS_SetOpaque(data, holder);
  runtime.state()->track(holder, [](void* pointer) {
    auto* tracked = static_cast<quickjsengine::FunctionHolder*>(pointer);
    tracked->state.reset();
    tracked->callback = {};
  });

  JSValue function = JS_NewCFunctionData(runtime.context(), quickjsengine::nativeFunctionCallData,
                                         static_cast<int>(parameterCount), 0, 1, &data);
  JS_FreeValue(runtime.context(), data);
  if (JS_IsException(function)) {
    throw JSError(runtime, "QuickJS host function allocation failed.");
  }

  std::string functionName = name.utf8(runtime);
  JSValue nameValue = JS_NewStringLen(runtime.context(), functionName.data(), functionName.size());
  JS_DefinePropertyValueStr(runtime.context(), function, "name", nameValue, JS_PROP_CONFIGURABLE);
  Function result = Function(Object::fromValueStorage(Value(runtime, function).storage_));
  JS_FreeValue(runtime.context(), function);
  return result;
}

Function Function::createFromHostConstructor(Runtime& runtime, const PropNameID& name,
                                             unsigned int paramCount,
                                             HostFunctionType callback) {
  quickjsengine::ensureClasses(runtime);
  JSContext* ctx = runtime.context();

  // An object of the engine's function class rather than JS_NewCFunctionData:
  // its JSClassCall receives QuickJS' call flags, which is the only way to tell
  // `new` from a plain call, and its constructor bit can be set. See the header.
  auto* holder = new quickjsengine::FunctionHolder(runtime.state(), std::move(callback));
  JSValue function = JS_NewObjectClass(ctx, quickjsengine::gFunctionClassId);
  if (JS_IsException(function)) {
    delete holder;
    throw JSError(runtime, "QuickJS host constructor allocation failed.");
  }
  JS_SetOpaque(function, holder);
  JS_SetConstructorBit(ctx, function, 1);

  // The class prototype registered in ensureClasses is a plain object, so
  // without this the constructor would not inherit call/apply/bind/toString and
  // `ctor instanceof Function` would be false.
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue functionCtor = JS_GetPropertyStr(ctx, global, "Function");
  JSValue functionProto = JS_GetPropertyStr(ctx, functionCtor, "prototype");
  if (JS_IsObject(functionProto)) {
    JS_SetPrototype(ctx, function, functionProto);
  }
  JS_FreeValue(ctx, functionProto);
  JS_FreeValue(ctx, functionCtor);
  JS_FreeValue(ctx, global);

  // JS_DefinePropertyValueStr, not a plain set: `name` is defined as an own
  // property, so the non-writable Function.prototype.name installed on the
  // prototype chain just above cannot swallow it the way it swallows a [[Set]]
  // in sloppy mode. (That is what made ctor.name read back as "" on JSC.)
  const std::string functionName = name.utf8(runtime);
  JS_DefinePropertyValueStr(
      ctx, function, "name",
      JS_NewStringLen(ctx, functionName.data(), functionName.size()), JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(ctx, function, "length",
                            JS_NewUint32(ctx, static_cast<uint32_t>(paramCount)),
                            JS_PROP_CONFIGURABLE);

  // WRITABLE is the whole point: napi_define_class reads this object back and
  // the runtime later reassigns it outright to chain class prototypes.
  //
  // The prototype also needs its `constructor` back-pointer. Per spec a
  // function's prototype carries a non-enumerable, writable, configurable
  // `constructor` naming the function; V8 (Function::New) and JSC install it
  // for us, but a bare JS_NewObject has none, so `instance.constructor` walked
  // straight past it to Object.prototype.constructor and every native class
  // reported its name as "Object".
  JSValue prototype = JS_NewObject(ctx);
  JS_DefinePropertyValueStr(ctx, prototype, "constructor", JS_DupValue(ctx, function),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(ctx, function, "prototype", prototype,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

  Function result = Function(Object::fromValueStorage(Value(runtime, function).storage_));
  JS_FreeValue(ctx, function);
  return result;
}

}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_QUICKJS
