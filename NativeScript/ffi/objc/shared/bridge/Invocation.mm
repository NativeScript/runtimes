bool isValidMetadataStringOffset(MDMetadataReader* metadata,
                                 MDSectionOffset offset) {
  if (metadata == nullptr || metadata->constantsOffset < metadata->stringsOffset) {
    return false;
  }
  return offset < metadata->constantsOffset - metadata->stringsOffset;
}

bool startsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string stripEnumSuffix(const std::string& enumName) {
  static const std::vector<std::string> suffixes = {
      "Options", "Option", "Enums", "Enum",   "Result", "Enumeration",
      "Orientation", "Style", "Mask", "Type", "Status", "Modes", "Mode", "s"};

  for (const auto& suffix : suffixes) {
    if (enumName.size() > suffix.size() && endsWith(enumName, suffix)) {
      return enumName.substr(0, enumName.size() - suffix.size());
    }
  }

  return enumName;
}

bool isNSComparisonResultOrderingName(const std::string& enumName,
                                      const std::string& member) {
  if (enumName != "NSComparisonResult") {
    return false;
  }
  return member == "Ascending" || member == "Same" || member == "Descending";
}

class NativeApiReturnStorage {
 public:
  explicit NativeApiReturnStorage(size_t size)
      : size_(std::max<size_t>(size, sizeof(void*))) {
    if (size_ > kInlineSize) {
      heap_.assign(size_, 0);
    } else {
      std::memset(inline_, 0, kInlineSize);
    }
  }

  void* data() { return heap_.empty() ? inline_ : heap_.data(); }
  unsigned char* bytes() { return static_cast<unsigned char*>(data()); }

 private:
  static constexpr size_t kInlineSize = 64;

  size_t size_ = 0;
  alignas(std::max_align_t) unsigned char inline_[kInlineSize] = {};
  std::vector<unsigned char> heap_;
};

class NativeApiPointerFrame {
 public:
  explicit NativeApiPointerFrame(size_t count) : count_(count) {
    if (count_ > kInlineCount) {
      heap_.resize(count_);
    }
  }

  void set(size_t index, void* value) {
    if (index >= count_) {
      throw std::out_of_range("Native invocation argument index out of range.");
    }
    if (count_ <= kInlineCount) {
      inline_[index] = value;
    } else {
      heap_[index] = value;
    }
  }

  void** data() {
    if (count_ == 0) {
      return nullptr;
    }
    return count_ <= kInlineCount ? inline_ : heap_.data();
  }

 private:
  static constexpr size_t kInlineCount = 10;

  size_t count_ = 0;
  void* inline_[kInlineCount] = {};
  std::vector<void*> heap_;
};

Value enumToObject(Runtime& runtime, MDMetadataReader* metadata,
                   const NativeApiSymbol& symbol) {
  Object result(runtime);
  if (metadata == nullptr || symbol.offset == MD_SECTION_OFFSET_NULL) {
    return result;
  }

  std::string enumName = symbol.name;
  std::string strippedPrefix = stripEnumSuffix(enumName);
  MDSectionOffset offset = symbol.offset + sizeof(MDSectionOffset);
  bool next = true;
  while (next) {
    auto nameOffset = metadata->getOffset(offset);
    next = (nameOffset & metagen::mdSectionOffsetNext) != 0;
    nameOffset &= ~metagen::mdSectionOffsetNext;
    offset += sizeof(MDSectionOffset);

    const char* memberName = metadata->resolveString(nameOffset);
    int64_t value = metadata->getEnumValue(offset);
    offset += sizeof(int64_t);

    std::string canonicalName = memberName != nullptr ? memberName : "";
    std::vector<std::string> aliases;
    aliases.push_back(canonicalName);

    if (!strippedPrefix.empty() && startsWith(canonicalName, strippedPrefix) &&
        canonicalName.size() > strippedPrefix.size()) {
      aliases.push_back(canonicalName.substr(strippedPrefix.size()));
    } else if (!strippedPrefix.empty() &&
               !startsWith(canonicalName, strippedPrefix)) {
      aliases.push_back(strippedPrefix + canonicalName);
    }

    if (startsWith(enumName, "NS") && !startsWith(canonicalName, "NS")) {
      aliases.push_back(std::string("NS") + canonicalName);
    }

    if (enumName == "NSStringCompareOptions" &&
        !endsWith(canonicalName, "Search")) {
      aliases.push_back(canonicalName + "Search");
      aliases.push_back(std::string("NS") + canonicalName + "Search");
    }

    if (!startsWith(canonicalName, "k")) {
      aliases.push_back(std::string("k") + enumName + canonicalName);
    }

    if (isNSComparisonResultOrderingName(enumName, canonicalName)) {
      aliases.push_back(std::string("Ordered") + canonicalName);
      aliases.push_back(std::string("NSOrdered") + canonicalName);
    }

    std::vector<std::string> uniqueAliases;
    std::unordered_set<std::string> seenAliases;
    for (const auto& alias : aliases) {
      if (!alias.empty() && seenAliases.insert(alias).second) {
        uniqueAliases.push_back(alias);
      }
    }

    for (const auto& alias : uniqueAliases) {
      result.setProperty(runtime, alias.c_str(), static_cast<double>(value));
    }

    char valueKey[32] = {};
    snprintf(valueKey, sizeof(valueKey), "%lld", static_cast<long long>(value));
    if (!result.hasProperty(runtime, valueKey)) {
      std::string reverseName =
          uniqueAliases.size() > 1 ? uniqueAliases[1] : canonicalName;
      result.setProperty(runtime, valueKey, makeString(runtime, reverseName));
    }
  }
  return result;
}

Value constantToValue(Runtime& runtime,
                      const std::shared_ptr<NativeApiBridge>& bridge,
                      const NativeApiSymbol& symbol) {
  MDMetadataReader* metadata = bridge->metadata();
  if (metadata == nullptr || symbol.offset == MD_SECTION_OFFSET_NULL) {
    return Value::undefined();
  }

  MDSectionOffset offset = symbol.offset + sizeof(MDSectionOffset);
  auto evalKind = metadata->getVariableEvalKind(offset);
  offset += sizeof(metagen::MDVariableEvalKind);

  switch (evalKind) {
    case metagen::mdEvalInt64:
      return static_cast<double>(metadata->getInt64(offset));
    case metagen::mdEvalDouble:
      return metadata->getDouble(offset);
    case metagen::mdEvalString: {
      if (isValidMetadataStringOffset(metadata, offset)) {
        auto stringOffset = metadata->getOffset(offset);
        return makeString(runtime, metadata->resolveString(stringOffset));
      }

      void* symbolPtr = dlsym(bridge->selfDl(), symbol.name.c_str());
      if (symbolPtr == nullptr) {
        return Value::undefined();
      }

      NativeApiType stringObjectType;
      stringObjectType.kind = metagen::mdTypeNSStringObject;
      stringObjectType.ffiType = &ffi_type_pointer;
      stringObjectType.supported = true;
      return convertNativeReturnValue(runtime, bridge, stringObjectType,
                                      symbolPtr);
    }
    case metagen::mdEvalNone:
      break;
  }

  MDSectionOffset typeOffset = offset;
  NativeApiType type = parseMetadataEngineType(metadata, &typeOffset, bridge.get());
  if (unsupportedEngineType(type)) {
    throw JSError(
        runtime, "Native constant type is not supported by backend: " +
                     symbol.name);
  }

  void* symbolPtr = dlsym(bridge->selfDl(), symbol.name.c_str());
  if (symbolPtr == nullptr) {
    return Value::undefined();
  }
  return convertNativeReturnValue(runtime, bridge, type, symbolPtr);
}

