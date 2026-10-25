class NativeApiFastEnumerationIteratorHostObject final : public HostObject {
 public:
  NativeApiFastEnumerationIteratorHostObject(
      std::shared_ptr<NativeApiBridge> bridge, id<NSFastEnumeration> collection)
      : bridge_(std::move(bridge)), collection_(collection) {
    [(id)collection_ retain];
  }

  ~NativeApiFastEnumerationIteratorHostObject() override {
    [(id)collection_ release];
    collection_ = nil;
  }

  Value get(Runtime& runtime, const PropNameID& name) override {
    bridge_->requireOwnerRuntime(runtime);
    std::string property = name.utf8(runtime);
    if (property == "next") {
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "next"), 0,
          [this](Runtime& runtime, const Value&, const Value*, size_t) -> Value {
            return next(runtime);
          });
    }
    return Value::undefined();
  }

  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override {
    bridge_->requireOwnerRuntime(runtime);
    std::vector<PropNameID> names;
    addPropertyName(runtime, names, "next");
    return names;
  }

 private:
  Value next(Runtime& runtime) {
    Object result(runtime);
    if (done_ || collection_ == nil) {
      result.setProperty(runtime, "done", true);
      return result;
    }

    if (stackIndex_ >= stackLength_) {
      stackLength_ = [collection_ countByEnumeratingWithState:&state_
                                                      objects:stack_
                                                        count:16];
      stackIndex_ = 0;
      if (stackLength_ == 0) {
        done_ = true;
        result.setProperty(runtime, "done", true);
        return result;
      }
    }

    id value = state_.itemsPtr[stackIndex_++];
    NativeApiType valueType = nativeObjectReturnTypeForClass(object_getClass(value));
    result.setProperty(runtime, "value",
                       convertNativeReturnValue(runtime, bridge_, valueType, &value));
    result.setProperty(runtime, "done", false);
    return result;
  }

  std::shared_ptr<NativeApiBridge> bridge_;
  id<NSFastEnumeration> collection_ = nil;
  NSFastEnumerationState state_ = {};
  id __unsafe_unretained stack_[16] = {};
  NSUInteger stackLength_ = 0;
  NSUInteger stackIndex_ = 0;
  bool done_ = false;
};

NativeApiSymbol nativeApiSymbolForRuntimeClass(
    const std::shared_ptr<NativeApiBridge>& bridge, Class cls) {
  const char* name = cls != Nil ? class_getName(cls) : "";
  if (bridge != nullptr) {
    if (const NativeApiSymbol* symbol = bridge->findClassForRuntimePointer(cls)) {
      return *symbol;
    }
    if (const NativeApiSymbol* symbol = bridge->findClassForRuntimeClass(cls)) {
      return *symbol;
    }
    if (name != nullptr) {
      if (const NativeApiSymbol* symbol = bridge->findClass(name)) {
        return *symbol;
      }
    }
  }

  return NativeApiSymbol{
      .kind = NativeApiSymbolKind::Class,
      .offset = MD_SECTION_OFFSET_NULL,
      .name = name != nullptr ? name : "",
      .runtimeName = name != nullptr ? name : "",
  };
}

std::optional<std::string> runtimeWritablePropertySetter(id object,
                                                         const std::string& property) {
  if (object == nil || property.empty()) {
    return std::nullopt;
  }

  Class current = object_getClass(object);
  while (current != Nil) {
    objc_property_t prop = class_getProperty(current, property.c_str());
    if (prop != nullptr) {
      if (char* readonly = property_copyAttributeValue(prop, "R")) {
        free(readonly);
        return std::nullopt;
      }

      std::string setter = setterSelectorForProperty(property);
      if (char* customSetter = property_copyAttributeValue(prop, "S")) {
        setter = customSetter;
        free(customSetter);
      }

      SEL selector = sel_getUid(setter.c_str());
      if ([object respondsToSelector:selector]) {
        return setter;
      }
    }

    current = class_getSuperclass(current);
  }

  std::string setter = setterSelectorForProperty(property);
  SEL selector = sel_getUid(setter.c_str());
  if ([object respondsToSelector:selector]) {
    return setter;
  }

  return std::nullopt;
}

std::optional<std::string> runtimeReadablePropertyGetter(id object,
                                                         const std::string& property) {
  if (object == nil || property.empty()) {
    return std::nullopt;
  }

  Class current = object_getClass(object);
  while (current != Nil) {
    objc_property_t prop = class_getProperty(current, property.c_str());
    if (prop != nullptr) {
      std::string getter = property;
      if (char* customGetter = property_copyAttributeValue(prop, "G")) {
        getter = customGetter;
        free(customGetter);
      }

      if (auto selector =
              respondingPropertyGetterSelector(object, property, getter)) {
        return selector;
      }
    }

    current = class_getSuperclass(current);
  }

  return respondingPropertyGetterSelector(object, property, property);
}

class NativeApiSuperHostObject final : public HostObject {
 public:
  NativeApiSuperHostObject(std::shared_ptr<NativeApiBridge> bridge,
                           id receiver, Class dispatchClass)
      : bridge_(std::move(bridge)),
        receiver_(receiver),
        dispatchClass_(dispatchClass) {
    if (receiver_ != nil) {
      [receiver_ retain];
    }
  }

  ~NativeApiSuperHostObject() override {
    if (receiver_ != nil) {
      [receiver_ release];
      receiver_ = nil;
    }
  }

