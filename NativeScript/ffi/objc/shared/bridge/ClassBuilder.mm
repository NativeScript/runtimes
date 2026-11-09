std::string readOptionalStringProperty(Runtime& runtime, const Object& object,
                                       const char* name) {
  if (name == nullptr || !object.hasProperty(runtime, name)) {
    return "";
  }
  Value value = object.getProperty(runtime, name);
  return value.isString() ? value.asString(runtime).utf8(runtime) : "";
}

struct NativeApiClassBuilderRegistration {
  std::shared_ptr<Runtime> runtimeOwner;
  Runtime* runtime = nullptr;
  std::shared_ptr<NativeApiBridge> bridge;
};

std::mutex gNativeApiClassBuilderMutex;
std::unordered_map<Class, NativeApiClassBuilderRegistration>
    gNativeApiClassBuilders;
struct NativeApiKnownExposedMethod {
  std::string selectorName;
  NativeApiSignature signature;
};
std::mutex gNativeApiKnownExposedMethodsMutex;
std::unordered_map<std::string, NativeApiKnownExposedMethod>
    gNativeApiKnownExposedMethods;

void rememberNativeApiClassBuilder(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    Class cls) {
  if (cls == Nil) {
    return;
  }
  std::lock_guard<std::mutex> lock(gNativeApiClassBuilderMutex);
  auto runtimeOwner = retainNativeApiRuntime(runtime);
  gNativeApiClassBuilders[cls] = NativeApiClassBuilderRegistration{
      .runtimeOwner = runtimeOwner,
      .runtime = runtimeOwner.get(),
      .bridge = bridge,
  };
}

void rememberNativeApiKnownExposedMethod(
    const std::string& selectorName, const NativeApiSignature& signature) {
  if (selectorName.empty()) {
    return;
  }
  NativeApiKnownExposedMethod method{
      .selectorName = selectorName,
      .signature = signature,
  };
  std::lock_guard<std::mutex> lock(gNativeApiKnownExposedMethodsMutex);
  gNativeApiKnownExposedMethods[selectorName] = method;
  gNativeApiKnownExposedMethods[jsifySelector(selectorName.c_str())] =
      std::move(method);
}

std::optional<NativeApiKnownExposedMethod> knownNativeApiExposedMethod(
    const std::string& name) {
  std::lock_guard<std::mutex> lock(gNativeApiKnownExposedMethodsMutex);
  auto it = gNativeApiKnownExposedMethods.find(name);
  if (it == gNativeApiKnownExposedMethods.end()) {
    return std::nullopt;
  }
  NativeApiKnownExposedMethod method = it->second;
  prepareEngineMethodSignature(&method.signature);
  return method;
}

// An ObjC init convention lets an initializer return an object other than
// `self` (most commonly `self` itself, but a class cluster/singleton
// initializer can return a completely different receiver). When it returns
// the SAME receiver we already wrapped for this call, the JS side should keep
// resolving to that one preserved wrapper rather than creating (and briefly
// GC'ing) a second, divergent one for the identical native object — so the
// stale/duplicate wrapper here is detached (its bridge state, e.g. expandos,
// handed to the preserved wrapper) rather than left to tear down the shared
// native receiver's bridge state on its own destruction.
std::optional<Value> preservedNativeApiInitializerSelfReturn(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const Value& result, const Value& receiverValue) {
  if (bridge == nullptr || receiver == nil || !receiverValue.isObject()) {
    return std::nullopt;
  }

  id resultObject =
      NativeApiObjectHostObject::nativeObjectFromValue(runtime, result);
  if (resultObject != receiver) {
    return std::nullopt;
  }

  Object receiverObject = receiverValue.asObject(runtime);
  if (!receiverObject.isHostObject<NativeApiObjectHostObject>(runtime)) {
    return std::nullopt;
  }

  auto receiverHostObject =
      receiverObject.getHostObject<NativeApiObjectHostObject>(runtime);
  if (receiverHostObject == nullptr ||
      receiverHostObject->object() != receiver) {
    return std::nullopt;
  }

  std::shared_ptr<NativeApiObjectHostObject> resultHostObject;
  if (result.isObject()) {
    Object resultObjectValue = result.asObject(runtime);
    if (resultObjectValue.isHostObject<NativeApiObjectHostObject>(runtime)) {
      resultHostObject =
          resultObjectValue.getHostObject<NativeApiObjectHostObject>(runtime);
    }
  }

  if (resultHostObject != nullptr && resultHostObject != receiverHostObject) {
    resultHostObject->detachObjectPreservingBridgeState(receiver);
  }

  Value preserved(runtime, receiverValue);
  bridge->rememberNativeObjectRoundTripValue(runtime, receiver, preserved);
  return preserved;
}

