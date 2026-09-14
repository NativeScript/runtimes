bool isObjectiveCObjectType(const NativeApiType& type) {
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

#ifndef NATIVESCRIPT_NATIVE_API_RETAIN_RUNTIME
std::shared_ptr<Runtime> retainNativeApiRuntime(Runtime& runtime) {
  return std::shared_ptr<Runtime>(&runtime, [](Runtime*) {});
}
#endif

#ifndef NATIVESCRIPT_NATIVE_API_RUNTIME_SCOPE
class NativeApiRuntimeScope final {
 public:
  explicit NativeApiRuntimeScope(Runtime&) {}
};
#endif

struct NativeApiSignature {
  ffi_cif cif = {};
  NativeApiType returnType;
  std::vector<NativeApiType> argumentTypes;
  std::vector<ffi_type*> ffiTypes;
  std::string selectorName;
  uint64_t signatureHash = 0;
  uint8_t dispatchFlags = 0;
  bool variadic = false;
  bool prepared = false;
  unsigned int implicitArgumentCount = 0;
};

enum class NativeApiCallbackThreadPolicy {
  Default,
  JS,
  Runtime,
};

// Per-method callback policy, attached to a JS function via
// NativeScriptRuntime.nativeMethodPolicy(fn, policy) (a `__nativeScriptMethodPolicy`
// expando read back in readEngineMethodCallbackPolicy below). Deliberately
// small: the only live consumers are (a) calling the ObjC super
// implementation before the JS override runs, and (b) suppressing a
// re-entrant callback while a native accessor/construction call is already
// in flight (see nativeAccessorCallbackPolicy and
// shouldSkipConstructingMethodCallback).
struct NativeApiMethodCallbackPolicy {
  bool callSuperBeforeCallback = false;
  // Associated-object keys checked on the callback's receiver; if any is
  // truthy, the callback is skipped entirely (zero-returned).
  std::vector<std::string> skipCallbackIfAssociatedObjectTruthy;
};

NativeApiCallbackThreadPolicy readEngineCallbackThreadPolicy(
    Runtime& runtime, Object& functionObject) {
  constexpr const char* propertyName = "__nativeScriptCallbackThread";
  try {
    if (!functionObject.hasProperty(runtime, propertyName)) {
      return NativeApiCallbackThreadPolicy::Default;
    }
    Value policyValue = functionObject.getProperty(runtime, propertyName);
    if (!policyValue.isString()) {
      return NativeApiCallbackThreadPolicy::Default;
    }
    std::string policy = policyValue.asString(runtime).utf8(runtime);
    if (policy == "js") {
      return NativeApiCallbackThreadPolicy::JS;
    }
    if (policy == "runtime" || policy == "worklet") {
      return NativeApiCallbackThreadPolicy::Runtime;
    }
  } catch (const std::exception&) {
  }
  return NativeApiCallbackThreadPolicy::Default;
}

Value optionalObjectProperty(Runtime& runtime, Object& object,
                             const char* name) {
  if (name == nullptr || !object.hasProperty(runtime, name)) {
    return Value::undefined();
  }
  return object.getProperty(runtime, name);
}

void appendEngineMethodPolicyAssociatedObjectKeys(
    Runtime& runtime, const Value& value, std::vector<std::string>& keys) {
  if (value.isString()) {
    keys.push_back(value.asString(runtime).utf8(runtime));
    return;
  }
  if (!value.isObject()) {
    return;
  }
  Object object = value.asObject(runtime);
  if (!object.isArray(runtime)) {
    return;
  }
  Array array = object.getArray(runtime);
  size_t size = array.size(runtime);
  for (size_t i = 0; i < size; i++) {
    Value item = array.getValueAtIndex(runtime, i);
    if (item.isString()) {
      keys.push_back(item.asString(runtime).utf8(runtime));
    }
  }
}

NativeApiMethodCallbackPolicy readEngineMethodCallbackPolicyValue(
    Runtime& runtime, const Value& policyValue) {
  NativeApiMethodCallbackPolicy policy;
  try {
    if (!policyValue.isObject()) {
      return policy;
    }

    Object policyObject = policyValue.asObject(runtime);
    Value callSuperBeforeValue = optionalObjectProperty(
        runtime, policyObject, "callSuperBeforeCallback");
    if (callSuperBeforeValue.isBool() && callSuperBeforeValue.getBool()) {
      policy.callSuperBeforeCallback = true;
    } else {
      Value callSuperValue =
          optionalObjectProperty(runtime, policyObject, "callSuper");
      if (callSuperValue.isString()) {
        policy.callSuperBeforeCallback =
            callSuperValue.asString(runtime).utf8(runtime) == "before";
      }
    }

    appendEngineMethodPolicyAssociatedObjectKeys(
        runtime,
        optionalObjectProperty(
            runtime, policyObject, "skipCallbackIfAssociatedObjectTruthy"),
        policy.skipCallbackIfAssociatedObjectTruthy);
  } catch (const std::exception&) {
    return NativeApiMethodCallbackPolicy{};
  }
  return policy;
}

// Reads the policy a JS function was tagged with via
// NativeScriptRuntime.nativeMethodPolicy(fn, policy).
NativeApiMethodCallbackPolicy readEngineMethodCallbackPolicy(
    Runtime& runtime, Object& functionObject) {
  constexpr const char* propertyName = "__nativeScriptMethodPolicy";
  try {
    if (!functionObject.hasProperty(runtime, propertyName)) {
      return NativeApiMethodCallbackPolicy{};
    }
    return readEngineMethodCallbackPolicyValue(
        runtime, functionObject.getProperty(runtime, propertyName));
  } catch (const std::exception&) {
    return NativeApiMethodCallbackPolicy{};
  }
}

bool isEmptyMethodCallbackPolicy(const NativeApiMethodCallbackPolicy& policy) {
  return !policy.callSuperBeforeCallback &&
         policy.skipCallbackIfAssociatedObjectTruthy.empty();
}

bool selectorEndsWithNSErrorParam(const std::string& selectorName) {
  constexpr const char* suffix = "error:";
  size_t suffixLength = std::strlen(suffix);
  return selectorName.size() >= suffixLength &&
         selectorName.compare(selectorName.size() - suffixLength, suffixLength,
                              suffix) == 0;
}

bool isNSErrorOutEngineMethodSignature(const NativeApiSignature& signature) {
  if (signature.argumentTypes.empty() || signature.variadic ||
      !selectorEndsWithNSErrorParam(signature.selectorName)) {
    return false;
  }

  return signature.argumentTypes.back().kind == metagen::mdTypePointer;
}

bool isNSErrorOutEngineMethodCallback(const NativeApiSignature& signature) {
  return signature.returnType.kind == metagen::mdTypeBool &&
         signature.implicitArgumentCount >= 2 &&
         isNSErrorOutEngineMethodSignature(signature);
}

class NativeApiArgumentFrame {
 public:
  explicit NativeApiArgumentFrame(size_t count) : count_(count) {
    if (count_ > kInlineArgumentCount) {
      heapStorage_.resize(count_);
      heapValues_.resize(count_);
    }
  }

  ~NativeApiArgumentFrame() {
    for (char* string : ownedCStrings_) {
      free(string);
    }
    for (void* buffer : ownedBuffers_) {
      free(buffer);
    }
    for (id object : ownedObjects_) {
      [object release];
    }
    for (const auto& entry : temporaryRoundTripValues_) {
      if (entry.bridge != nullptr && entry.runtime != nullptr) {
        entry.bridge->forgetRoundTripValue(*entry.runtime, entry.native);
      }
    }
    ownedLifetimes_.clear();
  }

  void* storageAt(size_t index, size_t size) {
    if (index >= count_) {
      throw std::out_of_range("Native argument index out of range.");
    }

    size = std::max<size_t>(size, sizeof(void*));
    if (count_ <= kInlineArgumentCount && size <= kInlineStorageSize) {
      std::memset(inlineStorage_[index], 0, kInlineStorageSize);
      inlineValues_[index] = inlineStorage_[index];
      return inlineValues_[index];
    }

    if (count_ <= kInlineArgumentCount) {
      overflowStorage_.emplace_back(size, 0);
      inlineValues_[index] = overflowStorage_.back().data();
      return inlineValues_[index];
    }

    heapStorage_[index].assign(size, 0);
    heapValues_[index] = heapStorage_[index].data();
    return heapValues_[index];
  }

  void addCString(char* value) { ownedCStrings_.push_back(value); }
  void* addBuffer(size_t size) {
    void* buffer = calloc(1, std::max<size_t>(size, 1));
    if (buffer == nullptr) {
      throw std::bad_alloc();
    }
    ownedBuffers_.push_back(buffer);
    return buffer;
  }
  void addObject(id value) { ownedObjects_.push_back(value); }
  void retainObject(id value) {
    if (value != nil) {
      [value retain];
      ownedObjects_.push_back(value);
    }
  }
  void addLifetime(std::shared_ptr<void> value) {
    if (value != nullptr) {
      ownedLifetimes_.push_back(std::move(value));
    }
  }
  void rememberRoundTripValue(
      const std::shared_ptr<NativeApiBridge>& bridge, Runtime& runtime,
      const void* native, const Value& value) {
    if (bridge == nullptr || native == nullptr) {
      return;
    }
    bridge->rememberRoundTripValue(runtime, native, value);
    temporaryRoundTripValues_.push_back({bridge, &runtime, native});
  }
  void** values() {
    if (count_ == 0) {
      return nullptr;
    }
    return count_ <= kInlineArgumentCount ? inlineValues_ : heapValues_.data();
  }

 private:
  static constexpr size_t kInlineArgumentCount = 8;
  static constexpr size_t kInlineStorageSize = 32;

  size_t count_ = 0;
  alignas(void*) unsigned char
      inlineStorage_[kInlineArgumentCount][kInlineStorageSize] = {};
  void* inlineValues_[kInlineArgumentCount] = {};
  std::vector<std::vector<unsigned char>> heapStorage_;
  std::vector<void*> heapValues_;
  std::vector<std::vector<unsigned char>> overflowStorage_;
  std::vector<char*> ownedCStrings_;
  std::vector<void*> ownedBuffers_;
  std::vector<id> ownedObjects_;
  std::vector<std::shared_ptr<void>> ownedLifetimes_;
  struct TemporaryRoundTripValue {
    std::shared_ptr<NativeApiBridge> bridge;
    Runtime* runtime = nullptr;
    const void* native = nullptr;
  };
  std::vector<TemporaryRoundTripValue> temporaryRoundTripValues_;
};

class NativeApiMutableBuffer final : public MutableBuffer {
 public:
  explicit NativeApiMutableBuffer(size_t size) : data_(size) {}
  NativeApiMutableBuffer(const void* data, size_t size) : data_(size) {
    if (data != nullptr && size > 0) {
      std::memcpy(data_.data(), data, size);
    }
  }

  size_t size() const override { return data_.size(); }
  uint8_t* data() override { return data_.empty() ? nullptr : data_.data(); }

 private:
  std::vector<uint8_t> data_;
};

void convertEngineArgument(Runtime& runtime,
                        const std::shared_ptr<NativeApiBridge>& bridge,
                        const NativeApiType& type,
                        const Value& value, void* target,
                        NativeApiArgumentFrame& frame);

Value convertNativeReturnValue(Runtime& runtime,
                               const std::shared_ptr<NativeApiBridge>& bridge,
                               const NativeApiType& type, void* value);

Value wrapNativeFunctionPointer(Runtime& runtime,
                                const std::shared_ptr<NativeApiBridge>& bridge,
                                const NativeApiType& type, void* pointer,
                                bool block);

bool isObjectiveCObjectType(const NativeApiType& type);

struct NativeApiBlockDescriptor {
  unsigned long reserved = 0;
  unsigned long size = 0;
  void (*copyHelper)(void*, void*) = nullptr;
  void (*disposeHelper)(void*) = nullptr;
  const char* signature = nullptr;
};

struct NativeApiBlockLiteral {
  void* isa = nullptr;
  int flags = 0;
  int reserved = 0;
  void* invoke = nullptr;
  NativeApiBlockDescriptor* descriptor = nullptr;
  void* callback = nullptr;
};

constexpr int kNativeApiBlockNeedsFree = (1 << 24);
constexpr int kNativeApiBlockHasCopyDispose = (1 << 25);
constexpr int kNativeApiBlockRefCountOne = (1 << 1);
constexpr int kNativeApiBlockHasSignature = (1 << 30);

void* nativeApiEngineMallocBlockIsa() {
  static void* isa = dlsym(RTLD_DEFAULT, "_NSConcreteMallocBlock");
  return isa;
}

void nativeApiEngineBlockCopy(void* dst, void* src);
void nativeApiEngineBlockDispose(void* src);

std::string objcEncodingForEngineType(const NativeApiType& type) {
  switch (type.kind) {
    case metagen::mdTypeVoid:
      return "v";
    case metagen::mdTypeBool:
      return "B";
    case metagen::mdTypeChar:
      return "c";
    case metagen::mdTypeUChar:
    case metagen::mdTypeUInt8:
      return "C";
    case metagen::mdTypeSShort:
      return "s";
    case metagen::mdTypeUShort:
    case metagen::mdTypeUnichar:
      return "S";
    case metagen::mdTypeSInt:
      return "i";
    case metagen::mdTypeUInt:
      return "I";
    case metagen::mdTypeSLong:
    case metagen::mdTypeSInt64:
      return "q";
    case metagen::mdTypeULong:
    case metagen::mdTypeUInt64:
      return "Q";
    case metagen::mdTypeFloat:
      return "f";
    case metagen::mdTypeDouble:
      return "d";
    case metagen::mdTypeString:
      return "*";
    case metagen::mdTypeAnyObject:
    case metagen::mdTypeProtocolObject:
    case metagen::mdTypeClassObject:
    case metagen::mdTypeInstanceObject:
    case metagen::mdTypeNSStringObject:
    case metagen::mdTypeNSMutableStringObject:
      return "@";
    case metagen::mdTypeClass:
      return "#";
    case metagen::mdTypeSelector:
      return ":";
    case metagen::mdTypeBlock:
      return "@?";
    case metagen::mdTypeFunctionPointer:
      return "^?";
    case metagen::mdTypePointer:
    case metagen::mdTypeOpaquePointer:
      if (type.elementType != nullptr &&
          type.elementType->kind != metagen::mdTypeVoid) {
        return "^" + objcEncodingForEngineType(*type.elementType);
      }
      return "^v";
    case metagen::mdTypeStruct:
      return "{" +
             (type.aggregateInfo != nullptr ? type.aggregateInfo->name
                                            : std::string("?")) +
             "=}";
    case metagen::mdTypeArray:
      return "[" + std::to_string(type.arraySize) +
             (type.elementType != nullptr ? objcEncodingForEngineType(*type.elementType)
                                          : std::string("?")) +
             "]";
    case metagen::mdTypeVector:
    case metagen::mdTypeExtVector:
    case metagen::mdTypeComplex:
      return type.elementType != nullptr ? objcEncodingForEngineType(*type.elementType)
                                         : "?";
    default:
      return "?";
  }
}

std::string objcBlockSignatureForEngineSignature(
    const NativeApiSignature& signature) {
  std::string encoding = objcEncodingForEngineType(signature.returnType);
  encoding += "@?";
  for (const auto& argType : signature.argumentTypes) {
    encoding += objcEncodingForEngineType(argType);
  }
  return encoding;
}

std::string objcMethodSignatureForEngineSignature(
    const NativeApiSignature& signature) {
  std::string encoding = objcEncodingForEngineType(signature.returnType);
  encoding += "@:";
  for (const auto& argType : signature.argumentTypes) {
    encoding += objcEncodingForEngineType(argType);
  }
  return encoding;
}

[[noreturn]] void throwNativeApiCallbackException(
    const std::string& message) {
  NSString* reason = [NSString stringWithUTF8String:message.c_str()];
  @throw [NSException exceptionWithName:@"NativeScriptEngineCallbackException"
                                 reason:reason
                               userInfo:nil];
}

class NativeApiCallback;

void nativeApiEngineCallbackTrampoline(ffi_cif* cif, void* ret, void* args[],
                                    void* data);

std::atomic<int> gActiveNativeThreadEngineCallbacks{0};

// A callback can outlive the scope in which its function argument was created
// (e.g. a block invoked asynchronously). Round-trip the function through the
// engine value copy constructor so any scope-bound/borrowed handle is promoted
// to a persistent one before it is stored.
Function persistentEngineFunction(Runtime& runtime, const Function& function) {
  Value shared(runtime, function);
  Value persistent(runtime, shared);
  return persistent.asObject(runtime).asFunction(runtime);
}

class NativeApiCallback final
    : public std::enable_shared_from_this<NativeApiCallback> {
 public:
  NativeApiCallback(Runtime& runtime,
                       std::shared_ptr<NativeApiBridge> bridge,
                       std::shared_ptr<NativeApiSignature> signature,
                       Function function, bool block,
                       NativeApiCallbackThreadPolicy threadPolicy =
                           NativeApiCallbackThreadPolicy::Default,
                       bool bindThis = false,
                       uintptr_t roundTripValidationKey = 0,
                       NativeApiMethodCallbackPolicy methodPolicy = {},
                       Class methodBaseClass = Nil)
      : runtimeOwner_(retainNativeApiRuntime(runtime)),
        runtime_(runtimeOwner_.get()),
        bridge_(std::move(bridge)),
        signature_(std::move(signature)),
        function_(std::make_shared<Function>(
            persistentEngineFunction(runtime, function))),
        block_(block),
        threadPolicy_(threadPolicy),
        bindThis_(bindThis),
        roundTripValidationKey_(roundTripValidationKey),
        methodPolicy_(std::move(methodPolicy)),
        methodBaseClass_(methodBaseClass) {
    closure_ = static_cast<ffi_closure*>(
        ffi_closure_alloc(sizeof(ffi_closure), &executable_));
    if (closure_ == nullptr || executable_ == nullptr ||
        signature_ == nullptr || !signature_->prepared) {
      throw JSError(runtime,
                                   "Unable to allocate native callback.");
    }

    ffi_status status = ffi_prep_closure_loc(
        closure_, &signature_->cif, nativeApiEngineCallbackTrampoline, this,
        executable_);
    if (status != FFI_OK) {
      ffi_closure_free(closure_);
      closure_ = nullptr;
      executable_ = nullptr;
      throw JSError(runtime,
                                   "Unable to prepare native callback.");
    }

    if (block_) {
      blockSignature_ = objcBlockSignatureForEngineSignature(*signature_);
      descriptor_ = std::make_unique<NativeApiBlockDescriptor>();
      descriptor_->reserved = 0;
      descriptor_->size = sizeof(NativeApiBlockLiteral);
      descriptor_->copyHelper = nativeApiEngineBlockCopy;
      descriptor_->disposeHelper = nativeApiEngineBlockDispose;
      descriptor_->signature = blockSignature_.c_str();

      blockLiteral_ = static_cast<NativeApiBlockLiteral*>(
          calloc(1, sizeof(NativeApiBlockLiteral)));
      if (blockLiteral_ == nullptr) {
        throw JSError(runtime, "Unable to allocate native block callback.");
      }
      void* blockIsa = nativeApiEngineMallocBlockIsa();
      if (blockIsa == nullptr) {
        free(blockLiteral_);
        blockLiteral_ = nullptr;
        throw JSError(runtime,
                     "Objective-C malloc block runtime is unavailable.");
      }
      blockLiteral_->isa = blockIsa;
      blockLiteral_->flags = kNativeApiBlockNeedsFree |
                             kNativeApiBlockHasCopyDispose |
                             kNativeApiBlockRefCountOne |
                             kNativeApiBlockHasSignature;
      blockLiteral_->invoke = executable_;
      blockLiteral_->descriptor = descriptor_.get();
      blockLiteral_->callback = this;
    }
  }

  ~NativeApiCallback() {
    if (closure_ != nullptr) {
      ffi_closure_free(closure_);
      closure_ = nullptr;
      executable_ = nullptr;
    }
  }

  void* functionPointer() const {
    return block_ && blockLiteral_ != nullptr
               ? static_cast<void*>(blockLiteral_)
               : executable_;
  }

  const NativeApiSignature& signature() const { return *signature_; }

  void retainInitialBlockLifetime(
      std::shared_ptr<NativeApiCallback> lifetime) {
    if (block_) {
      initialBlockLifetime_ = std::move(lifetime);
    }
  }

  void retainBlockCopy(const void* blockPointer) {
    if (!block_) {
      return;
    }
    auto self = shared_from_this();
    if (bridge_ != nullptr && runtime_ != nullptr && function_ != nullptr &&
        blockPointer != nullptr) {
      bridge_->rememberRoundTripValue(*runtime_, blockPointer,
                                      Value(*runtime_, *function_), false,
                                      roundTripValidationKey_);
    }
    std::lock_guard<std::mutex> lock(retainedBlockCopiesMutex_);
    retainedBlockCopies_.push_back({blockPointer, std::move(self)});
  }

  bool releaseBlockCopy(const void* blockPointer) {
    if (!block_) {
      return false;
    }

    bool canRelease = false;
    {
      std::lock_guard<std::mutex> lock(retainedBlockCopiesMutex_);
      auto it = retainedBlockCopies_.end();
      if (blockPointer != nullptr) {
        it = std::find_if(
            retainedBlockCopies_.begin(), retainedBlockCopies_.end(),
            [blockPointer](const RetainedBlockCopy& retained) {
              return retained.blockPointer == blockPointer;
            });
      }
      canRelease =
          it != retainedBlockCopies_.end() || blockPointer == blockLiteral_;
    }
    // Forgetting the round-trip value touches the JS engine global/context.
    // Block disposal can run during an autorelease-pool drain on an arbitrary
    // thread (e.g. an NSOperationQueue worker). Keep the retained block entry
    // in place until the JS-thread task runs so the callback and its engine
    // function are also destroyed on the JS thread.
    if (!canRelease) {
      return false;
    }

    auto bridge = bridge_;
    auto* runtime = runtime_;
    auto runtimeOwner = runtimeOwner_;
    auto releaseOnJS = [this, bridge, runtime, runtimeOwner, blockPointer]() {
      std::shared_ptr<NativeApiCallback> keepAlive;
      try {
        keepAlive = shared_from_this();
      } catch (const std::bad_weak_ptr&) {
        return;
      }

      const void* pointerToForget = nullptr;
      {
        std::lock_guard<std::mutex> lock(retainedBlockCopiesMutex_);
        auto it = retainedBlockCopies_.end();
        if (blockPointer != nullptr) {
          it = std::find_if(
              retainedBlockCopies_.begin(), retainedBlockCopies_.end(),
              [blockPointer](const RetainedBlockCopy& retained) {
                return retained.blockPointer == blockPointer;
              });
        }
        if (it != retainedBlockCopies_.end()) {
          pointerToForget = it->blockPointer;
          retainedBlockCopies_.erase(it);
        } else if (blockPointer == blockLiteral_) {
          pointerToForget = blockPointer;
          blockLiteral_ = nullptr;
          initialBlockLifetime_.reset();
        }
      }

      if (bridge != nullptr && runtime != nullptr &&
          pointerToForget != nullptr) {
        NativeApiRuntimeScope runtimeScope(*runtime);
        bridge->forgetRoundTripValue(*runtime, pointerToForget);
      }
    };

    if (bridge == nullptr) {
      releaseOnJS();
    } else if (const auto& asyncInvoker =
                   bridge->jsThreadAsyncCallbackInvoker()) {
      asyncInvoker(std::move(releaseOnJS));
    } else if (auto scheduler = bridge->scheduler()) {
      scheduler->invokeOnJS(std::move(releaseOnJS));
    } else if (std::this_thread::get_id() == bridge->jsThreadId()) {
      releaseOnJS();
    } else if (const auto& invoker = bridge->jsThreadCallbackInvoker()) {
      invoker(std::move(releaseOnJS));
    } else {
      releaseOnJS();
    }
    return true;
  }

  void invoke(void* ret, void* args[]) {
    if (runtime_ == nullptr || function_ == nullptr || signature_ == nullptr) {
      throwNativeApiCallbackException("Invalid callback.");
    }

    // Teardown-safety gate: once the host has signaled that callbacks are no
    // longer allowed (runtime shutting down/reloading), bail with a
    // zeroed return instead of running JS against a dying runtime.
    bool callbackAllowed = false;
    if (bridge_ != nullptr) {
      @try {
        callbackAllowed = bridge_->callbackInvocationAllowed();
      } @catch (...) {
        callbackAllowed = false;
      }
    }

    if (!callbackAllowed) {
      zeroReturnValue(ret);
      return;
    }

    if (methodPolicy_.callSuperBeforeCallback) {
      invokeMethodSuper(ret, args);
    }
    if (shouldSkipMethodCallback(args, ret)) {
      return;
    }

    std::string error;
    auto call = [&]() { invokeOnCurrentThread(ret, args, &error); };
    const auto& nativeCallbackInvoker = bridge_->nativeCallbackInvoker();
    const auto& runtimeCallbackInvoker = bridge_->runtimeCallbackInvoker();
    const auto& jsThreadCallbackInvoker = bridge_->jsThreadCallbackInvoker();
    bool currentThreadIsJs =
        std::this_thread::get_id() == bridge_->jsThreadId();

    auto callOnNativeCallerThread = [&]() {
      ScopedNativeCallerThreadEngineCallback callbackScope;
      if (nativeCallbackInvoker) {
        nativeCallbackInvoker(call);
      } else {
        call();
      }
    };
    auto callOnJSThread = [&]() {
      if (currentThreadIsJs) {
        call();
        return;
      }
      if (jsThreadCallbackInvoker) {
        jsThreadCallbackInvoker(call);
        return;
      }
      if (auto scheduler = bridge_->scheduler()) {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        scheduler->invokeOnJS([call, done]() mutable {
          call();
          dispatch_semaphore_signal(done);
        });
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        return;
      }
      error = "Native callback was invoked off the JS thread without a JS scheduler.";
    };
    auto callOnRuntimeThread = [&]() {
      if (currentThreadIsJs) {
        call();
        return;
      }
      if (runtimeCallbackInvoker) {
        runtimeCallbackInvoker(call);
        return;
      }
      error = "Native callback was invoked off its owning runtime thread without a runtime scheduler.";
    };

    if (threadPolicy_ == NativeApiCallbackThreadPolicy::JS) {
      callOnJSThread();
      if (!error.empty()) {
        if (!recordNativeCallbackException(error)) {
          throwNativeApiCallbackException(error);
        }
      }
      return;
    }
    if (threadPolicy_ == NativeApiCallbackThreadPolicy::Runtime) {
      callOnRuntimeThread();
      if (!error.empty()) {
        if (!recordNativeCallbackException(error)) {
          throwNativeApiCallbackException(error);
        }
      }
      return;
    }

    bool returnsVoid = signature_->returnType.kind == metagen::mdTypeVoid;
    bool nativeCallerThreadCallbacks =
        bridge_->invokeCallbacksOnNativeCallerThread();
    bool direct = currentThreadIsJs ||
                  gSynchronousNativeInvocationDepth > 0;
    bool waitForNativeThreadCallback =
        currentThreadIsJs && nativeCallbackInvoker &&
        gActiveNativeThreadEngineCallbacks.load(std::memory_order_acquire) > 0;
    auto dispatchZeroArgVoidBlockAsync = [&]() -> bool {
      if (currentThreadIsJs || !returnsVoid || !block_ ||
          !signature_->argumentTypes.empty()) {
        return false;
      }

      std::shared_ptr<NativeApiCallback> keepAlive;
      try {
        keepAlive = shared_from_this();
      } catch (const std::bad_weak_ptr&) {
        return false;
      }

      auto asyncCall = [keepAlive = std::move(keepAlive)]() mutable {
        std::string asyncError;
        keepAlive->invokeOnCurrentThread(nullptr, nullptr, &asyncError);
        if (!asyncError.empty()) {
          recordNativeCallbackException(asyncError);
        }
      };

      const auto& asyncInvoker = bridge_->jsThreadAsyncCallbackInvoker();
      if (asyncInvoker) {
        asyncInvoker(std::move(asyncCall));
        return true;
      }
      if (auto scheduler = bridge_->scheduler()) {
        scheduler->invokeOnJS(std::move(asyncCall));
        return true;
      }
      return false;
    };

    if (nativeCallerThreadCallbacks && !currentThreadIsJs) {
      callOnNativeCallerThread();
    } else if (dispatchZeroArgVoidBlockAsync()) {
      return;
    } else if (direct && !waitForNativeThreadCallback) {
      call();
    } else if (!currentThreadIsJs) {
      callOnJSThread();
    } else if (nativeCallbackInvoker) {
      bool nativeThreadCallback = !currentThreadIsJs;
      if (nativeThreadCallback) {
        gActiveNativeThreadEngineCallbacks.fetch_add(1,
                                                  std::memory_order_acq_rel);
      }
      try {
        nativeCallbackInvoker(call);
      } catch (...) {
        if (nativeThreadCallback) {
          gActiveNativeThreadEngineCallbacks.fetch_sub(
              1, std::memory_order_acq_rel);
        }
        throw;
      }
      if (nativeThreadCallback) {
        gActiveNativeThreadEngineCallbacks.fetch_sub(1,
                                                  std::memory_order_acq_rel);
      }
    } else if (auto scheduler = bridge_->scheduler()) {
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      scheduler->invokeOnJS([call, done]() mutable {
        call();
        dispatch_semaphore_signal(done);
      });
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    } else {
      error = "Native callback was invoked off the JS thread without a JS scheduler.";
    }

    if (!error.empty()) {
      if (!recordNativeCallbackException(error)) {
        throwNativeApiCallbackException(error);
      }
    }
  }

 private:
  // The ObjC class whose implementation `callSuperBeforeCallback`/
  // invokeMethodSuper() should dispatch against. A JS-subclass receiver
  // (ClassBuilder instance) dispatches to its own runtime superclass;
  // anything else falls back to methodBaseClass_ (the class the override was
  // registered against).
  Class dispatchSuperclassForMethodReceiver(id receiver) const {
    if (receiver == nil) {
      return Nil;
    }

    Class receiverClass = object_getClass(receiver);
    if (receiverClass != Nil &&
        class_conformsToProtocol(receiverClass,
                                  @protocol(NativeApiClassBuilderProtocol))) {
      Class superclass = class_getSuperclass(receiverClass);
      if (superclass != Nil) {
        return superclass;
      }
    }

    return methodBaseClass_;
  }

  // Method callback policies only ever target the callback's receiver (no
  // argument-index targeting), so this is just the bound `self`.
  id methodCallbackReceiver(void* args[]) const {
    if (!bindThis_ || args == nullptr) {
      return nil;
    }
    return *static_cast<id*>(args[0]);
  }

  id associatedObjectValue(id receiver, const std::string& key) const {
    if (receiver == nil || key.empty()) {
      return nil;
    }
    return objc_getAssociatedObject(receiver, sel_registerName(key.c_str()));
  }

  bool associatedObjectIsTruthy(id receiver, const std::string& key) const {
    id value = associatedObjectValue(receiver, key);
    if (value == nil) {
      return false;
    }

    if ([value respondsToSelector:@selector(boolValue)]) {
      return [value boolValue] == YES;
    }
    if ([value isKindOfClass:[NSString class]]) {
      NSString* stringValue = (NSString*)value;
      if (stringValue.length == 0) {
        return false;
      }
      NSString* lowercase = [stringValue lowercaseString];
      return ![lowercase isEqualToString:@"0"] &&
             ![lowercase isEqualToString:@"false"] &&
             ![lowercase isEqualToString:@"no"];
    }

    return true;
  }

  // Skips a re-entrant callback: an alloc/init construction already in
  // flight (marked via __nativeApiConstructionState) must not have its
  // non-init methods re-entered by a partially-constructed self.
  bool shouldSkipConstructingMethodCallback(void* args[], void* ret) {
    if (!bindThis_ || args == nullptr || signature_ == nullptr ||
        signature_->selectorName.rfind("init", 0) == 0) {
      return false;
    }

    id receiver = *static_cast<id*>(args[0]);
    if (receiver == nil ||
        objc_getAssociatedObject(
            receiver, sel_registerName("__nativeApiConstructionState")) == nil) {
      return false;
    }

    zeroReturnValue(ret);
    return true;
  }

  bool shouldSkipMethodCallback(void* args[], void* ret) {
    if (args == nullptr) {
      return false;
    }

    if (shouldSkipConstructingMethodCallback(args, ret)) {
      return true;
    }

    id receiver = methodCallbackReceiver(args);
    for (const auto& key :
         methodPolicy_.skipCallbackIfAssociatedObjectTruthy) {
      if (associatedObjectIsTruthy(receiver, key)) {
        zeroReturnValue(ret);
        return true;
      }
    }

    return false;
  }

  // callSuperBeforeCallback: run the ObjC super implementation before the JS
  // override does, via objc_msgSendSuper with the same arguments the JS
  // callback is about to receive.
  void invokeMethodSuper(void* ret, void* args[]) const {
    if (!bindThis_ || args == nullptr || signature_ == nullptr ||
        methodBaseClass_ == Nil) {
      return;
    }

    id receiver = *static_cast<id*>(args[0]);
    Class dispatchClass = dispatchSuperclassForMethodReceiver(receiver);
    if (receiver == nil || dispatchClass == Nil) {
      return;
    }

    struct objc_super superReceiver = {receiver, dispatchClass};
    struct objc_super* superReceiverPtr = &superReceiver;
    size_t nativeArgc =
        signature_->implicitArgumentCount + signature_->argumentTypes.size();
    std::vector<void*> values(nativeArgc);
    values[0] = &superReceiverPtr;
    values[1] = args[1];
    for (size_t i = 2; i < nativeArgc; i++) {
      values[i] = args[i];
    }

    std::vector<unsigned char> returnStorage;
    void* returnTarget = ret;
    if (returnTarget == nullptr) {
      returnStorage.resize(
          std::max<size_t>(nativeSizeForType(signature_->returnType), 1));
      returnTarget = returnStorage.data();
    }

    performNativeInvocation(*runtime_, bridge_->nativeInvocationInvoker(), [&]() {
#if defined(__x86_64__)
      bool isStret = signature_->returnType.ffiType->size > 16 &&
                     signature_->returnType.ffiType->type == FFI_TYPE_STRUCT;
      void (*target)(void) = isStret ? FFI_FN(objc_msgSendSuper_stret)
                                     : FFI_FN(objc_msgSendSuper);
      ffi_call(const_cast<ffi_cif*>(&signature_->cif), target, returnTarget,
               values.data());
#else
      ffi_call(const_cast<ffi_cif*>(&signature_->cif),
               FFI_FN(objc_msgSendSuper), returnTarget, values.data());
#endif
    });
  }

  void invokeOnCurrentThread(void* ret, void* args[], std::string* error) {
    try {
      NativeApiRuntimeScope runtimeScope(*runtime_);
      size_t nativeArgOffset = signature_->implicitArgumentCount;
      std::vector<Value> jsArgs;
      jsArgs.reserve(signature_->argumentTypes.size());
      for (size_t i = 0; i < signature_->argumentTypes.size(); i++) {
        jsArgs.emplace_back(convertNativeReturnValue(
            *runtime_, bridge_, signature_->argumentTypes[i],
            args[i + nativeArgOffset]));
      }

      Value result = Value::undefined();
      if (bindThis_ && nativeArgOffset >= 1) {
        id self = *static_cast<id*>(args[0]);
        // `this.super`/`$base` from inside this override should dispatch
        // against the class ABOVE the one the override was registered on,
        // not the receiver's own (possibly further-subclassed) runtime class.
        // `methodBaseClass_` (threaded from ClassBuilder's addEngineOverrideMethod
        // as `baseClass`, i.e. `class_getSuperclass(nativeClass)`) IS already
        // that class -- it must be used directly, not further superclassed
        // (which would skip straight past it to ITS superclass and make any
        // member declared exactly on methodBaseClass_, e.g. a method the
        // override shadows that isn't itself inherited, unreachable via
        // `this.super`). dispatchSuperclassForMethodReceiver() above already
        // relies on this same "use methodBaseClass_ as-is" convention.
        Class superDispatchClass = methodBaseClass_;
        Value thisValue =
            makeNativeObjectValue(
                *runtime_, bridge_, self, false, superDispatchClass);
        Object thisObject = thisValue.isObject()
                                ? thisValue.asObject(*runtime_)
                                : Object(*runtime_);
        result =
            jsArgs.empty()
                ? function_->callWithThis(*runtime_, thisObject)
                : function_->callWithThis(
                      *runtime_, thisObject,
                      static_cast<const Value*>(jsArgs.data()),
                      static_cast<size_t>(jsArgs.size()));
      } else {
        result =
            jsArgs.empty()
                ? function_->call(*runtime_)
                : function_->call(*runtime_,
                                  static_cast<const Value*>(jsArgs.data()),
                                  static_cast<size_t>(jsArgs.size()));
      }
      storeReturnValue(result, ret);
      if (std::this_thread::get_id() == bridge_->jsThreadId() &&
          gSynchronousNativeInvocationDepth == 0) {
        runtime_->drainMicrotasks();
      }
    } catch (const std::exception& exception) {
      if (isNSErrorOutEngineMethodCallback(*signature_)) {
        zeroReturnValue(ret);
        populateNSErrorOutArgument(args, exception.what());
        return;
      }
      if (error != nullptr) {
        *error = exception.what();
      }
      zeroReturnValue(ret);
    } catch (...) {
      if (isNSErrorOutEngineMethodCallback(*signature_)) {
        zeroReturnValue(ret);
        populateNSErrorOutArgument(args, "Unknown exception in native callback.");
        return;
      }
      if (error != nullptr) {
        *error = "Unknown exception in native callback.";
      }
      zeroReturnValue(ret);
    }
  }

  void populateNSErrorOutArgument(void* args[], const char* message) {
    if (args == nullptr || signature_ == nullptr ||
        signature_->argumentTypes.empty()) {
      return;
    }

    size_t outArgIndex = signature_->implicitArgumentCount +
                         signature_->argumentTypes.size() - 1;
    void* outArgValue = args[outArgIndex];
    NSError** outError =
        outArgValue != nullptr ? *reinterpret_cast<NSError***>(outArgValue)
                               : nullptr;
    if (outError == nullptr) {
      return;
    }

    NSString* nsMessage =
        message != nullptr ? [NSString stringWithUTF8String:message] : nil;
    if (nsMessage == nil) {
      nsMessage = @"JS error";
    }
    NSDictionary* userInfo = @{NSLocalizedDescriptionKey : nsMessage};
    *outError = [NSError errorWithDomain:@"TNSErrorDomain"
                                    code:1
                                userInfo:userInfo];
  }

  void zeroReturnValue(void* ret) {
    if (ret == nullptr || signature_ == nullptr ||
        signature_->returnType.kind == metagen::mdTypeVoid) {
      return;
    }
    size_t size = nativeSizeForType(signature_->returnType);
    if (size > 0) {
      std::memset(ret, 0, size);
    }
  }

  void storeReturnValue(const Value& result, void* ret) {
    if (ret == nullptr ||
        signature_->returnType.kind == metagen::mdTypeVoid) {
      return;
    }

    zeroReturnValue(ret);
    if (result.isUndefined() || result.isNull()) {
      return;
    }
    const auto& returnType = signature_->returnType;
    if (returnType.kind == metagen::mdTypeString && result.isString()) {
      std::string utf8 = result.asString(*runtime_).utf8(*runtime_);
      *static_cast<char**>(ret) = strdup(utf8.c_str());
      return;
    }
    if ((returnType.kind == metagen::mdTypePointer ||
         returnType.kind == metagen::mdTypeOpaquePointer) &&
        result.isString()) {
      std::string utf8 = result.asString(*runtime_).utf8(*runtime_);
      *static_cast<void**>(ret) = strdup(utf8.c_str());
      return;
    }

    NativeApiArgumentFrame frame(1);
    convertEngineArgument(*runtime_, bridge_, returnType, result, ret, frame);
    if (isObjectiveCObjectType(returnType)) {
      id object = *static_cast<id*>(ret);
      if (object != nil) {
        [object retain];
        [object autorelease];
      }
    }
  }

  std::shared_ptr<Runtime> runtimeOwner_;
  Runtime* runtime_ = nullptr;
  std::shared_ptr<NativeApiBridge> bridge_;
  std::shared_ptr<NativeApiSignature> signature_;
  std::shared_ptr<Function> function_;
  bool block_ = false;
  NativeApiCallbackThreadPolicy threadPolicy_ =
      NativeApiCallbackThreadPolicy::Default;
  bool bindThis_ = false;
  uintptr_t roundTripValidationKey_ = 0;
  NativeApiMethodCallbackPolicy methodPolicy_;
  Class methodBaseClass_ = Nil;
  ffi_closure* closure_ = nullptr;
  void* executable_ = nullptr;
  std::string blockSignature_;
  std::unique_ptr<NativeApiBlockDescriptor> descriptor_;
  NativeApiBlockLiteral* blockLiteral_ = nullptr;
  std::shared_ptr<NativeApiCallback> initialBlockLifetime_;
  struct RetainedBlockCopy {
    const void* blockPointer = nullptr;
    std::shared_ptr<NativeApiCallback> lifetime;
  };
  std::mutex retainedBlockCopiesMutex_;
  std::vector<RetainedBlockCopy> retainedBlockCopies_;
};

void nativeApiEngineBlockCopy(void* dst, void* src) {
  auto* dstBlock = static_cast<NativeApiBlockLiteral*>(dst);
  auto* srcBlock = static_cast<NativeApiBlockLiteral*>(src);
  if (dstBlock == nullptr || srcBlock == nullptr ||
      srcBlock->callback == nullptr) {
    return;
  }
  dstBlock->callback = srcBlock->callback;
  static_cast<NativeApiCallback*>(srcBlock->callback)
      ->retainBlockCopy(dstBlock);
}

void nativeApiEngineBlockDispose(void* src) {
  auto* block = static_cast<NativeApiBlockLiteral*>(src);
  if (block == nullptr || block->callback == nullptr) {
    return;
  }
  bool released =
      static_cast<NativeApiCallback*>(block->callback)->releaseBlockCopy(block);
  if (released) {
    block->callback = nullptr;
  }
}

void nativeApiEngineCallbackTrampoline(ffi_cif*, void* ret, void* args[],
                                    void* data) {
  auto callback = static_cast<NativeApiCallback*>(data);
  if (callback == nullptr) {
    return;
  }
  @try {
    callback->invoke(ret, args);
  } @catch (NSException* exception) {
    const char* description =
        exception.description != nil ? exception.description.UTF8String : nullptr;
    std::string message = description != nullptr
                              ? description
                              : "Objective-C exception in native callback.";
    if (!recordNativeCallbackException(message)) {
      @throw;
    }
  }
}

size_t nativeSizeForType(const NativeApiType& type) {
  switch (type.kind) {
    case metagen::mdTypeStruct:
      if (type.aggregateInfo != nullptr) {
        return type.aggregateInfo->size;
      }
      break;
    case metagen::mdTypeArray:
      if (type.elementType != nullptr) {
        return nativeSizeForType(*type.elementType) *
               static_cast<size_t>(type.arraySize);
      }
      break;
    case metagen::mdTypeVector:
    case metagen::mdTypeExtVector:
    case metagen::mdTypeComplex:
      if (type.elementType != nullptr) {
        size_t lanes = std::max<size_t>(type.arraySize, 1);
        size_t abiLanes = lanes == 3 ? 4 : lanes;
        return nativeSizeForType(*type.elementType) * abiLanes;
      }
      break;
    default:
      break;
  }

  if (type.ffiType != nullptr && type.ffiType->size > 0) {
    return type.ffiType->size;
  }
  if (type.ffiType == &ffi_type_void) {
    return 0;
  }
  return sizeof(void*);
}

Value signedInteger64ToEngineValue(Runtime& runtime, int64_t value) {
  constexpr int64_t maxSafeInteger = 9007199254740991LL;
  constexpr int64_t minSafeInteger = -9007199254740991LL;
  if (value >= minSafeInteger && value <= maxSafeInteger) {
    return static_cast<double>(value);
  }
  return BigInt::fromInt64(runtime, value);
}

Value unsignedInteger64ToEngineValue(Runtime& runtime, uint64_t value) {
  constexpr uint64_t maxSafeInteger = 9007199254740991ULL;
  if (value <= maxSafeInteger) {
    return static_cast<double>(value);
  }
  return BigInt::fromUint64(runtime, value);
}

bool parseIntegerTextToUintptr(const std::string& text, uintptr_t* address) {
  if (address == nullptr) {
    return false;
  }
  if (text.empty()) {
    return false;
  }

  char* end = nullptr;
  if (text[0] == '-') {
    long long signedValue = std::strtoll(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
      return false;
    }
    *address = static_cast<uintptr_t>(static_cast<intptr_t>(signedValue));
    return true;
  }

  int base = 10;
  const char* start = text.c_str();
  if (text.size() > 2 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
  }
  unsigned long long unsignedValue = std::strtoull(start, &end, base);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *address = static_cast<uintptr_t>(unsignedValue);
  return true;
}

bool parseBigIntToUintptr(Runtime& runtime, const BigInt& bigint,
                          uintptr_t* address) {
  return parseIntegerTextToUintptr(bigint.toString(runtime, 10).utf8(runtime),
                                   address);
}

bool readEngineBuffer(Runtime& runtime, const Object& object, const uint8_t** data,
                   size_t* byteLength) {
  if (data == nullptr || byteLength == nullptr) {
    return false;
  }

  if (object.isArrayBuffer(runtime)) {
    ArrayBuffer buffer = object.getArrayBuffer(runtime);
    *data = buffer.data(runtime);
    *byteLength = buffer.size(runtime);
    return true;
  }

  Value bufferValue = object.getProperty(runtime, "buffer");
  if (!bufferValue.isObject()) {
    return false;
  }
  Object bufferObject = bufferValue.asObject(runtime);
  if (!bufferObject.isArrayBuffer(runtime)) {
    return false;
  }

  size_t byteOffset = 0;
  size_t viewByteLength = 0;
  Value offsetValue = object.getProperty(runtime, "byteOffset");
  if (offsetValue.isNumber()) {
    byteOffset = static_cast<size_t>(std::max<double>(0, offsetValue.getNumber()));
  }
  Value lengthValue = object.getProperty(runtime, "byteLength");
  if (lengthValue.isNumber()) {
    viewByteLength = static_cast<size_t>(std::max<double>(0, lengthValue.getNumber()));
  }

  ArrayBuffer buffer = bufferObject.getArrayBuffer(runtime);
  if (byteOffset > buffer.size(runtime)) {
    return false;
  }
  if (viewByteLength == 0 || byteOffset + viewByteLength > buffer.size(runtime)) {
    viewByteLength = buffer.size(runtime) - byteOffset;
  }
  *data = buffer.data(runtime) + byteOffset;
  *byteLength = viewByteLength;
  return true;
}

uint32_t rawTypeKind(MDTypeKind kind) {
  return static_cast<uint32_t>(kind);
}

MDTypeKind stripTypeFlags(MDTypeKind kind) {
  uint32_t raw = rawTypeKind(kind);
  raw &= ~static_cast<uint32_t>(metagen::mdTypeFlagNext);
  raw &= ~static_cast<uint32_t>(metagen::mdTypeFlagVariadic);
  return static_cast<MDTypeKind>(raw);
}

size_t alignUp(size_t value, size_t alignment) {
  if (alignment == 0) {
    return value;
  }
  return ((value + alignment - 1) / alignment) * alignment;
}

ffi_type* ffiTypeForEngineKind(MDTypeKind kind) {
  switch (kind) {
    case metagen::mdTypeChar:
      return &ffi_type_sint8;
    case metagen::mdTypeUChar:
    case metagen::mdTypeUInt8:
    case metagen::mdTypeBool:
      return &ffi_type_uint8;
    case metagen::mdTypeSShort:
      return &ffi_type_sint16;
    case metagen::mdTypeUShort:
    case metagen::mdTypeUnichar:
      return &ffi_type_uint16;
    case metagen::mdTypeSInt:
      return &ffi_type_sint32;
    case metagen::mdTypeUInt:
      return &ffi_type_uint32;
    case metagen::mdTypeSLong:
    case metagen::mdTypeSInt64:
      return &ffi_type_sint64;
    case metagen::mdTypeULong:
    case metagen::mdTypeUInt64:
      return &ffi_type_uint64;
    case metagen::mdTypeFloat:
      return &ffi_type_float;
    case metagen::mdTypeDouble:
      return &ffi_type_double;
    case metagen::mdTypeVoid:
      return &ffi_type_void;
    case metagen::mdTypeString:
    case metagen::mdTypeAnyObject:
    case metagen::mdTypeProtocolObject:
    case metagen::mdTypeClassObject:
    case metagen::mdTypeInstanceObject:
    case metagen::mdTypeNSStringObject:
    case metagen::mdTypeNSMutableStringObject:
    case metagen::mdTypeClass:
    case metagen::mdTypeSelector:
    case metagen::mdTypePointer:
    case metagen::mdTypeOpaquePointer:
    case metagen::mdTypeBlock:
    case metagen::mdTypeFunctionPointer:
      return &ffi_type_pointer;
    default:
      return nullptr;
  }
}

bool isSupportedEngineKind(MDTypeKind kind) {
  switch (kind) {
    default:
      return ffiTypeForEngineKind(kind) != nullptr;
  }
}

void skipMetadataEngineTypePayload(MDMetadataReader* metadata, MDSectionOffset* offset,
                                MDTypeKind kind);

void skipMetadataEngineType(MDMetadataReader* metadata, MDSectionOffset* offset) {
  MDTypeKind kind = stripTypeFlags(metadata->getTypeKind(*offset));
  *offset += sizeof(MDTypeKind);
  skipMetadataEngineTypePayload(metadata, offset, kind);
}

void skipMetadataEngineTypePayload(MDMetadataReader* metadata, MDSectionOffset* offset,
                                MDTypeKind kind) {
  switch (kind) {
    case metagen::mdTypeClassObject: {
      auto classOffset = metadata->getOffset(*offset);
      *offset += sizeof(MDSectionOffset);
      bool next = (classOffset & metagen::mdSectionOffsetNext) != 0;
      while (next) {
        auto protocolOffset = metadata->getOffset(*offset);
        *offset += sizeof(MDSectionOffset);
        next = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
      }
      break;
    }
    case metagen::mdTypeProtocolObject: {
      bool next = true;
      while (next) {
        auto protocolOffset = metadata->getOffset(*offset);
        *offset += sizeof(MDSectionOffset);
        next = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
      }
      break;
    }
    case metagen::mdTypeArray:
    case metagen::mdTypeVector:
    case metagen::mdTypeExtVector:
    case metagen::mdTypeComplex:
      *offset += sizeof(uint16_t);
      skipMetadataEngineType(metadata, offset);
      break;
    case metagen::mdTypeStruct:
      *offset += sizeof(MDSectionOffset);
      break;
    case metagen::mdTypePointer:
      skipMetadataEngineType(metadata, offset);
      break;
    case metagen::mdTypeBlock:
    case metagen::mdTypeFunctionPointer:
      *offset += sizeof(MDSectionOffset);
      break;
    default:
      break;
  }
}

NativeApiType parseMetadataEngineType(MDMetadataReader* metadata,
                                      MDSectionOffset* offset,
                                      NativeApiBridge* bridge) {
  MDTypeKind rawKind = metadata->getTypeKind(*offset);
  MDTypeKind kind = stripTypeFlags(rawKind);
  *offset += sizeof(MDTypeKind);

  NativeApiType type;
  type.kind = kind;

  switch (kind) {
    case metagen::mdTypeArray: {
      type.arraySize = metadata->getArraySize(*offset);
      *offset += sizeof(uint16_t);
      type.elementType =
          std::make_shared<NativeApiType>(
              parseMetadataEngineType(metadata, offset, bridge));
      auto ffiOwner = std::make_shared<NativeApiFfiType>();
      ffiOwner->elements.reserve(static_cast<size_t>(type.arraySize) + 1);
      ffi_type* elementFfiType = type.elementType->ffiType != nullptr
                                     ? type.elementType->ffiType
                                     : &ffi_type_pointer;
      for (uint16_t i = 0; i < type.arraySize; i++) {
        ffiOwner->elements.push_back(elementFfiType);
      }
      ffiOwner->finalize();
      type.ownedFfiType = ffiOwner;
      type.ffiType = &ffiOwner->type;
      type.supported = type.elementType->supported;
      return type;
    }
    case metagen::mdTypeVector:
    case metagen::mdTypeExtVector:
    case metagen::mdTypeComplex: {
      type.arraySize = metadata->getArraySize(*offset);
      *offset += sizeof(uint16_t);
      type.elementType =
          std::make_shared<NativeApiType>(
              parseMetadataEngineType(metadata, offset, bridge));
      auto ffiOwner = std::make_shared<NativeApiFfiType>();
#if defined(FFI_TYPE_EXT_VECTOR)
      ffiOwner->type.type =
          kind == metagen::mdTypeComplex ? FFI_TYPE_COMPLEX : FFI_TYPE_EXT_VECTOR;
#else
      ffiOwner->type.type =
          kind == metagen::mdTypeComplex ? FFI_TYPE_COMPLEX : FFI_TYPE_STRUCT;
#endif
      ffi_type* elementFfiType = type.elementType->ffiType != nullptr
                                     ? type.elementType->ffiType
                                     : &ffi_type_float;
      size_t lanes = std::max<size_t>(type.arraySize, 1);
      size_t abiLanes = lanes == 3 ? 4 : lanes;
      size_t elementSize = std::max<size_t>(elementFfiType->size, sizeof(float));
      size_t elementAlignment =
          std::max<size_t>(elementFfiType->alignment, static_cast<size_t>(1));
      ffiOwner->elements.reserve(abiLanes + 1);
      for (size_t i = 0; i < abiLanes; i++) {
        ffiOwner->elements.push_back(elementFfiType);
      }
      ffiOwner->finalize();
      size_t vectorAlignment = elementAlignment;
      if (kind != metagen::mdTypeComplex) {
        size_t packedSize = abiLanes * elementSize;
        size_t preferredAlignment = packedSize >= 16 ? 16 : packedSize;
        vectorAlignment = std::max(vectorAlignment, preferredAlignment);
      }
      vectorAlignment = std::min<size_t>(vectorAlignment, 16);
      ffiOwner->type.alignment = static_cast<unsigned short>(vectorAlignment);
      ffiOwner->type.size = alignUp(abiLanes * elementSize, vectorAlignment);
      type.ownedFfiType = ffiOwner;
      type.ffiType = &ffiOwner->type;
      type.supported = type.elementType->supported;
      return type;
    }
    case metagen::mdTypeStruct: {
      auto structOffset = metadata->getOffset(*offset);
      *offset += sizeof(MDSectionOffset);
      bool isUnion = (structOffset & metagen::mdSectionOffsetNext) != 0;
      structOffset &= ~metagen::mdSectionOffsetNext;
      if (structOffset == MD_SECTION_OFFSET_NULL || bridge == nullptr) {
        type.kind = metagen::mdTypePointer;
        type.ffiType = &ffi_type_pointer;
        type.supported = true;
        return type;
      }

      MDSectionOffset absoluteOffset =
          structOffset + (isUnion ? metadata->unionsOffset : metadata->structsOffset);
      type.aggregateOffset = absoluteOffset;
      type.aggregateIsUnion = isUnion;
      type.aggregateInfo = bridge->aggregateInfoFor(absoluteOffset, isUnion);
      type.ffiType = type.aggregateInfo != nullptr && type.aggregateInfo->ffi != nullptr
                         ? &type.aggregateInfo->ffi->type
                         : nullptr;
      type.supported = type.ffiType != nullptr;
      return type;
    }
    case metagen::mdTypePointer:
      type.elementType =
          std::make_shared<NativeApiType>(
              parseMetadataEngineType(metadata, offset, bridge));
      type.ffiType = &ffi_type_pointer;
      type.supported = true;
      return type;
    case metagen::mdTypeBlock:
    case metagen::mdTypeFunctionPointer:
      type.signatureOffset = metadata->getOffset(*offset) + metadata->signaturesOffset;
      *offset += sizeof(MDSectionOffset);
      type.ffiType = &ffi_type_pointer;
      type.supported = true;
      return type;
    case metagen::mdTypeClassObject: {
      auto classOffset = metadata->getOffset(*offset);
      *offset += sizeof(MDSectionOffset);
      bool next = (classOffset & metagen::mdSectionOffsetNext) != 0;
      while (next) {
        auto protocolOffset = metadata->getOffset(*offset);
        *offset += sizeof(MDSectionOffset);
        next = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
      }
      break;
    }
    case metagen::mdTypeProtocolObject: {
      bool next = true;
      while (next) {
        auto protocolOffset = metadata->getOffset(*offset);
        *offset += sizeof(MDSectionOffset);
        next = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
      }
      break;
    }
    default:
      break;
  }

  type.ffiType = ffiTypeForEngineKind(kind);
  type.supported = type.ffiType != nullptr && isSupportedEngineKind(kind);
  return type;
}

std::shared_ptr<NativeApiAggregateInfo> NativeApiBridge::aggregateInfoFor(
    MDSectionOffset aggregateOffset, bool isUnion) {
  if (metadata_ == nullptr || aggregateOffset == MD_SECTION_OFFSET_NULL) {
    return nullptr;
  }

  auto cached = aggregateInfoByOffset_.find(aggregateOffset);
  if (cached != aggregateInfoByOffset_.end()) {
    return cached->second;
  }

  auto info = std::make_shared<NativeApiAggregateInfo>();
  info->offset = aggregateOffset;
  info->isUnion = isUnion;
  aggregateInfoByOffset_[aggregateOffset] = info;

  if (aggregateInfoInProgress_.find(aggregateOffset) !=
      aggregateInfoInProgress_.end()) {
    auto ffiOwner = std::make_shared<NativeApiFfiType>();
    ffiOwner->elements.push_back(&ffi_type_pointer);
    ffiOwner->finalize();
    info->ffi = ffiOwner;
    return info;
  }

  aggregateInfoInProgress_.insert(aggregateOffset);

  MDSectionOffset offset = aggregateOffset;
  const char* name = metadata_->getString(offset);
  info->name = name != nullptr ? name : "";
  offset += sizeof(MDSectionOffset);
  info->size = metadata_->getArraySize(offset);
  offset += sizeof(uint16_t);

  bool next = true;
  while (next) {
    MDSectionOffset nameOffset = metadata_->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    next = (nameOffset & metagen::mdSectionOffsetNext) != 0;
    nameOffset &= ~metagen::mdSectionOffsetNext;
    if (nameOffset == MD_SECTION_OFFSET_NULL) {
      break;
    }

    NativeApiAggregateField field;
    const char* fieldName = metadata_->resolveString(nameOffset);
    field.name = fieldName != nullptr ? fieldName : "";
    if (!isUnion) {
      field.offset = metadata_->getArraySize(offset);
      offset += sizeof(uint16_t);
    }
    field.type = parseMetadataEngineType(metadata_.get(), &offset, this);
    info->fields.push_back(std::move(field));
  }

  auto ffiOwner = std::make_shared<NativeApiFfiType>();
  if (isUnion) {
    ffi_type* largest = &ffi_type_uint8;
    size_t largestSize = 0;
    for (const auto& field : info->fields) {
      size_t fieldSize = nativeSizeForType(field.type);
      if (field.type.ffiType != nullptr && fieldSize >= largestSize) {
        largest = field.type.ffiType;
        largestSize = fieldSize;
      }
    }
    ffiOwner->elements.push_back(largest);
  } else {
    for (const auto& field : info->fields) {
      ffiOwner->elements.push_back(field.type.ffiType != nullptr
                                       ? field.type.ffiType
                                       : &ffi_type_pointer);
    }
    if (ffiOwner->elements.empty()) {
      ffiOwner->elements.push_back(&ffi_type_uint8);
    }
  }
  ffiOwner->finalize();
  info->ffi = ffiOwner;
  aggregateInfoInProgress_.erase(aggregateOffset);
  return info;
}

ffi_type* ffiTypeForEngineArgument(const NativeApiType& type) {
  switch (type.kind) {
    case metagen::mdTypeArray:
      return &ffi_type_pointer;
    default:
      return type.ffiType != nullptr ? type.ffiType : &ffi_type_pointer;
  }
}

std::optional<NativeApiSignature> parseMetadataEngineSignature(
    MDMetadataReader* metadata, MDSectionOffset signatureOffset,
    unsigned int implicitArgumentCount, NativeApiBridge* bridge,
    bool returnOwned = false) {
  if (metadata == nullptr || signatureOffset == MD_SECTION_OFFSET_NULL) {
    return std::nullopt;
  }

  NativeApiSignature signature;
  signature.implicitArgumentCount = implicitArgumentCount;
  signature.signatureHash = isPreparedGeneratedDispatchRequired()
      ? metadataSignatureHash(metadata, signatureOffset)
      : 0;
  signature.dispatchFlags = returnOwned ? 1 : 0;

  MDSectionOffset offset = signatureOffset;
  MDTypeKind returnKind = metadata->getTypeKind(offset);
  uint32_t returnKindRaw = rawTypeKind(returnKind);
  bool next =
      (returnKindRaw & static_cast<uint32_t>(metagen::mdTypeFlagNext)) != 0;
  signature.variadic =
      (returnKindRaw & static_cast<uint32_t>(metagen::mdTypeFlagVariadic)) != 0;
  signature.returnType = parseMetadataEngineType(metadata, &offset, bridge);
  signature.returnType.returnOwned = returnOwned;

  while (next) {
    MDTypeKind argKind = metadata->getTypeKind(offset);
    next = (rawTypeKind(argKind) &
            static_cast<uint32_t>(metagen::mdTypeFlagNext)) != 0;
    signature.argumentTypes.push_back(parseMetadataEngineType(metadata, &offset, bridge));
  }

  signature.ffiTypes.reserve(signature.argumentTypes.size() +
                             implicitArgumentCount);
  for (unsigned int i = 0; i < implicitArgumentCount; i++) {
    signature.ffiTypes.push_back(&ffi_type_pointer);
  }
  for (const auto& argType : signature.argumentTypes) {
    signature.ffiTypes.push_back(ffiTypeForEngineArgument(argType));
  }

  ffi_status status = ffi_prep_cif(
      &signature.cif, FFI_DEFAULT_ABI,
      static_cast<unsigned int>(signature.ffiTypes.size()),
      signature.returnType.ffiType != nullptr ? signature.returnType.ffiType
                                              : &ffi_type_void,
      signature.ffiTypes.empty() ? nullptr : signature.ffiTypes.data());
  signature.prepared = status == FFI_OK;
  return signature;
}

bool prepareEngineCallbackSignature(NativeApiSignature* signature) {
  if (signature == nullptr) {
    return false;
  }

  signature->ffiTypes.clear();
  signature->ffiTypes.reserve(signature->argumentTypes.size() +
                              signature->implicitArgumentCount);
  for (unsigned int i = 0; i < signature->implicitArgumentCount; i++) {
    signature->ffiTypes.push_back(&ffi_type_pointer);
  }
  for (const auto& argType : signature->argumentTypes) {
    signature->ffiTypes.push_back(ffiTypeForEngineArgument(argType));
  }

  ffi_status status = ffi_prep_cif(
      &signature->cif, FFI_DEFAULT_ABI,
      static_cast<unsigned int>(signature->ffiTypes.size()),
      signature->returnType.ffiType != nullptr ? signature->returnType.ffiType
                                               : &ffi_type_void,
      signature->ffiTypes.empty() ? nullptr : signature->ffiTypes.data());
  signature->prepared = status == FFI_OK;
  return signature->prepared;
}

const char* skipObjCTypeQualifiers(const char* encoding) {
  while (encoding != nullptr && *encoding != '\0' &&
         std::strchr("rnNoORV", *encoding) != nullptr) {
    encoding++;
  }
  return encoding;
}

const char* skipObjCTypeFrameOffset(const char* encoding) {
  while (encoding != nullptr && *encoding >= '0' && *encoding <= '9') {
    encoding++;
  }
  return encoding;
}

const char* skipObjCTypeFieldName(const char* encoding, std::string* name) {
  if (encoding == nullptr || *encoding != '"') {
    return encoding;
  }

  encoding++;
  const char* start = encoding;
  while (*encoding != '\0' && *encoding != '"') {
    encoding++;
  }
  if (name != nullptr) {
    *name = std::string(start, static_cast<size_t>(encoding - start));
  }
  return *encoding == '"' ? encoding + 1 : encoding;
}

std::string normalizedObjCAggregateName(std::string name) {
  if (!name.empty() && name.front() == '_') {
    name.erase(name.begin());
  }
  return name;
}

std::vector<std::string> knownObjCAggregateFieldNames(
    const std::string& aggregateName, size_t fieldCount) {
  std::string name = normalizedObjCAggregateName(aggregateName);
  std::vector<std::string> fields;
  if (name == "CGPoint" || name == "NSPoint") {
    fields = {"x", "y"};
  } else if (name == "CGSize" || name == "NSSize") {
    fields = {"width", "height"};
  } else if (name == "CGRect" || name == "NSRect") {
    fields = {"origin", "size"};
  } else if (name == "CGVector") {
    fields = {"dx", "dy"};
  } else if (name == "UIEdgeInsets" || name == "NSEdgeInsets") {
    fields = {"top", "left", "bottom", "right"};
  } else if (name == "NSDirectionalEdgeInsets") {
    fields = {"top", "leading", "bottom", "trailing"};
  } else if (name == "NSRange" || name == "CFRange") {
    fields = {"location", "length"};
  } else if (name == "CGAffineTransform") {
    fields = {"a", "b", "c", "d", "tx", "ty"};
  } else if (name == "CATransform3D") {
    fields = {"m11", "m12", "m13", "m14", "m21", "m22", "m23", "m24",
              "m31", "m32", "m33", "m34", "m41", "m42", "m43", "m44"};
  }

  if (fields.size() != fieldCount) {
    fields.clear();
  }
  return fields;
}

const NativeApiSymbol* findObjCAggregateSymbol(
    NativeApiBridge* bridge, const std::string& name, bool isUnion) {
  if (bridge == nullptr || name.empty()) {
    return nullptr;
  }

  std::vector<std::string> candidates;
  candidates.push_back(name);
  std::string normalized = normalizedObjCAggregateName(name);
  if (normalized != name) {
    candidates.push_back(normalized);
  } else {
    candidates.push_back("_" + name);
  }
  constexpr const char* suffix = "Struct";
  if (normalized.size() > std::strlen(suffix) &&
      normalized.compare(normalized.size() - std::strlen(suffix),
                         std::strlen(suffix), suffix) == 0) {
    candidates.push_back(
        normalized.substr(0, normalized.size() - std::strlen(suffix)));
  } else {
    candidates.push_back(normalized + suffix);
  }

  for (const auto& candidate : candidates) {
    const NativeApiSymbol* symbol =
        isUnion ? bridge->findUnion(candidate) : bridge->findStruct(candidate);
    if (symbol == nullptr) {
      symbol = bridge->findAggregate(candidate);
    }
    if (symbol != nullptr) {
      return symbol;
    }
  }

  return nullptr;
}

void applyObjCEncodingSizeAndAlignment(const char* encoding,
                                       NativeApiFfiType* ffiType,
                                       uint16_t* sizeOut = nullptr) {
  if (encoding == nullptr || ffiType == nullptr) {
    return;
  }

  NSUInteger size = 0;
  NSUInteger alignment = 0;
  NSGetSizeAndAlignment(encoding, &size, &alignment);
  if (size > 0) {
    ffiType->type.size = static_cast<size_t>(size);
    if (sizeOut != nullptr) {
      *sizeOut = static_cast<uint16_t>(std::min<NSUInteger>(
          size, static_cast<NSUInteger>(std::numeric_limits<uint16_t>::max())));
    }
  }
  if (alignment > 0) {
    ffiType->type.alignment = static_cast<unsigned short>(alignment);
  }
}

NativeApiType parseObjCEncodedEngineType(
    const char* encoding, NativeApiBridge* bridge = nullptr,
    const char** endEncoding = nullptr);

bool unsupportedEngineType(const NativeApiType& type);

NativeApiType parseObjCEncodedAggregateEngineType(
    const char* encoding, NativeApiBridge* bridge, const char** endEncoding) {
  NativeApiType type;
  type.kind = metagen::mdTypeStruct;

  const bool isUnion = *encoding == '(';
  const char close = isUnion ? ')' : '}';
  const char* cursor = encoding + 1;
  const char* nameStart = cursor;
  while (*cursor != '\0' && *cursor != '=' && *cursor != close) {
    cursor++;
  }
  std::string aggregateName(nameStart, static_cast<size_t>(cursor - nameStart));

  if (const NativeApiSymbol* symbol =
          findObjCAggregateSymbol(bridge, aggregateName, isUnion)) {
    type.aggregateOffset = symbol->offset;
    type.aggregateIsUnion = symbol->kind == NativeApiSymbolKind::Union;
    type.aggregateInfo = bridge->aggregateInfoFor(*symbol);
    type.ffiType = type.aggregateInfo != nullptr && type.aggregateInfo->ffi != nullptr
                       ? &type.aggregateInfo->ffi->type
                       : nullptr;
    type.supported = type.ffiType != nullptr;

    int depth = 0;
    const char* end = encoding;
    do {
      if (*end == *encoding) {
        depth++;
      } else if (*end == close) {
        depth--;
      }
      end++;
    } while (*end != '\0' && depth > 0);
    if (endEncoding != nullptr) {
      *endEncoding = end;
    }
    return type;
  }

  auto info = std::make_shared<NativeApiAggregateInfo>();
  info->name = aggregateName;
  info->isUnion = isUnion;
  info->offset = MD_SECTION_OFFSET_NULL;

  if (*cursor == '=') {
    cursor++;
  }

  size_t computedOffset = 0;
  size_t maxFieldSize = 0;
  size_t fieldIndex = 0;
  while (*cursor != '\0' && *cursor != close) {
    NativeApiAggregateField field;
    std::string encodedFieldName;
    cursor = skipObjCTypeFieldName(cursor, &encodedFieldName);
    const char* fieldStart = cursor;
    const char* fieldEnd = cursor;
    field.type = parseObjCEncodedEngineType(cursor, bridge, &fieldEnd);
    if (fieldEnd == fieldStart || unsupportedEngineType(field.type)) {
      type.supported = false;
      type.ffiType = nullptr;
      if (endEncoding != nullptr) {
        *endEncoding = fieldEnd;
      }
      return type;
    }

    NSUInteger fieldSize = 0;
    NSUInteger fieldAlignment = 0;
    @try {
      NSGetSizeAndAlignment(fieldStart, &fieldSize, &fieldAlignment);
    } @catch (NSException*) {
      // Some valid method encodings contain standalone bitfields, which
      // Foundation refuses to size. Mark the runtime-derived aggregate as
      // unsupported so metadata remains authoritative instead of leaking an
      // Objective-C exception through the engine host-object boundary.
      type.supported = false;
      type.ffiType = nullptr;
      if (endEncoding != nullptr) {
        *endEncoding = fieldEnd;
      }
      return type;
    }
    size_t nativeFieldSize =
        fieldSize > 0 ? static_cast<size_t>(fieldSize)
                      : nativeSizeForType(field.type);
    size_t nativeFieldAlignment =
        fieldAlignment > 0 ? static_cast<size_t>(fieldAlignment)
                           : std::max<size_t>(1, field.type.ffiType != nullptr
                                                     ? field.type.ffiType->alignment
                                                     : 1);
    if (isUnion) {
      field.offset = 0;
      maxFieldSize = std::max(maxFieldSize, nativeFieldSize);
    } else {
      computedOffset = alignUp(computedOffset, nativeFieldAlignment);
      field.offset = static_cast<uint16_t>(std::min<size_t>(
          computedOffset, std::numeric_limits<uint16_t>::max()));
      computedOffset += nativeFieldSize;
    }
    field.name = !encodedFieldName.empty()
                     ? encodedFieldName
                     : "field" + std::to_string(fieldIndex);
    info->fields.push_back(std::move(field));
    fieldIndex++;
    cursor = fieldEnd;
  }

  if (*cursor == close) {
    cursor++;
  }
  if (endEncoding != nullptr) {
    *endEncoding = cursor;
  }

  auto knownNames = knownObjCAggregateFieldNames(aggregateName, info->fields.size());
  for (size_t i = 0; i < knownNames.size(); i++) {
    info->fields[i].name = knownNames[i];
  }

  auto ffiOwner = std::make_shared<NativeApiFfiType>();
  if (isUnion) {
    ffi_type* largest = &ffi_type_uint8;
    size_t largestSize = 0;
    for (const auto& field : info->fields) {
      size_t fieldSize = nativeSizeForType(field.type);
      if (field.type.ffiType != nullptr && fieldSize >= largestSize) {
        largest = field.type.ffiType;
        largestSize = fieldSize;
      }
    }
    ffiOwner->elements.push_back(largest);
  } else {
    for (const auto& field : info->fields) {
      ffiOwner->elements.push_back(field.type.ffiType != nullptr
                                       ? field.type.ffiType
                                       : &ffi_type_pointer);
    }
  }
  if (ffiOwner->elements.empty()) {
    ffiOwner->elements.push_back(&ffi_type_uint8);
  }
  ffiOwner->finalize();
  applyObjCEncodingSizeAndAlignment(encoding, ffiOwner.get(), &info->size);
  if (info->size == 0) {
    info->size = static_cast<uint16_t>(std::min<size_t>(
        isUnion ? maxFieldSize : computedOffset,
        std::numeric_limits<uint16_t>::max()));
  }

  info->ffi = ffiOwner;
  type.aggregateInfo = info;
  type.aggregateOffset = MD_SECTION_OFFSET_NULL;
  type.aggregateIsUnion = isUnion;
  type.ownedFfiType = ffiOwner;
  type.ffiType = &ffiOwner->type;
  type.supported = true;
  return type;
}

NativeApiType parseObjCEncodedArrayEngineType(
    const char* encoding, NativeApiBridge* bridge, const char** endEncoding) {
  NativeApiType type;
  type.kind = metagen::mdTypeArray;

  const char* cursor = encoding + 1;
  uint16_t count = 0;
  while (*cursor >= '0' && *cursor <= '9') {
    count = static_cast<uint16_t>(
        std::min<int>(std::numeric_limits<uint16_t>::max(),
                      (count * 10) + (*cursor - '0')));
    cursor++;
  }
  type.arraySize = count;

  const char* elementEnd = cursor;
  type.elementType = std::make_shared<NativeApiType>(
      parseObjCEncodedEngineType(cursor, bridge, &elementEnd));
  cursor = elementEnd;
  if (*cursor == ']') {
    cursor++;
  }
  if (endEncoding != nullptr) {
    *endEncoding = cursor;
  }

  auto ffiOwner = std::make_shared<NativeApiFfiType>();
  ffi_type* elementFfiType =
      type.elementType != nullptr && type.elementType->ffiType != nullptr
          ? type.elementType->ffiType
          : &ffi_type_pointer;
  for (uint16_t i = 0; i < count; i++) {
    ffiOwner->elements.push_back(elementFfiType);
  }
  if (ffiOwner->elements.empty()) {
    ffiOwner->elements.push_back(&ffi_type_uint8);
  }
  ffiOwner->finalize();
  applyObjCEncodingSizeAndAlignment(encoding, ffiOwner.get());

  type.ownedFfiType = ffiOwner;
  type.ffiType = &ffiOwner->type;
  type.supported = type.elementType != nullptr && type.elementType->supported;
  return type;
}

NativeApiType parseObjCEncodedEngineType(
    const char* encoding, NativeApiBridge* bridge, const char** endEncoding) {
  encoding = skipObjCTypeQualifiers(encoding);
  NativeApiType type;

  if (encoding == nullptr || *encoding == '\0') {
    type.kind = metagen::mdTypePointer;
    type.ffiType = &ffi_type_pointer;
    if (endEncoding != nullptr) {
      *endEncoding = encoding;
    }
    return type;
  }

  auto finishPrimitive = [&](const char* end) {
    type.ffiType = ffiTypeForEngineKind(type.kind);
    type.supported = type.ffiType != nullptr;
    if (endEncoding != nullptr) {
      *endEncoding = end;
    }
    return type;
  };

  switch (*encoding) {
    case 'c':
      type.kind = metagen::mdTypeChar;
      break;
    case 'i':
      type.kind = metagen::mdTypeSInt;
      break;
    case 's':
      type.kind = metagen::mdTypeSShort;
      break;
    case 'l':
    case 'q':
      type.kind = metagen::mdTypeSInt64;
      break;
    case 'C':
      type.kind = metagen::mdTypeUInt8;
      break;
    case 'I':
      type.kind = metagen::mdTypeUInt;
      break;
    case 'S':
      type.kind = metagen::mdTypeUShort;
      break;
    case 'L':
    case 'Q':
      type.kind = metagen::mdTypeUInt64;
      break;
    case 'f':
      type.kind = metagen::mdTypeFloat;
      break;
    case 'd':
      type.kind = metagen::mdTypeDouble;
      break;
    case 'B':
      type.kind = metagen::mdTypeBool;
      break;
    case 'v':
      type.kind = metagen::mdTypeVoid;
      break;
    case '*':
      type.kind = metagen::mdTypeString;
      break;
    case '@':
      if (encoding[1] == '?') {
        type.kind = metagen::mdTypeBlock;
        return finishPrimitive(encoding + 2);
      }
      {
        const char* objectEnd = encoding + 1;
        if (*objectEnd == '"') {
          objectEnd++;
          while (*objectEnd != '\0' && *objectEnd != '"') {
            objectEnd++;
          }
          if (*objectEnd == '"') {
            objectEnd++;
          }
        }
        if (std::strncmp(encoding, "@\"NSString\"", 11) == 0) {
          type.kind = metagen::mdTypeNSStringObject;
        } else if (std::strncmp(encoding, "@\"NSMutableString\"", 18) == 0) {
          type.kind = metagen::mdTypeNSMutableStringObject;
        } else {
          type.kind = metagen::mdTypeAnyObject;
        }
        return finishPrimitive(objectEnd);
      }
    case '#':
      type.kind = metagen::mdTypeClass;
      break;
    case ':':
      type.kind = metagen::mdTypeSelector;
      break;
    case '^':
      type.kind = metagen::mdTypePointer;
      {
        const char* elementEnd = encoding + 1;
        type.elementType = std::make_shared<NativeApiType>(
            parseObjCEncodedEngineType(encoding + 1, bridge, &elementEnd));
        type.ffiType = &ffi_type_pointer;
        type.supported = true;
        if (elementEnd == encoding + 1 && encoding[1] != '\0') {
          elementEnd = encoding + 2;
        }
        if (endEncoding != nullptr) {
          *endEncoding = elementEnd;
        }
      }
      return type;
    case '{':
    case '(':
      return parseObjCEncodedAggregateEngineType(encoding, bridge, endEncoding);
    case '[':
      return parseObjCEncodedArrayEngineType(encoding, bridge, endEncoding);
    case 'b': {
      type.kind = metagen::mdTypeUInt;
      const char* cursor = encoding + 1;
      while (*cursor >= '0' && *cursor <= '9') {
        cursor++;
      }
      return finishPrimitive(cursor);
    }
    case '?':
      type.kind = metagen::mdTypeOpaquePointer;
      break;
    default:
      type.kind = metagen::mdTypePointer;
      break;
  }

  return finishPrimitive(encoding + 1);
}

std::optional<NativeApiSignature> parseObjCCallbackEngineSignature(
    const std::string& encodingString, bool block, NativeApiBridge* bridge) {
  const char* cursor = skipObjCTypeQualifiers(encodingString.c_str());
  if (cursor == nullptr || *cursor == '\0') {
    return std::nullopt;
  }

  NativeApiSignature signature;
  signature.implicitArgumentCount = block ? 1 : 0;

  const char* returnEnd = cursor;
  signature.returnType = parseObjCEncodedEngineType(cursor, bridge, &returnEnd);
  if (returnEnd == cursor) {
    return std::nullopt;
  }
  cursor = skipObjCTypeFrameOffset(returnEnd);

  if (block) {
    const char* blockSelf = skipObjCTypeQualifiers(cursor);
    if (blockSelf != nullptr && blockSelf[0] == '@' && blockSelf[1] == '?') {
      cursor = skipObjCTypeFrameOffset(blockSelf + 2);
    }
  }

  while (cursor != nullptr && *cursor != '\0') {
    const char* argStart = skipObjCTypeQualifiers(cursor);
    if (argStart == nullptr || *argStart == '\0') {
      break;
    }
    const char* argEnd = argStart;
    NativeApiType argType = parseObjCEncodedEngineType(argStart, bridge, &argEnd);
    if (argEnd == argStart) {
      return std::nullopt;
    }
    signature.argumentTypes.push_back(std::move(argType));
    cursor = skipObjCTypeFrameOffset(argEnd);
  }

  prepareEngineCallbackSignature(&signature);
  return signature;
}

std::optional<NativeApiSignature> parseObjCMethodEngineSignature(
    Method method, NativeApiBridge* bridge = nullptr) {
  if (method == nullptr) {
    return std::nullopt;
  }

  NativeApiSignature signature;
  signature.implicitArgumentCount = 2;

  char* returnEncoding = method_copyReturnType(method);
  signature.returnType = parseObjCEncodedEngineType(returnEncoding, bridge);
  if (returnEncoding != nullptr) {
    free(returnEncoding);
  }

  unsigned int totalArgc = method_getNumberOfArguments(method);
  for (unsigned int i = 2; i < totalArgc; i++) {
    char* argEncoding = method_copyArgumentType(method, i);
    signature.argumentTypes.push_back(parseObjCEncodedEngineType(argEncoding, bridge));
    if (argEncoding != nullptr) {
      free(argEncoding);
    }
  }

  signature.ffiTypes.reserve(totalArgc);
  signature.ffiTypes.push_back(&ffi_type_pointer);
  signature.ffiTypes.push_back(&ffi_type_pointer);
  for (const auto& argType : signature.argumentTypes) {
    signature.ffiTypes.push_back(ffiTypeForEngineArgument(argType));
  }

  ffi_status status = ffi_prep_cif(
      &signature.cif, FFI_DEFAULT_ABI,
      static_cast<unsigned int>(signature.ffiTypes.size()),
      signature.returnType.ffiType != nullptr ? signature.returnType.ffiType
                                              : &ffi_type_void,
      signature.ffiTypes.data());
  signature.prepared = status == FFI_OK;
  return signature;
}

bool prepareEngineMethodSignature(NativeApiSignature* signature) {
  if (signature == nullptr) {
    return false;
  }
  signature->implicitArgumentCount = 2;
  signature->ffiTypes.clear();
  signature->ffiTypes.reserve(signature->argumentTypes.size() + 2);
  signature->ffiTypes.push_back(&ffi_type_pointer);
  signature->ffiTypes.push_back(&ffi_type_pointer);
  for (const auto& argType : signature->argumentTypes) {
    ffi_type* ffiType = ffiTypeForEngineArgument(argType);
    if (ffiType == nullptr) {
      signature->prepared = false;
      return false;
    }
    signature->ffiTypes.push_back(ffiType);
  }
  ffi_type* returnFfiType =
      signature->returnType.ffiType != nullptr ? signature->returnType.ffiType
                                               : &ffi_type_void;
  signature->prepared =
      ffi_prep_cif(&signature->cif, FFI_DEFAULT_ABI,
                   static_cast<unsigned int>(signature->ffiTypes.size()),
                   returnFfiType, signature->ffiTypes.data()) == FFI_OK;
  return signature->prepared;
}

bool reconcileObjCMethodRuntimeType(NativeApiType* metadataType,
                                    const NativeApiType& runtimeType,
                                    bool* abiChanged) {
  if (metadataType == nullptr || unsupportedEngineType(runtimeType)) {
    return false;
  }

  if (runtimeType.kind == metagen::mdTypeBlock &&
      metadataType->kind == metagen::mdTypeFunctionPointer) {
    metadataType->kind = metagen::mdTypeBlock;
    metadataType->ffiType = runtimeType.ffiType;
    metadataType->supported = runtimeType.supported;
    return true;
  }

  // Do not overwrite aggregate (struct/union) metadata types with the
  // anonymous ObjC runtime encoding: the metadata type carries the real
  // field names and layout that the runtime encoding (e.g. "{?=qqq}") lacks.
  (void)abiChanged;
  return false;
}

bool reconcileObjCMethodRuntimeSignature(NativeApiSignature* signature,
                                         const NativeApiSignature& runtime) {
  if (signature == nullptr ||
      signature->argumentTypes.size() != runtime.argumentTypes.size()) {
    return false;
  }

  bool changed = false;
  bool abiChanged = false;
  changed |= reconcileObjCMethodRuntimeType(&signature->returnType,
                                            runtime.returnType, &abiChanged);
  for (size_t i = 0; i < signature->argumentTypes.size(); i++) {
    changed |= reconcileObjCMethodRuntimeType(&signature->argumentTypes[i],
                                              runtime.argumentTypes[i],
                                              &abiChanged);
  }

  if (abiChanged) {
    signature->signatureHash = 0;
  }
  return !changed || prepareEngineMethodSignature(signature);
}

bool unsupportedEngineType(const NativeApiType& type) {
  if (type.kind == metagen::mdTypeStruct && type.aggregateInfo != nullptr &&
      type.aggregateInfo->ffi != nullptr) {
    return false;
  }
  return !type.supported || type.ffiType == nullptr;
}

bool signatureSupportedForEngineCallback(const NativeApiSignature& signature) {
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

std::shared_ptr<NativeApiCallback> createEngineCallback(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, Function function, bool block,
    NativeApiCallbackThreadPolicy threadPolicy =
        NativeApiCallbackThreadPolicy::Default) {
  if (bridge == nullptr || bridge->metadata() == nullptr ||
      type.signatureOffset == MD_SECTION_OFFSET_NULL) {
    throw JSError(
        runtime, "Native callback metadata is unavailable.");
  }

  auto parsed = parseMetadataEngineSignature(
      bridge->metadata(), type.signatureOffset, block ? 1 : 0, bridge.get());
  if (!parsed || !signatureSupportedForEngineCallback(*parsed)) {
    throw JSError(
        runtime, "Native callback signature is not supported by backend.");
  }

  auto signature =
      std::make_shared<NativeApiSignature>(std::move(*parsed));
  uintptr_t roundTripValidationKey =
      NativeApiBridge::callbackRoundTripValidationKey(type);
  auto callback = std::make_shared<NativeApiCallback>(
      runtime, bridge, std::move(signature), std::move(function), block,
      threadPolicy, false, roundTripValidationKey);
  if (block) {
    callback->retainInitialBlockLifetime(callback);
  } else {
    bridge->retainEngineLifetime(callback);
  }
  return callback;
}

std::shared_ptr<NativeApiCallback> createEngineCallback(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const std::string& objcSignatureEncoding, Function function, bool block,
    NativeApiCallbackThreadPolicy threadPolicy =
        NativeApiCallbackThreadPolicy::Default,
    uintptr_t roundTripValidationKey = 0) {
  if (bridge == nullptr || objcSignatureEncoding.empty()) {
    throw JSError(runtime, "Native callback encoding is unavailable.");
  }

  auto parsed = parseObjCCallbackEngineSignature(
      objcSignatureEncoding, block, bridge.get());
  if (!parsed || !signatureSupportedForEngineCallback(*parsed)) {
    throw JSError(
        runtime, "Native callback signature is not supported by backend.");
  }

  auto signature =
      std::make_shared<NativeApiSignature>(std::move(*parsed));
  auto callback = std::make_shared<NativeApiCallback>(
      runtime, bridge, std::move(signature), std::move(function), block,
      threadPolicy, false, roundTripValidationKey);
  if (block) {
    callback->retainInitialBlockLifetime(callback);
  } else {
    bridge->retainEngineLifetime(callback);
  }
  return callback;
}

std::shared_ptr<NativeApiCallback> createEngineMethodCallback(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const std::string& selectorName, MDSectionOffset signatureOffset,
    Function function, bool returnOwned, Class methodBaseClass = Nil,
    NativeApiMethodCallbackPolicy methodPolicy = {}) {
  if (bridge == nullptr || bridge->metadata() == nullptr ||
      signatureOffset == MD_SECTION_OFFSET_NULL) {
    throw JSError(
        runtime, "Native method callback metadata is unavailable.");
  }

  auto parsed = parseMetadataEngineSignature(
      bridge->metadata(), signatureOffset, 2, bridge.get(), returnOwned);
  if (!parsed || !signatureSupportedForEngineCallback(*parsed)) {
    throw JSError(
        runtime, "Native method callback signature is not supported by backend.");
  }
  parsed->selectorName = selectorName;

  auto signature =
      std::make_shared<NativeApiSignature>(std::move(*parsed));
  auto threadPolicy = readEngineCallbackThreadPolicy(runtime, function);
  // A policy passed explicitly by the caller (e.g. the auto-installed
  // accessor/construction re-entry guards) wins; otherwise fall back to
  // whatever the JS function itself was tagged with via nativeMethodPolicy().
  if (isEmptyMethodCallbackPolicy(methodPolicy)) {
    methodPolicy = readEngineMethodCallbackPolicy(runtime, function);
  }
  auto callback = std::make_shared<NativeApiCallback>(
      runtime, bridge, std::move(signature), std::move(function), false,
      threadPolicy, true, 0, std::move(methodPolicy), methodBaseClass);
  bridge->retainEngineLifetime(callback);
  return callback;
}

std::shared_ptr<NativeApiCallback> createEngineMethodCallback(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const std::string& selectorName, NativeApiSignature signature,
    Function function, Class methodBaseClass = Nil,
    NativeApiMethodCallbackPolicy methodPolicy = {}) {
  signature.selectorName = selectorName;
  prepareEngineMethodSignature(&signature);
  if (!signatureSupportedForEngineCallback(signature)) {
    throw JSError(
        runtime, "Native method callback signature is not supported by backend.");
  }

  auto sharedSignature =
      std::make_shared<NativeApiSignature>(std::move(signature));
  auto threadPolicy = readEngineCallbackThreadPolicy(runtime, function);
  if (isEmptyMethodCallbackPolicy(methodPolicy)) {
    methodPolicy = readEngineMethodCallbackPolicy(runtime, function);
  }
  auto callback = std::make_shared<NativeApiCallback>(
      runtime, bridge, std::move(sharedSignature), std::move(function), false,
      threadPolicy, true, 0, std::move(methodPolicy), methodBaseClass);
  bridge->retainEngineLifetime(callback);
  return callback;
}
