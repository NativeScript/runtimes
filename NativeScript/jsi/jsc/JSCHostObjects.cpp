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
void setFunctionPrototype(JSGlobalContextRef context, JSObjectRef function);

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
  if (shouldDeferToNativeInstancePrototype(context, object, propertyName,
                                           holder)) {
    return nullptr;
  }
  Runtime runtime(holder->state);
  try {
    Value result = holder->hostObject->get(runtime, PropNameID(stringToUtf8(propertyName)));
    return result.isUndefined() ? nullptr : result.local(runtime);
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
    return holder->hostObject->set(runtime, PropNameID(stringToUtf8(propertyName)),
                            Value::borrowed(runtime, value));
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
  } catch (const std::exception& error) {
    setException(context, exception, error);
    return JSValueMakeUndefined(context);
  }
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

}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_JSC