// $base/super dispatch wrapper for ClassBuilder subclasses: calls the ObjC
// super implementation, then — for initializers only — applies the
// preserved-self-return handling above.
Value callNativeApiBaseObjectSelector(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const Object& receiverObject,
    const std::shared_ptr<NativeApiObjectHostObject>& receiverHostObject,
    id receiver, const std::string& selectorName,
    const NativeApiMember* member, const Value* args, size_t count,
    Class dispatchClass) {
  Value result = receiverHostObject->callObjectSelector(
      runtime, selectorName, member, args, count, dispatchClass);

  if (selectorName.rfind("init", 0) != 0) {
    return result;
  }

  if (auto preserved = preservedNativeApiInitializerSelfReturn(
          runtime, bridge, receiver, result, Value(runtime, receiverObject))) {
    return std::move(*preserved);
  }
  return result;
}

std::optional<NativeApiClassBuilderRegistration>
findNativeApiClassBuilder(id object) {
  Class cls = object != nil ? object_getClass(object) : Nil;
  std::lock_guard<std::mutex> lock(gNativeApiClassBuilderMutex);
  while (cls != Nil) {
    auto it = gNativeApiClassBuilders.find(cls);
    if (it != gNativeApiClassBuilders.end()) {
      return it->second;
    }
    cls = class_getSuperclass(cls);
  }
  return std::nullopt;
}

const char* nativeApiEngineFastEnumerationEncoding() {
  static const char* encoding = nullptr;
  if (encoding == nullptr) {
    struct objc_method_description desc = protocol_getMethodDescription(
        @protocol(NSFastEnumeration),
        @selector(countByEnumeratingWithState:objects:count:), YES, YES);
    encoding = desc.types;
  }
  return encoding;
}

NSUInteger nativeApiEngineSymbolIteratorCountByEnumerating(
    id self, SEL, NSFastEnumerationState* state,
    id __unsafe_unretained stackbuf[], NSUInteger len) {
  if (len == 0 || state == nullptr || stackbuf == nullptr) {
    return 0;
  }

  auto registration = findNativeApiClassBuilder(self);
  if (!registration || registration->runtime == nullptr ||
      registration->bridge == nullptr) {
    return 0;
  }

  Runtime& runtime = *registration->runtime;
  NativeApiRuntimeScope runtimeScope(runtime);
  auto bridge = registration->bridge;
  try {
    Value receiver = makeNativeObjectValue(runtime, bridge, self, false);
    if (!receiver.isObject()) {
      return 0;
    }

    Value iteratorFactoryValue =
        runtime.global().getProperty(runtime,
                                     "__nativeScriptCreateNativeApiIterator");
    if (!iteratorFactoryValue.isObject() ||
        !iteratorFactoryValue.asObject(runtime).isFunction(runtime)) {
      return 0;
    }

    Function iteratorFactory =
        iteratorFactoryValue.asObject(runtime).asFunction(runtime);
    Value prototype =
        bridge->findClassPrototype(runtime, object_getClass(self));
    Value iteratorValue =
        prototype.isObject()
            ? iteratorFactory.call(runtime, Value(runtime, receiver),
                                   Value(runtime, prototype))
            : iteratorFactory.call(runtime, Value(runtime, receiver));
    if (!iteratorValue.isObject()) {
      return 0;
    }
    Object iterator = iteratorValue.asObject(runtime);
    Value nextValue = iterator.getProperty(runtime, "next");
    if (!nextValue.isObject() ||
        !nextValue.asObject(runtime).isFunction(runtime)) {
      return 0;
    }
    Function next = nextValue.asObject(runtime).asFunction(runtime);

    auto callNext = [&]() -> Value {
      return next.callWithThis(runtime, iterator);
    };

    for (unsigned long skipped = 0; skipped < state->state; skipped++) {
      Value skippedResult = callNext();
      if (!skippedResult.isObject()) {
        return 0;
      }
      Value doneValue =
          skippedResult.asObject(runtime).getProperty(runtime, "done");
      if (doneValue.isBool() && doneValue.getBool()) {
        return 0;
      }
    }

    NSUInteger count = 0;
    while (count < len) {
      Value nextResult = callNext();
      if (!nextResult.isObject()) {
        break;
      }
      Object nextObject = nextResult.asObject(runtime);
      Value doneValue = nextObject.getProperty(runtime, "done");
      if (doneValue.isBool() && doneValue.getBool()) {
        break;
      }

      Value value = nextObject.getProperty(runtime, "value");
      NativeApiArgumentFrame frame(1);
      id nativeValue = objectFromEngineValue(runtime, bridge, value, frame, false);
      if (nativeValue != nil) {
        [nativeValue retain];
        [nativeValue autorelease];
      }
      stackbuf[count++] = nativeValue;
    }

    state->itemsPtr = stackbuf;
    state->mutationsPtr = &state->extra[0];
    state->extra[0] = 0;
    state->state += count;
    return count;
  } catch (const std::exception&) {
    return 0;
  }
}

