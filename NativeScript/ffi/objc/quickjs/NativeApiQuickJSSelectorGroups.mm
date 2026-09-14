// Included by NativeApiQuickJS.mm inside the NativeScript anonymous namespace.

#include "../shared/bridge/SelectorGroupData.h"

#include "NativeApiQuickJSMarshalling.mm"

#include "NativeApiQuickJSGsd.mm"

#include "../shared/bridge/SelectorGroupCall.h"


void* lookupGeneratedEngineObjCGsdInvoker(uint64_t dispatchId) {
  return reinterpret_cast<void*>(lookupObjCGsdInvoker(dispatchId));
}

bool tryCallGeneratedEngineObjCSelector(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const NativeApiPreparedObjCInvocation& prepared,
    const Value* args, size_t count, Class dispatchSuperClass, Value* result) {
  if (result == nullptr || receiver == nil ||
      !prepared.gsdEngineCallable || dispatchSuperClass != Nil ||
      count != prepared.gsdEngineArgumentCount) {
    return false;
  }

  auto invoker = reinterpret_cast<ObjCGsdInvoker>(prepared.engineInvoker);
  GsdObjCContext ctx{runtime, bridge, receiver, prepared.selector,
                     runtime.context(), nullptr,
                     prepared.signature.returnType};
  ctx.valueArguments = args;
  ctx.materializeValueResult = true;
  if (!invoker(ctx)) {
    return false;
  }
  *result = std::move(ctx.valueResult);
  return true;
}