void prepareEngineArgument(Runtime& runtime,
                        const std::shared_ptr<NativeApiBridge>& bridge,
                        const NativeApiType& type, const Value& arg,
                        size_t index, NativeApiArgumentFrame& frame) {
  ffi_type* ffiType = ffiTypeForEngineArgument(type);
  size_t size =
      ffiType != nullptr && ffiType->size > 0 ? ffiType->size : nativeSizeForType(type);
  void* target = frame.storageAt(index, size);
  convertEngineFfiArgument(runtime, bridge, type, arg, target, frame);
}

void prepareEngineArguments(Runtime& runtime,
                         const std::shared_ptr<NativeApiBridge>& bridge,
                         const NativeApiSignature& signature,
                         const Value* args, size_t count,
                         NativeApiArgumentFrame& frame) {
  if (count != signature.argumentTypes.size()) {
    throw JSError(
        runtime, "Actual arguments count: \"" + std::to_string(count) +
                     "\". Expected: \"" +
                     std::to_string(signature.argumentTypes.size()) + "\".");
  }

  for (size_t i = 0; i < signature.argumentTypes.size(); i++) {
    prepareEngineArgument(runtime, bridge, signature.argumentTypes[i], args[i], i,
                       frame);
  }
}

inline uint64_t dispatchIdForEngineSignature(
    const NativeApiSignature& signature, SignatureCallKind kind) {
  if (signature.signatureHash == 0) {
    return 0;
  }
  return composeSignatureDispatchId(signature.signatureHash, kind,
                                    signature.dispatchFlags);
}

struct NativeApiPreparedCFunctionInvocation {
  NativeApiSymbol symbol;
  bool initialized = false;
  void* function = nullptr;
  NativeApiSignature signature;
  CFunctionPreparedInvoker preparedInvoker = nullptr;
};

bool tryCallFastEngineCFunction(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    void* function, const NativeApiSignature& signature, const Value* args,
    size_t count, Value* result);

Value callNativeFunctionPointer(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, void* pointer, bool block, const Value* args,
    size_t count) {
  if (pointer == nullptr) {
    throw JSError(runtime, "Native function pointer is null.");
  }
  if (bridge == nullptr || bridge->metadata() == nullptr ||
      type.signatureOffset == MD_SECTION_OFFSET_NULL) {
    throw JSError(
        runtime, "Native function pointer metadata is unavailable.");
  }

  auto signature = parseMetadataEngineSignature(
      bridge->metadata(), type.signatureOffset, block ? 1 : 0, bridge.get());
  if (!signature || !signature->prepared || signature->variadic ||
      unsupportedEngineType(signature->returnType)) {
    throw JSError(
        runtime,
        "Native function pointer signature is not supported by backend.");
  }

  NativeApiArgumentFrame frame(signature->argumentTypes.size());
  prepareEngineArguments(runtime, bridge, *signature, args, count, frame);

  NativeApiPointerFrame values(signature->argumentTypes.size() + 1);
  if (block) {
    values.set(0, &pointer);
    for (size_t i = 0; i < signature->argumentTypes.size(); i++) {
      values.set(i + 1, frame.values()[i]);
    }
  }

  void* callable = pointer;
  if (block) {
    auto literal = static_cast<NativeApiBlockLiteral*>(pointer);
    if (literal == nullptr || literal->invoke == nullptr) {
      throw JSError(runtime, "Native block invoke pointer is null.");
    }
    callable = literal->invoke;
  }

  if (!block) {
    Value fastResult;
    if (tryCallFastEngineCFunction(runtime, bridge, callable, *signature, args,
                                   count, &fastResult)) {
      return fastResult;
    }
  }

  NativeApiReturnStorage returnStorage(
      nativeSizeForType(signature->returnType));
  BlockPreparedInvoker blockPreparedInvoker = nullptr;
  CFunctionPreparedInvoker functionPreparedInvoker = nullptr;
  if (block) {
    blockPreparedInvoker = lookupBlockPreparedInvoker(dispatchIdForEngineSignature(
        *signature, SignatureCallKind::BlockInvoke));
  } else {
    functionPreparedInvoker = lookupCFunctionPreparedInvoker(
        dispatchIdForEngineSignature(*signature, SignatureCallKind::CFunction));
  }
  performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
    if (block) {
      if (blockPreparedInvoker != nullptr) {
        blockPreparedInvoker(callable, values.data(), returnStorage.data());
        return;
      }
    } else {
      if (functionPreparedInvoker != nullptr) {
        functionPreparedInvoker(callable, frame.values(), returnStorage.data());
        return;
      }
    }
    ffi_call(&signature->cif, FFI_FN(callable), returnStorage.data(),
             block ? values.data() : frame.values());
  });

  return convertNativeReturnValue(runtime, bridge, signature->returnType,
                                  returnStorage.data());
}

Value wrapNativeFunctionPointer(Runtime& runtime,
                                const std::shared_ptr<NativeApiBridge>& bridge,
                                const NativeApiType& type, void* pointer,
                                bool block) {
  const char* functionName = block ? "NativeApiBlock" : "NativeApiFunctionPointer";
  auto function = Function::createFromHostFunction(
      runtime, PropNameID::forAscii(runtime, functionName), 0,
      [bridge, type, pointer, block](Runtime& runtime, const Value&,
                                     const Value* args, size_t count) -> Value {
        return callNativeFunctionPointer(runtime, bridge, type, pointer, block,
                                         args, count);
      });
  function.setProperty(runtime, "kind",
                       makeString(runtime, block ? "block" : "functionPointer"));
  function.setProperty(
      runtime, "__nativeApiPointerObject",
      createPointer(runtime, bridge, pointer));
  function.setProperty(
      runtime, "__nativeApiPointer",
      static_cast<double>(reinterpret_cast<uintptr_t>(pointer)));
  function.setProperty(
      runtime, "nativeAddress",
      static_cast<double>(reinterpret_cast<uintptr_t>(pointer)));
  function.setProperty(runtime, "sizeof",
                       static_cast<double>(sizeof(void*)));
  function.setProperty(
      runtime, "toString",
      Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "toString"), 0,
          [pointer, block](Runtime& runtime, const Value&, const Value*,
                           size_t) -> Value {
            char address[32] = {};
            snprintf(address, sizeof(address), "%p", pointer);
            return makeString(runtime,
                              std::string("[NativeApi ") +
                                  (block ? "Block " : "FunctionPointer ") +
                                  address + "]");
          }));
  return function;
}

