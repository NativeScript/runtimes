#ifndef NATIVESCRIPT_NATIVE_API_BACKEND_NAME
#error Engine backends must define NATIVESCRIPT_NATIVE_API_BACKEND_NAME.
#endif

extern "C" {
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);
}

class NativeApiAutoreleasePool final {
 public:
  NativeApiAutoreleasePool() : pool_(objc_autoreleasePoolPush()) {}
  ~NativeApiAutoreleasePool() { objc_autoreleasePoolPop(pool_); }

 private:
  void* pool_;
};

#ifndef NATIVESCRIPT_NATIVE_API_RUNTIME_NAME
#define NATIVESCRIPT_NATIVE_API_RUNTIME_NAME NATIVESCRIPT_NATIVE_API_BACKEND_NAME
#endif

#ifndef NATIVESCRIPT_NATIVE_API_HAS_ENGINE_LAZY_GLOBALS
inline bool InstallNativeApiLazyGlobal(
    Runtime&, std::shared_ptr<NativeApiBridge>, const std::string&,
    const std::string&, bool) {
  return false;
}
#endif

#ifndef NATIVESCRIPT_NATIVE_API_HAS_ENGINE_SELECTOR_GROUP_FUNCTION
#error Engine backends must provide an engine selector group function.
#endif

class NativeApiHostObject final : public HostObject {
 public:
  explicit NativeApiHostObject(std::shared_ptr<NativeApiBridge> bridge)
      : bridge_(std::move(bridge)) {}

  // General accessor (not RN-specific): lets any caller holding the
  // per-runtime `__nativeScriptNativeApi` global's HostObject recover the
  // underlying bridge, e.g. to wrap/unwrap a native object into an engine
  // value via the same mechanism every other crossing already uses (see the
  // Hermes-backend wrap/unwrap helpers, ffi/objc/hermes/; this file stays
  // engine-neutral and does not itself reference any engine-specific type).
  const std::shared_ptr<NativeApiBridge>& bridge() const { return bridge_; }

  Value get(Runtime& runtime, const PropNameID& name) override {
    std::string property = name.utf8(runtime);
    if (property == "runtime") {
      return makeString(runtime, NATIVESCRIPT_NATIVE_API_RUNTIME_NAME);
    }
    if (property == "backend") {
      return makeString(runtime, NATIVESCRIPT_NATIVE_API_BACKEND_NAME);
    }
    if (property == "metadata") {
      return metadataObject(runtime);
    }
    if (property == "hasScheduler") {
      return bridge_->scheduler() != nullptr;
    }
    if (property == "interop") {
      return createInteropObject(runtime, bridge_);
    }
    if (property == "autoreleasepool") {
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "autoreleasepool"), 1,
          [](Runtime& runtime, const Value&, const Value* args,
             size_t count) -> Value {
            if (count < 1 || !args[0].isObject() ||
                !args[0].asObject(runtime).isFunction(runtime)) {
              throw JSError(runtime,
                            "autoreleasepool expects a callback function.");
            }
            NativeApiAutoreleasePool pool;
            return args[0].asObject(runtime).asFunction(runtime).call(runtime);
          });
    }
#ifdef NATIVESCRIPT_NATIVE_API_HAS_ENGINE_LAZY_GLOBALS
    if (property == "__defineLazyGlobal") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "__defineLazyGlobal"), 3,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            std::string name = readStringArg(runtime, args, count, 0, "name");
            std::string kind = readStringArg(runtime, args, count, 1, "kind");
            bool force = count > 2 && args[2].isBool() && args[2].getBool();
            return InstallNativeApiLazyGlobal(runtime, bridge, name, kind,
                                                    force);
          });
    }