  Value get(Runtime& runtime, const PropNameID& name) override {
    bridge_->requireOwnerRuntime(runtime);
    std::string property = name.utf8(runtime);
    if (property == "kind") {
      return makeString(runtime, "super");
    }
    if (property == "toString") {
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "toString"), 0,
          [](Runtime& runtime, const Value&, const Value*, size_t) -> Value {
            return makeString(runtime, "[NativeApiSuper]");
          });
    }
    if (receiver_ == nil || dispatchClass_ == Nil) {
      return Value::undefined();
    }

    if (const NativeApiSymbol* symbol =
            bridge_->findClassForRuntimeClass(dispatchClass_)) {
      const auto& members = bridge_->membersForClass(*symbol);
      if (const NativeApiMember* propertyMember =
              selectPropertyMember(members, property, false)) {
        SEL selector = sel_getUid(propertyMember->selectorName.c_str());
        if (class_getInstanceMethod(dispatchClass_, selector) != nullptr) {
          return callObjCSelector(runtime, bridge_, receiver_, false,
                                  propertyMember->selectorName, propertyMember,
                                  nullptr, 0, dispatchClass_);
        }
      }

      if (hasMethodMember(members, property, false)) {
        auto bridge = bridge_;
        id receiver = receiver_;
        Class dispatchClass = dispatchClass_;
        std::string memberName = property;
        return Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, property.c_str()), 0,
            [bridge, receiver, dispatchClass, memberName](
                Runtime& runtime, const Value&, const Value* args,
                size_t count) -> Value {
              const NativeApiSymbol* symbol =
                  bridge->findClassForRuntimeClass(dispatchClass);
              if (symbol == nullptr) {
                throw JSError(
                    runtime, "Objective-C metadata is not available for super.");
              }
              const NativeApiMember* selected = selectMethodMember(
                  bridge->membersForClass(*symbol), memberName, false, count);
              if (selected == nullptr) {
                throw JSError(
                    runtime, "Objective-C super selector is not available: " +
                                 memberName);
              }
              return callObjCSelector(runtime, bridge, receiver, false,
                                      selected->selectorName, selected, args,
                                      count, dispatchClass);
            });
      }
    }

    return Value::undefined();
  }

  NativeApiHostSetResult set(Runtime& runtime, const PropNameID& name, const Value& value) override {
    bridge_->requireOwnerRuntime(runtime);
    std::string property = name.utf8(runtime);
    if (receiver_ == nil || dispatchClass_ == Nil) {
      throw JSError(runtime, "Cannot set property on nil super.");
    }

    if (const NativeApiSymbol* symbol =
            bridge_->findClassForRuntimeClass(dispatchClass_)) {
      const auto& members = bridge_->membersForClass(*symbol);
      if (const NativeApiMember* propertyMember =
              selectWritablePropertyMember(members, property, false)) {
        if (propertyMember->readonly ||
            propertyMember->setterSelectorName.empty()) {
          throw JSError(
              runtime, "Attempted to assign to readonly property.");
        }
        NativeApiMember setterMember = *propertyMember;
        setterMember.selectorName = propertyMember->setterSelectorName;
        setterMember.signatureOffset = propertyMember->setterSignatureOffset;
        Value args[] = {Value(runtime, value)};
        callObjCSelector(runtime, bridge_, receiver_, false,
                         setterMember.selectorName, &setterMember, args, 1,
                         dispatchClass_);
        NATIVE_API_SET_RETURN(true);
      }
    }

    std::string setterSelectorName = setterSelectorForProperty(property);
    SEL selector = sel_getUid(setterSelectorName.c_str());
    if (class_getInstanceMethod(dispatchClass_, selector) != nullptr) {
      Value args[] = {Value(runtime, value)};
      callObjCSelector(runtime, bridge_, receiver_, false, setterSelectorName,
                       nullptr, args, 1, dispatchClass_);
      NATIVE_API_SET_RETURN(true);
    }

    throw JSError(runtime,
                                 "No writable native super property: " +
                                     property);
  }

  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override {
    bridge_->requireOwnerRuntime(runtime);
    std::vector<PropNameID> names;
    addPropertyName(runtime, names, "kind");
    addPropertyName(runtime, names, "toString");
    return names;
  }

 private:
  std::shared_ptr<NativeApiBridge> bridge_;
  id receiver_ = nil;
  Class dispatchClass_ = Nil;
};

struct NativeApiRuntimeMember {
  std::string name;
  std::string selectorName;
  size_t argumentCount = 0;
};

using NativeApiRuntimeMembers = std::vector<NativeApiRuntimeMember>;

struct NativeApiRuntimeMemberIndex {
  NativeApiRuntimeMembers members;
  std::unordered_set<std::string> memberNames;
  std::unordered_map<std::string, std::unordered_map<size_t, std::string>>
      selectorsByNameAndCount;
};

struct NativeApiRuntimeMembersCacheKey {
  Class cls = Nil;
  bool staticMembers = false;

  bool operator==(const NativeApiRuntimeMembersCacheKey& other) const {
    return cls == other.cls && staticMembers == other.staticMembers;
  }
};

struct NativeApiRuntimeMembersCacheKeyHash {
  size_t operator()(const NativeApiRuntimeMembersCacheKey& key) const {
    size_t classHash = std::hash<uintptr_t>{}(reinterpret_cast<uintptr_t>(key.cls));
    return classHash ^ (key.staticMembers ? 0x9e3779b97f4a7c15ULL : 0);
  }
};

std::mutex& runtimeMembersCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<NativeApiRuntimeMembersCacheKey,
                   std::shared_ptr<const NativeApiRuntimeMemberIndex>,
                   NativeApiRuntimeMembersCacheKeyHash>&
runtimeMembersCache() {
  static std::unordered_map<NativeApiRuntimeMembersCacheKey,
                            std::shared_ptr<const NativeApiRuntimeMemberIndex>,
                            NativeApiRuntimeMembersCacheKeyHash>
      cache;
  return cache;
}

std::shared_ptr<const NativeApiRuntimeMemberIndex> emptyRuntimeMembers() {
  static auto empty = std::make_shared<const NativeApiRuntimeMemberIndex>();
  return empty;
}

NativeApiRuntimeMemberIndex buildRuntimeMembersForClass(Class cls,
                                                        bool staticMembers) {
  NativeApiRuntimeMemberIndex index;
  if (cls == Nil) {
    return index;
  }

  std::unordered_set<std::string> seen;
  Class current = staticMembers ? object_getClass(cls) : cls;
  while (current != Nil) {
    unsigned int methodCount = 0;
    Method* methods = class_copyMethodList(current, &methodCount);
    for (unsigned int i = 0; i < methodCount; i++) {
      SEL selector = method_getName(methods[i]);
      const char* selectorName = selector != nullptr ? sel_getName(selector) : nullptr;
      if (selectorName == nullptr || selectorName[0] == '\0') {
        continue;
      }

      std::string selectorString(selectorName);
      std::string name = jsifySelector(selectorString.c_str());
      if (name.empty()) {
        continue;
      }

      size_t argumentCount = selectorArgumentCount(selectorString);
      std::string key = name + "\x1f" + std::to_string(argumentCount);
      if (!seen.insert(key).second) {
        continue;
      }

      index.memberNames.insert(name);
      index.selectorsByNameAndCount[name].emplace(argumentCount, selectorString);
      index.members.push_back(NativeApiRuntimeMember{
          .name = std::move(name),
          .selectorName = std::move(selectorString),
          .argumentCount = argumentCount,
      });
    }
    if (methods != nullptr) {
      free(methods);
    }
    current = class_getSuperclass(current);
  }

  return index;
}