Value callCFunction(Runtime& runtime,
                    const std::shared_ptr<NativeApiBridge>& bridge,
                    const std::shared_ptr<NativeApiPreparedCFunctionInvocation>& prepared,
                    const Value* args,
                    size_t count) {
  if (prepared == nullptr) {
    throw JSError(runtime, "Native function state is unavailable.");
  }
  NativeApiRoundTripCacheFrameGuard roundTripFrame(bridge);

  MDMetadataReader* metadata = bridge->metadata();
  if (metadata == nullptr) {
    throw JSError(runtime, "Native metadata is not loaded.");
  }

  if (!prepared->initialized) {
    void* fnptr = dlsym(bridge->selfDl(), prepared->symbol.name.c_str());
    if (fnptr == nullptr) {
      throw JSError(runtime,
                                   "Native function is not available: " +
                                       prepared->symbol.name);
    }

    MDSectionOffset signatureOffset =
        metadata->signaturesOffset +
        metadata->getOffset(prepared->symbol.offset + sizeof(MDSectionOffset));
    auto signature = parseMetadataEngineSignature(
        metadata, signatureOffset, 0, bridge.get(),
        (metadata->getFunctionFlag(
             prepared->symbol.offset + sizeof(MDSectionOffset) * 2) &
         metagen::mdFunctionReturnOwned) != 0);
    if (!signature || !signature->prepared || signature->variadic ||
        unsupportedEngineType(signature->returnType)) {
      throw JSError(
          runtime, "Native function signature is not supported by backend: " +
                       prepared->symbol.name);
    }

    prepared->function = fnptr;
    prepared->signature = std::move(*signature);
    prepared->preparedInvoker = lookupCFunctionPreparedInvoker(
        dispatchIdForEngineSignature(prepared->signature,
                                     SignatureCallKind::CFunction));
    prepared->initialized = true;
  }

  NativeApiSignature& signature = prepared->signature;
  Value fastResult;
  if (tryCallFastEngineCFunction(runtime, bridge, prepared->function, signature,
                                 args, count, &fastResult)) {
    return fastResult;
  }

  NativeApiArgumentFrame frame(signature.argumentTypes.size());
  prepareEngineArguments(runtime, bridge, signature, args, count, frame);

  if (prepared->symbol.name == "NSApplicationMain" ||
      prepared->symbol.name == "UIApplicationMain") {
    runtime.drainMicrotasks();
  }

  NativeApiReturnStorage returnStorage(
      nativeSizeForType(signature.returnType));
  performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
    if (prepared->preparedInvoker != nullptr) {
      prepared->preparedInvoker(prepared->function, frame.values(),
                                returnStorage.data());
    } else {
      ffi_call(&signature.cif, FFI_FN(prepared->function), returnStorage.data(),
               frame.values());
    }
  });

  NativeApiType returnType = signature.returnType;
  if (prepared->symbol.name == "CFBagContainsValue" &&
      (returnType.kind == metagen::mdTypeChar ||
       returnType.kind == metagen::mdTypeUChar ||
       returnType.kind == metagen::mdTypeUInt8)) {
    return *returnStorage.bytes() != 0;
  }
  return convertNativeReturnValue(runtime, bridge, returnType,
                                  returnStorage.data());
}

Value callCFunction(Runtime& runtime,
                    const std::shared_ptr<NativeApiBridge>& bridge,
                    const NativeApiSymbol& symbol, const Value* args,
                    size_t count) {
  auto prepared = std::make_shared<NativeApiPreparedCFunctionInvocation>();
  prepared->symbol = symbol;
  return callCFunction(runtime, bridge, prepared, args, count);
}

bool signatureSupportedForEngineInvocation(
    const std::optional<NativeApiSignature>& signature) {
  if (!signature || !signature->prepared || signature->variadic ||
      unsupportedEngineType(signature->returnType)) {
    return false;
  }
  for (const auto& argType : signature->argumentTypes) {
    if (unsupportedEngineType(argType)) {
      return false;
    }
  }
  return true;
}

bool signatureSupportedForEngineInvocation(
    const NativeApiSignature& signature) {
  if (!signature.prepared || signature.variadic ||
      unsupportedEngineType(signature.returnType)) {
    return false;
  }
  for (const auto& argType : signature.argumentTypes) {
    if (unsupportedEngineType(argType)) {
      return false;
    }
  }
  return true;
}

struct NativeApiPreparedObjCInvocation {
  SEL selector = nullptr;
  Class receiverClass = Nil;
  std::string selectorName;
  NativeApiSignature signature;
  ObjCPreparedInvoker preparedInvoker = nullptr;
  void* engineInvoker = nullptr;  // Engine-neutral GSD invoker (ObjCGsdInvoker)
  bool isNSErrorOutMethod = false;  // Cached: avoids per-call selector scan.
  bool isInitMethod = false;        // Cached: avoids per-call "init" rfind.
  bool isStaticAppearanceSelector = false;
  bool hasAppearanceSideEffects = false;
  bool gsdEngineCallable = false;
  uint8_t gsdEngineArgumentCount = 0;
  bool fastEngineCallable = false;
  uint8_t fastEngineArgumentCount = 0;
  uint8_t fastEngineFirstArgKind = 0;
  uint8_t fastEngineSecondArgKind = 0;
  // Cold UIAppearance metadata stays after the hot signature/invoker fields
  // so ordinary prepared dispatch retains its compact cache layout.
  std::string propertySetterName;
};

bool preparedObjCInvocationIsInit(
    const NativeApiPreparedObjCInvocation& prepared) {
  return prepared.isInitMethod;
}

void cachePreparedAppearanceProxySetterValue(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const NativeApiPreparedObjCInvocation& prepared,
    const Value* args, size_t count) {
  if (prepared.propertySetterName.empty() || args == nullptr || count == 0) {
    return;
  }
  cacheAppearanceProxyPropertyValue(runtime, bridge, receiver,
                                    prepared.propertySetterName, args[0]);
}

bool isFastEngineObjectType(const NativeApiType& type) {
  switch (type.kind) {
    case metagen::mdTypeAnyObject:
    case metagen::mdTypeProtocolObject:
    case metagen::mdTypeClassObject:
    case metagen::mdTypeInstanceObject:
    case metagen::mdTypeNSStringObject:
    case metagen::mdTypeNSMutableStringObject:
      return true;
    default:
      return false;
  }
}

bool isFastEngineSignedIntegerType(const NativeApiType& type) {
  switch (type.kind) {
    case metagen::mdTypeChar:
    case metagen::mdTypeSShort:
    case metagen::mdTypeSInt:
    case metagen::mdTypeSLong:
    case metagen::mdTypeSInt64:
      return true;
    default:
      return false;
  }
}

bool isFastEngineUnsignedIntegerType(const NativeApiType& type) {
  switch (type.kind) {
    case metagen::mdTypeUChar:
    case metagen::mdTypeUInt8:
    case metagen::mdTypeUInt:
    case metagen::mdTypeULong:
    case metagen::mdTypeUInt64:
      return true;
    default:
      return false;
  }
}

enum class NativeApiFastEngineArgKind : uint8_t {
  Bool,
  SignedInteger,
  UnsignedInteger,
  Float,
  Double,
  Object,
  Class,
  Selector,
};

std::optional<NativeApiFastEngineArgKind> fastEngineArgKind(
    const NativeApiType& type) {
  if (isFastEngineObjectType(type)) {
    return NativeApiFastEngineArgKind::Object;
  }
  if (isFastEngineSignedIntegerType(type)) {
    return NativeApiFastEngineArgKind::SignedInteger;
  }
  if (isFastEngineUnsignedIntegerType(type)) {
    return NativeApiFastEngineArgKind::UnsignedInteger;
  }
  switch (type.kind) {
    case metagen::mdTypeBool:
      return NativeApiFastEngineArgKind::Bool;
    case metagen::mdTypeFloat:
      return NativeApiFastEngineArgKind::Float;
    case metagen::mdTypeDouble:
      return NativeApiFastEngineArgKind::Double;
    case metagen::mdTypeClass:
      return NativeApiFastEngineArgKind::Class;
    case metagen::mdTypeSelector:
      return NativeApiFastEngineArgKind::Selector;
    default:
      return std::nullopt;
  }
}