#endif
    if (property == "__fastEnumeration") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "__fastEnumeration"), 1,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            if (count < 1 || !args[0].isObject()) {
              throw JSError(
                  runtime, "Fast enumeration expects a native object.");
            }
            id object = NativeApiObjectHostObject::nativeObjectFromValue(runtime, args[0]);
            if (object == nil) {
              throw JSError(
                  runtime, "Fast enumeration expects a native object.");
            }
            if (![object conformsToProtocol:@protocol(NSFastEnumeration)]) {
              throw JSError(
                  runtime, "Object does not conform to NSFastEnumeration.");
            }
            return Object::createFromHostObject(
                runtime,
                std::make_shared<NativeApiFastEnumerationIteratorHostObject>(
                    bridge, static_cast<id<NSFastEnumeration>>(object)));
          });
    }
    if (property == "import") {
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "import"), 1,
          [](Runtime& runtime, const Value&, const Value* args,
             size_t count) -> Value {
            std::string path = readStringArg(runtime, args, count, 0, "path");
            std::string frameworkPath = path;
            if (!frameworkPath.empty() && frameworkPath[0] != '/') {
              frameworkPath = "/System/Library/Frameworks/" + frameworkPath +
                              ".framework";
            }

            NSBundle* bundle = [NSBundle
                bundleWithPath:[NSString stringWithUTF8String:frameworkPath.c_str()]];
            if (bundle == nil || ![bundle load]) {
              throw JSError(
                  runtime, "Could not load bundle: " + frameworkPath);
            }
            return true;
          });
    }
    if (property == "lookup") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "lookup"), 1,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            std::string symbolName =
                readStringArg(runtime, args, count, 0, "name");
            const NativeApiSymbol* symbol = bridge->find(symbolName);
            if (symbol == nullptr) {
              return Value::null();
            }
            return symbolToObject(runtime, *symbol);
          });
    }
    if (property == "getClass") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "getClass"), 1,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            std::string className =
                readStringArg(runtime, args, count, 0, "name");
            const NativeApiSymbol* symbol = bridge->findClass(className);
            if (symbol == nullptr) {
              Class cls = objc_lookUpClass(className.c_str());
              if (cls == nil) {
                return Value::null();
              }
              NativeApiSymbol runtimeSymbol{
                  .kind = NativeApiSymbolKind::Class,
                  .offset = MD_SECTION_OFFSET_NULL,
                  .name = className,
                  .runtimeName = className,
              };
              return makeNativeClassValue(runtime, bridge,
                                          std::move(runtimeSymbol));
            }

            return makeNativeClassValue(runtime, bridge, *symbol);
          });
    }
    if (property == "__extendClass") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "__extendClass"), 2,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            return extendNativeApiClass(runtime, bridge, args, count);
          });
    }
    if (property == "__invokeBase") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "__invokeBase"), 3,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            return invokeNativeApiBaseMethod(runtime, bridge, args, count);
          });
    }
    if (property == "__makeSelectorGroupFunction") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "__makeSelectorGroupFunction"),
          3,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            if (count < 3 || !args[1].isBool() || !args[2].isObject() ||
                !args[2].asObject(runtime).isArray(runtime)) {
              throw JSError(
                  runtime,
                  "__makeSelectorGroupFunction expects class, receiver kind, "
                  "and selector table.");
            }

            Class lookupClass = classFromEngineValue(runtime, args[0]);
            if (lookupClass == Nil) {
              throw JSError(runtime,
                            "__makeSelectorGroupFunction class is invalid.");
            }

            bool receiverIsClass = args[1].getBool();
            Array selectorTable = args[2].asObject(runtime).getArray(runtime);
            size_t selectorCount = selectorTable.size(runtime);
            auto selectors =
                std::make_shared<
                    std::vector<NativeApiSelectorGroupEntry>>(
                    selectorCount);
            for (size_t i = 0; i < selectorCount; i++) {
              Value selectorValue = selectorTable.getValueAtIndex(runtime, i);
              if (selectorValue.isString()) {
                (*selectors)[i].selectorName =
                    selectorValue.asString(runtime).utf8(runtime);
              } else if (selectorValue.isObject()) {
                Object descriptor = selectorValue.asObject(runtime);
                Value selectorNameValue =
                    descriptor.getProperty(runtime, "selectorName");
                if (!selectorNameValue.isString()) {
                  continue;
                }
                NativeApiMember member;
                member.selectorName =
                    selectorNameValue.asString(runtime).utf8(runtime);
                Value nameValue = descriptor.getProperty(runtime, "name");
                if (nameValue.isString()) {
                  member.name = nameValue.asString(runtime).utf8(runtime);
                }
                Value setterSelectorNameValue =
                    descriptor.getProperty(runtime, "setterSelectorName");
                if (setterSelectorNameValue.isString()) {
                  member.setterSelectorName =
                      setterSelectorNameValue.asString(runtime).utf8(runtime);
                }
                Value signatureOffsetValue =
                    descriptor.getProperty(runtime, "signatureOffset");
                if (signatureOffsetValue.isNumber()) {
                  member.signatureOffset = static_cast<MDSectionOffset>(
                      signatureOffsetValue.getNumber());
                }
                Value setterSignatureOffsetValue =
                    descriptor.getProperty(runtime, "setterSignatureOffset");
                if (setterSignatureOffsetValue.isNumber()) {
                  member.setterSignatureOffset = static_cast<MDSectionOffset>(
                      setterSignatureOffsetValue.getNumber());
                }
                Value flagsValue = descriptor.getProperty(runtime, "flags");
                if (flagsValue.isNumber()) {
                  member.flags = static_cast<metagen::MDMemberFlag>(
                      static_cast<uint32_t>(flagsValue.getNumber()));
                }
                Value propertyValue = descriptor.getProperty(runtime, "property");
                if (propertyValue.isBool()) {
                  member.property = propertyValue.getBool();
                }
                Value readonlyValue = descriptor.getProperty(runtime, "readonly");
                if (readonlyValue.isBool()) {
                  member.readonly = readonlyValue.getBool();
                }
                (*selectors)[i].selectorName = member.selectorName;
                (*selectors)[i].member = std::move(member);
                (*selectors)[i].hasMember = true;
              }
            }

            auto preparedInvocations = std::make_shared<std::vector<
                std::shared_ptr<NativeApiPreparedObjCInvocation>>>(
		                selectors->size());

            return CreateNativeApiSelectorGroupFunction(
                runtime, bridge, lookupClass, receiverIsClass, selectors,
                preparedInvocations);
          });
    }
    if (property == "__rememberClassWrapper") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "__rememberClassWrapper"), 3,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            if (count < 2) {
              return Value::undefined();
            }
            Class cls = classFromEngineValue(runtime, args[0]);
            if (cls == Nil) {
              return Value::undefined();
            }
            bridge->rememberClassValue(runtime, cls, args[1]);
            if (count >= 3 && args[2].isObject()) {
              bridge->rememberClassPrototype(runtime, cls, args[2]);
            }
            return Value::undefined();
          });
    }
    if (property == "__rememberObjectClassWrapper") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "__rememberObjectClassWrapper"),
          2,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            if (count < 2) {
              return Value::undefined();
            }
            id object = NativeApiObjectHostObject::nativeObjectFromValue(
                runtime, args[0]);
            if (object == nil) {
              return Value::undefined();
            }
            // A factory/class method may return an instance of a different
            // class (e.g. +[TNSSwiftLikeFactory create] returns a TNSSwiftLike).
            // Only label the object with this wrapper when it actually is an
            // instance of the wrapper's class, so `constructor` resolves to the
            // object's real class instead of the calling class.
            if (args[1].isObject()) {
              Class wrapperClass = classFromEngineValue(runtime, args[1]);
              if (wrapperClass != Nil && ![object isKindOfClass:wrapperClass]) {
                return Value::undefined();
              }
            }
            bridge->setObjectExpando(runtime, object,
                                     "__nativeApiClassWrapper", args[1]);
            if (args[1].isObject()) {
              Object classWrapper = args[1].asObject(runtime);
              Value prototypeValue =
                  classWrapper.getProperty(runtime, "prototype");
              if (prototypeValue.isObject()) {
                Object instanceObject = args[0].asObject(runtime);
                Object prototype = prototypeValue.asObject(runtime);
                SetNativeApiObjectPrototype(runtime, instanceObject,
                                                 prototype);
              }
            }
            return Value::undefined();
          });
    }
    // Re-entry guards for JS-subclass instance construction/accessors,
    // backed by an associated object (not a JS expando — see the
    // memo/expando-proxy notes: expandos never round-trip on these
    // proxies). ClassBuilder wires these around alloc/init and native
    // accessor dispatch.
    if (property == "__setObjectConstructionState") {
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "__setObjectConstructionState"),
          2,
          [](Runtime& runtime, const Value&, const Value* args,
             size_t count) -> Value {
            if (count < 1) {
              return Value::undefined();
            }
            id object = NativeApiObjectHostObject::nativeObjectFromValue(
                runtime, args[0]);
            if (object == nil) {
              return Value::undefined();
            }
            bool constructing =
                count >= 2 && args[1].isBool() && args[1].getBool();
            objc_setAssociatedObject(
                object, sel_registerName("__nativeApiConstructionState"),
                constructing ? @YES : nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            return Value::undefined();
          });
    }
    if (property == "__setObjectAccessorCallbackState") {
      // Depth-counted (not a bool) so nested/re-entrant accessor calls on the
      // same object (e.g. a getter that reads another property) still clear
      // correctly on unwind.
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(
                       runtime, "__setObjectAccessorCallbackState"),
          2,
          [](Runtime& runtime, const Value&, const Value* args,
             size_t count) -> Value {
            if (count < 1) {
              return Value::undefined();
            }
            id object = NativeApiObjectHostObject::nativeObjectFromValue(
                runtime, args[0]);
            if (object == nil) {
              return Value::undefined();
            }
            bool active = count >= 2 && args[1].isBool() && args[1].getBool();
            SEL key = sel_registerName("__nativeApiAccessorCallbackState");
            NSNumber* current = (NSNumber*)objc_getAssociatedObject(object, key);
            NSInteger depth = current != nil ? current.integerValue : 0;
            if (active) {
              depth += 1;
            } else if (depth > 0) {
              depth -= 1;
            }
            objc_setAssociatedObject(
                object, key, depth > 0 ? @(depth) : nil,
                OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            return Value::undefined();
          });
    }
    if (property == "CC_SHA256") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "CC_SHA256"), 3,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            if (count < 3 || !args[1].isNumber()) {
              throw JSError(
                  runtime, "CC_SHA256 expects data, length, and output.");
            }
            void* commonCrypto =
                dlopen("/usr/lib/system/libcommonCrypto.dylib",
                       RTLD_NOW | RTLD_LOCAL);
            void* symbol = commonCrypto != nullptr
                               ? dlsym(commonCrypto, "CC_SHA256")
                               : nullptr;
            if (symbol == nullptr && commonCrypto != nullptr) {
              symbol = dlsym(commonCrypto, "_CC_SHA256");
            }
            if (symbol == nullptr) {
              throw JSError(runtime,
                                           "CC_SHA256 is not available.");
            }
            NativeApiArgumentFrame frame(3);
            void* data = pointerFromEngineValue(runtime, bridge, args[0], frame);
            void* output =
                pointerFromEngineValue(runtime, bridge, args[2], frame);
            using CC_SHA256_Fn = unsigned char* (*)(const void*, unsigned long,
                                                    unsigned char*);
            auto fn = reinterpret_cast<CC_SHA256_Fn>(symbol);
            unsigned char* result =
                fn(data, static_cast<unsigned long>(args[1].getNumber()),
                   static_cast<unsigned char*>(output));
            return createPointer(runtime, bridge, result);
          });
    }
    if (property == "getFunction") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "getFunction"), 1,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            std::string functionName =
                readStringArg(runtime, args, count, 0, "name");
            const NativeApiSymbol* symbol = bridge->findFunction(functionName);
            if (symbol == nullptr) {
              return Value::null();
            }
            auto prepared =
                std::make_shared<NativeApiPreparedCFunctionInvocation>();
            prepared->symbol = *symbol;
            auto function = Function::createFromHostFunction(
                runtime, PropNameID::forAscii(runtime, symbol->name), 0,
                [bridge, prepared](Runtime& runtime, const Value&,
                                   const Value* args,
                                   size_t count) -> Value {
                  return callCFunction(runtime, bridge, prepared, args, count);
            });
            function.setProperty(runtime, "kind", makeString(runtime, "function"));
            function.setProperty(runtime, "nativeName",
                                 makeString(runtime, symbol->name));
            function.setProperty(runtime, "metadataOffset",
                                 static_cast<double>(symbol->offset));
            function.setProperty(runtime, "sizeof",
                                 static_cast<double>(sizeof(void*)));
            return function;
          });
    }
    if (property == "getConstant") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "getConstant"), 1,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            std::string constantName =
                readStringArg(runtime, args, count, 0, "name");
            const NativeApiSymbol* symbol = bridge->findConstant(constantName);
            if (symbol == nullptr) {
              return Value::undefined();
            }
            return constantToValue(runtime, bridge, *symbol);
          });
    }
    if (property == "getEnum") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "getEnum"), 1,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            std::string enumName = readStringArg(runtime, args, count, 0, "name");
            const NativeApiSymbol* symbol = bridge->findEnum(enumName);
            if (symbol == nullptr) {
              return Value::undefined();
            }
            return enumToObject(runtime, bridge->metadata(), *symbol);
          });
    }
    if (property == "getProtocol") {
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "getProtocol"), 1,
          [bridge](Runtime& runtime, const Value&, const Value* args,
                   size_t count) -> Value {
            std::string protocolName =
                readStringArg(runtime, args, count, 0, "name");
            const NativeApiSymbol* symbol = bridge->findProtocol(protocolName);
            if (symbol == nullptr) {
              Protocol* protocol = lookupProtocolByNativeName(protocolName);
              if (protocol == nullptr) {
                return Value::null();
              }
              const char* runtimeName = protocol_getName(protocol);
              NativeApiSymbol runtimeSymbol{
                  .kind = NativeApiSymbolKind::Protocol,
                  .offset = MD_SECTION_OFFSET_NULL,
                  .name = protocolName,
                  .runtimeName = runtimeName != nullptr ? runtimeName : protocolName,
              };
              return makeNativeProtocolValue(runtime, bridge,
                                             std::move(runtimeSymbol));
            }
            return makeNativeProtocolValue(runtime, bridge, *symbol);
          });
    }
    if (property == "getStruct" || property == "getUnion") {
      auto bridge = bridge_;
      bool isUnion = property == "getUnion";
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 1,
          [bridge, isUnion](Runtime& runtime, const Value&, const Value* args,
                            size_t count) -> Value {
            std::string aggregateName =
                readStringArg(runtime, args, count, 0, "name");
            const NativeApiSymbol* symbol =
                isUnion ? bridge->findUnion(aggregateName)
                        : bridge->findStruct(aggregateName);
            if (symbol == nullptr) {
              return Value::undefined();
            }
            return makeAggregateConstructor(runtime, bridge, *symbol);
          });
    }

    if (const NativeApiSymbol* classSymbol = bridge_->findClass(property)) {
      return makeNativeClassValue(runtime, bridge_, *classSymbol);
    }

    if (const NativeApiSymbol* functionSymbol = bridge_->findFunction(property)) {
      auto prepared =
          std::make_shared<NativeApiPreparedCFunctionInvocation>();
      prepared->symbol = *functionSymbol;
      auto bridge = bridge_;
      Function function = Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 0,
          [bridge, prepared](Runtime& runtime, const Value&,
                             const Value* args,
                             size_t count) -> Value {
            return callCFunction(runtime, bridge, prepared, args, count);
          });
      function.setProperty(runtime, "kind", makeString(runtime, "function"));
      function.setProperty(runtime, "nativeName",
                           makeString(runtime, functionSymbol->name));
      function.setProperty(runtime, "metadataOffset",
                           static_cast<double>(functionSymbol->offset));
      function.setProperty(runtime, "sizeof",
                           static_cast<double>(sizeof(void*)));
      return function;
    }

    if (const NativeApiSymbol* constantSymbol = bridge_->findConstant(property)) {
      return constantToValue(runtime, bridge_, *constantSymbol);
    }

    if (const NativeApiSymbol* enumSymbol = bridge_->findEnum(property)) {
      return enumToObject(runtime, bridge_->metadata(), *enumSymbol);
    }

    if (const NativeApiSymbol* protocolSymbol =
            bridge_->findProtocol(property)) {
      return makeNativeProtocolValue(runtime, bridge_, *protocolSymbol);
    }

    if (const NativeApiSymbol* aggregateSymbol =
            bridge_->findAggregate(property)) {
      return makeAggregateConstructor(runtime, bridge_, *aggregateSymbol);
    }

    return Value::undefined();
  }

  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override {
    std::vector<PropNameID> names;
    names.reserve(11);
    addPropertyName(runtime, names, "runtime");
    addPropertyName(runtime, names, "backend");
    addPropertyName(runtime, names, "metadata");
    addPropertyName(runtime, names, "hasScheduler");
    addPropertyName(runtime, names, "interop");
    addPropertyName(runtime, names, "autoreleasepool");
#ifdef NATIVESCRIPT_NATIVE_API_HAS_ENGINE_LAZY_GLOBALS
    addPropertyName(runtime, names, "__defineLazyGlobal");
#endif
    addPropertyName(runtime, names, "import");
    addPropertyName(runtime, names, "lookup");
    addPropertyName(runtime, names, "getClass");
    addPropertyName(runtime, names, "__extendClass");
    addPropertyName(runtime, names, "__invokeBase");
    addPropertyName(runtime, names, "__makeSelectorGroupFunction");
    addPropertyName(runtime, names, "__rememberClassWrapper");
    addPropertyName(runtime, names, "__rememberObjectClassWrapper");
    addPropertyName(runtime, names, "__setObjectConstructionState");
    addPropertyName(runtime, names, "getFunction");
    addPropertyName(runtime, names, "getConstant");
    addPropertyName(runtime, names, "getEnum");
    addPropertyName(runtime, names, "getProtocol");
    addPropertyName(runtime, names, "getStruct");
    addPropertyName(runtime, names, "getUnion");
    return names;
  }

 private:
  Object metadataObject(Runtime& runtime) const {
    Object metadata(runtime);
    metadata.setProperty(runtime, "classes",
                         static_cast<double>(bridge_->classCount()));
    metadata.setProperty(runtime, "functions",
                         static_cast<double>(bridge_->functionCount()));
    metadata.setProperty(runtime, "constants",
                         static_cast<double>(bridge_->constantCount()));
    metadata.setProperty(runtime, "protocols",
                         static_cast<double>(bridge_->protocolCount()));
    metadata.setProperty(runtime, "enums",
                         static_cast<double>(bridge_->enumCount()));
    metadata.setProperty(runtime, "structs",
                         static_cast<double>(bridge_->structCount()));
    metadata.setProperty(runtime, "unions",
                         static_cast<double>(bridge_->unionCount()));

    metadata.setProperty(
        runtime, "classNames",
        Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, "classNames"), 0,
            [bridge = bridge_](Runtime& runtime, const Value&, const Value*,
                               size_t) -> Value {
              return namesToArray(runtime, bridge->classNames());
            }));
    metadata.setProperty(
        runtime, "functionNames",
        Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, "functionNames"), 0,
            [bridge = bridge_](Runtime& runtime, const Value&, const Value*,
                               size_t) -> Value {
              return namesToArray(runtime, bridge->functionNames());
            }));
    metadata.setProperty(
        runtime, "constantNames",
        Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, "constantNames"), 0,
            [bridge = bridge_](Runtime& runtime, const Value&, const Value*,
                               size_t) -> Value {
              return namesToArray(runtime, bridge->constantNames());
            }));
    metadata.setProperty(
        runtime, "protocolNames",
        Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, "protocolNames"), 0,
            [bridge = bridge_](Runtime& runtime, const Value&, const Value*,
                               size_t) -> Value {
              return namesToArray(runtime, bridge->protocolNames());
            }));
    metadata.setProperty(
        runtime, "enumNames",
        Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, "enumNames"), 0,
            [bridge = bridge_](Runtime& runtime, const Value&, const Value*,
                               size_t) -> Value {
              return namesToArray(runtime, bridge->enumNames());
            }));
    metadata.setProperty(
        runtime, "structNames",
        Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, "structNames"), 0,
            [bridge = bridge_](Runtime& runtime, const Value&, const Value*,
                               size_t) -> Value {
              return namesToArray(runtime, bridge->structNames());
            }));
    metadata.setProperty(
        runtime, "unionNames",
        Function::createFromHostFunction(
            runtime, PropNameID::forAscii(runtime, "unionNames"), 0,
            [bridge = bridge_](Runtime& runtime, const Value&, const Value*,
                               size_t) -> Value {
              return namesToArray(runtime, bridge->unionNames());
            }));
    return metadata;
  }

  std::shared_ptr<NativeApiBridge> bridge_;
};