std::shared_ptr<const NativeApiRuntimeMemberIndex> runtimeMembersForClass(
    Class cls, bool staticMembers) {
  if (cls == Nil) {
    return emptyRuntimeMembers();
  }

  NativeApiRuntimeMembersCacheKey key{.cls = cls,
                                      .staticMembers = staticMembers};

  {
    std::lock_guard<std::mutex> lock(runtimeMembersCacheMutex());
    auto& cache = runtimeMembersCache();
    auto cached = cache.find(key);
    if (cached != cache.end()) {
      return cached->second;
    }
  }

  auto members =
      std::make_shared<const NativeApiRuntimeMemberIndex>(
          buildRuntimeMembersForClass(cls, staticMembers));

  {
    std::lock_guard<std::mutex> lock(runtimeMembersCacheMutex());
    auto& cache = runtimeMembersCache();
    auto [cached, inserted] = cache.emplace(key, members);
    return inserted ? members : cached->second;
  }
}

bool hasRuntimeMemberForName(Class cls, bool staticMembers,
                             const std::string& name) {
  auto index = runtimeMembersForClass(cls, staticMembers);
  return index->memberNames.find(name) != index->memberNames.end();
}

std::optional<std::string> selectRuntimeSelectorForName(
    Class cls, bool staticMembers, const std::string& name, size_t count) {
  auto index = runtimeMembersForClass(cls, staticMembers);
  auto selectorsForName = index->selectorsByNameAndCount.find(name);
  if (selectorsForName == index->selectorsByNameAndCount.end()) {
    return std::nullopt;
  }
  auto selector = selectorsForName->second.find(count);
  if (selector == selectorsForName->second.end()) {
    return std::nullopt;
  }
  return selector->second;
}

Array runtimeMembersArray(Runtime& runtime, Class cls, bool staticMembers) {
  auto index = runtimeMembersForClass(cls, staticMembers);
  Array result(runtime, index->members.size());
  for (size_t i = 0; i < index->members.size(); i++) {
    const auto& member = index->members[i];
    Object descriptor(runtime);
    descriptor.setProperty(runtime, "name", makeString(runtime, member.name));
    descriptor.setProperty(runtime, "selectorName",
                           makeString(runtime, member.selectorName));
    descriptor.setProperty(runtime, "argumentCount",
                           static_cast<double>(member.argumentCount));
    descriptor.setProperty(runtime, "property", false);
    descriptor.setProperty(runtime, "readonly", false);
    descriptor.setProperty(runtime, "setterSelectorName", makeString(runtime, ""));
    result.setValueAtIndex(runtime, i, descriptor);
  }
  return result;
}