bool readFastEngineBoolArgument(Runtime& runtime, const Value& value,
                                BOOL* result) {
  if (result == nullptr || !value.isBool()) {
    return false;
  }
  *result = value.getBool() ? YES : NO;
  return true;
}

bool readFastEngineSignedIntegerArgument(Runtime& runtime, const Value& value,
                                         NSInteger* result) {
  if (result == nullptr) {
    return false;
  }
  if (value.isNumber()) {
    *result = static_cast<NSInteger>(value.getNumber());
    return true;
  }
  return false;
}

bool readFastEngineUnsignedIntegerArgument(Runtime& runtime, const Value& value,
                                           NSUInteger* result) {
  if (result == nullptr) {
    return false;
  }
  if (value.isNumber()) {
    *result = static_cast<NSUInteger>(value.getNumber());
    return true;
  }
  return false;
}

bool readFastEngineFloatArgument(Runtime&, const Value& value, float* result) {
  if (result == nullptr || !value.isNumber()) {
    return false;
  }
  *result = static_cast<float>(value.getNumber());
  return true;
}

bool readFastEngineDoubleArgument(Runtime&, const Value& value, double* result) {
  if (result == nullptr || !value.isNumber()) {
    return false;
  }
  *result = value.getNumber();
  return true;
}

bool readFastEngineObjectArgument(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, const Value& value,
    NativeApiArgumentFrame& frame, id* result) {
  if (result == nullptr) {
    return false;
  }
  *result = objectFromEngineValue(
      runtime, bridge, value, frame,
      type.kind == metagen::mdTypeNSMutableStringObject);
  if (valueIsNativeObjectHostObject(runtime, value)) {
    frame.retainObject(*result);
  }
  return true;
}

class NativeApiScopedObjCObjectRetain {
 public:
  explicit NativeApiScopedObjCObjectRetain(id object) : object_(object) {
    if (object_ != nil) {
      [object_ retain];
    }
  }

  ~NativeApiScopedObjCObjectRetain() {
    if (object_ != nil) {
      [object_ release];
    }
  }

 private:
  id object_ = nil;
};

bool readFastEngineClassArgument(Runtime& runtime, const Value& value,
                                 Class* result) {
  if (result == nullptr) {
    return false;
  }
  *result = classFromEngineValue(runtime, value);
  return *result != Nil;
}

bool readFastEngineSelectorArgument(Runtime& runtime, const Value& value,
                                    SEL* result) {
  if (result == nullptr) {
    return false;
  }
  if (value.isNull() || value.isUndefined()) {
    *result = nullptr;
    return true;
  }
  if (!value.isString()) {
    return false;
  }
  std::string selectorName = value.asString(runtime).utf8(runtime);
  *result = sel_registerName(selectorName.c_str());
  return true;
}

template <typename... Args>
Value callFastEngineCFunctionWithReturn(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    void* function, NativeApiType returnType, Args... nativeArgs) {
  auto finalizeObjectReturn = [&](id object) -> Value {
    return convertNativeReturnValue(runtime, bridge, returnType, &object);
  };

  switch (returnType.kind) {
    case metagen::mdTypeVoid: {
      performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
        using Fn = void (*)(Args...);
        reinterpret_cast<Fn>(function)(nativeArgs...);
      });
      return Value::undefined();
    }
    case metagen::mdTypeBool: {
      BOOL nativeResult = NO;
      performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
        using Fn = BOOL (*)(Args...);
        nativeResult = reinterpret_cast<Fn>(function)(nativeArgs...);
      });
      uint8_t storage = nativeResult ? 1 : 0;
      return convertNativeReturnValue(runtime, bridge, returnType, &storage);
    }
    case metagen::mdTypeFloat: {
      float nativeResult = 0;
      performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
        using Fn = float (*)(Args...);
        nativeResult = reinterpret_cast<Fn>(function)(nativeArgs...);
      });
      return convertNativeReturnValue(runtime, bridge, returnType,
                                      &nativeResult);
    }
    case metagen::mdTypeDouble: {
      double nativeResult = 0;
      performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
        using Fn = double (*)(Args...);
        nativeResult = reinterpret_cast<Fn>(function)(nativeArgs...);
      });
      return convertNativeReturnValue(runtime, bridge, returnType,
                                      &nativeResult);
    }
    default:
      break;
  }

  if (isFastEngineObjectType(returnType) ||
      returnType.kind == metagen::mdTypeClass) {
    id nativeResult = nil;
    performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
      using Fn = id (*)(Args...);
      nativeResult = reinterpret_cast<Fn>(function)(nativeArgs...);
    });
    return finalizeObjectReturn(nativeResult);
  }

  if (returnType.kind == metagen::mdTypeSelector) {
    SEL nativeResult = nullptr;
    performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
      using Fn = SEL (*)(Args...);
      nativeResult = reinterpret_cast<Fn>(function)(nativeArgs...);
    });
    return convertNativeReturnValue(runtime, bridge, returnType,
                                    &nativeResult);
  }

  if (isFastEngineSignedIntegerType(returnType)) {
    int64_t nativeResult = 0;
    performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
      using Fn = int64_t (*)(Args...);
      nativeResult = reinterpret_cast<Fn>(function)(nativeArgs...);
    });
    return convertNativeReturnValue(runtime, bridge, returnType,
                                    &nativeResult);
  }

  if (isFastEngineUnsignedIntegerType(returnType)) {
    uint64_t nativeResult = 0;
    performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
      using Fn = uint64_t (*)(Args...);
      nativeResult = reinterpret_cast<Fn>(function)(nativeArgs...);
    });
    return convertNativeReturnValue(runtime, bridge, returnType,
                                    &nativeResult);
  }

  throw JSError(runtime, "C function return type is not engine fast-callable.");
}

bool isFastEngineCallableReturnType(const NativeApiType& returnType) {
  return isFastEngineObjectType(returnType) ||
         returnType.kind == metagen::mdTypeVoid ||
         returnType.kind == metagen::mdTypeBool ||
         returnType.kind == metagen::mdTypeFloat ||
         returnType.kind == metagen::mdTypeDouble ||
         returnType.kind == metagen::mdTypeClass ||
         returnType.kind == metagen::mdTypeSelector ||
         isFastEngineSignedIntegerType(returnType) ||
         isFastEngineUnsignedIntegerType(returnType);
}