NativeApiSymbol runtimeSymbolForClass(
    const std::shared_ptr<NativeApiBridge>& bridge, Class cls) {
  if (bridge != nullptr) {
    if (const NativeApiSymbol* symbol = bridge->findClassForRuntimeClass(cls)) {
      return *symbol;
    }
  }

  const char* name = cls != Nil ? class_getName(cls) : "";
  return NativeApiSymbol{
      .kind = NativeApiSymbolKind::Class,
      .offset = MD_SECTION_OFFSET_NULL,
      .name = name != nullptr ? name : "",
      .runtimeName = name != nullptr ? name : "",
  };
}

std::string nextAvailableEngineClassName(const std::string& requestedName) {
  if (requestedName.empty()) {
    return "";
  }
  if (objc_lookUpClass(requestedName.c_str()) == Nil) {
    return requestedName;
  }

  size_t suffix = 1;
  std::string candidate;
  do {
    candidate = requestedName + std::to_string(suffix++);
  } while (objc_lookUpClass(candidate.c_str()) != Nil);
  return candidate;
}

std::vector<NativeApiMember> methodOverridesForName(
    const std::vector<NativeApiMember>& members, const std::string& name) {
  std::vector<NativeApiMember> result;
  std::unordered_set<std::string> selectors;
  for (const auto& member : members) {
    if (member.property || member.name != name ||
        (member.flags & metagen::mdMemberStatic) != 0 ||
        member.selectorName.empty()) {
      continue;
    }
    if (selectors.insert(member.selectorName).second) {
      result.push_back(member);
    }
  }
  return result;
}

const NativeApiMember* propertyOverrideForName(
    const std::vector<NativeApiMember>& members, const std::string& name) {
  const NativeApiMember* propertyMember = nullptr;
  for (const auto& member : members) {
    if (member.property && member.name == name &&
        (member.flags & metagen::mdMemberStatic) == 0) {
      if (propertyMember == nullptr) {
        propertyMember = &member;
      }
      if (!member.readonly && !member.setterSelectorName.empty()) {
        return &member;
      }
    }
  }
  return propertyMember;
}

void addEngineOverrideMethod(Runtime& runtime,
                          const std::shared_ptr<NativeApiBridge>& bridge,
                          Class nativeClass, Class baseClass,
                          const std::string& selectorName,
                          MDSectionOffset signatureOffset,
                          bool returnOwned, Function function,
                          NativeApiMethodCallbackPolicy methodPolicy = {}) {
  if (selectorName.empty() || signatureOffset == MD_SECTION_OFFSET_NULL) {
    return;
  }

  auto callback = createEngineMethodCallback(runtime, bridge, selectorName,
                                          signatureOffset, std::move(function),
                                          returnOwned, baseClass,
                                          std::move(methodPolicy));
  SEL selector = sel_registerName(selectorName.c_str());
  std::string metadataEncoding =
      objcMethodSignatureForEngineSignature(callback->signature());
  class_replaceMethod(nativeClass, selector,
                      reinterpret_cast<IMP>(callback->functionPointer()),
                      metadataEncoding.c_str());
}

