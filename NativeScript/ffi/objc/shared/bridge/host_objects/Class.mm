class NativeApiClassHostObject final : public HostObject {
 public:
  NativeApiClassHostObject(std::shared_ptr<NativeApiBridge> bridge,
                           NativeApiSymbol symbol)
      : bridge_(std::move(bridge)), symbol_(std::move(symbol)) {}

  Class nativeClass() const {
    return objc_lookUpClass(symbol_.runtimeName.c_str());
  }

  static Class classRespondingToClassSelector(Class cls, SEL selector) {
    for (Class current = cls; current != Nil;
         current = class_getSuperclass(current)) {
      if (class_getClassMethod(current, selector) != nullptr) {
        return current;
      }
    }
    return Nil;
  }

  Value get(Runtime& runtime, const PropNameID& name) override {
    std::string property = name.utf8(runtime);
    if (property == "kind") {
      return makeString(runtime, "class");
    }
    if (property == "name") {
      return makeString(runtime, symbol_.name);
    }
    if (property == "runtimeName") {
      return makeString(runtime, symbol_.runtimeName);
    }
    if (property == "available") {
      return objc_lookUpClass(symbol_.runtimeName.c_str()) != nil;
    }
	    if (property == "metadataOffset") {
	      return static_cast<double>(symbol_.offset);
	    }
	    if (property == "__superclass") {
	      if (symbol_.superclassOffset == MD_SECTION_OFFSET_NULL) {
	        return Value::undefined();
	      }
	      const NativeApiSymbol* superclass =
	          bridge_->findClassByOffset(symbol_.superclassOffset);
	      if (superclass == nullptr) {
	        return Value::undefined();
	      }
	      return makeNativeClassValue(runtime, bridge_, *superclass);
	    }
	    if (property == "__runtimeStaticMembers" ||
	        property == "__runtimeInstanceMembers") {
	      return runtimeMembersArray(runtime, nativeClass(),
	                                 property == "__runtimeStaticMembers");
	    }
	    if (property == "__staticMembers" || property == "__instanceMembers") {
	      bool staticMembers = property == "__staticMembers";
      const auto& members = bridge_->surfaceMembersForClass(symbol_);
      Array result(runtime, members.size());
      size_t index = 0;
      for (const auto& member : members) {
        bool memberIsStatic =
            (member.flags & metagen::mdMemberStatic) != 0;
        if (memberIsStatic != staticMembers) {
          continue;
        }
        Object descriptor(runtime);
        descriptor.setProperty(runtime, "name", makeString(runtime, member.name));
        descriptor.setProperty(runtime, "selectorName",
                               makeString(runtime, member.selectorName));
        descriptor.setProperty(
            runtime, "argumentCount",
            static_cast<double>(selectorArgumentCount(member.selectorName)));
        descriptor.setProperty(runtime, "property", member.property);
        descriptor.setProperty(runtime, "readonly", member.readonly);
        descriptor.setProperty(runtime, "signatureOffset",
                               static_cast<double>(member.signatureOffset));
        descriptor.setProperty(
            runtime, "setterSignatureOffset",
            static_cast<double>(member.setterSignatureOffset));
        descriptor.setProperty(runtime, "flags",
                               static_cast<double>(member.flags));
        descriptor.setProperty(runtime, "setterSelectorName",
                               makeString(runtime, member.setterSelectorName));
        result.setValueAtIndex(runtime, index++, descriptor);
      }
      Array compact(runtime, index);
      for (size_t i = 0; i < index; i++) {
        compact.setValueAtIndex(runtime, i, result.getValueAtIndex(runtime, i));
      }
      return compact;
    }
    if (property == "toString") {
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "toString"), 0,
          [symbol = symbol_](Runtime& runtime, const Value&,
                             const Value*, size_t) -> Value {
            return makeString(runtime,
                              "[NativeApiClass " + symbol.name + "]");
          });
    }
    if (property == "construct" || property == "alloc" || property == "new") {
      auto bridge = bridge_;
      auto symbol = symbol_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property), 0,
          [bridge, symbol, property](Runtime& runtime, const Value&,
                                     const Value* args, size_t count) -> Value {
            Class cls = objc_lookUpClass(symbol.runtimeName.c_str());
            if (cls == nil) {
              throw JSError(
                  runtime, "Objective-C class is not available: " + symbol.name);
            }

            id result = nil;
            if (property == "construct" && count == 1) {
              void* pointer = nullptr;
              if (args[0].isNumber()) {
                pointer = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(args[0].getNumber()));
              } else if (args[0].isObject()) {
                Object object = args[0].asObject(runtime);
                if (object.isHostObject<NativeApiPointerHostObject>(runtime)) {
                  auto pointerHost =
                      object.getHostObject<NativeApiPointerHostObject>(
                          runtime);
                  pointer = pointerHost->pointer();
                  if (pointerHost->backingValue() != nullptr) {
                    Value backingValue(runtime, *pointerHost->backingValue());
                    id backingObject =
                        NativeApiObjectHostObject::nativeObjectFromValue(
                            runtime, backingValue);
                    if (backingObject == static_cast<id>(pointer) &&
                        backingObject != nil &&
                        [backingObject isKindOfClass:cls]) {
                      return backingValue;
                    }
                  }
                } else if (object.isHostObject<NativeApiReferenceHostObject>(
                               runtime)) {
                  auto referenceHost =
                      object.getHostObject<NativeApiReferenceHostObject>(
                          runtime);
                  pointer = referenceHost->data();
                  if (referenceHost->backingValue() != nullptr) {
                    Value backingValue(runtime, *referenceHost->backingValue());
                    id backingObject =
                        NativeApiObjectHostObject::nativeObjectFromValue(
                            runtime, backingValue);
                    if (backingObject == static_cast<id>(pointer) &&
                        backingObject != nil &&
                        [backingObject isKindOfClass:cls]) {
                      return backingValue;
                    }
                  }
                } else if (object.isHostObject<NativeApiObjectHostObject>(
                               runtime)) {
                  pointer = object
                                .getHostObject<NativeApiObjectHostObject>(
                                    runtime)
                                ->object();
                }
              }
              return makeNativeObjectValue(runtime, bridge,
                                           static_cast<id>(pointer), false);
            }

            if (property == "new") {
              if (count != 0) {
                throw JSError(
                    runtime, "new does not take arguments; use invoke for an "
                             "explicit Objective-C selector.");
              }
              performDirectObjCInvocation(runtime,
                                          [&]() { result = [[cls alloc] init]; });
            } else {
              if (count != 0) {
                throw JSError(
                    runtime, "alloc does not take arguments; call invoke on the "
                             "allocated object for an explicit init selector.");
              }
              performDirectObjCInvocation(runtime,
                                          [&]() { result = [cls alloc]; });
            }

            return makeNativeObjectValue(runtime, bridge, result, true);
          });
    }
    if (property == "invoke" || property == "send") {
      auto bridge = bridge_;
      auto symbol = symbol_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 1,
          [bridge, symbol](Runtime& runtime, const Value&, const Value* args,
                           size_t count) -> Value {
            std::string selectorName =
                readStringArg(runtime, args, count, 0, "selector");
            Class cls = objc_lookUpClass(symbol.runtimeName.c_str());
            if (cls == nil) {
              throw JSError(
                  runtime, "Objective-C class is not available: " + symbol.name);
            }
            return callObjCSelector(runtime, bridge, static_cast<id>(cls), true,
                                    selectorName, nullptr, args + 1,
                                    count - 1);
          });
    }

    Class cls = nativeClass();
    if (cls != Nil) {
      Value expando = bridge_->findObjectExpando(runtime, cls, property);
      if (!expando.isUndefined()) {
        return expando;
      }
    }

    const auto& members = bridge_->membersForClass(symbol_);
    // `[SomeClass appearance]` (and the whenContainedIn:/
    // appearanceWhenContainedIn: overloads) intercepted here so the returned
    // opaque UIAppearance proxy gets tagged with the class it represents and
    // has its property accessors installed — otherwise it would just be a
    // callable selector-group function, not the property-settable object
    // callers expect (see host_objects/Appearance.mm).
    if (property == "appearance" &&
        selectorGroupEntriesForMethod(members, property, true) != nullptr) {
      auto bridge = bridge_;
      auto symbol = symbol_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 0,
          [bridge, symbol](Runtime& runtime, const Value&,
                           const Value* args, size_t count) -> Value {
            Class cls = objc_lookUpClass(symbol.runtimeName.c_str());
            if (cls == Nil) {
              throw JSError(
                  runtime, "Objective-C class is not available: " +
                               symbol.name);
            }

            const auto& members = bridge->membersForClass(symbol);
            const NativeApiMember* selected =
                selectMethodMember(members, "appearance", true, count);
            if (selected == nullptr) {
              throw JSError(runtime,
                            "Objective-C selector is not available: appearance");
            }

            Value result = callObjCSelector(
                runtime, bridge, static_cast<id>(cls), true,
                selected->selectorName, selected, args, count);
            if (result.isObject()) {
              Object resultObject = result.asObject(runtime);
              if (resultObject.isHostObject<NativeApiObjectHostObject>(
                      runtime)) {
                id native = resultObject
                                .getHostObject<NativeApiObjectHostObject>(
                                    runtime)
                                ->object();
                Class customizableClass =
                    tagStaticAppearanceNativeResult(runtime, bridge, cls,
                                                    native);
                installAppearanceProxyPropertyAccessors(
                    runtime, bridge, customizableClass, native, resultObject);
              }
            }
            return result;
          });
    }

    if (const NativeApiMember* propertyMember =
            selectWritablePropertyMember(members, property, true)) {
      auto bridge = bridge_;
      auto symbol = symbol_;
      Class cls = objc_lookUpClass(symbol.runtimeName.c_str());
      if (cls == nil) {
        throw JSError(
            runtime, "Objective-C class is not available: " + symbol.name);
      }
      SEL selector = sel_getUid(propertyMember->selectorName.c_str());
      Class dispatchClass = classRespondingToClassSelector(cls, selector);
      if (dispatchClass != Nil) {
        return callObjCSelector(runtime, bridge, static_cast<id>(dispatchClass), true,
                                propertyMember->selectorName, propertyMember,
                                nullptr, 0);
      }
    }

    auto selectors = selectorGroupEntriesForMethod(members, property, true);
    if (selectors != nullptr) {
      if (cls == Nil) {
        throw JSError(
            runtime, "Objective-C class is not available: " + symbol_.name);
      }
      auto preparedInvocations = std::make_shared<std::vector<
          std::shared_ptr<NativeApiPreparedObjCInvocation>>>(selectors->size());
      Value methodFunction = CreateNativeApiSelectorGroupFunction(
          runtime, bridge_, cls, true, selectors, preparedInvocations);
      bridge_->setObjectExpando(runtime, cls, property, methodFunction);
      return methodFunction;
    }

    return Value::undefined();
  }

  NativeApiHostSetResult set(Runtime& runtime, const PropNameID& name, const Value& value) override {
    std::string property = name.utf8(runtime);
    Class cls = objc_lookUpClass(symbol_.runtimeName.c_str());
    if (cls == nil) {
      throw JSError(
          runtime, "Objective-C class is not available: " + symbol_.name);
    }

    const auto& members = bridge_->membersForClass(symbol_);
    if (const NativeApiMember* propertyMember =
            selectPropertyMember(members, property, true)) {
      if (propertyMember->readonly) {
        throw JSError(
            runtime, "Attempted to assign to readonly property.");
      }
      NativeApiMember setterMember = *propertyMember;
      setterMember.selectorName = propertyMember->setterSelectorName;
      setterMember.signatureOffset = propertyMember->setterSignatureOffset;
      SEL selector = sel_getUid(setterMember.selectorName.c_str());
      Class dispatchClass = classRespondingToClassSelector(cls, selector);
      if (dispatchClass == Nil) {
        throw JSError(runtime,
                      "Objective-C selector is not available: " +
                          setterMember.selectorName);
      }
      Value args[] = {Value(runtime, value)};
      callObjCSelector(runtime, bridge_, static_cast<id>(dispatchClass), true,
                       setterMember.selectorName, &setterMember, args, 1);
      NATIVE_API_SET_RETURN(true);
    }

    throw JSError(runtime,
                                 "No writable native property: " + property);
  }

  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override {
    std::vector<PropNameID> names;
    names.reserve(8);
    addPropertyName(runtime, names, "kind");
    addPropertyName(runtime, names, "name");
    addPropertyName(runtime, names, "runtimeName");
    addPropertyName(runtime, names, "available");
    addPropertyName(runtime, names, "metadataOffset");
    addPropertyName(runtime, names, "toString");
    addPropertyName(runtime, names, "construct");
    addPropertyName(runtime, names, "alloc");
    addPropertyName(runtime, names, "new");
    addPropertyName(runtime, names, "invoke");
    addPropertyName(runtime, names, "send");
    return names;
  }

 private:
  std::shared_ptr<NativeApiBridge> bridge_;
  NativeApiSymbol symbol_;
};