bool tryCallFastEngineCFunction(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    void* function, const NativeApiSignature& signature, const Value* args,
    size_t count, Value* result) {
  if (result == nullptr || function == nullptr || signature.variadic ||
      count != signature.argumentTypes.size() || count > 2 ||
      unsupportedEngineType(signature.returnType) ||
      !isFastEngineCallableReturnType(signature.returnType)) {
    return false;
  }

  std::optional<NativeApiFastEngineArgKind> firstArgKind;
  std::optional<NativeApiFastEngineArgKind> secondArgKind;
  if (count > 0) {
    firstArgKind = fastEngineArgKind(signature.argumentTypes[0]);
    if (!firstArgKind) {
      return false;
    }
  }
  if (count > 1) {
    secondArgKind = fastEngineArgKind(signature.argumentTypes[1]);
    if (!secondArgKind) {
      return false;
    }
  }

  if (count == 0) {
    *result = callFastEngineCFunctionWithReturn(
        runtime, bridge, function, signature.returnType);
    return true;
  }

  NativeApiArgumentFrame frame(count);
  auto callOne = [&](auto nativeArg0) -> Value {
    return callFastEngineCFunctionWithReturn(runtime, bridge, function,
                                             signature.returnType, nativeArg0);
  };
  auto callTwo = [&](auto nativeArg0, auto nativeArg1) -> Value {
    return callFastEngineCFunctionWithReturn(runtime, bridge, function,
                                             signature.returnType, nativeArg0,
                                             nativeArg1);
  };

  auto callWithSecondArg = [&](auto nativeArg0) -> bool {
    switch (*secondArgKind) {
      case NativeApiFastEngineArgKind::Bool: {
        BOOL arg1 = NO;
        if (!readFastEngineBoolArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::SignedInteger: {
        NSInteger arg1 = 0;
        if (!readFastEngineSignedIntegerArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::UnsignedInteger: {
        NSUInteger arg1 = 0;
        if (!readFastEngineUnsignedIntegerArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Float: {
        float arg1 = 0;
        if (!readFastEngineFloatArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Double: {
        double arg1 = 0;
        if (!readFastEngineDoubleArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Object: {
        id arg1 = nil;
        if (!readFastEngineObjectArgument(
                runtime, bridge, signature.argumentTypes[1], args[1], frame,
                &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Class: {
        Class arg1 = Nil;
        if (!readFastEngineClassArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Selector: {
        SEL arg1 = nullptr;
        if (!readFastEngineSelectorArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
    }
    return false;
  };

  switch (*firstArgKind) {
    case NativeApiFastEngineArgKind::Bool: {
      BOOL arg0 = NO;
      if (!readFastEngineBoolArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::SignedInteger: {
      NSInteger arg0 = 0;
      if (!readFastEngineSignedIntegerArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::UnsignedInteger: {
      NSUInteger arg0 = 0;
      if (!readFastEngineUnsignedIntegerArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Float: {
      float arg0 = 0;
      if (!readFastEngineFloatArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Double: {
      double arg0 = 0;
      if (!readFastEngineDoubleArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Object: {
      id arg0 = nil;
      if (!readFastEngineObjectArgument(runtime, bridge,
                                        signature.argumentTypes[0], args[0],
                                        frame, &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Class: {
      Class arg0 = Nil;
      if (!readFastEngineClassArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Selector: {
      SEL arg0 = nullptr;
      if (!readFastEngineSelectorArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
  }

  return false;
}

template <typename... Args>
Value callFastEngineObjCWithReturn(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, SEL selector, NativeApiType returnType,
    const std::string& selectorName, Args... nativeArgs) {
  auto finalizeObjectReturn = [&](id object) -> Value {
    NativeApiType effectiveReturnType = returnType;
    if ((selectorName == "valueForKey:" || selectorName == "valueForKeyPath:") &&
        isObjectiveCObjectType(effectiveReturnType)) {
      effectiveReturnType.kind = metagen::mdTypeAnyObject;
    }
    if (startsWith(selectorName, "init") &&
        isObjectiveCObjectType(effectiveReturnType)) {
      effectiveReturnType.kind = metagen::mdTypeInstanceObject;
    }
    return convertNativeReturnValue(runtime, bridge, effectiveReturnType,
                                    &object);
  };

  switch (returnType.kind) {
    case metagen::mdTypeVoid: {
      performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
        using Fn = void (*)(id, SEL, Args...);
        reinterpret_cast<Fn>(objc_msgSend)(receiver, selector, nativeArgs...);
      });
      return Value::undefined();
    }
    case metagen::mdTypeBool: {
      BOOL nativeResult = NO;
      performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
        using Fn = BOOL (*)(id, SEL, Args...);
        nativeResult =
            reinterpret_cast<Fn>(objc_msgSend)(receiver, selector, nativeArgs...);
      });
      uint8_t storage = nativeResult ? 1 : 0;
      return convertNativeReturnValue(runtime, bridge, returnType, &storage);
    }
    case metagen::mdTypeFloat: {
      float nativeResult = 0;
      performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
        using Fn = float (*)(id, SEL, Args...);
        nativeResult =
            reinterpret_cast<Fn>(objc_msgSend)(receiver, selector, nativeArgs...);
      });
      return convertNativeReturnValue(runtime, bridge, returnType,
                                      &nativeResult);
    }
    case metagen::mdTypeDouble: {
      double nativeResult = 0;
      performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
        using Fn = double (*)(id, SEL, Args...);
        nativeResult =
            reinterpret_cast<Fn>(objc_msgSend)(receiver, selector, nativeArgs...);
      });
      return convertNativeReturnValue(runtime, bridge, returnType,
                                      &nativeResult);
    }
    default:
      break;
  }

  if (isFastEngineObjectType(returnType) || returnType.kind == metagen::mdTypeClass) {
    id nativeResult = nil;
    performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
      using Fn = id (*)(id, SEL, Args...);
      nativeResult =
          reinterpret_cast<Fn>(objc_msgSend)(receiver, selector, nativeArgs...);
    });
    return finalizeObjectReturn(nativeResult);
  }

  if (isFastEngineSignedIntegerType(returnType)) {
    int64_t nativeResult = 0;
    performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
      using Fn = int64_t (*)(id, SEL, Args...);
      nativeResult =
          reinterpret_cast<Fn>(objc_msgSend)(receiver, selector, nativeArgs...);
    });
    return convertNativeReturnValue(runtime, bridge, returnType,
                                    &nativeResult);
  }

  if (isFastEngineUnsignedIntegerType(returnType)) {
    uint64_t nativeResult = 0;
    performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
      using Fn = uint64_t (*)(id, SEL, Args...);
      nativeResult =
          reinterpret_cast<Fn>(objc_msgSend)(receiver, selector, nativeArgs...);
    });
    return convertNativeReturnValue(runtime, bridge, returnType,
                                    &nativeResult);
  }

  throw JSError(runtime, "Objective-C return type is not engine fast-callable.");
}

template <typename A0>
Value callFastEngineObjC1(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, SEL selector, const NativeApiType& returnType,
    const std::string& selectorName, A0 arg0) {
  return callFastEngineObjCWithReturn(runtime, bridge, receiver, selector,
                                      returnType, selectorName, arg0);
}

template <typename A0, typename A1>
Value callFastEngineObjC2(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, SEL selector, const NativeApiType& returnType,
    const std::string& selectorName, A0 arg0, A1 arg1) {
  return callFastEngineObjCWithReturn(runtime, bridge, receiver, selector,
                                      returnType, selectorName, arg0, arg1);
}

bool tryCallFastEngineObjCSelector(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const NativeApiPreparedObjCInvocation& prepared,
    const Value* args, size_t count, Class dispatchSuperClass, Value* result) {
  if (result == nullptr || receiver == nil || dispatchSuperClass != Nil) {
    return false;
  }

  const NativeApiSignature& signature = prepared.signature;
  if (!prepared.fastEngineCallable ||
      count != prepared.fastEngineArgumentCount) {
    return false;
  }

  NativeApiFastEngineArgKind firstArgKind =
      static_cast<NativeApiFastEngineArgKind>(prepared.fastEngineFirstArgKind);
  NativeApiFastEngineArgKind secondArgKind =
      static_cast<NativeApiFastEngineArgKind>(prepared.fastEngineSecondArgKind);

  SEL selector = prepared.selector;
  if (count == 0) {
    *result = callFastEngineObjCWithReturn(
        runtime, bridge, receiver, selector, signature.returnType,
        prepared.selectorName);
    return true;
  }

  NativeApiArgumentFrame frame(count);
  auto callOne = [&](auto nativeArg0) -> Value {
    return callFastEngineObjC1(runtime, bridge, receiver, selector,
                               signature.returnType, prepared.selectorName,
                               nativeArg0);
  };
  auto callTwo = [&](auto nativeArg0, auto nativeArg1) -> Value {
    return callFastEngineObjC2(runtime, bridge, receiver, selector,
                               signature.returnType, prepared.selectorName,
                               nativeArg0, nativeArg1);
  };
  auto callWithSecondArg = [&](auto nativeArg0) -> bool {
    switch (secondArgKind) {
      case NativeApiFastEngineArgKind::Bool: {
        BOOL arg1 = NO;
        if (!readFastEngineBoolArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::SignedInteger: {
        NSInteger arg1 = 0;
        if (!readFastEngineSignedIntegerArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::UnsignedInteger: {
        NSUInteger arg1 = 0;
        if (!readFastEngineUnsignedIntegerArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Float: {
        float arg1 = 0;
        if (!readFastEngineFloatArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Double: {
        double arg1 = 0;
        if (!readFastEngineDoubleArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Object: {
        id arg1 = nil;
        if (!readFastEngineObjectArgument(
                runtime, bridge, signature.argumentTypes[1], args[1], frame,
                &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Class: {
        Class arg1 = Nil;
        if (!readFastEngineClassArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
      case NativeApiFastEngineArgKind::Selector: {
        SEL arg1 = nullptr;
        if (!readFastEngineSelectorArgument(runtime, args[1], &arg1)) {
          return false;
        }
        *result = callTwo(nativeArg0, arg1);
        return true;
      }
    }
    return false;
  };

  switch (firstArgKind) {
    case NativeApiFastEngineArgKind::Bool: {
      BOOL arg0 = NO;
      if (!readFastEngineBoolArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::SignedInteger: {
      NSInteger arg0 = 0;
      if (!readFastEngineSignedIntegerArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::UnsignedInteger: {
      NSUInteger arg0 = 0;
      if (!readFastEngineUnsignedIntegerArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Float: {
      float arg0 = 0;
      if (!readFastEngineFloatArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Double: {
      double arg0 = 0;
      if (!readFastEngineDoubleArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Object: {
      id arg0 = nil;
      if (!readFastEngineObjectArgument(runtime, bridge, signature.argumentTypes[0],
                                        args[0], frame, &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Class: {
      Class arg0 = Nil;
      if (!readFastEngineClassArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
    case NativeApiFastEngineArgKind::Selector: {
      SEL arg0 = nullptr;
      if (!readFastEngineSelectorArgument(runtime, args[0], &arg0)) {
        return false;
      }
      if (count == 1) {
        *result = callOne(arg0);
        return true;
      }
      return callWithSecondArg(arg0);
    }
  }

  return false;
}

bool isFastEngineObjCReturnType(const NativeApiType& returnType) {
  return !unsupportedEngineType(returnType) &&
         (isFastEngineObjectType(returnType) ||
          returnType.kind == metagen::mdTypeVoid ||
          returnType.kind == metagen::mdTypeBool ||
          returnType.kind == metagen::mdTypeFloat ||
          returnType.kind == metagen::mdTypeDouble ||
          returnType.kind == metagen::mdTypeClass ||
          isFastEngineSignedIntegerType(returnType) ||
          isFastEngineUnsignedIntegerType(returnType));
}

void configureFastEngineObjCInvocation(
    NativeApiPreparedObjCInvocation& prepared) {
  prepared.fastEngineCallable = false;
  prepared.fastEngineArgumentCount = 0;
  prepared.fastEngineFirstArgKind = 0;
  prepared.fastEngineSecondArgKind = 0;

  const NativeApiSignature& signature = prepared.signature;
  if (signature.variadic || prepared.isNSErrorOutMethod ||
      signature.argumentTypes.size() > 2 ||
      !isFastEngineObjCReturnType(signature.returnType)) {
    return;
  }

  if (!signature.argumentTypes.empty()) {
    std::optional<NativeApiFastEngineArgKind> firstArgKind =
        fastEngineArgKind(signature.argumentTypes[0]);
    if (!firstArgKind) {
      return;
    }
    prepared.fastEngineFirstArgKind = static_cast<uint8_t>(*firstArgKind);
  }
  if (signature.argumentTypes.size() > 1) {
    std::optional<NativeApiFastEngineArgKind> secondArgKind =
        fastEngineArgKind(signature.argumentTypes[1]);
    if (!secondArgKind) {
      return;
    }
    prepared.fastEngineSecondArgKind = static_cast<uint8_t>(*secondArgKind);
  }

  prepared.fastEngineArgumentCount =
      static_cast<uint8_t>(signature.argumentTypes.size());
  prepared.fastEngineCallable = true;
}

void configureGeneratedEngineObjCInvocation(
    NativeApiPreparedObjCInvocation& prepared) {
  prepared.gsdEngineCallable = false;
  prepared.gsdEngineArgumentCount = 0;

  const NativeApiSignature& signature = prepared.signature;
  if (prepared.engineInvoker == nullptr || signature.variadic ||
      prepared.isNSErrorOutMethod || signature.argumentTypes.size() > 255) {
    return;
  }

  prepared.gsdEngineArgumentCount =
      static_cast<uint8_t>(signature.argumentTypes.size());
  prepared.gsdEngineCallable = true;
}

std::shared_ptr<NativeApiPreparedObjCInvocation>
prepareNativeApiObjCInvocation(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    Class lookupClass, bool receiverIsClass, const std::string& selectorName,
    const NativeApiMember* member) {
  if (lookupClass == Nil) {
    throw JSError(runtime,
                  "Objective-C class is not available for selector: " +
                      selectorName);
  }

  SEL selector = sel_registerName(selectorName.c_str());
  Method method = receiverIsClass ? class_getClassMethod(lookupClass, selector)
                                  : class_getInstanceMethod(lookupClass, selector);
  if (method == nullptr) {
    throw JSError(runtime,
                  "Objective-C selector is not available: " + selectorName);
  }

  std::optional<NativeApiSignature> signature;
  std::optional<NativeApiSignature> runtimeSignature;
  if (member != nullptr &&
      member->signatureOffset != MD_SECTION_OFFSET_NULL &&
      member->signatureOffset != 0) {
    signature = parseMetadataEngineSignature(
        bridge->metadata(), member->signatureOffset, 2, bridge.get(),
        (member->flags & metagen::mdMemberReturnOwned) != 0);
  }
  if (method != nullptr) {
    runtimeSignature = parseObjCMethodEngineSignature(method, bridge.get());
  }
  if (signatureSupportedForEngineInvocation(signature) &&
      signatureSupportedForEngineInvocation(runtimeSignature)) {
    reconcileObjCMethodRuntimeSignature(&*signature, *runtimeSignature);
  }
  if (!signatureSupportedForEngineInvocation(signature) && runtimeSignature) {
    signature = std::move(runtimeSignature);
  }

  if (!signatureSupportedForEngineInvocation(signature)) {
    throw JSError(
        runtime, "Objective-C signature is not supported by backend: " +
                     selectorName);
  }
  signature->selectorName = selectorName;

  auto prepared = std::make_shared<NativeApiPreparedObjCInvocation>();
  prepared->selector = selector;
  prepared->receiverClass = receiverIsClass ? lookupClass : Nil;
  prepared->selectorName = selectorName;
  if (member != nullptr && member->property && !member->name.empty() &&
      !member->setterSelectorName.empty() &&
      selectorName == member->setterSelectorName &&
      selectorArgumentCount(selectorName) == 1) {
    prepared->propertySetterName = member->name;
  }
  prepared->signature = std::move(*signature);
  prepared->preparedInvoker = lookupObjCPreparedInvoker(
      dispatchIdForEngineSignature(prepared->signature,
                                   SignatureCallKind::ObjCMethod));
  prepared->engineInvoker = lookupGeneratedEngineObjCGsdInvoker(
      dispatchIdForEngineSignature(prepared->signature,
                                   SignatureCallKind::ObjCMethod));
  prepared->isNSErrorOutMethod =
      isNSErrorOutEngineMethodSignature(prepared->signature);
  prepared->isInitMethod = prepared->selectorName.rfind("init", 0) == 0;
  prepared->isStaticAppearanceSelector =
      prepared->receiverClass != Nil &&
      prepared->selectorName.rfind("appearance", 0) == 0;
  prepared->hasAppearanceSideEffects =
      prepared->isStaticAppearanceSelector ||
      !prepared->propertySetterName.empty();
  configureGeneratedEngineObjCInvocation(*prepared);
  configureFastEngineObjCInvocation(*prepared);
  return prepared;
}

bool isPreparedStaticAppearanceSelector(
    const NativeApiPreparedObjCInvocation& prepared) {
  return prepared.isStaticAppearanceSelector;
}

bool preparedInvocationHasAppearanceSideEffects(
    const NativeApiPreparedObjCInvocation& prepared) {
  return prepared.hasAppearanceSideEffects;
}

Value tagPreparedStaticAppearanceSelectorResult(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const NativeApiPreparedObjCInvocation& prepared,
    Value result) {
  return tagStaticAppearanceSelectorResult(
      runtime, bridge, receiver, isPreparedStaticAppearanceSelector(prepared),
      prepared.selectorName, std::move(result));
}

void tagPreparedStaticAppearanceNativeReturn(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const NativeApiPreparedObjCInvocation& prepared,
    const NativeApiType& returnType, void* returnData) {
  if (!isPreparedStaticAppearanceSelector(prepared) ||
      !isObjectiveCObjectType(returnType) || returnData == nullptr) {
    return;
  }
  tagStaticAppearanceNativeResult(
      runtime, bridge, static_cast<Class>(receiver),
      *static_cast<id*>(returnData));
}

Value callPreparedObjCSelector(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, bool receiverIsClass,
    const NativeApiPreparedObjCInvocation& prepared, const Value* args,
    size_t count, Class dispatchSuperClass) {
  if (receiver == nil) {
    throw JSError(runtime,
                  "Cannot send Objective-C selector to nil.");
  }
  NativeApiRoundTripCacheFrameGuard roundTripFrame(bridge);

  const NativeApiSignature& signature = prepared.signature;
  Value fastResult;
  if (tryCallGeneratedEngineObjCSelector(runtime, bridge, receiver, prepared,
                                         args, count, dispatchSuperClass,
                                         &fastResult)) {
    if (!preparedInvocationHasAppearanceSideEffects(prepared)) {
      return fastResult;
    }
    cachePreparedAppearanceProxySetterValue(runtime, bridge, receiver,
                                            prepared, args, count);
    return tagPreparedStaticAppearanceSelectorResult(
        runtime, bridge, receiver, prepared, std::move(fastResult));
  }
  if (tryCallFastEngineObjCSelector(runtime, bridge, receiver, prepared, args,
                                    count, dispatchSuperClass, &fastResult)) {
    if (!preparedInvocationHasAppearanceSideEffects(prepared)) {
      return fastResult;
    }
    cachePreparedAppearanceProxySetterValue(runtime, bridge, receiver,
                                            prepared, args, count);
    return tagPreparedStaticAppearanceSelectorResult(
        runtime, bridge, receiver, prepared, std::move(fastResult));
  }

  NativeApiArgumentFrame frame(signature.argumentTypes.size());
  frame.retainObject(receiver);
  const bool isNSErrorOutMethod = prepared.isNSErrorOutMethod;
  if (isNSErrorOutMethod) {
    size_t expected = signature.argumentTypes.size();
    if (count > expected || count + 1 < expected) {
      throw JSError(
          runtime, "Actual arguments count: \"" + std::to_string(count) +
                       "\". Expected: \"" + std::to_string(expected) + "\".");
    }
  }

  const bool hasImplicitNSErrorOutArg =
      isNSErrorOutMethod && count + 1 == signature.argumentTypes.size();
  NSError* implicitNSError = nil;
  if (hasImplicitNSErrorOutArg) {
    for (size_t i = 0; i < count; i++) {
      prepareEngineArgument(runtime, bridge, signature.argumentTypes[i], args[i],
                            i, frame);
    }

    size_t outArgIndex = signature.argumentTypes.size() - 1;
    void* target = frame.storageAt(outArgIndex, sizeof(NSError**));
    NSError** implicitNSErrorOutArg = &implicitNSError;
    *static_cast<void**>(target) = implicitNSErrorOutArg;
  } else {
    prepareEngineArguments(runtime, bridge, signature, args, count, frame);
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
  if ((prepared.selectorName == "valueForKey:" ||
       prepared.selectorName == "valueForKeyPath:") &&
      isObjectiveCObjectType(returnType)) {
    returnType.kind = metagen::mdTypeAnyObject;
  }
  if (prepared.isInitMethod &&
      isObjectiveCObjectType(returnType)) {
    returnType.kind = metagen::mdTypeInstanceObject;
  }
  if (hasImplicitNSErrorOutArg && implicitNSError != nil) {
    const char* errorMessage = [[implicitNSError description] UTF8String];
    throw JSError(
        runtime, errorMessage != nullptr ? errorMessage : "Unknown NSError");
  }
  Value result = convertNativeReturnValue(runtime, bridge, returnType,
                                          returnStorage.data());
  if (!preparedInvocationHasAppearanceSideEffects(prepared)) {
    return result;
  }
  cachePreparedAppearanceProxySetterValue(runtime, bridge, receiver, prepared,
                                          args, count);
  return tagPreparedStaticAppearanceSelectorResult(
      runtime, bridge, receiver, prepared, std::move(result));
}

Value callObjCSelector(Runtime& runtime,
                       const std::shared_ptr<NativeApiBridge>& bridge,
                       id receiver, bool receiverIsClass,
                       const std::string& selectorName,
                       const NativeApiMember* member,
                       const Value* args, size_t count,
                       Class dispatchSuperClass) {
  if (receiver == nil) {
    throw JSError(runtime,
                                 "Cannot send Objective-C selector to nil.");
  }
  NativeApiRoundTripCacheFrameGuard roundTripFrame(bridge);

  SEL selector = sel_registerName(selectorName.c_str());
  Class receiverClass =
      receiverIsClass ? static_cast<Class>(receiver) : object_getClass(receiver);
  Class lookupClass = dispatchSuperClass != Nil ? dispatchSuperClass : receiverClass;
  Method method = receiverIsClass ? class_getClassMethod(lookupClass, selector)
                                  : class_getInstanceMethod(lookupClass, selector);
  // A UIAppearance proxy is opaque to class_getInstanceMethod (it forwards
  // selectors dynamically, so no Method exists for them) — allow through the
  // exact property getter/setter selectors respondsToSelector: already
  // vetted elsewhere, since -respondsToSelector: on the proxy itself can
  // still return NO for a selector it will happily forward.
  bool allowForwardedAppearancePropertySelector = false;
  if (method == nullptr && !receiverIsClass && member != nullptr &&
      member->property && bridge != nullptr &&
      taggedAppearanceProxyClass(runtime, bridge, receiver) != Nil) {
    allowForwardedAppearancePropertySelector =
        (count == 0 && selectorName == member->selectorName) ||
        (count == 1 && !member->setterSelectorName.empty() &&
         selectorName == member->setterSelectorName);
  }
  if (method == nullptr &&
      !allowForwardedAppearancePropertySelector &&
      (dispatchSuperClass != Nil || ![receiver respondsToSelector:selector])) {
    throw JSError(runtime,
                                 "Objective-C selector is not available: " +
                                     selectorName);
  }

  std::optional<NativeApiSignature> signature;
  std::optional<NativeApiSignature> runtimeSignature;
  if (member != nullptr &&
      member->signatureOffset != MD_SECTION_OFFSET_NULL &&
      member->signatureOffset != 0) {
    signature = parseMetadataEngineSignature(
        bridge->metadata(), member->signatureOffset, 2, bridge.get(),
        (member->flags & metagen::mdMemberReturnOwned) != 0);
  }
  if (method != nullptr) {
    runtimeSignature = parseObjCMethodEngineSignature(method, bridge.get());
  }
  if (signatureSupportedForEngineInvocation(signature) &&
      signatureSupportedForEngineInvocation(runtimeSignature)) {
    reconcileObjCMethodRuntimeSignature(&*signature, *runtimeSignature);
  }
  if (!signatureSupportedForEngineInvocation(signature) && runtimeSignature) {
    signature = std::move(runtimeSignature);
  }

  if (!signatureSupportedForEngineInvocation(signature)) {
    throw JSError(
        runtime, "Objective-C signature is not supported by backend: " +
                     selectorName);
  }
  signature->selectorName = selectorName;

  NativeApiPreparedObjCInvocation engineInvocation;
  engineInvocation.selector = selector;
  engineInvocation.selectorName = selectorName;
  engineInvocation.signature = *signature;
  engineInvocation.engineInvoker = lookupGeneratedEngineObjCGsdInvoker(
      dispatchIdForEngineSignature(*signature, SignatureCallKind::ObjCMethod));
  engineInvocation.isNSErrorOutMethod =
      isNSErrorOutEngineMethodSignature(*signature);
  engineInvocation.isInitMethod = selectorName.rfind("init", 0) == 0;
  configureGeneratedEngineObjCInvocation(engineInvocation);
  configureFastEngineObjCInvocation(engineInvocation);
  Value fastResult;
  if (tryCallGeneratedEngineObjCSelector(runtime, bridge, receiver,
                                         engineInvocation, args, count,
                                         dispatchSuperClass, &fastResult)) {
    return tagStaticAppearanceSelectorResult(
        runtime, bridge, receiver, receiverIsClass, selectorName,
        std::move(fastResult));
  }
  if (tryCallFastEngineObjCSelector(runtime, bridge, receiver,
                                    engineInvocation, args, count,
                                    dispatchSuperClass, &fastResult)) {
    return tagStaticAppearanceSelectorResult(
        runtime, bridge, receiver, receiverIsClass, selectorName,
        std::move(fastResult));
  }

  NativeApiArgumentFrame frame(signature->argumentTypes.size());
  const bool isNSErrorOutMethod = engineInvocation.isNSErrorOutMethod;
  if (isNSErrorOutMethod) {
    size_t expected = signature->argumentTypes.size();
    if (count > expected || count + 1 < expected) {
      throw JSError(
          runtime, "Actual arguments count: \"" + std::to_string(count) +
                       "\". Expected: \"" + std::to_string(expected) + "\".");
    }
  }

  const bool hasImplicitNSErrorOutArg =
      isNSErrorOutMethod && count + 1 == signature->argumentTypes.size();
  NSError* implicitNSError = nil;
  if (hasImplicitNSErrorOutArg) {
    for (size_t i = 0; i < count; i++) {
      prepareEngineArgument(runtime, bridge, signature->argumentTypes[i], args[i], i,
                         frame);
    }

    size_t outArgIndex = signature->argumentTypes.size() - 1;
    void* target = frame.storageAt(outArgIndex, sizeof(NSError**));
    NSError** implicitNSErrorOutArg = &implicitNSError;
    *static_cast<void**>(target) = implicitNSErrorOutArg;
  } else {
    prepareEngineArguments(runtime, bridge, *signature, args, count, frame);
  }

  NativeApiPointerFrame values(signature->argumentTypes.size() + 2);
  size_t valueIndex = 0;
  struct objc_super superReceiver = {receiver, dispatchSuperClass};
  struct objc_super* superReceiverPtr = &superReceiver;
  if (dispatchSuperClass != Nil) {
    values.set(valueIndex++, &superReceiverPtr);
  } else {
    values.set(valueIndex++, &receiver);
  }
  values.set(valueIndex++, &selector);
  for (size_t i = 0; i < signature->argumentTypes.size(); i++) {
    values.set(valueIndex++, frame.values()[i]);
  }

  NativeApiReturnStorage returnStorage(
      nativeSizeForType(signature->returnType));
  auto preparedInvoker =
      dispatchSuperClass == Nil
          ? lookupObjCPreparedInvoker(dispatchIdForEngineSignature(
                *signature, SignatureCallKind::ObjCMethod))
          : nullptr;
  performNativeInvocation(runtime, bridge->nativeInvocationInvoker(), [&]() {
    if (preparedInvoker != nullptr) {
      preparedInvoker(reinterpret_cast<void*>(objc_msgSend), values.data(),
                      returnStorage.data());
    } else {
#if defined(__x86_64__)
      bool isStret = signature->returnType.ffiType->size > 16 &&
                     signature->returnType.ffiType->type == FFI_TYPE_STRUCT;
      void (*target)(void) =
          dispatchSuperClass != Nil
              ? (isStret ? FFI_FN(objc_msgSendSuper_stret)
                         : FFI_FN(objc_msgSendSuper))
              : (isStret ? FFI_FN(objc_msgSend_stret) : FFI_FN(objc_msgSend));
      ffi_call(&signature->cif, target, returnStorage.data(), values.data());
#else
      ffi_call(&signature->cif,
               dispatchSuperClass != Nil ? FFI_FN(objc_msgSendSuper)
                                         : FFI_FN(objc_msgSend),
               returnStorage.data(), values.data());
#endif
    }
  });

  NativeApiType returnType = signature->returnType;
  if ((selectorName == "valueForKey:" || selectorName == "valueForKeyPath:") &&
      isObjectiveCObjectType(returnType)) {
    returnType.kind = metagen::mdTypeAnyObject;
  }
  if (engineInvocation.isInitMethod && isObjectiveCObjectType(returnType)) {
    returnType.kind = metagen::mdTypeInstanceObject;
  }
  if (hasImplicitNSErrorOutArg && implicitNSError != nil) {
    const char* errorMessage = [[implicitNSError description] UTF8String];
    throw JSError(
        runtime, errorMessage != nullptr ? errorMessage : "Unknown NSError");
  }
  Value result = convertNativeReturnValue(runtime, bridge, returnType,
                                          returnStorage.data());
  return tagStaticAppearanceSelectorResult(
      runtime, bridge, receiver, receiverIsClass, selectorName,
      std::move(result));
}