Value getObjectPropertyOrUndefined(Runtime& runtime, const Object& object,
                                   const std::string& name) {
  return object.hasProperty(runtime, name.c_str())
             ? object.getProperty(runtime, name.c_str())
             : Value::undefined();
}

// Auto-applied to native accessor (getter/setter) overrides: suppresses
// re-entrancy while the accessor machinery is already dispatching through
// this same receiver (set/cleared around JS-subclass accessor invocation).
NativeApiMethodCallbackPolicy nativeAccessorCallbackPolicy(
    NativeApiMethodCallbackPolicy policy = {}) {
  policy.skipCallbackIfAssociatedObjectTruthy.push_back(
      "__nativeApiAccessorCallbackState");
  return policy;
}

Class dispatchSuperclassForEngineDerivedReceiver(id receiver,
                                                Class defaultSuperclass) {
  if (receiver == nil) {
    return Nil;
  }

  Class receiverClass = object_getClass(receiver);
  if (receiverClass == Nil ||
      !class_conformsToProtocol(receiverClass,
                                @protocol(NativeApiClassBuilderProtocol))) {
    return Nil;
  }

  Class superclass = class_getSuperclass(receiverClass);
  return superclass != Nil ? superclass : defaultSuperclass;
}

std::optional<Function> functionForSelector(Runtime& runtime,
                                            const Object& methods,
                                            const std::string& selectorName) {
  Value value = getObjectPropertyOrUndefined(runtime, methods, selectorName);
  if (!value.isObject() || !value.asObject(runtime).isFunction(runtime)) {
    std::string jsName = jsifySelector(selectorName.c_str());
    if (jsName != selectorName) {
      value = getObjectPropertyOrUndefined(runtime, methods, jsName);
    }
  }
  if (!value.isObject() || !value.asObject(runtime).isFunction(runtime)) {
    return std::nullopt;
  }
  return value.asObject(runtime).asFunction(runtime);
}

std::optional<NativeApiType> readExposedType(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const Object& descriptor, const char* propertyName) {
  if (!descriptor.hasProperty(runtime, propertyName)) {
    return std::nullopt;
  }
  return interopTypeFromValue(runtime, bridge,
                              descriptor.getProperty(runtime, propertyName));
}

std::optional<NativeApiSignature> exposedMethodSignature(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const std::string& selectorName, const Object& descriptor) {
  NativeApiSignature signature;
  if (auto returnType = readExposedType(runtime, bridge, descriptor, "returns")) {
    signature.returnType = *returnType;
  } else {
    signature.returnType = primitiveInteropType(metagen::mdTypeVoid);
  }

  Value paramsValue = getObjectPropertyOrUndefined(runtime, descriptor, "params");
  if (!paramsValue.isUndefined() && !paramsValue.isNull()) {
    if (!paramsValue.isObject() || !paramsValue.asObject(runtime).isArray(runtime)) {
      throw JSError(
          runtime, "exposedMethods params must be an array.");
    }
    Array params = paramsValue.asObject(runtime).getArray(runtime);
    for (size_t i = 0; i < params.size(runtime); i++) {
      Value typeValue = params.getValueAtIndex(runtime, i);
      auto type = interopTypeFromValue(runtime, bridge, typeValue);
      if (!type) {
        throw JSError(
            runtime, "exposedMethods contains an unsupported parameter type.");
      }
      signature.argumentTypes.push_back(*type);
    }
  }

  // A colon-less selector may still declare params (@nativescript/core
  // exposes onReceive with one NSNotification param); the Objective-C runtime
  // accepts such methods and callers like NSNotificationCenter invoke them
  // with the argument. Only reject a mismatch when the selector explicitly
  // declares argument slots.
  size_t selectorArguments = selectorArgumentCount(selectorName);
  if (selectorArguments != signature.argumentTypes.size() && selectorArguments != 0) {
    throw JSError(
        runtime, "exposedMethods selector argument count does not match params.");
  }

  prepareEngineMethodSignature(&signature);
  return signature;
}