Value makeNativeObjectValue(Runtime& runtime,
                            const std::shared_ptr<NativeApiBridge>& bridge,
                            id object, bool ownsObject,
                            Class superDispatchClass) {
  if (object == nil) {
    return Value::null();
  }

  Value cached = bridge->findRoundTripValue(runtime, object, nullptr, true);
  if (!cached.isUndefined()) {
    // A consumed wrapper (e.g. an alloc'd placeholder singleton already passed
    // to an initializer) must not be reused: drop the stale entry and re-wrap.
    auto cachedHost =
        cached.isObject()
            ? cached.asObject(runtime).getHostObject<NativeApiObjectHostObject>(runtime)
            : nullptr;
    if (cachedHost != nullptr && cachedHost->object() != nil) {
      if (superDispatchClass != Nil) {
        cachedHost->setSuperDispatchClass(superDispatchClass);
      }
      if (ownsObject) {
        [object release];
      }
      return cached;
    }
    bridge->forgetRoundTripValue(runtime, object);
  }

  Object result = createNativeInstanceHostObject(
      runtime,
      std::make_shared<NativeApiObjectHostObject>(
          bridge, object, ownsObject, superDispatchClass));
  Value prototypeValue = Value::undefined();
  Value classWrapperValue =
      bridge->findObjectExpando(runtime, object, "__nativeApiClassWrapper");
  if (classWrapperValue.isObject()) {
    Object classWrapper = classWrapperValue.asObject(runtime);
    prototypeValue = classWrapper.getProperty(runtime, "prototype");
  }
  if (!prototypeValue.isObject()) {
    prototypeValue = bridge->findClassPrototype(runtime, object_getClass(object));
  }
  if (!prototypeValue.isObject()) {
    Value classWrapper = makeNativeClassValue(
        runtime, bridge,
        nativeApiSymbolForRuntimeClass(bridge, object_getClass(object)));
    if (classWrapper.isObject()) {
      prototypeValue =
          classWrapper.asObject(runtime).getProperty(runtime, "prototype");
    }
  }
  if (prototypeValue.isObject()) {
    Object prototype = prototypeValue.asObject(runtime);
    SetNativeApiObjectPrototype(runtime, result, prototype);
  }
  bridge->rememberScopedRoundTripValue(
      runtime, object, Value(runtime, result),
      nativeObjectIsStringLike(object));
  bridge->rememberWeakNativeObjectRoundTripValue(
      runtime, object, Value(runtime, result),
      nativeObjectIsStringLike(object));
  return result;
}