class NativeApiObjectHostObject final
    : public HostObject,
      public std::enable_shared_from_this<NativeApiObjectHostObject> {
 public:
  NativeApiObjectHostObject(std::shared_ptr<NativeApiBridge> bridge,
                            id object, bool ownsObject)
      : bridge_(std::move(bridge)),
        object_(object),
        ownsObject_(ownsObject),
        lifetimeState_(std::make_shared<NativeApiObjectLifetimeState>(object)) {
    if (bridge_ != nullptr && object_ != nil) {
      bridge_->retainObjectExpandoOwner(object_);
    }
    if (object_ != nil && !ownsObject_) {
      [object_ retain];
      ownsObject_ = true;
      wrapperRetainedObject_ = true;
    }
  }

  ~NativeApiObjectHostObject() override {
    if (bridge_ != nullptr && object_ != nil) {
      bridge_->forgetRoundTripValue(object_);
      bridge_->releaseObjectExpandoOwner(
          object_, class_conformsToProtocol(object_getClass(object_),
                                            @protocol(NativeApiClassBuilderProtocol)));
    }
    if (lifetimeState_ != nullptr) {
      lifetimeState_->clear();
    }
    if (ownsObject_ && object_ != nil) {
      [object_ release];
      object_ = nil;
    }
  }

  id object() const { return object_; }
  std::shared_ptr<NativeApiObjectLifetimeState> lifetimeState() const {
    return lifetimeState_;
  }

  // Store a JS-owned property as a bridge expando (read back by get()). Used by
  // engine adapters whose exotic property storage doesn't fall back to own
  // properties when the host set handler defers.
  void storeOwnExpando(Runtime& runtime, const std::string& property,
                       const Value& value) {
    if (object_ != nil) {
      bridge_->setObjectExpando(runtime, object_, property, value);
    }
  }

  void disownObject(id expected, bool preserveExpandos = false) {
    if (object_ == expected) {
      if (bridge_ != nullptr && expected != nil) {
        bridge_->forgetRoundTripValue(expected);
        bridge_->releaseObjectExpandoOwner(expected, preserveExpandos);
      }
      ownsObject_ = false;
      wrapperRetainedObject_ = false;
      object_ = nil;
      if (lifetimeState_ != nullptr) {
        lifetimeState_->clear();
      }
    }
  }

  static bool isInitializerSelector(const std::string& selectorName) {
    return selectorName.rfind("init", 0) == 0;
  }

  static id nativeObjectFromValue(Runtime& runtime, const Value& value) {
    if (!value.isObject()) {
      return nil;
    }
    Object object = value.asObject(runtime);
    if (!object.isHostObject<NativeApiObjectHostObject>(runtime)) {
      return nil;
    }
    return object.getHostObject<NativeApiObjectHostObject>(runtime)->object();
  }

  static Value descriptionString(Runtime& runtime, id object) {
    NSString* description = nil;
    performDirectObjCInvocation(runtime, [&]() {
      description = [(object != nil ? [object description] : @"<nil>") copy];
    });
    std::string text = description.UTF8String ?: "";
    [description release];
    return makeString(runtime, text);
  }

  Value callObjectSelector(Runtime& runtime, const std::string& selectorName,
                           const NativeApiMember* member, const Value* args,
                           size_t count, Class dispatchSuperClass = Nil) {
    id receiver = object_;
    if (receiver == nil) {
      throw JSError(runtime,
                                   "Cannot send Objective-C selector to nil.");
    }

    const bool initializer = isInitializerSelector(selectorName);
    std::optional<Object> classWrapper;
    if (initializer) {
      Value classWrapperValue = bridge_->findObjectExpando(
          runtime, receiver, "__nativeApiClassWrapper");
      if (classWrapperValue.isObject()) {
        classWrapper.emplace(classWrapperValue.asObject(runtime));
      }
      bridge_->forgetRoundTripValue(runtime, receiver);
    }

    Value result =
        callObjCSelector(runtime, bridge_, receiver, false, selectorName, member,
                         args, count, dispatchSuperClass);
    if (initializer) {
      id resultObject = nativeObjectFromValue(runtime, result);
      disownObject(receiver, resultObject == receiver);
      if (resultObject != nil) {
        // Re-adopt the init result on this host object so that JS overrides
        // returning `this` still have a valid native object.
        object_ = resultObject;
        ownsObject_ = true;
        wrapperRetainedObject_ = true;
        if (bridge_ != nullptr) {
          bridge_->retainObjectExpandoOwner(object_);
        }
        if (lifetimeState_ != nullptr) {
          lifetimeState_->setObject(object_);
        }
        [object_ retain];
        if (classWrapper) {
          bridge_->setObjectExpando(runtime, resultObject,
                                    "__nativeApiClassWrapper",
                                    Value(runtime, *classWrapper));
          if (result.isObject()) {
            Value prototypeValue = classWrapper->getProperty(runtime, "prototype");
            if (prototypeValue.isObject()) {
              Object resultValue = result.asObject(runtime);
              Object prototype = prototypeValue.asObject(runtime);
              SetNativeApiObjectPrototype(runtime, resultValue, prototype);
            }
          }
        }
      }
    }
    return result;
  }

  Value callPreparedObjectSelector(
      Runtime& runtime, const NativeApiPreparedObjCInvocation& prepared,
      const Value* args, size_t count, Class dispatchSuperClass = Nil) {
    id receiver = object_;
    if (receiver == nil) {
      throw JSError(runtime,
                    "Cannot send Objective-C selector to nil.");
    }

    const bool initializer = preparedObjCInvocationIsInit(prepared);
    std::optional<Object> classWrapper;
    if (initializer) {
      Value classWrapperValue = bridge_->findObjectExpando(
          runtime, receiver, "__nativeApiClassWrapper");
      if (classWrapperValue.isObject()) {
        classWrapper.emplace(classWrapperValue.asObject(runtime));
      }
      bridge_->forgetRoundTripValue(runtime, receiver);
    }

    Value result = callPreparedObjCSelector(
        runtime, bridge_, receiver, false, prepared, args, count,
        dispatchSuperClass);
    if (initializer) {
      id resultObject = nativeObjectFromValue(runtime, result);
      disownObject(receiver, resultObject == receiver);
      if (resultObject != nil) {
        // Re-adopt the init result on this host object so that JS overrides
        // returning `this` still have a valid native object.
        object_ = resultObject;
        ownsObject_ = true;
        wrapperRetainedObject_ = true;
        if (bridge_ != nullptr) {
          bridge_->retainObjectExpandoOwner(object_);
        }
        if (lifetimeState_ != nullptr) {
          lifetimeState_->setObject(object_);
        }
        [object_ retain];
        if (classWrapper) {
          bridge_->setObjectExpando(runtime, resultObject,
                                    "__nativeApiClassWrapper",
                                    Value(runtime, *classWrapper));
          if (result.isObject()) {
            Value prototypeValue = classWrapper->getProperty(runtime, "prototype");
            if (prototypeValue.isObject()) {
              Object resultValue = result.asObject(runtime);
              Object prototype = prototypeValue.asObject(runtime);
              SetNativeApiObjectPrototype(runtime, resultValue, prototype);
            }
          }
        }
      }
    }
    return result;
  }

  Value classPrototypeForObject(Runtime& runtime) {
    if (object_ == nil) {
      return Value::undefined();
    }

    Value classWrapperValue = bridge_->findObjectExpando(
        runtime, object_, "__nativeApiClassWrapper");
    if (!classWrapperValue.isObject()) {
      classWrapperValue = bridge_->findClassValue(runtime, object_getClass(object_));
    }
    if (!classWrapperValue.isObject()) {
      if (const NativeApiSymbol* symbol =
              bridge_->findClassForRuntimeClass(object_getClass(object_))) {
        classWrapperValue = bridge_->findClassValue(
            runtime, objc_lookUpClass(symbol->runtimeName.c_str()));
      }
    }
    if (classWrapperValue.isObject()) {
      Object classWrapper = classWrapperValue.asObject(runtime);
      Value prototypeValue = classWrapper.getProperty(runtime, "prototype");
      if (prototypeValue.isObject()) {
        return prototypeValue;
      }
    }
    return bridge_->findClassPrototype(runtime, object_getClass(object_));
  }

  Value engineThisValueForObject(Runtime& runtime) {
    Value thisValue = bridge_->findRoundTripValue(runtime, object_,
                                                  nullptr, true);
    if (thisValue.isObject()) {
      return thisValue;
    }
    return makeNativeObjectValue(runtime, bridge_, object_, false);
  }

  Value prototypeFunctionForProperty(Runtime& runtime,
                                     const std::string& property) {
    if (property.empty()) {
      return Value::undefined();
    }

    Value prototypeValue = classPrototypeForObject(runtime);
    if (!prototypeValue.isObject()) {
      return Value::undefined();
    }

    Object objectConstructor =
        runtime.global().getPropertyAsObject(runtime, "Object");
    Function getOwnPropertyDescriptor =
        objectConstructor.getPropertyAsFunction(runtime,
                                                "getOwnPropertyDescriptor");
    Function getPrototypeOf =
        objectConstructor.getPropertyAsFunction(runtime, "getPrototypeOf");
    Value propertyName = makeString(runtime, property);
    Value currentValue(runtime, prototypeValue);

    for (size_t depth = 0; depth < 64 && currentValue.isObject(); depth++) {
      Object current = currentValue.asObject(runtime);
      Value descriptorValue =
          getOwnPropertyDescriptor.call(runtime, Value(runtime, current),
                                        propertyName);
      if (descriptorValue.isObject()) {
        Value functionValue =
            descriptorValue.asObject(runtime).getProperty(runtime, "value");
        if (functionValue.isObject() &&
            functionValue.asObject(runtime).isFunction(runtime)) {
          bridge_->setObjectExpando(runtime, object_, property, functionValue);
          return functionValue;
        }
        return Value::undefined();
      }
      currentValue =
          getPrototypeOf.call(runtime, Value(runtime, current));
    }

    return Value::undefined();
  }

  // Invoke a JS-prototype getter accessor with this instance as the receiver.
  // Sets *found and returns the resolved value.
  Value resolveEnginePrototypeGetter(Runtime& runtime,
                                     const std::string& property, bool* found) {
    *found = false;
    if (object_ == nil || property.empty()) {
      return Value::undefined();
    }
    Value prototypeValue = classPrototypeForObject(runtime);
    if (!prototypeValue.isObject()) {
      return Value::undefined();
    }
    Object objectConstructor =
        runtime.global().getPropertyAsObject(runtime, "Object");
    Function getOwnPropertyDescriptor =
        objectConstructor.getPropertyAsFunction(runtime, "getOwnPropertyDescriptor");
    Function getPrototypeOf =
        objectConstructor.getPropertyAsFunction(runtime, "getPrototypeOf");
    Value propertyName = makeString(runtime, property);
    Value currentValue(runtime, prototypeValue);
    for (size_t depth = 0; depth < 64 && currentValue.isObject(); depth++) {
      Object current = currentValue.asObject(runtime);
      Value descriptorValue = getOwnPropertyDescriptor.call(
          runtime, Value(runtime, current), propertyName);
      if (descriptorValue.isObject()) {
        Object descriptor = descriptorValue.asObject(runtime);
        Value getterValue = descriptor.getProperty(runtime, "get");
        if (getterValue.isObject() &&
            getterValue.asObject(runtime).isFunction(runtime)) {
          Value thisValue = engineThisValueForObject(runtime);
          if (thisValue.isObject()) {
            *found = true;
            return getterValue.asObject(runtime).asFunction(runtime).callWithThis(
                runtime, thisValue.asObject(runtime),
                static_cast<const Value*>(nullptr), static_cast<size_t>(0));
          }
        }
        Value dataValue = descriptor.getProperty(runtime, "value");
        if (!dataValue.isUndefined()) {
          *found = true;
          return dataValue;
        }
        return Value::undefined();
      }
      currentValue = getPrototypeOf.call(runtime, Value(runtime, current));
    }
    return Value::undefined();
  }

  // Invoke a JS-prototype setter accessor with this instance as the receiver.
  // Returns true when a setter was found and invoked.
  bool invokeEnginePrototypeSetter(Runtime& runtime, const std::string& property,
                                   const Value& value) {
    if (object_ == nil || property.empty()) {
      return false;
    }
    Value prototypeValue = classPrototypeForObject(runtime);
    if (!prototypeValue.isObject()) {
      return false;
    }
    Object objectConstructor =
        runtime.global().getPropertyAsObject(runtime, "Object");
    Function getOwnPropertyDescriptor =
        objectConstructor.getPropertyAsFunction(runtime, "getOwnPropertyDescriptor");
    Function getPrototypeOf =
        objectConstructor.getPropertyAsFunction(runtime, "getPrototypeOf");
    Value propertyName = makeString(runtime, property);
    Value currentValue(runtime, prototypeValue);
    for (size_t depth = 0; depth < 64 && currentValue.isObject(); depth++) {
      Object current = currentValue.asObject(runtime);
      Value descriptorValue = getOwnPropertyDescriptor.call(
          runtime, Value(runtime, current), propertyName);
      if (descriptorValue.isObject()) {
        Value setterValue =
            descriptorValue.asObject(runtime).getProperty(runtime, "set");
        if (setterValue.isObject() &&
            setterValue.asObject(runtime).isFunction(runtime)) {
          Value thisValue = engineThisValueForObject(runtime);
          if (thisValue.isObject()) {
            Value args[] = {Value(runtime, value)};
            setterValue.asObject(runtime).asFunction(runtime).callWithThis(
                runtime, thisValue.asObject(runtime),
                static_cast<const Value*>(args), static_cast<size_t>(1));
            return true;
          }
        }
        return false;
      }
      currentValue = getPrototypeOf.call(runtime, Value(runtime, current));
    }
    return false;
  }

  Value get(Runtime& runtime, const PropNameID& name) override {
    bridge_->requireOwnerRuntime(runtime);
    std::string property = name.utf8(runtime);

    // Fast path: check expando cache first (hot path for method calls).
    Value expando = bridge_->findObjectExpando(runtime, object_, property);
    if (!expando.isUndefined()) {
      return expando;
    }

    // Fast path: cached metadata property-getter resolution. Skips the
    // special-name chain + per-access metadata discovery for hot getters
    // (hash/length/count/...). Only populated for genuine non-extended
    // metadata property members below, so a hit is always safe to serve.
    if (object_ != nil) {
      if (const auto* cached = bridge_->findCachedPropertyGetter(
              object_getClass(object_), property)) {
        if (cached->preparedInvocation != nullptr) {
          return callPreparedObjectSelector(runtime,
                                            *cached->preparedInvocation,
                                            nullptr, 0);
        }
        return callObjectSelector(runtime, cached->selectorName, cached->member,
                                  nullptr, 0);
      }
    }

    if (property == "kind") {
      return makeString(runtime, "object");
    }
    if (property == "className") {
      return makeString(runtime, object_ != nil ? object_getClassName(object_) : "");
    }
    if (property == "nativeAddress") {
      char address[32] = {};
      snprintf(address, sizeof(address), "%p", object_);
      return makeString(runtime, address);
    }
    if (property == "class") {
      auto bridge = bridge_;
      id object = object_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "class"), 0,
          [bridge, object](Runtime& runtime, const Value&, const Value*,
                           size_t) -> Value {
            if (object == nil) {
              return Value::undefined();
            }
            Value classWrapper = bridge->findObjectExpando(
                runtime, object, "__nativeApiClassWrapper");
            if (classWrapper.isObject()) {
              return classWrapper;
            }
            NativeApiSymbol symbol =
                nativeApiSymbolForRuntimeClass(bridge, object_getClass(object));
            return makeNativeClassValue(runtime, bridge, std::move(symbol));
          });
    }
    if (property == "constructor") {
      if (object_ == nil) {
        return Value::undefined();
      }
      // Check class wrapper expando first (set during class setup).
      Value classWrapper = bridge_->findObjectExpando(
          runtime, object_, "__nativeApiClassWrapper");
      if (classWrapper.isObject()) {
        return classWrapper;
      }
      // Try cached class value.
      Class objClass = object_getClass(object_);
      Value cached = bridge_->findClassValue(runtime, objClass);
      if (!cached.isUndefined()) {
        return cached;
      }
      // Resolve through metadata and global.
      NativeApiSymbol symbol =
          nativeApiSymbolForRuntimeClass(bridge_, objClass);
      // Try the global by the symbol's name (which may be the JS-friendly name
      // from metadata, different from the ObjC runtime name for Swift classes).
      if (!symbol.name.empty()) {
        Object global = runtime.global();
        if (global.hasProperty(runtime, symbol.name.c_str())) {
          Value globalClass = global.getProperty(runtime, symbol.name.c_str());
          if (!globalClass.isUndefined() && !globalClass.isNull()) {
            return globalClass;
          }
        }
        // Also try the runtime name if different.
        if (symbol.runtimeName != symbol.name &&
            global.hasProperty(runtime, symbol.runtimeName.c_str())) {
          Value globalClass = global.getProperty(runtime, symbol.runtimeName.c_str());
          if (!globalClass.isUndefined() && !globalClass.isNull()) {
            return globalClass;
          }
        }
      }
      // For Swift classes: try findClass by runtime name which checks
      // classSymbolsByRuntimeName_ and may return a different JS-friendly name.
      if (bridge_ != nullptr) {
        const char* runtimeName = class_getName(objClass);
        if (runtimeName != nullptr) {
          if (const NativeApiSymbol* found = bridge_->findClass(runtimeName)) {
            if (found->name != symbol.name) {
              Object global = runtime.global();
              if (global.hasProperty(runtime, found->name.c_str())) {
                Value globalClass = global.getProperty(runtime, found->name.c_str());
                if (!globalClass.isUndefined() && !globalClass.isNull()) {
                  return globalClass;
                }
              }
            }
          }
        }
      }
      return makeNativeClassValue(runtime, bridge_, std::move(symbol));
    }
    if (property == "superclass") {
      if (object_ == nil) {
        return Value::undefined();
      }
      Class superclass = class_getSuperclass(object_getClass(object_));
      if (superclass == Nil) {
        return Value::null();
      }
      // Try cached class value.
      Value cached = bridge_->findClassValue(runtime, superclass);
      if (!cached.isUndefined()) {
        return cached;
      }
      // Try global lookup by class name.
      const char* name = class_getName(superclass);
      if (name != nullptr && name[0] != '\0') {
        Object global = runtime.global();
        if (global.hasProperty(runtime, name)) {
          Value globalClass = global.getProperty(runtime, name);
          if (!globalClass.isUndefined()) {
            return globalClass;
          }
        }
      }
      NativeApiSymbol symbol = nativeApiSymbolForRuntimeClass(bridge_, superclass);
      return makeNativeClassValue(runtime, bridge_, std::move(symbol));
    }
    if (property == "super") {
      Class dispatchClass =
          object_ != nil ? class_getSuperclass(object_getClass(object_)) : Nil;
      return Object::createFromHostObject(
          runtime,
          std::make_shared<NativeApiSuperHostObject>(bridge_, object_,
                                                     dispatchClass));
    }
    if (property == "invoke" || property == "send") {
      auto bridge = bridge_;
      id object = object_;
      std::weak_ptr<NativeApiObjectHostObject> weakSelf = shared_from_this();
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 1,
          [bridge, object, weakSelf](Runtime& runtime, const Value&,
                                     const Value* args,
                                     size_t count) -> Value {
            std::string selectorName =
                readStringArg(runtime, args, count, 0, "selector");
            if (auto self = weakSelf.lock()) {
              return self->callObjectSelector(runtime, selectorName, nullptr,
                                              args + 1, count - 1);
            }
            return callObjCSelector(runtime, bridge, object, false, selectorName,
                                    nullptr, args + 1, count - 1);
          });
    }
    if (property == "takeRetainedValue" || property == "takeUnretainedValue") {
      bool retained = property == "takeRetainedValue";
      std::weak_ptr<NativeApiObjectHostObject> weakSelf = shared_from_this();
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 0,
          [weakSelf, retained](Runtime& runtime, const Value&, const Value*,
                               size_t) -> Value {
            auto self = weakSelf.lock();
            if (!self || self->object_ == nil || self->consumed_) {
              throw JSError(runtime, "Unmanaged value has already been consumed.");
            }

            id object = self->object_;
            bool ownsObject = self->ownsObject_;
            bool wrapperRetainedObject = self->wrapperRetainedObject_;
            if (self->bridge_ != nullptr) {
              self->bridge_->forgetRoundTripValue(runtime, object);
              self->bridge_->releaseObjectExpandoOwner(object);
            }
            self->object_ = nil;
            self->ownsObject_ = false;
            self->wrapperRetainedObject_ = false;
            if (self->lifetimeState_ != nullptr) {
              self->lifetimeState_->clear();
            }
            self->consumed_ = true;
            const bool releasePreviousOwnership =
                ownsObject && (!retained || wrapperRetainedObject);
            try {
              Value result =
                  makeNativeObjectValue(runtime, self->bridge_, object, retained);
              if (releasePreviousOwnership) {
                [object release];
              }
              return result;
            } catch (...) {
              if (releasePreviousOwnership) {
                [object release];
              }
              throw;
            }
          });
    }
    if (property == "toString") {
      id object = object_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "toString"), 0,
          [object](Runtime& runtime, const Value&, const Value*, size_t) -> Value {
            return NativeApiObjectHostObject::descriptionString(runtime, object);
          });
    }
    if (property == "description") {
      return descriptionString(runtime, object_);
    }
    if (property == "URL" && object_ != nil &&
        [object_ respondsToSelector:@selector(URL)]) {
      return callObjectSelector(runtime, "URL", nullptr, nullptr, 0);
    }
    if (property == "Symbol.iterator" ||
        property == "Symbol(Symbol.iterator)" ||
        property == "@@iterator") {
      auto bridge = bridge_;
      id object = object_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "Symbol.iterator"), 0,
          [bridge, object](Runtime& runtime, const Value&, const Value*,
                           size_t) -> Value {
            if (object == nil ||
                ![object conformsToProtocol:@protocol(NSFastEnumeration)]) {
              throw JSError(
                  runtime, "Object does not conform to NSFastEnumeration.");
            }
            return Object::createFromHostObject(
                runtime,
                std::make_shared<NativeApiFastEnumerationIteratorHostObject>(
                    bridge, static_cast<id<NSFastEnumeration>>(object)));
          });
    }