std::optional<NativeApiSignature> runtimeProtocolMethodSignature(
    const char* types) {
  if (types == nullptr) {
    return std::nullopt;
  }

  NSMethodSignature* methodSignature =
      [NSMethodSignature signatureWithObjCTypes:types];
  if (methodSignature == nil || methodSignature.numberOfArguments < 2) {
    return std::nullopt;
  }

  NativeApiSignature signature;
  signature.implicitArgumentCount = 2;
  signature.returnType =
      parseObjCEncodedEngineType(methodSignature.methodReturnType);
  for (NSUInteger i = 2; i < methodSignature.numberOfArguments; i++) {
    signature.argumentTypes.push_back(
        parseObjCEncodedEngineType([methodSignature getArgumentTypeAtIndex:i]));
  }
  if (unsupportedEngineType(signature.returnType)) {
    return std::nullopt;
  }
  for (const auto& argumentType : signature.argumentTypes) {
    if (unsupportedEngineType(argumentType)) {
      return std::nullopt;
    }
  }
  return signature;
}

std::optional<NativeApiSymbol> protocolSymbolFromEngineValue(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const Value& value) {
  if (value.isString()) {
    std::string name = value.asString(runtime).utf8(runtime);
    if (const NativeApiSymbol* symbol = bridge->findProtocol(name)) {
      return *symbol;
    }
    return std::nullopt;
  }
  if (!value.isObject()) {
    return std::nullopt;
  }

  Object object = value.asObject(runtime);
  if (object.isHostObject<NativeApiProtocolHostObject>(runtime)) {
    return object.getHostObject<NativeApiProtocolHostObject>(runtime)->symbol();
  }

  if (stringPropertyOrEmpty(runtime, object, "kind") != "protocol") {
    return std::nullopt;
  }

  std::string runtimeName = stringPropertyOrEmpty(runtime, object, "runtimeName");
  if (!runtimeName.empty()) {
    if (const NativeApiSymbol* symbol = bridge->findProtocol(runtimeName)) {
      return *symbol;
    }
  }

  std::string name = stringPropertyOrEmpty(runtime, object, "name");
  if (!name.empty()) {
    if (const NativeApiSymbol* symbol = bridge->findProtocol(name)) {
      return *symbol;
    }
  }

  return std::nullopt;
}

void addEngineExposedMethod(Runtime& runtime,
                         const std::shared_ptr<NativeApiBridge>& bridge,
                         Class nativeClass, const std::string& selectorName,
                         NativeApiSignature signature, Function function,
                         Class methodBaseClass = Nil,
                         NativeApiMethodCallbackPolicy methodPolicy = {}) {
  if (selectorName.empty()) {
    return;
  }
  auto callback = createEngineMethodCallback(runtime, bridge, selectorName,
                                          std::move(signature), std::move(function),
                                          methodBaseClass,
                                          std::move(methodPolicy));
  std::string encoding = objcMethodSignatureForEngineSignature(callback->signature());
  class_replaceMethod(nativeClass, sel_registerName(selectorName.c_str()),
                      reinterpret_cast<IMP>(callback->functionPointer()),
                      encoding.c_str());
}

bool addRuntimeProtocolOverrideForName(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    Class nativeClass, const std::vector<Protocol*>& protocols,
    const std::string& propertyName, Function function) {
  std::unordered_set<Protocol*> visited;
  std::function<bool(Protocol*)> visit = [&](Protocol* protocol) -> bool {
    if (protocol == nullptr || !visited.insert(protocol).second) {
      return false;
    }

    Protocol** inherited = protocol_copyProtocolList(protocol, nullptr);
    if (inherited != nullptr) {
      unsigned int inheritedCount = 0;
      free(inherited);
      inherited = protocol_copyProtocolList(protocol, &inheritedCount);
      for (unsigned int i = 0; i < inheritedCount; i++) {
        if (visit(inherited[i])) {
          free(inherited);
          return true;
        }
      }
      free(inherited);
    }

    for (BOOL required : {YES, NO}) {
      unsigned int count = 0;
      objc_method_description* descriptions =
          protocol_copyMethodDescriptionList(protocol, required, YES, &count);
      for (unsigned int i = 0; i < count; i++) {
        SEL selector = descriptions[i].name;
        const char* selectorName =
            selector != nullptr ? sel_getName(selector) : nullptr;
        if (selectorName == nullptr ||
            jsifySelector(selectorName) != propertyName) {
          continue;
        }
        auto signature = runtimeProtocolMethodSignature(descriptions[i].types);
        if (signature) {
          addEngineExposedMethod(runtime, bridge, nativeClass, selectorName,
                              std::move(*signature), std::move(function));
          free(descriptions);
          return true;
        }
      }
      free(descriptions);
    }
    return false;
  };

  for (Protocol* protocol : protocols) {
    if (visit(protocol)) {
      return true;
    }
  }
  return false;
}

