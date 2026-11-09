#include "jsi/jsc/JSCRuntime.h"
#include "jsi/shared/NativeApiStackValueArray.h"

#ifdef TARGET_ENGINE_JSC

namespace nativescript {
namespace engine {

namespace jscengine {

namespace {
std::mutex& runtimeStatesMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::unordered_map<JSGlobalContextRef, std::shared_ptr<RuntimeState>>&
runtimeStates() {
  static auto* states =
      new std::unordered_map<JSGlobalContextRef, std::shared_ptr<RuntimeState>>();
  return *states;
}
}  // namespace

std::shared_ptr<RuntimeState> stateForContext(JSGlobalContextRef context) {
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

void releaseStateForContext(JSGlobalContextRef context) {
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

JSClassRef hostClass(Runtime& runtime);
JSClassRef functionClass(Runtime& runtime);
JSClassRef constructorClass(Runtime& runtime);
void setFunctionPrototype(JSGlobalContextRef context, JSObjectRef function);

// Hand a caught JSError back to JSC as the thrown value it actually was.
//
// setException() below rebuilds an Error from what(), which is right for a
// native failure but wrong for an exception that started life in JS: it drops
// the identity of the object -- NativeScriptException's `nativeException`, a
// SyntaxError's name -- and the stack. JSError now carries the value
// (jscengine::toJSError), so prefer it and keep the rebuild as the fallback for
// errors this layer raised itself.
void setEngineException(Runtime& runtime, JSContextRef context, JSValueRef* exception,
                        const JSError& error) {
  if (exception == nullptr) {
    return;
  }
  if (const Value* thrown = error.value()) {
    if (!thrown->isUndefined() && !thrown->isNull()) {
      *exception = thrown->local(runtime);
      return;
    }
  }
  *exception = makeError(context, error.what());
}


// JSC has no indexed callback: `a[0]` reaches getProperty/setProperty as the
// property name "0". Reading the index straight off the UTF-16 buffer the
// JSStringRef already holds costs a handful of instructions and allocates
// nothing, where stringToUtf8 builds a std::string (sized by
// JSStringGetMaximumUTF8CStringSize, so it over-allocates) that the host object
// then has to parse back into an integer.
//
// Canonicalisation matches what a host object doing this in terms of the name
// would apply: no leading zeros, at most 2^32 - 2.
bool stringAsArrayIndex(JSStringRef name, uint32_t* index) {
  size_t length = JSStringGetLength(name);
  if (length == 0 || length > 10) {  // a uint32 has at most 10 digits
    return false;
  }
  const JSChar* chars = JSStringGetCharactersPtr(name);
  if (chars == nullptr) {
    return false;
  }
  if (chars[0] == '0') {  // canonical form has no leading zeros; only "0" itself
    if (length != 1) {
      return false;
    }
    *index = 0;
    return true;
  }
  uint64_t value = 0;
  for (size_t i = 0; i < length; i++) {
    if (chars[i] < '0' || chars[i] > '9') {
      return false;
    }
    value = value * 10 + static_cast<uint64_t>(chars[i] - '0');
  }
  if (value > 4294967294ULL) {  // the largest array index is 2^32 - 2
    return false;
  }
  *index = static_cast<uint32_t>(value);
  return true;
}

bool isNativeInstancePrototypeBypassExcluded(JSStringRef propertyName) {
  return JSStringIsEqualToUTF8CString(propertyName, "kind") ||
         JSStringIsEqualToUTF8CString(propertyName, "className") ||
         JSStringIsEqualToUTF8CString(propertyName, "nativeAddress") ||
         JSStringIsEqualToUTF8CString(propertyName, "class") ||
         JSStringIsEqualToUTF8CString(propertyName, "constructor") ||
         JSStringIsEqualToUTF8CString(propertyName, "super") ||
         JSStringIsEqualToUTF8CString(propertyName, "invoke") ||
         JSStringIsEqualToUTF8CString(propertyName, "send") ||
         JSStringIsEqualToUTF8CString(propertyName, "takeRetainedValue") ||
         JSStringIsEqualToUTF8CString(propertyName, "takeUnretainedValue") ||
         JSStringIsEqualToUTF8CString(propertyName, "toString");
}

bool shouldDeferToNativeInstancePrototype(JSContextRef context,
                                          JSObjectRef object,
                                          JSStringRef propertyName,
                                          HostObjectHolder* holder) {
  if (context == nullptr || object == nullptr || propertyName == nullptr ||
      holder == nullptr ||
      !holder->nativeInstance ||
      isNativeInstancePrototypeBypassExcluded(propertyName)) {
    return false;
  }

  JSValueRef prototypeValue = JSObjectGetPrototype(context, object);
  if (prototypeValue == nullptr || !JSValueIsObject(context, prototypeValue)) {
    return false;
  }

  JSValueRef exception = nullptr;
  JSObjectRef prototypeObject =
      JSValueToObject(context, prototypeValue, &exception);
  if (exception != nullptr || prototypeObject == nullptr) {
    return false;
  }

  exception = nullptr;
  bool found = JSObjectHasProperty(context, prototypeObject, propertyName);
  return exception == nullptr && found;
}

JSValueRef hostGetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
                           JSValueRef* exception) {
  auto* holder = static_cast<HostObjectHolder*>(JSObjectGetPrivate(object));
  if (holder == nullptr || holder->hostObject == nullptr) {
    return nullptr;
  }
  Runtime runtime(holder->state);
  // Ahead of the prototype deferral: an index is never a named property of a
  // native instance, so that lookup can only fail for one.
  uint32_t index = 0;
  if (holder->hostObject->hasIndexedAccess() &&
      stringAsArrayIndex(propertyName, &index)) {
    try {
      const Value receiver = Value::borrowed(runtime, object);
      HostObject::ReceiverScope receiverScope(*holder->hostObject, receiver);
      Value result = holder->hostObject->getValueAtIndex(runtime, index);
      return result.isUndefined() ? nullptr : result.local(runtime);
    } catch (const JSError& error) {
      setEngineException(runtime, context, exception, error);
      return JSValueMakeUndefined(context);
    } catch (const std::exception& error) {
      setException(context, exception, error);
      return JSValueMakeUndefined(context);
    }
  }
  if (shouldDeferToNativeInstancePrototype(context, object, propertyName,
                                           holder)) {
    return nullptr;
  }
  try {
    // Non-owning, for this dispatch only; see HostObject::receiver().
    const Value receiver = Value::borrowed(runtime, object);
    HostObject::ReceiverScope receiverScope(*holder->hostObject, receiver);
    Value result = holder->hostObject->get(runtime, PropNameID(stringToUtf8(propertyName)));
    return result.isUndefined() ? nullptr : result.local(runtime);
  } catch (const JSError& error) {
    setEngineException(runtime, context, exception, error);
    return JSValueMakeUndefined(context);
  } catch (const std::exception& error) {
    setException(context, exception, error);
    return JSValueMakeUndefined(context);
  }
}

bool hostSetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
                     JSValueRef value, JSValueRef* exception) {
  auto* holder = static_cast<HostObjectHolder*>(JSObjectGetPrivate(object));
  if (holder == nullptr || holder->hostObject == nullptr) {
    return false;
  }
  Runtime runtime(holder->state);
  try {
    const Value receiver = Value::borrowed(runtime, object);
    HostObject::ReceiverScope receiverScope(*holder->hostObject, receiver);
    uint32_t index = 0;
    if (holder->hostObject->hasIndexedAccess() &&
        stringAsArrayIndex(propertyName, &index)) {
      return holder->hostObject->setValueAtIndex(runtime, index,
                                                 Value::borrowed(runtime, value));
    }
    return holder->hostObject->set(runtime, PropNameID(stringToUtf8(propertyName)),
                            Value::borrowed(runtime, value));
  } catch (const JSError& error) {
    setEngineException(runtime, context, exception, error);
    return true;
  } catch (const std::exception& error) {
    setException(context, exception, error);
    return true;
  }
}

void hostGetPropertyNames(JSContextRef, JSObjectRef object,
                          JSPropertyNameAccumulatorRef propertyNames) {
  auto* holder = static_cast<HostObjectHolder*>(JSObjectGetPrivate(object));
  if (holder == nullptr || holder->hostObject == nullptr) {
    return;
  }
  Runtime runtime(holder->state);
  try {
    const Value receiver = Value::borrowed(runtime, object);
    HostObject::ReceiverScope receiverScope(*holder->hostObject, receiver);
    for (const auto& property : holder->hostObject->getPropertyNames(runtime)) {
      JSStringRef name = makeJSString(property.utf8(runtime));
      JSPropertyNameAccumulatorAddName(propertyNames, name);
      JSStringRelease(name);
    }
  } catch (const std::exception&) {
  }
}

void hostFinalize(JSObjectRef object) {
  auto* holder = static_cast<HostObjectHolder*>(JSObjectGetPrivate(object));
  if (holder != nullptr && holder->state != nullptr) {
    holder->state->untrack(holder);
  }
  delete holder;
}

JSValueRef functionCall(JSContextRef context, JSObjectRef function, JSObjectRef thisObject,
                        size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception) {
  auto* holder = static_cast<FunctionHolder*>(JSObjectGetPrivate(function));
  if (holder == nullptr || !holder->callback) {
    return JSValueMakeUndefined(context);
  }
  Runtime runtime(holder->state);
  StackValueArray<Value, 8> args(argumentCount);
  for (size_t i = 0; i < argumentCount; i++) {
    args.emplace(i, Value::borrowed(runtime, arguments[i]));
  }
  try {
    Value thisValue = Value::borrowed(runtime, thisObject);
    Value result =
        holder->callback(runtime, thisValue, args.size() == 0 ? nullptr : args.data(),
                         args.size());
    return result.local(runtime);
  } catch (const JSError& error) {
    setEngineException(runtime, context, exception, error);
    return JSValueMakeUndefined(context);
  } catch (const std::exception& error) {
    setException(context, exception, error);
    return JSValueMakeUndefined(context);
  }
}

// `new Ctor(...)` on a function built by createFromHostConstructor.
//
// JSObjectCallAsConstructorCallback is handed no receiver and must return an
// object, so this synthesises one the way OrdinaryCreateFromConstructor does:
// a plain object whose [[Prototype]] is the constructor's `prototype`. The
// host callback then sees it as `this`, which is what napi_define_class's
// constructor expects (it calls napi_wrap on it and usually returns nothing).
JSObjectRef functionConstruct(JSContextRef context, JSObjectRef constructor,
                              size_t argumentCount, const JSValueRef arguments[],
                              JSValueRef* exception) {
  auto* holder = static_cast<FunctionHolder*>(JSObjectGetPrivate(constructor));
  if (holder == nullptr || !holder->callback) {
    return JSObjectMake(context, nullptr, nullptr);
  }
  Runtime runtime(holder->state);

  JSObjectRef self = JSObjectMake(context, nullptr, nullptr);
  JSStringRef prototypeName = makeJSString("prototype");
  JSValueRef prototype = JSObjectGetProperty(context, constructor, prototypeName, nullptr);
  JSStringRelease(prototypeName);
  if (prototype != nullptr && JSValueIsObject(context, prototype)) {
    JSObjectSetPrototype(context, self, prototype);
  }

  StackValueArray<Value, 8> args(argumentCount);
  for (size_t i = 0; i < argumentCount; i++) {
    args.emplace(i, Value::borrowed(runtime, arguments[i]));
  }
  try {
    Value thisValue = Value::borrowed(runtime, self);
    Value result = holder->callback(runtime, thisValue, args.size() == 0 ? nullptr : args.data(),
                                    args.size());
    // A constructor may legitimately return a different object (the runtime's
    // ObjectManager does for already-known Java instances); anything else means
    // "keep the receiver", matching [[Construct]].
    if (result.isObject()) {
      JSValueRef returned = result.local(runtime);
      if (returned != nullptr && JSValueIsObject(context, returned)) {
        return JSValueToObject(context, returned, nullptr);
      }
    }
    return self;
  } catch (const JSError& error) {
    setEngineException(runtime, context, exception, error);
    return self;
  } catch (const std::exception& error) {
    setException(context, exception, error);
    return self;
  }
}

// `value instanceof Ctor`, restoring the ordinary meaning.
//
// JSC does not fall back to the prototype-chain walk for a JSObjectMake'd
// object: JSCallbackObject overrides customHasInstance, and that override
// consults only the JSClass chain's hasInstance callbacks and returns false
// when there is none. So without this every `instanceof` against a
// napi_define_class constructor answered false -- the whole
// When_calling_instanceof_* family, "should return true for instances that
// inherit from a base type", and the generated-proxy specs.
//
// Symbol.hasInstance still wins over this, which matters: MetadataNode installs
// one on every Java interface so `obj instanceof SomeInterface` reports
// implementation rather than inheritance. JSC checks that first.
bool functionHasInstance(JSContextRef context, JSObjectRef constructor,
                         JSValueRef possibleInstance, JSValueRef* exception) {
  if (possibleInstance == nullptr || !JSValueIsObject(context, possibleInstance)) {
    return false;
  }
  JSStringRef name = makeJSString("prototype");
  JSValueRef prototype = JSObjectGetProperty(context, constructor, name, exception);
  JSStringRelease(name);
  if (prototype == nullptr || !JSValueIsObject(context, prototype)) {
    return false;
  }
  JSObjectRef object = JSValueToObject(context, possibleInstance, nullptr);
  if (object == nullptr) {
    return false;
  }
  for (JSValueRef walk = JSObjectGetPrototype(context, object);
       walk != nullptr && JSValueIsObject(context, walk);) {
    if (JSValueIsStrictEqual(context, walk, prototype)) {
      return true;
    }
    JSObjectRef step = JSValueToObject(context, walk, nullptr);
    if (step == nullptr) {
      break;
    }
    walk = JSObjectGetPrototype(context, step);
  }
  return false;
}

void functionFinalize(JSObjectRef object) {
  auto* holder = static_cast<FunctionHolder*>(JSObjectGetPrivate(object));
  if (holder != nullptr && holder->state != nullptr) {
    holder->state->untrack(holder);
  }
  delete holder;
}

JSClassRef hostClass(Runtime& runtime) {
  auto state = runtime.state();
  if (state->hostClass == nullptr) {
    JSClassDefinition definition = kJSClassDefinitionEmpty;
    definition.className = "NativeScriptEngineHostObject";
    definition.getProperty = hostGetProperty;
    definition.setProperty = hostSetProperty;
    definition.getPropertyNames = hostGetPropertyNames;
    definition.finalize = hostFinalize;
    state->hostClass = JSClassCreate(&definition);
  }
  return state->hostClass;
}

JSClassRef functionClass(Runtime& runtime) {
  auto state = runtime.state();
  if (state->functionClass == nullptr) {
    JSClassDefinition definition = kJSClassDefinitionEmpty;
    definition.className = "NativeScriptEngineFunction";
    definition.callAsFunction = functionCall;
    definition.finalize = functionFinalize;
    state->functionClass = JSClassCreate(&definition);
  }
  return state->functionClass;
}

JSClassRef constructorClass(Runtime& runtime) {
  auto state = runtime.state();
  if (state->constructorClass == nullptr) {
    JSClassDefinition definition = kJSClassDefinitionEmpty;
    definition.className = "NativeScriptEngineConstructor";
    // Both callbacks: a class constructor is still callable (napi_is_function
    // must answer true, and JSC's ordinary instanceof requires IsCallable).
    definition.callAsFunction = functionCall;
    definition.callAsConstructor = functionConstruct;
    definition.hasInstance = functionHasInstance;
    definition.finalize = functionFinalize;
    state->constructorClass = JSClassCreate(&definition);
  }
  return state->constructorClass;
}

void setFunctionPrototype(JSGlobalContextRef context, JSObjectRef function) {
  if (context == nullptr || function == nullptr) {
    return;
  }

  JSValueRef exception = nullptr;
  JSStringRef functionName = makeJSString("Function");
  JSValueRef functionValue =
      JSObjectGetProperty(context, JSContextGetGlobalObject(context), functionName, &exception);
  JSStringRelease(functionName);
  if (exception != nullptr || functionValue == nullptr ||
      !JSValueIsObject(context, functionValue)) {
    return;
  }

  exception = nullptr;
  JSObjectRef functionConstructor = JSValueToObject(context, functionValue, &exception);
  if (exception != nullptr || functionConstructor == nullptr) {
    return;
  }

  JSStringRef prototypeName = makeJSString("prototype");
  JSValueRef prototypeValue =
      JSObjectGetProperty(context, functionConstructor, prototypeName, &exception);
  JSStringRelease(prototypeName);
  if (exception != nullptr || prototypeValue == nullptr ||
      !JSValueIsObject(context, prototypeValue)) {
    return;
  }

  JSObjectSetPrototype(context, function, prototypeValue);
}

}  // namespace jscengine

Object Object::createFromHostObjectWithToken(
    Runtime& runtime, std::shared_ptr<HostObject> host, const void* typeToken,
    bool nativeInstance) {
  auto* holder = new jscengine::HostObjectHolder(
      runtime.state(), std::move(host), typeToken, nativeInstance);
  runtime.state()->track(holder, [](void* pointer) {
    auto* tracked = static_cast<jscengine::HostObjectHolder*>(pointer);
    tracked->hostObject.reset();
    tracked->state.reset();
  });
  JSObjectRef object = JSObjectMake(runtime.context(), jscengine::hostClass(runtime), holder);
  return Object::fromValueStorage(Value(runtime, object).storage_);
}

Function Function::createFromHostFunction(Runtime& runtime, const PropNameID& name, unsigned int,
                                          HostFunctionType callback) {
  auto* holder = new jscengine::FunctionHolder(runtime.state(), std::move(callback));
  runtime.state()->track(holder, [](void* pointer) {
    auto* tracked = static_cast<jscengine::FunctionHolder*>(pointer);
    tracked->callback = {};
    tracked->state.reset();
  });
  JSObjectRef function = JSObjectMake(runtime.context(), jscengine::functionClass(runtime), holder);
  jscengine::setFunctionPrototype(runtime.context(), function);
  std::string functionName = name.utf8(runtime);
  if (!functionName.empty()) {
    JSStringRef property = jscengine::makeJSString("name");
    JSStringRef valueString = jscengine::makeJSString(functionName);
    JSValueRef value = JSValueMakeString(runtime.context(), valueString);
    JSObjectSetProperty(runtime.context(), function, property, value, kJSPropertyAttributeReadOnly,
                        nullptr);
    JSStringRelease(valueString);
    JSStringRelease(property);
  }
  return Function(Object::fromValueStorage(Value(runtime, function).storage_));
}

Function Function::createFromHostConstructor(Runtime& runtime, const PropNameID& name,
                                             unsigned int, HostFunctionType callback) {
  // Same holder/finalizer lifetime as createFromHostFunction; only the class
  // differs (it carries callAsConstructor) and a `prototype` is installed.
  auto* holder = new jscengine::FunctionHolder(runtime.state(), std::move(callback));
  JSObjectRef function =
      JSObjectMake(runtime.context(), jscengine::constructorClass(runtime), holder);

  // `name` is installed BEFORE the function prototype, and the order is
  // load-bearing.
  //
  // JSObjectSetProperty only defines an *own* property when the name is absent
  // from the whole prototype chain; otherwise it does an ordinary put. Once
  // Function.prototype is in the chain, `name` is found there -- non-writable,
  // per spec -- so the put failed silently in sloppy mode and the constructor
  // inherited Function.prototype.name, which is the empty string. That is what
  // `java.lang.Object.name` and `obj.constructor.name` read back as.
  std::string functionName = name.utf8(runtime);
  if (!functionName.empty()) {
    JSStringRef property = jscengine::makeJSString("name");
    JSStringRef valueString = jscengine::makeJSString(functionName);
    JSValueRef value = JSValueMakeString(runtime.context(), valueString);
    JSObjectSetProperty(runtime.context(), function, property, value, kJSPropertyAttributeReadOnly,
                        nullptr);
    JSStringRelease(valueString);
    JSStringRelease(property);
  }

  jscengine::setFunctionPrototype(runtime.context(), function);

  // A writable, non-enumerable own `prototype`. MetadataNode chains class
  // prototypes with a plain `ctor.prototype = ...` assignment
  // (napi_util::set_prototype); a JSObjectMake'd object has no `prototype` at
  // all, so without this the assignment creates one on first write but
  // `ctor.prototype` reads undefined until then -- and napi_define_class reads
  // it immediately to hang the methods off.
  {
    JSStringRef property = jscengine::makeJSString("prototype");
    JSObjectRef prototype = JSObjectMake(runtime.context(), nullptr, nullptr);
    // The prototype's `constructor` back-pointer. Per spec a function's
    // prototype carries one (non-enumerable, writable, configurable); V8's
    // Function::New installs it, but a JSObjectMake'd object has none, so
    // `instance.constructor` walked past the class prototype to
    // Object.prototype.constructor and every native class reported its name as
    // "Object".
    {
      JSStringRef ctorName = jscengine::makeJSString("constructor");
      JSObjectSetProperty(runtime.context(), prototype, ctorName, function,
                          kJSPropertyAttributeDontEnum, nullptr);
      JSStringRelease(ctorName);
    }
    JSObjectSetProperty(runtime.context(), function, property, prototype,
                        kJSPropertyAttributeDontEnum, nullptr);
    JSStringRelease(property);
  }

  return Function(Object::fromValueStorage(Value(runtime, function).storage_));
}

}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_JSC