#if TARGET_OS_OSX
    if (property == "initWithRedGreenBlueAlpha") {
      Class nsColorClass = NSClassFromString(@"NSColor");
      if (object_ != nil && nsColorClass != Nil &&
          [object_ isKindOfClass:nsColorClass]) {
        auto bridge = bridge_;
        return Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, property.c_str()), 4,
            [bridge, nsColorClass](Runtime& runtime, const Value&,
                                   const Value* args, size_t count) -> Value {
              const char* selectors[] = {
                  "colorWithSRGBRed:green:blue:alpha:",
                  "colorWithCalibratedRed:green:blue:alpha:",
                  "colorWithDeviceRed:green:blue:alpha:",
              };
              for (const char* selectorName : selectors) {
                if (class_getClassMethod(nsColorClass,
                                         sel_getUid(selectorName)) != nullptr) {
                  return callObjCSelector(runtime, bridge,
                                          static_cast<id>(nsColorClass), true,
                                          selectorName, nullptr, args, count);
                }
              }
              throw JSError(
                  runtime, "NSColor RGB initializer is not available.");
            });
      }
    }
#endif

    if (property == "initWithFireDateIntervalTargetSelectorUserInfoRepeats") {
      Class timerClass = NSClassFromString(@"NSTimer");
      if (object_ != nil && timerClass != Nil &&
          [object_ isKindOfClass:timerClass]) {
        auto bridge = bridge_;
        return Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, property.c_str()), 6,
            [bridge, timerClass](Runtime& runtime, const Value&,
                                 const Value* args, size_t count) -> Value {
              if (count < 6) {
                throw JSError(
                    runtime, "NSTimer initializer expects six arguments.");
              }
              return callObjCSelector(
                  runtime, bridge, static_cast<id>(timerClass), true,
                  "timerWithTimeInterval:target:selector:userInfo:repeats:",
                  nullptr, args + 1, count - 1);
            });
      }
    }

    if (object_ != nil && [object_ isKindOfClass:[NSArray class]]) {
      NSArray* array = static_cast<NSArray*>(object_);
      if (property == "length") {
        return static_cast<double>(array.count);
      }
      if (auto index = parseArrayIndexProperty(property)) {
        if (*index >= array.count) {
          return Value::undefined();
        }
        id element = [array objectAtIndex:*index];
        NativeApiType elementType = nativeObjectReturnType();
        return convertNativeReturnValue(runtime, bridge_, elementType, &element);
      }
    }

    if (object_ != nil && property == "length" &&
        ![object_ respondsToSelector:@selector(length)]) {
      return Value::undefined();
    }
    if (object_ != nil && property == "count" &&
        ![object_ respondsToSelector:@selector(count)]) {
      return Value::undefined();
    }

    // For JS-extended instances, metadata property accessors live on the
    // prototype chain (native accessors plus any JS overrides), so defer to the
    // engine instead of reading the native property here and shadowing a JS
    // override.
    bool isEngineExtendedInstance =
        object_ != nil &&
        class_conformsToProtocol(object_getClass(object_),
                                 @protocol(NativeApiClassBuilderProtocol));

    if (object_ != nil && !isEngineExtendedInstance) {
      if (const NativeApiSymbol* symbol =
              bridge_->findClassForRuntimeClass(object_getClass(object_))) {
        const auto& members = bridge_->membersForClass(*symbol);
        if (const NativeApiMember* propertyMember =
                selectPropertyMember(members, property, false)) {
          if (auto getter = respondingPropertyGetterSelector(
                  object_, property, propertyMember->selectorName)) {
            NativeApiMember getterMember = *propertyMember;
            getterMember.selectorName = *getter;
            std::shared_ptr<NativeApiPreparedObjCInvocation> preparedGetter;
            try {
              preparedGetter = prepareNativeApiObjCInvocation(
                  runtime, bridge_, object_getClass(object_), false,
                  getterMember.selectorName, &getterMember);
            } catch (const std::exception&) {
            }
            bridge_->cachePropertyGetter(object_getClass(object_), property,
                                         propertyMember,
                                         getterMember.selectorName,
                                         preparedGetter);
            if (preparedGetter != nullptr) {
              return callPreparedObjectSelector(runtime, *preparedGetter,
                                                nullptr, 0);
            }
            return callObjectSelector(runtime, getterMember.selectorName,
                                      &getterMember, nullptr, 0);
          }
        }

        // Resolve metadata methods to a bound selector-group function. The
        // bound receiver keeps method-call semantics correct even on engines
        // whose host-object interceptor does not preserve `this`, while the
        // engine backend can still use its direct selector-group/GSD path.
        if (hasMethodMember(members, property, false)) {
          auto selectors =
              selectorGroupEntriesForMethod(members, property, false);
          if (selectors != nullptr) {
            auto preparedInvocations = std::make_shared<std::vector<
                std::shared_ptr<NativeApiPreparedObjCInvocation>>>(
                selectors->size());
            Value methodFunction = CreateNativeApiBoundSelectorGroupFunction(
                runtime, bridge_, object_getClass(object_), shared_from_this(),
                selectors, preparedInvocations);
            // Cache the resolved host function so repeated method access does
            // not reallocate it on every call (hot path).
            bridge_->setObjectExpando(runtime, object_, property,
                                      methodFunction);
            return methodFunction;
          }
        }
      }
    }

    Value prototypeFunction = prototypeFunctionForProperty(runtime, property);
    if (!prototypeFunction.isUndefined()) {
      return prototypeFunction;
    }

    // JS-subclassed instances own their members in JS (prototype accessors and
    // methods); defer so the engine resolves them instead of the bridge
    // returning a registered getter IMP as a raw callable.
    if (isEngineExtendedInstance) {
#ifdef NATIVESCRIPT_NATIVE_API_HOST_EXPLICIT_OVERRIDE
      // Engines whose exotic property handler invokes prototype accessors with
      // the wrong receiver need the JS-prototype getter resolved here with this
      // instance as the receiver.
      bool found = false;
      Value resolved = resolveEnginePrototypeGetter(runtime, property, &found);
      if (found) {
        return resolved;
      }
#endif
      if (auto selector =
              runtimeReadablePropertyGetter(object_, property)) {
        return callObjectSelector(runtime, *selector, nullptr, nullptr, 0);
      }
      return Value::undefined();
    }

    if (object_ != nil) {
      // A runtime ObjC property (e.g. from a protocol the concrete, non-metadata
      // class adopts) must be invoked as a getter, not returned as a callable.
      if (objc_property_t prop =
              class_getProperty(object_getClass(object_), property.c_str())) {
        std::string getter = property;
        if (char* customGetter = property_copyAttributeValue(prop, "G")) {
          getter = customGetter;
          free(customGetter);
        }
        if (auto selector =
                respondingPropertyGetterSelector(object_, property, getter)) {
          return callObjectSelector(runtime, *selector, nullptr, nullptr, 0);
        }
      }
    }

    if (object_ != nil &&
        hasRuntimeMemberForName(object_getClass(object_), false, property)) {
      std::weak_ptr<NativeApiObjectHostObject> weakSelf = shared_from_this();
      std::string memberName = property;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 0,
          [weakSelf, memberName](Runtime& runtime, const Value&,
                                 const Value* args, size_t count) -> Value {
            auto self = weakSelf.lock();
            if (!self || self->object_ == nil) {
              throw JSError(runtime,
                            "Cannot send Objective-C selector to nil.");
            }
            auto selectorName = selectRuntimeSelectorForName(
                object_getClass(self->object_), false, memberName, count);
            if (!selectorName) {
              throw JSError(runtime,
                            "Objective-C selector is not available: " +
                                memberName);
            }
            return self->callObjectSelector(runtime, *selectorName, nullptr,
                                            args, count);
          });
    }

    return Value::undefined();
  }

  NativeApiHostSetResult set(Runtime& runtime, const PropNameID& name, const Value& value) override {
    bridge_->requireOwnerRuntime(runtime);
    std::string property = name.utf8(runtime);
    if (object_ == nil) {
      throw JSError(runtime, "Cannot set property on nil object.");
    }

    if (const NativeApiSymbol* symbol =
            bridge_->findClassForRuntimeClass(object_getClass(object_))) {
      const auto& members = bridge_->membersForClass(*symbol);
      if (const NativeApiMember* propertyMember =
              selectWritablePropertyMember(members, property, false)) {
        if (propertyMember->readonly) {
          throw JSError(
              runtime, "Attempted to assign to readonly property.");
        }
        NativeApiMember setterMember = *propertyMember;
        setterMember.selectorName = propertyMember->setterSelectorName;
        setterMember.signatureOffset = propertyMember->setterSignatureOffset;
        Value args[] = {Value(runtime, value)};
        callObjCSelector(runtime, bridge_, object_, false,
                         setterMember.selectorName, &setterMember, args, 1);
        NATIVE_API_SET_RETURN(true);
      }
    }

    if (auto setterSelectorName =
            runtimeWritablePropertySetter(object_, property)) {
      Value args[] = {Value(runtime, value)};
      callObjCSelector(runtime, bridge_, object_, false,
                       *setterSelectorName, nullptr, args, 1);
      NATIVE_API_SET_RETURN(true);
    }

    // For JS-subclassed instances, an unknown property is owned by the JS
    // prototype (e.g. a JS-defined accessor); defer so the engine runs it instead of
    // shadowing it with a bridge expando.
    if (class_conformsToProtocol(object_getClass(object_),
                                 @protocol(NativeApiClassBuilderProtocol))) {
#ifdef NATIVESCRIPT_NATIVE_API_HOST_EXPLICIT_OVERRIDE
      // Engines whose exotic property storage doesn't fall back to own
      // properties need the JS-owned set resolved here: invoke a JS-prototype
      // setter if present, otherwise store the value as a bridge expando.
      bool invokedPrototypeSetter =
          invokeEnginePrototypeSetter(runtime, property, value);
      if (!invokedPrototypeSetter) {
        storeOwnExpando(runtime, property, value);
      }
      NATIVE_API_SET_RETURN(true);
#else
      NATIVE_API_SET_RETURN(false);
#endif
    }

    bridge_->setObjectExpando(runtime, object_, property, value);
    NATIVE_API_SET_RETURN(true);
  }

  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override {
    bridge_->requireOwnerRuntime(runtime);
    std::vector<PropNameID> names;
    names.reserve(6);
    addPropertyName(runtime, names, "kind");
    addPropertyName(runtime, names, "className");
    addPropertyName(runtime, names, "nativeAddress");
    addPropertyName(runtime, names, "constructor");
    addPropertyName(runtime, names, "superclass");
    addPropertyName(runtime, names, "super");
    addPropertyName(runtime, names, "invoke");
    addPropertyName(runtime, names, "send");
    addPropertyName(runtime, names, "takeRetainedValue");
    addPropertyName(runtime, names, "takeUnretainedValue");
    addPropertyName(runtime, names, "toString");
    return names;
  }

 private:
  std::shared_ptr<NativeApiBridge> bridge_;
  id object_ = nil;
  bool ownsObject_ = false;
  bool wrapperRetainedObject_ = false;
  bool consumed_ = false;
  std::shared_ptr<NativeApiObjectLifetimeState> lifetimeState_;
};