Object getOwnPropertyDescriptor(Runtime& runtime, const Object& object,
                                const std::string& name) {
  Object objectCtor = runtime.global().getPropertyAsObject(runtime, "Object");
  Function getOwnPropertyDescriptor =
      objectCtor.getPropertyAsFunction(runtime, "getOwnPropertyDescriptor");
  Value args[] = {Value(runtime, object), makeString(runtime, name)};
  Value descriptorValue =
      getOwnPropertyDescriptor.call(runtime, static_cast<const Value*>(args),
                                    static_cast<size_t>(2));
  return descriptorValue.isObject() ? descriptorValue.asObject(runtime)
                                    : Object(runtime);
}

Value extendNativeApiClass(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const Value* args, size_t count) {
  if (count < 2 || !args[0].isObject() || !args[1].isObject()) {
    throw JSError(
        runtime, "extendClass expects a native class and method object.");
  }

  Class baseClass = classFromEngineValue(runtime, args[0]);
  if (baseClass == Nil) {
    throw JSError(
        runtime, "extendClass can only extend native class constructors.");
  }
  if (class_conformsToProtocol(baseClass,
                               @protocol(NativeApiClassBuilderProtocol))) {
    throw JSError(runtime,
                                 "Cannot extend an already extended class.");
  }

  Object methods = args[1].asObject(runtime);
  Object options = count >= 3 && args[2].isObject()
                       ? args[2].asObject(runtime)
                       : Object(runtime);
  std::string requestedName = readOptionalStringProperty(runtime, options, "name");
  if (requestedName.empty()) {
    const char* baseName = class_getName(baseClass);
    requestedName = std::string(baseName != nullptr ? baseName : "NSObject") +
                    "_Extended_" + std::to_string(rand());
  }

  std::string className = nextAvailableEngineClassName(requestedName);
  Class nativeClass = objc_allocateClassPair(baseClass, className.c_str(), 0);
  if (nativeClass == Nil) {
    throw JSError(runtime, "Failed to allocate Objective-C class.");
  }

  class_addProtocol(nativeClass, @protocol(NativeApiClassBuilderProtocol));
  rememberNativeApiClassBuilder(runtime, bridge, nativeClass);

  NativeApiSymbol baseSymbol = runtimeSymbolForClass(bridge, baseClass);
  std::vector<NativeApiMember> extensionMembers =
      bridge->membersForClass(baseSymbol);
  std::vector<Protocol*> optionProtocols;
  Value protocolsValue = getObjectPropertyOrUndefined(runtime, options, "protocols");
  if (protocolsValue.isObject() &&
      protocolsValue.asObject(runtime).isArray(runtime)) {
    Array protocols = protocolsValue.asObject(runtime).getArray(runtime);
    for (size_t i = 0; i < protocols.size(runtime); i++) {
      Value protocolValue = protocols.getValueAtIndex(runtime, i);
      Protocol* protocol = protocolFromEngineValue(runtime, protocolValue);
      std::optional<NativeApiSymbol> protocolSymbol =
          protocolSymbolFromEngineValue(runtime, bridge, protocolValue);
      if (protocol != nullptr) {
        optionProtocols.push_back(protocol);
        class_addProtocol(nativeClass, protocol);
        if (!protocolSymbol) {
          if (const NativeApiSymbol* runtimeSymbol =
                  bridge->findProtocolForRuntimePointer(protocol)) {
            protocolSymbol = *runtimeSymbol;
          }
        }
      }
      if (protocolSymbol) {
        const auto& protocolMembers = bridge->membersForProtocol(*protocolSymbol);
        extensionMembers.insert(extensionMembers.begin(),
                                protocolMembers.begin(),
                                protocolMembers.end());
      }
    }
  }
  const auto& members = extensionMembers;
  Array propertyNames = methods.getPropertyNames(runtime);
  for (size_t i = 0; i < propertyNames.size(runtime); i++) {
    Value propertyNameValue = propertyNames.getValueAtIndex(runtime, i);
    if (!propertyNameValue.isString()) {
      continue;
    }

    std::string propertyName = propertyNameValue.asString(runtime).utf8(runtime);
    Object descriptor = getOwnPropertyDescriptor(runtime, methods, propertyName);

    Value value = descriptor.getProperty(runtime, "value");
    if (value.isObject() && value.asObject(runtime).isFunction(runtime)) {
      auto overrides = methodOverridesForName(members, propertyName);
      bool addedOverride = false;
      for (const auto& member : overrides) {
        if (member.selectorName.empty() ||
            member.signatureOffset == MD_SECTION_OFFSET_NULL ||
            member.signatureOffset == 0) {
          continue;
        }
        addEngineOverrideMethod(
            runtime, bridge, nativeClass, baseClass, member.selectorName,
            member.signatureOffset,
            (member.flags & metagen::mdMemberReturnOwned) != 0,
            value.asObject(runtime).asFunction(runtime));
        addedOverride = true;
      }
	      if (!addedOverride) {
	        bool addedRuntimeProtocolOverride = addRuntimeProtocolOverrideForName(
	            runtime, bridge, nativeClass, optionProtocols, propertyName,
	            value.asObject(runtime).asFunction(runtime));
	        if (!addedRuntimeProtocolOverride) {
	          if (auto known = knownNativeApiExposedMethod(propertyName)) {
	            addEngineExposedMethod(runtime, bridge, nativeClass,
	                                known->selectorName,
	                                std::move(known->signature),
	                                value.asObject(runtime).asFunction(runtime),
	                                baseClass);
	          }
	        }
	      }
	    }

    const NativeApiMember* propertyMember =
        propertyOverrideForName(members, propertyName);

    Value getter = descriptor.getProperty(runtime, "get");
    if (propertyMember != nullptr && getter.isObject() &&
        getter.asObject(runtime).isFunction(runtime)) {
      addEngineOverrideMethod(
          runtime, bridge, nativeClass, baseClass,
          propertyMember->selectorName, propertyMember->signatureOffset,
          (propertyMember->flags & metagen::mdMemberReturnOwned) != 0,
          getter.asObject(runtime).asFunction(runtime),
          nativeAccessorCallbackPolicy());
    } else if (propertyMember == nullptr && getter.isObject() &&
               getter.asObject(runtime).isFunction(runtime)) {
      auto overrides = methodOverridesForName(members, propertyName);
      for (const auto& member : overrides) {
        if (selectorArgumentCount(member.selectorName) != 0) {
          continue;
        }
        addEngineOverrideMethod(
            runtime, bridge, nativeClass, baseClass, member.selectorName,
            member.signatureOffset,
            (member.flags & metagen::mdMemberReturnOwned) != 0,
            getter.asObject(runtime).asFunction(runtime),
            nativeAccessorCallbackPolicy());
      }
    }

    Value setter = descriptor.getProperty(runtime, "set");
    if (propertyMember != nullptr &&
        setter.isObject() && setter.asObject(runtime).isFunction(runtime) &&
        !propertyMember->setterSelectorName.empty()) {
      addEngineOverrideMethod(runtime, bridge, nativeClass, baseClass,
                           propertyMember->setterSelectorName,
                           propertyMember->setterSignatureOffset, false,
                           setter.asObject(runtime).asFunction(runtime),
                           nativeAccessorCallbackPolicy());
    }
  }

  Value exposedMethodsValue =
      getObjectPropertyOrUndefined(runtime, options, "exposedMethods");
  if (!exposedMethodsValue.isObject()) {
    exposedMethodsValue =
        getObjectPropertyOrUndefined(runtime, methods, "ObjCExposedMethods");
  }
  if (exposedMethodsValue.isObject()) {
    Object exposedMethods = exposedMethodsValue.asObject(runtime);
    Array exposedNames = exposedMethods.getPropertyNames(runtime);
    for (size_t i = 0; i < exposedNames.size(runtime); i++) {
      Value selectorValue = exposedNames.getValueAtIndex(runtime, i);
      if (!selectorValue.isString()) {
        continue;
      }
      std::string selectorName = selectorValue.asString(runtime).utf8(runtime);
      Value descriptorValue =
          getObjectPropertyOrUndefined(runtime, exposedMethods, selectorName);
      if (!descriptorValue.isObject()) {
        continue;
      }
      auto function = functionForSelector(runtime, methods, selectorName);
      if (!function) {
        continue;
      }
	      auto signature = exposedMethodSignature(
	          runtime, bridge, selectorName, descriptorValue.asObject(runtime));
	      if (signature) {
	        rememberNativeApiKnownExposedMethod(selectorName, *signature);
	        addEngineExposedMethod(runtime, bridge, nativeClass, selectorName,
	                            std::move(*signature), std::move(*function),
	                            baseClass);
	      }
    }
  }

  Value hasIteratorValue =
      getObjectPropertyOrUndefined(runtime, options, "__hasIterator");
  if (hasIteratorValue.isBool() && hasIteratorValue.getBool()) {
    class_addProtocol(nativeClass, @protocol(NSFastEnumeration));
    if (const char* encoding = nativeApiEngineFastEnumerationEncoding()) {
      class_replaceMethod(
          nativeClass,
          @selector(countByEnumeratingWithState:objects:count:),
          reinterpret_cast<IMP>(nativeApiEngineSymbolIteratorCountByEnumerating),
          encoding);
    }
  }

  objc_registerClassPair(nativeClass);

	  NativeApiSymbol newSymbol = baseSymbol;
	  newSymbol.name = className;
	  newSymbol.runtimeName = className;
	  newSymbol.superclassOffset = baseSymbol.offset;
	  return makeNativeClassValue(runtime, bridge, std::move(newSymbol));
	}

