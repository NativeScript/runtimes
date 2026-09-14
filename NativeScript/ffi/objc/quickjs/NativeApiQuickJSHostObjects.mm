#include "NativeApiQuickJSRuntime.h"
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
  Runtime runtime(stateForContext(ctx));
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(obj, gHostClassId));
  if (holder == nullptr || holder->hostObject == nullptr) {
    return JS_UNDEFINED;
  }
  try {
    bool handledByPrototype = false;
    JSValue prototypeResult =
        nativePrototypeProperty(ctx, obj, atom, receiver, holder,
                                &handledByPrototype);
    if (handledByPrototype) {
      return prototypeResult;
    }

    Value result = holder->hostObject->get(runtime, PropNameID(atomToUtf8(ctx, atom)));
    if (!result.isUndefined()) {
      return result.local(runtime);
    }
    return JS_UNDEFINED;
  } catch (const std::exception& error) {
    return throwError(ctx, error);
  }
}

static int nativeHostSet(JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst value,
                         JSValueConst, int) {
  Runtime runtime(stateForContext(ctx));
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(obj, gHostClassId));
  if (holder == nullptr || holder->hostObject == nullptr) {
    return 0;
  }
  try {
    bool handled = holder->hostObject->set(
        runtime, PropNameID(atomToUtf8(ctx, atom)),
        Value::borrowed(runtime, value));
    return handled ? 1 : 0;
  } catch (const std::exception& error) {
    throwError(ctx, error);
    return -1;
  }
}

static int nativeHostHas(JSContext* ctx, JSValueConst obj, JSAtom atom) {
  Runtime runtime(stateForContext(ctx));
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(obj, gHostClassId));
  if (holder == nullptr || holder->hostObject == nullptr) {
    return 0;
  }
  try {
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
  Runtime runtime(stateForContext(ctx));
  auto* holder = static_cast<HostObjectHolder*>(JS_GetOpaque(obj, gHostClassId));
  if (holder == nullptr || holder->hostObject == nullptr) {
    *ptab = nullptr;
    *plen = 0;
    return 0;
  }
  auto names = holder->hostObject->getPropertyNames(runtime);
  *plen = static_cast<uint32_t>(names.size());
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
  Runtime runtime(stateForContext(ctx));
  if (holder == nullptr || !holder->callback) {
    return JS_UNDEFINED;
  }
  StackValueArray<Value, 8> args(static_cast<size_t>(argc));
  for (int i = 0; i < argc; i++) {
    args.emplace(static_cast<size_t>(i), Value::borrowed(runtime, argv[i]));
  }
  try {
    Value self = Value::borrowed(runtime, thisValue);
    Value result =
        holder->callback(runtime, self, args.size() == 0 ? nullptr : args.data(), args.size());
    return result.local(runtime);
  } catch (const std::exception& error) {
    return throwError(ctx, error);
  }
}

static JSValue nativeFunctionCall(JSContext* ctx, JSValue function, JSValue thisValue, int argc,
                                  JSValue* argv, int) {
  auto* holder = static_cast<FunctionHolder*>(JS_GetOpaque(function, gFunctionClassId));
  return invokeFunctionHolder(ctx, holder, thisValue, argc, argv);
}

static JSValue nativeFunctionCallData(JSContext* ctx, JSValue thisValue, int argc, JSValue* argv,
                                      int, JSValue* data) {
  auto* holder = static_cast<FunctionHolder*>(JS_GetOpaque(data[0], gFunctionClassId));
  return invokeFunctionHolder(ctx, holder, thisValue, argc, argv);
}

static void nativeFunctionFinalize(JSRuntime*, JSValue value) {
  auto* holder = static_cast<FunctionHolder*>(JS_GetOpaque(value, gFunctionClassId));
  if (holder != nullptr && holder->state != nullptr) {
    holder->state->untrack(holder);
  }
  delete holder;
}

static JSClassExoticMethods hostExoticMethods = {
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
  if (gHostClassId == 0) {
    JS_NewClassID(rt, &gHostClassId);
  }
  if (!state->hostClassRegistered) {
    JSClassDef def = {};
    def.class_name = "NativeScriptEngineHostObject";
    def.exotic = &hostExoticMethods;
    def.finalizer = nativeHostFinalize;
    JS_NewClass(rt, gHostClassId, &def);
    JS_SetClassProto(runtime.context(), gHostClassId, JS_NewObject(runtime.context()));
    state->hostClassRegistered = true;
  }
  if (gFunctionClassId == 0) {
    JS_NewClassID(rt, &gFunctionClassId);
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

}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_QUICKJS