JSValue setQuickJSEnginePreparedObjCResult(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const NativeApiPreparedObjCInvocation& prepared,
    const std::shared_ptr<NativeApiObjectHostObject>& receiverHostObject,
    const std::optional<Object>& initializerClassWrapper,
    size_t providedCount, JSValueConst arguments[],
    Class dispatchSuperClass) {
  const NativeApiSignature& signature = prepared.signature;
  if (receiver == nil || signature.variadic ||
      unsupportedEngineType(signature.returnType)) {
    throw JSError(runtime,
                  "Objective-C selector is not supported by QuickJS engine: " +
                      prepared.selectorName);
  }

  const bool isNSErrorOutMethod = prepared.isNSErrorOutMethod;
  if (isNSErrorOutMethod) {
    size_t expected = signature.argumentTypes.size();
    if (providedCount > expected || providedCount + 1 < expected) {
      throw JSError(
          runtime, "Actual arguments count: \"" + std::to_string(providedCount) +
                       "\". Expected: \"" + std::to_string(expected) + "\".");
    }
  } else if (providedCount != signature.argumentTypes.size()) {
    throw JSError(
        runtime, "Actual arguments count: \"" + std::to_string(providedCount) +
                     "\". Expected: \"" +
                     std::to_string(signature.argumentTypes.size()) + "\".");
  }

  // GSD fast path: the generated invoker reads args directly from the QuickJS
  // arguments, calls objc_msgSend with a typed cast, and produces the JS
  // return value — bypassing all generic marshalling. Excludes appearance
  // static selectors — those need the generic path's proxy tagging.
  if (prepared.gsdEngineCallable && dispatchSuperClass == Nil &&
      providedCount == prepared.gsdEngineArgumentCount &&
      !initializerClassWrapper && !isNSErrorOutMethod &&
      !isPreparedStaticAppearanceSelector(prepared)) {
    auto invoker = reinterpret_cast<ObjCGsdInvoker>(prepared.engineInvoker);
    GsdObjCContext ctx{runtime,           bridge,    receiver, prepared.selector,
                       runtime.context(), arguments, signature.returnType};
    if (invoker(ctx)) {
      if (providedCount > 0) {
        Value setterValue = Value::borrowed(runtime, arguments[0]);
        cachePreparedAppearanceProxySetterValue(runtime, bridge, receiver,
                                                prepared, &setterValue, 1);
      }
      return ctx.result;
    }
  }

  if (dispatchSuperClass == Nil && !initializerClassWrapper &&
      providedCount <= 2) {
    Value fastArgs[2];
    for (size_t i = 0; i < providedCount; i++) {
      fastArgs[i] = Value::borrowed(runtime, arguments[i]);
    }
    Value fastResult;
    if (tryCallFastEngineObjCSelector(runtime, bridge, receiver, prepared,
                                      fastArgs, providedCount, Nil,
                                      &fastResult)) {
      cachePreparedAppearanceProxySetterValue(runtime, bridge, receiver,
                                              prepared, fastArgs,
                                              providedCount);
      fastResult = tagPreparedStaticAppearanceSelectorResult(
          runtime, bridge, receiver, prepared, std::move(fastResult));
      return fastResult.local(runtime);
    }
  }

  NativeApiArgumentFrame frame(signature.argumentTypes.size());
  for (size_t i = 0; i < providedCount; i++) {
    if (!prepareQuickJSEngineArgument(runtime, bridge,
                                      signature.argumentTypes[i],
                                      arguments[i], frame, i)) {
      throw JSError(runtime,
                    "Objective-C argument is not supported by QuickJS engine: " +
                        prepared.selectorName);
    }
  }

  const bool hasImplicitNSErrorOutArg =
      isNSErrorOutMethod && providedCount + 1 == signature.argumentTypes.size();
  NSError* implicitNSError = nil;
  if (hasImplicitNSErrorOutArg) {
    size_t outArgIndex = signature.argumentTypes.size() - 1;
    void* target = frame.storageAt(outArgIndex, sizeof(NSError**));
    NSError** implicitNSErrorOutArg = &implicitNSError;
    *static_cast<void**>(target) = implicitNSErrorOutArg;
  }

  NativeApiPointerFrame values(signature.argumentTypes.size() + 2);
  size_t valueIndex = 0;
  struct objc_super superReceiver = {receiver, dispatchSuperClass};
  struct objc_super* superReceiverPtr = &superReceiver;
  if (dispatchSuperClass != Nil) {
    values.set(valueIndex++, &superReceiverPtr);
  } else {
    values.set(valueIndex++, &receiver);
  }
  values.set(valueIndex++, const_cast<SEL*>(&prepared.selector));
  for (size_t i = 0; i < signature.argumentTypes.size(); i++) {
    values.set(valueIndex++, frame.values()[i]);
  }

  NativeApiReturnStorage returnStorage(
      nativeSizeForType(signature.returnType));
  performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
    if (prepared.preparedInvoker != nullptr && dispatchSuperClass == Nil) {
      prepared.preparedInvoker(reinterpret_cast<void*>(objc_msgSend),
                               values.data(), returnStorage.data());
    } else {
#if defined(__x86_64__)
      bool isStret = signature.returnType.ffiType->size > 16 &&
                     signature.returnType.ffiType->type == FFI_TYPE_STRUCT;
      void (*target)(void) =
          dispatchSuperClass != Nil
              ? (isStret ? FFI_FN(objc_msgSendSuper_stret)
                         : FFI_FN(objc_msgSendSuper))
              : (isStret ? FFI_FN(objc_msgSend_stret) : FFI_FN(objc_msgSend));
      ffi_call(const_cast<ffi_cif*>(&signature.cif), target,
               returnStorage.data(), values.data());
#else
      ffi_call(const_cast<ffi_cif*>(&signature.cif),
               dispatchSuperClass != Nil ? FFI_FN(objc_msgSendSuper)
                                         : FFI_FN(objc_msgSend),
               returnStorage.data(), values.data());
#endif
    }
  });

  NativeApiType returnType = signature.returnType;
  if (hasImplicitNSErrorOutArg && implicitNSError != nil) {
    const char* errorMessage = [[implicitNSError description] UTF8String];
    throw JSError(
        runtime, errorMessage != nullptr ? errorMessage : "Unknown NSError");
  }
  if (providedCount > 0) {
    Value setterValue = Value::borrowed(runtime, arguments[0]);
    cachePreparedAppearanceProxySetterValue(runtime, bridge, receiver,
                                            prepared, &setterValue, 1);
  }
  if (initializerClassWrapper) {
    id resultObject = nil;
    if (isObjectiveCObjectType(returnType)) {
      resultObject = *static_cast<id*>(returnStorage.data());
    }
    if (receiverHostObject != nullptr && resultObject != receiver) {
      receiverHostObject->disownObject(receiver);
    }
    if (resultObject != nil) {
      bridge->setObjectExpando(runtime, resultObject,
                               "__nativeApiClassWrapper",
                               Value(runtime, *initializerClassWrapper));
    }
  }
  tagPreparedStaticAppearanceNativeReturn(
      runtime, bridge, receiver, prepared, returnType, returnStorage.data());
  return setQuickJSEngineReturnValue(runtime, bridge, returnType,
                                     returnStorage.data(),
                                     prepared.selectorName);
}

static JSClassID gNativeApiSelectorGroupDataClassId = 0;

void NativeApiSelectorGroupFinalize(JSRuntime*, JSValue value) {
  auto* data = static_cast<NativeApiSelectorGroupData*>(
      JS_GetOpaque(value, gNativeApiSelectorGroupDataClassId));
  if (data != nullptr && data->runtime.state() != nullptr) {
    data->runtime.state()->untrack(data);
  }
  delete data;
}

void EnsureNativeApiSelectorGroupClass(Runtime& runtime) {
  JSRuntime* jsRuntime = JS_GetRuntime(runtime.context());
  if (gNativeApiSelectorGroupDataClassId == 0) {
    JS_NewClassID(jsRuntime, &gNativeApiSelectorGroupDataClassId);
  }

  auto state = runtime.state();
  if (!state->selectorGroupDataClassRegistered) {
    JSClassDef definition = {};
    definition.class_name = "NativeScriptEngineSelectorGroupData";
    definition.finalizer = NativeApiSelectorGroupFinalize;
    JS_NewClass(jsRuntime, gNativeApiSelectorGroupDataClassId,
                &definition);
    state->selectorGroupDataClassRegistered = true;
  }
}

