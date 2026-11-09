// Included by NativeApiV8.mm inside the NativeScript anonymous namespace.

#include "../shared/bridge/SelectorGroupData.h"

#include "NativeApiV8Marshalling.mm"

#include "NativeApiV8Gsd.mm"

#include "../shared/bridge/SelectorGroupCall.h"


void* lookupGeneratedEngineObjCGsdInvoker(uint64_t dispatchId) {
  return reinterpret_cast<void*>(lookupObjCGsdInvoker(dispatchId));
}

bool tryCallGeneratedEngineObjCSelector(
    Runtime&, const std::shared_ptr<NativeApiBridge>&, id,
    const NativeApiPreparedObjCInvocation&, const Value*, size_t, Class,
    Value*) {
  return false;
}

void setV8EnginePreparedObjCResult(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const NativeApiPreparedObjCInvocation& prepared,
    const std::shared_ptr<NativeApiObjectHostObject>& receiverHostObject,
    const std::optional<Object>& initializerClassWrapper,
    const v8::FunctionCallbackInfo<v8::Value>& info,
    Class dispatchSuperClass) {
  const NativeApiSignature& signature = prepared.signature;
  if (receiver == nil || signature.variadic ||
      unsupportedEngineType(signature.returnType)) {
    throw JSError(runtime,
                  "Objective-C selector is not supported by V8 engine: " +
                      prepared.selectorName);
  }

  const bool isNSErrorOutMethod = prepared.isNSErrorOutMethod;
  const size_t providedCount = static_cast<size_t>(info.Length());
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

  // GSD fast path: the generated invoker reads args directly from
  // FunctionCallbackInfo, calls objc_msgSend with a typed cast, and sets the
  // return via the V8 API — all in one generated function. Bypasses all
  // generic marshalling.
  if (prepared.gsdEngineCallable && dispatchSuperClass == Nil &&
      providedCount == prepared.gsdEngineArgumentCount &&
      !initializerClassWrapper && !isNSErrorOutMethod) {
    auto invoker = reinterpret_cast<ObjCGsdInvoker>(prepared.engineInvoker);
    GsdObjCContext ctx{runtime,
                       bridge,
                       receiver,
                       prepared.selector,
                       info,
                       runtime.isolate(),
                       runtime.context(),
                       signature.returnType};
    if (invoker(ctx)) {
      return;
    }
  }

  if (dispatchSuperClass == Nil && !initializerClassWrapper &&
      providedCount <= 2) {
    Value fastArgs[2];
    for (size_t i = 0; i < providedCount; i++) {
      fastArgs[i] = Value::borrowed(runtime, info[static_cast<int>(i)]);
    }
    Value fastResult;
    if (tryCallFastEngineObjCSelector(runtime, bridge, receiver, prepared,
                                      fastArgs, providedCount, Nil,
                                      &fastResult)) {
      info.GetReturnValue().Set(fastResult.local(runtime));
      return;
    }
  }

  NativeApiArgumentFrame frame(signature.argumentTypes.size());
  for (size_t i = 0; i < providedCount; i++) {
    if (!prepareV8EngineArgument(runtime, bridge, signature.argumentTypes[i],
                                 info[static_cast<int>(i)], frame, i)) {
      throw JSError(runtime,
                    "Objective-C argument is not supported by V8 engine: " +
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
  setV8EngineReturnValue(runtime, bridge, returnType, returnStorage.data(),
                         prepared.selectorName, info);
}

struct NativeApiSelectorGroupDataHolder {
  explicit NativeApiSelectorGroupDataHolder(
      std::shared_ptr<NativeApiSelectorGroupData> data)
      : data(std::move(data)) {}

  ~NativeApiSelectorGroupDataHolder() { function.Reset(); }

  std::shared_ptr<NativeApiSelectorGroupData> data;
  v8::Global<v8::Function> function;
};

void NativeApiSelectorGroupWeakCallback(
    const v8::WeakCallbackInfo<NativeApiSelectorGroupDataHolder>& info) {
  engine::v8engine::untrackRuntimeAllocation(info.GetIsolate(),
                                             info.GetParameter());
  delete info.GetParameter();
}

void NativeApiSelectorGroupCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* holder = static_cast<NativeApiSelectorGroupDataHolder*>(
      info.Data().As<v8::External>()->Value(
          v8::kExternalPointerTypeTagDefault));
  auto* data = holder != nullptr ? holder->data.get() : nullptr;
  if (data == nullptr || data->selectors == nullptr ||
      data->preparedInvocations == nullptr) {
    return;
  }

  Runtime& runtime = data->runtime;
  v8::HandleScope handleScope(runtime.isolate());
  try {
    NativeApiRoundTripCacheFrameGuard roundTripFrame(data->bridge);
    size_t count = static_cast<size_t>(info.Length());
    auto call = resolveNativeApiSelectorGroupCall<true>(
        runtime, *data, count,
        [&]() -> id {
          // The V8 handle keeps this raw host object alive for the call.
          auto* host =
              v8HostObjectRaw<NativeApiObjectHostObject>(info.This());
          return host != nullptr ? host->object() : nil;
        },
        [&]() {
          return v8HostObject<NativeApiObjectHostObject>(runtime, info.This());
        },
        [](uint64_t dispatchId) { return lookupObjCGsdInvoker(dispatchId); });
    if (call.hasImmediateResult) {
      info.GetReturnValue().Set(call.immediateResult.local(runtime));
      return;
    }
    // Inline GSD fast path: skip the setV8EnginePreparedObjCResult call and its
    // argument-count/NSError preamble entirely for the common case. The
    // generated invoker reads args, calls objc_msgSend, and sets the return.
    // Excludes appearance static selectors (gsdAllowed) — those need the
    // generic path's proxy tagging.
    if (call.prepared->gsdEngineCallable && call.dispatchClass == Nil &&
        !call.prepared->isInitMethod &&
        count == call.prepared->gsdEngineArgumentCount &&
        call.gsdAllowed) {
      auto invoker =
          reinterpret_cast<ObjCGsdInvoker>(call.prepared->engineInvoker);
      GsdObjCContext ctx{runtime,
                         data->bridge,
                         call.receiver,
                         call.prepared->selector,
                         info,
                         runtime.isolate(),
                         runtime.context(),
                         call.prepared->signature.returnType};
      if (invoker(ctx)) {
        if (count > 0) {
          Value setterValue = Value::borrowed(runtime, info[0]);
          cachePreparedAppearanceProxySetterValue(runtime, data->bridge,
                                                  call.receiver, *call.prepared,
                                                  &setterValue, 1);
        }
        return;
      }
    }
    setV8EnginePreparedObjCResult(
        runtime, data->bridge, call.receiver, *call.prepared,
        call.receiverHostObject, call.initializerClassWrapper, info,
        call.dispatchClass);
    if (!data->receiverIsClass && call.prepared->isInitMethod) {
      if (auto preserved = preservedNativeApiInitializerSelfReturn(
              runtime, data->bridge, call.receiver,
              Value(runtime, info.GetReturnValue().Get()),
              Value(runtime, info.This()))) {
        info.GetReturnValue().Set(preserved->local(runtime));
      }
    }
  } catch (const std::exception& exception) {
    engine::v8engine::throwV8Exception(info.GetIsolate(), exception);
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
  auto data = std::make_shared<NativeApiSelectorGroupData>(
      runtime.state(), std::move(bridge), lookupClass, receiverIsClass,
      std::move(selectors), std::move(preparedInvocations),
      std::move(boundReceiver), std::move(boundReceiverState));
  auto* holder = new NativeApiSelectorGroupDataHolder(std::move(data));
  engine::v8engine::trackRuntimeAllocation(runtime.isolate(), holder);

  v8::Local<v8::External> external =
      v8::External::New(runtime.isolate(), holder,
                        v8::kExternalPointerTypeTagDefault);
  v8::Local<v8::FunctionTemplate> functionTemplate =
      v8::FunctionTemplate::New(runtime.isolate(),
                                NativeApiSelectorGroupCallback, external);
  v8::Local<v8::Function> function =
      functionTemplate->GetFunction(runtime.context()).ToLocalChecked();
  function->SetName(
      engine::v8engine::makeV8String(runtime.isolate(), "__nativeSelectorGroup"));
  holder->function.Reset(runtime.isolate(), function);
  holder->function.SetWeak(holder, NativeApiSelectorGroupWeakCallback,
                           v8::WeakCallbackType::kParameter);
  Value functionValue(runtime, function);
  return functionValue.asObject(runtime).asFunction(runtime);
}