Value invokeNativeApiBaseMethod(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const Value* args, size_t count) {
  if (count < 3 || !args[0].isObject() || !args[1].isObject() ||
      !args[2].isString()) {
    throw JSError(
        runtime, "__invokeBase expects base class, receiver, and member name.");
  }

  Class baseClass = classFromEngineValue(runtime, args[0]);
  if (baseClass == Nil) {
    throw JSError(runtime, "__invokeBase base class is invalid.");
  }

  Object receiverObject = args[1].asObject(runtime);
  if (!receiverObject.isHostObject<NativeApiObjectHostObject>(runtime)) {
    throw JSError(runtime, "__invokeBase receiver is not native.");
  }

  auto receiverHostObject =
      receiverObject.getHostObject<NativeApiObjectHostObject>(runtime);
  id receiver = receiverHostObject->object();
  std::string memberName = args[2].asString(runtime).utf8(runtime);
  size_t actualArgc = count - 3;

  NativeApiSymbol baseSymbol = runtimeSymbolForClass(bridge, baseClass);
  const auto& members = bridge->membersForClass(baseSymbol);
  const NativeApiMember* member =
      selectMethodMember(members, memberName, false, actualArgc);
  if (member == nullptr) {
    if (const NativeApiMember* propertyMember =
            selectWritablePropertyMember(members, memberName, false)) {
      if (actualArgc == 0) {
        Class dispatchClass =
            dispatchSuperclassForEngineDerivedReceiver(receiver, baseClass);
        return callNativeApiBaseObjectSelector(
            runtime, bridge, receiverObject, receiverHostObject, receiver,
            propertyMember->selectorName, propertyMember, nullptr, 0,
            dispatchClass);
      }
      if (actualArgc == 1 && !propertyMember->setterSelectorName.empty() &&
          !propertyMember->readonly) {
        Class dispatchClass =
            dispatchSuperclassForEngineDerivedReceiver(receiver, baseClass);
        NativeApiMember setterMember = *propertyMember;
        setterMember.selectorName = propertyMember->setterSelectorName;
        setterMember.signatureOffset = propertyMember->setterSignatureOffset;
        return callNativeApiBaseObjectSelector(
            runtime, bridge, receiverObject, receiverHostObject, receiver,
            setterMember.selectorName, &setterMember, args + 3, actualArgc,
            dispatchClass);
      }
    }
  }
  if (member == nullptr) {
    throw JSError(
        runtime, "Objective-C base selector is not available: " + memberName);
  }

  Class dispatchClass =
      dispatchSuperclassForEngineDerivedReceiver(receiver, baseClass);
  return callNativeApiBaseObjectSelector(
      runtime, bridge, receiverObject, receiverHostObject, receiver,
      member->selectorName, member, args + 3, actualArgc, dispatchClass);
}