JSValue NativeApiSelectorGroupCall(JSContext* context, JSValue thisValue,
                                   int argc, JSValue* argv, int,
                                   JSValue* dataValues) {
  auto* data = static_cast<NativeApiSelectorGroupData*>(
      JS_GetOpaque(dataValues[0], gNativeApiSelectorGroupDataClassId));
  if (data == nullptr || data->selectors == nullptr ||
      data->preparedInvocations == nullptr) {
    return JS_UNDEFINED;
  }

  Runtime& runtime = data->runtime;
  try {
    NativeApiRoundTripCacheFrameGuard roundTripFrame(data->bridge);
    size_t count = argc > 0 ? static_cast<size_t>(argc) : 0;
    auto call = resolveNativeApiSelectorGroupCall<true>(
        runtime, *data, count,
        [&]() -> id {
          auto* host = quickJSHostObjectRaw<NativeApiObjectHostObject>(
              runtime, thisValue);
          return host != nullptr ? host->object() : nil;
        },
        [&]() {
          return data->boundReceiverState == nullptr
                     ? quickJSHostObject<NativeApiObjectHostObject>(runtime,
                                                                    thisValue)
                     : nullptr;
        },
        [](uint64_t dispatchId) { return lookupObjCGsdInvoker(dispatchId); });
    if (call.hasImmediateResult) {
      return call.immediateResult.local(runtime);
    }
    JSValue result = setQuickJSEnginePreparedObjCResult(
        runtime, data->bridge, call.receiver, *call.prepared,
        call.receiverHostObject, call.initializerClassWrapper, count, argv,
        call.dispatchClass);
    if (!data->receiverIsClass && call.prepared->isInitMethod) {
      if (auto preserved = preservedNativeApiInitializerSelfReturn(
              runtime, data->bridge, call.receiver, Value::borrowed(runtime, result),
              Value::borrowed(runtime, thisValue))) {
        JS_FreeValue(context, result);
        return preserved->local(runtime);
      }
    }
    return result;
  } catch (const std::exception& error) {
    return engine::quickjsengine::throwError(context, error);
  }
}

Function CreateNativeApiSelectorGroupFunctionImpl(
    Runtime& runtime, std::shared_ptr<NativeApiBridge> bridge,
    Class lookupClass, bool receiverIsClass,
    std::shared_ptr<std::vector<NativeApiSelectorGroupEntry>> selectors,
    std::shared_ptr<
        std::vector<std::shared_ptr<NativeApiPreparedObjCInvocation>>>
        preparedInvocations,
    std::weak_ptr<NativeApiObjectHostObject> boundReceiver,
    std::shared_ptr<NativeApiObjectLifetimeState> boundReceiverState) {
  EnsureNativeApiSelectorGroupClass(runtime);
  auto* data = new NativeApiSelectorGroupData(
      runtime.state(), std::move(bridge), lookupClass, receiverIsClass,
      std::move(selectors), std::move(preparedInvocations),
      std::move(boundReceiver), std::move(boundReceiverState));

  JSValue dataObject =
      JS_NewObjectClass(runtime.context(),
                        gNativeApiSelectorGroupDataClassId);
  if (JS_IsException(dataObject)) {
    delete data;
    throw JSError(runtime, "QuickJS selector group allocation failed.");
  }
  JS_SetOpaque(dataObject, data);
  runtime.state()->track(data, [](void* pointer) {
    auto* tracked = static_cast<NativeApiSelectorGroupData*>(pointer);
    tracked->runtime.detachState();
    tracked->bridge.reset();
    tracked->selectors.reset();
    tracked->preparedInvocations.reset();
    tracked->boundReceiver.reset();
    tracked->boundReceiverState.reset();
  });

  JSValue function =
      JS_NewCFunctionData(runtime.context(), NativeApiSelectorGroupCall,
                          0, 0, 1, &dataObject);
  JS_FreeValue(runtime.context(), dataObject);
  if (JS_IsException(function)) {
    throw JSError(runtime, "QuickJS selector group function allocation failed.");
  }

  JSValue nameValue = JS_NewStringLen(runtime.context(), "__nativeSelectorGroup",
                                      std::strlen("__nativeSelectorGroup"));
  JS_DefinePropertyValueStr(runtime.context(), function, "name", nameValue,
                            JS_PROP_CONFIGURABLE);
  Value functionValue(runtime, function);
  Function result = functionValue.asObject(runtime).asFunction(runtime);
  JS_FreeValue(runtime.context(), function);
  return result;
}
