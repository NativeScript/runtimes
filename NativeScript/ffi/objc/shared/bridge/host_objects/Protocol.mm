class NativeApiProtocolHostObject final : public HostObject {
 public:
  NativeApiProtocolHostObject(std::shared_ptr<NativeApiBridge> bridge,
                              NativeApiSymbol symbol)
      : bridge_(std::move(bridge)), symbol_(std::move(symbol)) {}

  Protocol* nativeProtocol() const {
    Protocol* protocol = lookupProtocolByNativeName(symbol_.runtimeName);
    if (protocol == nullptr && symbol_.runtimeName != symbol_.name) {
      protocol = lookupProtocolByNativeName(symbol_.name);
    }
    return protocol;
  }

  const NativeApiSymbol& symbol() const { return symbol_; }

  Value get(Runtime& runtime, const PropNameID& name) override {
    std::string property = name.utf8(runtime);
    if (property == "kind") {
      return makeString(runtime, "protocol");
    }
    if (property == "name") {
      return makeString(runtime, symbol_.name);
    }
    if (property == "runtimeName") {
      return makeString(runtime, symbol_.runtimeName);
    }
    if (property == "available") {
      return nativeProtocol() != nullptr;
    }
    if (property == "metadataOffset") {
      return static_cast<double>(symbol_.offset);
    }
    if (property == "nativeAddress") {
      return static_cast<double>(
          reinterpret_cast<uintptr_t>(nativeProtocol()));
    }
    if (property == "prototype") {
      Object prototype(runtime);
      for (const auto& member : bridge_->membersForProtocol(symbol_)) {
        if (prototype.hasProperty(runtime, member.name.c_str())) {
          continue;
        }
        if (member.property) {
          defineProtocolProperty(runtime, prototype, member, false);
        } else {
          prototype.setProperty(runtime, member.name.c_str(),
                                makeProtocolMemberFunction(runtime, member,
                                                           false));
        }
      }
      return prototype;
    }
    if (property == "toString") {
      auto symbol = symbol_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "toString"), 0,
          [symbol](Runtime& runtime, const Value&, const Value*, size_t) -> Value {
            return makeString(runtime,
                              "[NativeApiProtocol " + symbol.name + "]");
          });
    }
    const auto& members = bridge_->membersForProtocol(symbol_);
    if (const NativeApiMember* propertyMember =
            selectPropertyMember(members, property, true)) {
      return makeProtocolPropertyGetter(runtime, *propertyMember, true);
    }
    if (const NativeApiMember* propertyMember =
            selectPropertyMember(members, property, false)) {
      return makeProtocolPropertyGetter(runtime, *propertyMember, true);
    }
    for (const auto& member : members) {
      if (member.property || member.name != property) {
        continue;
      }
      bool memberIsStatic = (member.flags & metagen::mdMemberStatic) != 0;
      if (memberIsStatic) {
        return makeProtocolMemberFunction(runtime, member, true);
      }
    }
    for (const auto& member : members) {
      if (member.property || member.name != property) {
        continue;
      }
      bool memberIsStatic = (member.flags & metagen::mdMemberStatic) != 0;
      if (!memberIsStatic) {
        return makeProtocolMemberFunction(runtime, member, true);
      }
    }
    return Value::undefined();
  }

  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override {
    std::vector<PropNameID> names;
    addPropertyName(runtime, names, "kind");
    addPropertyName(runtime, names, "name");
    addPropertyName(runtime, names, "runtimeName");
    addPropertyName(runtime, names, "available");
    addPropertyName(runtime, names, "metadataOffset");
    addPropertyName(runtime, names, "nativeAddress");
    addPropertyName(runtime, names, "prototype");
    addPropertyName(runtime, names, "toString");
    for (const auto& member : bridge_->membersForProtocol(symbol_)) {
      addPropertyName(runtime, names, member.name.c_str());
    }
    return names;
  }

 private:
  static Class classReceiverFromThis(Runtime& runtime, const Value& thisValue) {
    if (!thisValue.isObject()) {
      return Nil;
    }

    Object object = thisValue.asObject(runtime);
    if (object.isHostObject<NativeApiClassHostObject>(runtime)) {
      return object.getHostObject<NativeApiClassHostObject>(runtime)->nativeClass();
    }

    Value wrappedClass = object.getProperty(runtime, "__nativeApiClass");
    if (wrappedClass.isObject()) {
      Object wrappedObject = wrappedClass.asObject(runtime);
      if (wrappedObject.isHostObject<NativeApiClassHostObject>(runtime)) {
        return wrappedObject.getHostObject<NativeApiClassHostObject>(runtime)
            ->nativeClass();
      }
    }

    Value kindValue = object.getProperty(runtime, "kind");
    if (kindValue.isString() &&
        kindValue.asString(runtime).utf8(runtime) == "class") {
      Value runtimeNameValue = object.getProperty(runtime, "runtimeName");
      if (!runtimeNameValue.isString()) {
        runtimeNameValue = object.getProperty(runtime, "name");
      }
      if (runtimeNameValue.isString()) {
        std::string runtimeName =
            runtimeNameValue.asString(runtime).utf8(runtime);
        return objc_lookUpClass(runtimeName.c_str());
      }
    }

    return Nil;
  }

  id objectReceiverFromThis(Runtime& runtime, const Value& thisValue) const {
    if (!thisValue.isObject()) {
      return nil;
    }

    Object object = thisValue.asObject(runtime);
    if (object.isHostObject<NativeApiObjectHostObject>(runtime)) {
      return object.getHostObject<NativeApiObjectHostObject>(runtime)->object();
    }

    return nil;
  }

  Value makeProtocolMemberFunction(Runtime& runtime, NativeApiMember member,
                                   bool receiverIsClass) const {
    auto bridge = bridge_;
    return Function::createFromHostFunction(
        runtime, PropNameID::forAscii(runtime, member.name.c_str()), 0,
        [bridge, member, receiverIsClass](Runtime& runtime,
                                          const Value& thisValue,
                                          const Value* args,
                                          size_t count) -> Value {
          id receiver = nil;
          if (receiverIsClass) {
            receiver = static_cast<id>(
                classReceiverFromThis(runtime, thisValue));
          } else if (thisValue.isObject()) {
            Object object = thisValue.asObject(runtime);
            if (object.isHostObject<NativeApiObjectHostObject>(runtime)) {
              receiver = object.getHostObject<NativeApiObjectHostObject>(runtime)
                             ->object();
            }
          }

          if (receiver == nil) {
            throw JSError(
                runtime, "Protocol member requires a native receiver.");
          }
          return callObjCSelector(runtime, bridge, receiver, receiverIsClass,
                                  member.selectorName, &member, args, count);
        });
  }

  Value makeProtocolPropertyGetter(Runtime& runtime, NativeApiMember member,
                                   bool receiverIsClass) const {
    auto bridge = bridge_;
    return Function::createFromHostFunction(
        runtime, PropNameID::forAscii(runtime, member.name.c_str()), 0,
        [bridge, member, receiverIsClass](Runtime& runtime,
                                          const Value& thisValue,
                                          const Value*, size_t) -> Value {
          id receiver = nil;
          if (receiverIsClass) {
            receiver = static_cast<id>(
                classReceiverFromThis(runtime, thisValue));
          } else if (thisValue.isObject()) {
            Object object = thisValue.asObject(runtime);
            if (object.isHostObject<NativeApiObjectHostObject>(runtime)) {
              receiver = object.getHostObject<NativeApiObjectHostObject>(runtime)
                             ->object();
            }
          }

          if (receiver == nil) {
            throw JSError(
                runtime, "Protocol property requires a native receiver.");
          }
          Value appearanceExpando = cachedAppearanceProxyPropertyValue(
              runtime, bridge, receiver, member.name);
          if (!appearanceExpando.isUndefined()) {
            return appearanceExpando;
          }
          NativeApiMember getterMember = member;
          if (auto selector = respondingPropertyGetterSelector(
                  receiver, member.name, member.selectorName)) {
            getterMember.selectorName = *selector;
          }
          return callObjCSelector(runtime, bridge, receiver, receiverIsClass,
                                  getterMember.selectorName, &getterMember,
                                  nullptr, 0);
        });
  }

  Value makeProtocolPropertySetter(Runtime& runtime, NativeApiMember member,
                                   bool receiverIsClass) const {
    auto bridge = bridge_;
    return Function::createFromHostFunction(
        runtime, PropNameID::forAscii(runtime, member.setterSelectorName.c_str()),
        1,
        [bridge, member, receiverIsClass](Runtime& runtime,
                                          const Value& thisValue,
                                          const Value* args,
                                          size_t count) -> Value {
          id receiver = nil;
          if (receiverIsClass) {
            receiver = static_cast<id>(
                classReceiverFromThis(runtime, thisValue));
          } else if (thisValue.isObject()) {
            Object object = thisValue.asObject(runtime);
            if (object.isHostObject<NativeApiObjectHostObject>(runtime)) {
              receiver = object.getHostObject<NativeApiObjectHostObject>(runtime)
                             ->object();
            }
          }

          if (receiver == nil) {
            throw JSError(
                runtime, "Protocol property requires a native receiver.");
          }
          if (count < 1) {
            throw JSError(
                runtime, "Protocol property setter expects a value.");
          }

          NativeApiMember setterMember = member;
          setterMember.selectorName = member.setterSelectorName;
          setterMember.signatureOffset = member.setterSignatureOffset;
          Value result = callObjCSelector(
              runtime, bridge, receiver, receiverIsClass,
              setterMember.selectorName, &setterMember, args, 1);
          cacheAppearanceProxyPropertyValue(runtime, bridge, receiver,
                                            member.name, args[0]);
          return result;
        });
  }

  void defineProtocolProperty(Runtime& runtime, Object& target,
                              const NativeApiMember& member,
                              bool receiverIsClass) const {
    try {
      Object objectCtor = runtime.global().getPropertyAsObject(runtime, "Object");
      Function defineProperty =
          objectCtor.getPropertyAsFunction(runtime, "defineProperty");
      Object descriptor(runtime);
      descriptor.setProperty(runtime, "configurable", true);
      descriptor.setProperty(runtime, "enumerable", true);
      descriptor.setProperty(runtime, "get",
                             makeProtocolPropertyGetter(runtime, member,
                                                        receiverIsClass));
      if (!member.readonly && !member.setterSelectorName.empty()) {
        descriptor.setProperty(runtime, "set",
                               makeProtocolPropertySetter(runtime, member,
                                                          receiverIsClass));
      }
      defineProperty.call(runtime, target, makeString(runtime, member.name),
                          descriptor);
    } catch (const std::exception&) {
    }
  }

  std::shared_ptr<NativeApiBridge> bridge_;
  NativeApiSymbol symbol_;
};

Value makeNativeProtocolValue(Runtime& runtime,
                              const std::shared_ptr<NativeApiBridge>& bridge,
                              NativeApiSymbol symbol) {
  Value globalValue = globalNativeSymbolValue(runtime, symbol, "protocol");
  if (!globalValue.isUndefined()) {
    return globalValue;
  }
  return Object::createFromHostObject(
      runtime,
      std::make_shared<NativeApiProtocolHostObject>(bridge, std::move(symbol)));
}

Class nativeClassFromEngineObject(Runtime& runtime, const Object& object) {
  if (object.isHostObject<NativeApiClassHostObject>(runtime)) {
    return object.getHostObject<NativeApiClassHostObject>(runtime)->nativeClass();
  }

  Value wrappedClass = object.getProperty(runtime, "__nativeApiClass");
  if (wrappedClass.isObject()) {
    Object wrappedObject = wrappedClass.asObject(runtime);
    if (wrappedObject.isHostObject<NativeApiClassHostObject>(runtime)) {
      return wrappedObject.getHostObject<NativeApiClassHostObject>(runtime)
          ->nativeClass();
    }
  }
  return Nil;
}