Value globalNativeSymbolValue(Runtime& runtime, const NativeApiSymbol& symbol,
                              const char* expectedKind) {
  Object global = runtime.global();
  Value cacheValue = global.getProperty(
      runtime, "__nativeScriptNativeApiGlobalCache");
  if (!cacheValue.isObject()) {
    return Value::undefined();
  }

  Object cache = cacheValue.asObject(runtime);
  auto readCache = [&](const std::string& name) -> Value {
    if (name.empty()) {
      return Value::undefined();
    }

    Value value = cache.getProperty(runtime, name.c_str());
    if (!value.isObject()) {
      return Value::undefined();
    }

    try {
      Object object = value.asObject(runtime);
      Value kindValue = object.getProperty(runtime, "kind");
      if (kindValue.isString() &&
          kindValue.asString(runtime).utf8(runtime) == expectedKind) {
        return value;
      }
    } catch (const std::exception&) {
    }

    return Value::undefined();
  };

  Value value = readCache(symbol.name);
  if (!value.isUndefined()) {
    return value;
  }
  if (symbol.runtimeName != symbol.name) {
    value = readCache(symbol.runtimeName);
    if (!value.isUndefined()) {
      return value;
    }
  }

  try {
    if (std::strcmp(expectedKind, "class") == 0) {
      Value classResolverValue = global.getProperty(
          runtime, "__nativeScriptResolveNativeApiClassWrapper");
      if (classResolverValue.isObject() &&
          classResolverValue.asObject(runtime).isFunction(runtime)) {
        Function classResolver =
            classResolverValue.asObject(runtime).asFunction(runtime);
        auto resolveClassWrapper = [&](const std::string& name) -> Value {
          if (name.empty()) {
            return Value::undefined();
          }
          Value resolved = classResolver.call(runtime, makeString(runtime, name));
          return resolved.isObject() ? std::move(resolved) : Value::undefined();
        };

        value = resolveClassWrapper(symbol.name);
        if (!value.isUndefined()) {
          return value;
        }
        if (symbol.runtimeName != symbol.name) {
          value = resolveClassWrapper(symbol.runtimeName);
          if (!value.isUndefined()) {
            return value;
          }
        }
      }
    }

    Value resolverValue =
        global.getProperty(runtime, "__nativeScriptResolveNativeApiGlobal");
    if (resolverValue.isObject() &&
        resolverValue.asObject(runtime).isFunction(runtime)) {
      Function resolver = resolverValue.asObject(runtime).asFunction(runtime);
      auto resolveGlobal = [&](const std::string& name) -> Value {
        if (name.empty()) {
          return Value::undefined();
        }
        Value resolved = resolver.call(runtime, makeString(runtime, name),
                                       makeString(runtime, expectedKind));
        if (resolved.isObject()) {
          return resolved;
        }
        return Value::undefined();
      };

      value = resolveGlobal(symbol.name);
      if (!value.isUndefined()) {
        return value;
      }
      if (symbol.runtimeName != symbol.name) {
        value = resolveGlobal(symbol.runtimeName);
        if (!value.isUndefined()) {
          return value;
        }
      }
    }
  } catch (const std::exception&) {
  }

  return Value::undefined();
}

Value makeNativeClassValue(Runtime& runtime,
                           const std::shared_ptr<NativeApiBridge>& bridge,
                           NativeApiSymbol symbol) {
  Class cls = objc_lookUpClass(symbol.runtimeName.c_str());
  Value cachedClass = bridge->findClassValue(runtime, cls);
  if (!cachedClass.isUndefined()) {
    return cachedClass;
  }
  Value globalValue = globalNativeSymbolValue(runtime, symbol, "class");
  if (!globalValue.isUndefined()) {
    return globalValue;
  }
  return Object::createFromHostObject(
      runtime,
      std::make_shared<NativeApiClassHostObject>(bridge, std::move(symbol)));
}

Protocol* lookupProtocolByNativeName(const std::string& name) {
  Protocol* protocol = objc_getProtocol(name.c_str());
  if (protocol != nullptr) {
    return protocol;
  }
  constexpr const char* suffix = "Protocol";
  size_t suffixLength = std::strlen(suffix);
  if (name.size() > suffixLength &&
      name.compare(name.size() - suffixLength, suffixLength, suffix) == 0) {
    protocol = objc_getProtocol(
        name.substr(0, name.size() - suffixLength).c_str());
  }
  return protocol;
}
