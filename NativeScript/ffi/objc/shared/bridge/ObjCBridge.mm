thread_local int gSynchronousNativeInvocationDepth = 0;
thread_local int gNativeCallerThreadEngineCallbackDepth = 0;
thread_local std::vector<std::string*> gNativeCallbackExceptionCaptureStack;
std::atomic<int> gActiveSynchronousNativeInvocationDepth{0};

class ScopedNativeApiSynchronousInvocation final {
 public:
  ScopedNativeApiSynchronousInvocation() {
    gSynchronousNativeInvocationDepth += 1;
    gActiveSynchronousNativeInvocationDepth.fetch_add(1,
                                                      std::memory_order_acq_rel);
  }

  ~ScopedNativeApiSynchronousInvocation() {
    gSynchronousNativeInvocationDepth -= 1;
    gActiveSynchronousNativeInvocationDepth.fetch_sub(1,
                                                      std::memory_order_acq_rel);
  }
};

class ScopedNativeCallerThreadEngineCallback final {
 public:
  ScopedNativeCallerThreadEngineCallback() {
    gNativeCallerThreadEngineCallbackDepth += 1;
  }

  ~ScopedNativeCallerThreadEngineCallback() {
    gNativeCallerThreadEngineCallbackDepth -= 1;
  }

  ScopedNativeCallerThreadEngineCallback(
      const ScopedNativeCallerThreadEngineCallback&) = delete;
  ScopedNativeCallerThreadEngineCallback& operator=(
      const ScopedNativeCallerThreadEngineCallback&) = delete;
};

class ScopedNativeCallbackExceptionCapture final {
 public:
  explicit ScopedNativeCallbackExceptionCapture(std::string* message)
      : message_(message) {
    gNativeCallbackExceptionCaptureStack.push_back(message_);
  }

  ~ScopedNativeCallbackExceptionCapture() {
    if (!gNativeCallbackExceptionCaptureStack.empty() &&
        gNativeCallbackExceptionCaptureStack.back() == message_) {
      gNativeCallbackExceptionCaptureStack.pop_back();
    }
  }

  ScopedNativeCallbackExceptionCapture(
      const ScopedNativeCallbackExceptionCapture&) = delete;
  ScopedNativeCallbackExceptionCapture& operator=(
      const ScopedNativeCallbackExceptionCapture&) = delete;

 private:
  std::string* message_ = nullptr;
};

bool recordNativeCallbackException(const std::string& message) {
  if (gNativeCallbackExceptionCaptureStack.empty()) {
    return false;
  }

  std::string* captured = gNativeCallbackExceptionCaptureStack.back();
  if (captured == nullptr) {
    return false;
  }

  if (captured->empty()) {
    *captured = message;
  }
  return true;
}

template <typename Invocation>
void performNativeInvocation(Runtime& runtime,
                             const std::function<void(std::function<void()>)>&
                                 invoker,
                             Invocation&& invocation) {
  NSString* exceptionDescription = nil;
  std::string callbackException;
  auto run = [&]() {
    ScopedNativeApiSynchronousInvocation synchronousInvocation;
    ScopedNativeCallbackExceptionCapture callbackExceptionCapture(
        &callbackException);
    @try {
      invocation();
    } @catch (NSException* exception) {
      exceptionDescription = [exception.description copy];
    }
  };

  bool skipInvoker = gNativeCallerThreadEngineCallbackDepth > 0;
  if (invoker && !skipInvoker) {
    invoker(run);
  } else {
    run();
  }

  if (exceptionDescription != nil) {
    std::string message = exceptionDescription.UTF8String ?: "";
    [exceptionDescription release];
    throw JSError(runtime, message);
  }
  if (!callbackException.empty()) {
    throw JSError(runtime, callbackException);
  }
}

template <typename Invocation>
void performDirectObjCInvocation(Runtime& runtime, Invocation&& invocation) {
  NSString* exceptionDescription = nil;
  auto run = [&]() {
    @try {
      invocation();
    } @catch (NSException* exception) {
      exceptionDescription = [exception.description copy];
    }
  };

  run();

  if (exceptionDescription != nil) {
    std::string message = exceptionDescription.UTF8String ?: "";
    [exceptionDescription release];
    throw JSError(runtime, message);
  }
}

enum class NativeApiSymbolKind {
  Class,
  Function,
  Constant,
  Protocol,
  Enum,
  Struct,
  Union,
};

struct NativeApiSymbol {
  NativeApiSymbolKind kind;
  MDSectionOffset offset = 0;
  MDSectionOffset superclassOffset = MD_SECTION_OFFSET_NULL;
  std::string name;
  std::string runtimeName;
};

struct NativeApiMember {
  std::string name;
  std::string selectorName;
  std::string setterSelectorName;
  MDSectionOffset signatureOffset = MD_SECTION_OFFSET_NULL;
  MDSectionOffset setterSignatureOffset = MD_SECTION_OFFSET_NULL;
  MDMemberFlag flags = metagen::mdMemberFlagNull;
  bool property = false;
  bool readonly = false;
};

struct NativeApiSelectorGroupEntry {
  std::string selectorName;
  NativeApiMember member;
  bool hasMember = false;
  bool propertyGetterResolved = false;
  bool propertyGetterCanPrepare = true;
  bool propertyGetterHasAdjustedMember = false;
  std::string propertyGetterSelectorName;
  NativeApiMember propertyGetterMember;
};

struct NativeApiSelectorGroupCallTarget {
  const std::string* selectorName = nullptr;
  const NativeApiMember* member = nullptr;
  bool canPrepare = true;
};

struct NativeApiAggregateInfo;

struct NativeApiFfiType {
  ffi_type type = {};
  std::vector<ffi_type*> elements;

  NativeApiFfiType() {
    type.type = FFI_TYPE_STRUCT;
    type.size = 0;
    type.alignment = 0;
    type.elements = nullptr;
  }

  void finalize() {
    elements.push_back(nullptr);
    type.elements = elements.data();
  }
};

struct NativeApiType {
  MDTypeKind kind = metagen::mdTypeVoid;
  ffi_type* ffiType = &ffi_type_void;
  bool supported = true;
  bool returnOwned = false;
  MDSectionOffset signatureOffset = MD_SECTION_OFFSET_NULL;
  MDSectionOffset aggregateOffset = MD_SECTION_OFFSET_NULL;
  bool aggregateIsUnion = false;
  uint16_t arraySize = 0;
  std::shared_ptr<NativeApiType> elementType;
  std::shared_ptr<NativeApiAggregateInfo> aggregateInfo;
  std::shared_ptr<NativeApiFfiType> ownedFfiType;
};

struct NativeApiAggregateField {
  std::string name;
  uint16_t offset = 0;
  NativeApiType type;
};

struct NativeApiAggregateInfo {
  std::string name;
  uint16_t size = 0;
  bool isUnion = false;
  MDSectionOffset offset = MD_SECTION_OFFSET_NULL;
  std::vector<NativeApiAggregateField> fields;
  std::shared_ptr<NativeApiFfiType> ffi;
};

std::string jsifySelector(const char* selector) {
  std::string jsifiedSelector;
  bool nextUpper = false;
  for (const char* c = selector; c != nullptr && *c != '\0'; c++) {
    if (*c == ':') {
      nextUpper = true;
    } else if (nextUpper) {
      jsifiedSelector += static_cast<char>(toupper(*c));
      nextUpper = false;
    } else {
      jsifiedSelector += *c;
    }
  }
  return jsifiedSelector;
}

std::string booleanGetterSelectorForProperty(const std::string& property) {
  if (property.empty()) {
    return property;
  }

  std::string selector = "is";
  selector += static_cast<char>(toupper(property[0]));
  selector += property.substr(1);
  return selector;
}

std::optional<std::string> respondingPropertyGetterSelector(
    id receiver, const std::string& property,
    const std::string& preferredSelector) {
  if (receiver == nil) {
    return std::nullopt;
  }

  auto respondsToSelectorName = [receiver](const std::string& selectorName) {
    return !selectorName.empty() &&
           [receiver respondsToSelector:sel_getUid(selectorName.c_str())];
  };

  if (respondsToSelectorName(preferredSelector)) {
    return preferredSelector;
  }
  if (preferredSelector != property && respondsToSelectorName(property)) {
    return property;
  }

  std::string booleanSelector = booleanGetterSelectorForProperty(property);
  if (booleanSelector != preferredSelector && booleanSelector != property &&
      respondsToSelectorName(booleanSelector)) {
    return booleanSelector;
  }

  return std::nullopt;
}

std::string setterSelectorForProperty(const std::string& property) {
  if (property.empty()) {
    return property;
  }

  std::string selector = "set";
  selector += static_cast<char>(toupper(property[0]));
  selector += property.substr(1);
  selector += ":";
  return selector;
}

size_t selectorArgumentCount(const std::string& selector) {
  return static_cast<size_t>(
      std::count(selector.begin(), selector.end(), ':'));
}

const NativeApiMember* selectMethodMember(
    const std::vector<NativeApiMember>& members, const std::string& property,
    bool staticMethod, size_t argumentCount) {
  for (const auto& member : members) {
    if (member.property || member.name != property) {
      continue;
    }

    bool memberIsStatic = (member.flags & metagen::mdMemberStatic) != 0;
    if (memberIsStatic != staticMethod) {
      continue;
    }

    if (selectorArgumentCount(member.selectorName) == argumentCount) {
      return &member;
    }
  }
  return nullptr;
}

bool hasMethodMember(const std::vector<NativeApiMember>& members,
                     const std::string& property, bool staticMethod) {
  for (const auto& member : members) {
    if (member.property || member.name != property) {
      continue;
    }
    bool memberIsStatic = (member.flags & metagen::mdMemberStatic) != 0;
    if (memberIsStatic == staticMethod) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<std::vector<NativeApiSelectorGroupEntry>>
selectorGroupEntriesForMethod(const std::vector<NativeApiMember>& members,
                              const std::string& property, bool staticMethod) {
  auto selectors = std::make_shared<std::vector<NativeApiSelectorGroupEntry>>();
  for (const auto& member : members) {
    if (member.property || member.name != property || member.selectorName.empty()) {
      continue;
    }

    bool memberIsStatic = (member.flags & metagen::mdMemberStatic) != 0;
    if (memberIsStatic != staticMethod) {
      continue;
    }

    size_t argumentCount = selectorArgumentCount(member.selectorName);
    if (selectors->size() <= argumentCount) {
      selectors->resize(argumentCount + 1);
    }
    if ((*selectors)[argumentCount].selectorName.empty()) {
      (*selectors)[argumentCount].selectorName = member.selectorName;
      (*selectors)[argumentCount].member = member;
      (*selectors)[argumentCount].hasMember = true;
    }

    if (argumentCount > 0 && member.selectorName.size() >= 6 &&
        member.selectorName.compare(member.selectorName.size() - 6, 6,
                                    "error:") == 0) {
      size_t omittedErrorCount = argumentCount - 1;
      if (selectors->size() <= omittedErrorCount) {
        selectors->resize(omittedErrorCount + 1);
      }
      if ((*selectors)[omittedErrorCount].selectorName.empty()) {
        (*selectors)[omittedErrorCount].selectorName = member.selectorName;
        (*selectors)[omittedErrorCount].member = member;
        (*selectors)[omittedErrorCount].hasMember = true;
      }
    }
  }
  return selectors->empty() ? nullptr : selectors;
}

bool selectorGroupCanPrepareSelector(id receiver, Class lookupClass,
                                     bool receiverIsClass,
                                     const std::string& selectorName) {
  if (selectorName.empty()) {
    return false;
  }
  SEL selector = sel_registerName(selectorName.c_str());
  if (receiverIsClass) {
    return lookupClass != Nil &&
           class_getClassMethod(lookupClass, selector) != nullptr;
  }
  if (lookupClass != Nil &&
      class_getInstanceMethod(lookupClass, selector) != nullptr) {
    return true;
  }
  return receiver != nil &&
         class_getInstanceMethod(object_getClass(receiver), selector) != nullptr;
}

std::string selectorGroupPropertyGetterSelector(
    id receiver, Class lookupClass, bool receiverIsClass,
    const NativeApiMember& member) {
  if (selectorGroupCanPrepareSelector(receiver, lookupClass, receiverIsClass,
                                      member.selectorName)) {
    return member.selectorName;
  }
  if (member.selectorName != member.name &&
      selectorGroupCanPrepareSelector(receiver, lookupClass, receiverIsClass,
                                      member.name)) {
    return member.name;
  }

  std::string booleanSelector = booleanGetterSelectorForProperty(member.name);
  if (booleanSelector != member.selectorName && booleanSelector != member.name &&
      selectorGroupCanPrepareSelector(receiver, lookupClass, receiverIsClass,
                                      booleanSelector)) {
    return booleanSelector;
  }

  if (auto responding = respondingPropertyGetterSelector(
          receiver, member.name, member.selectorName)) {
    return *responding;
  }

  return member.selectorName != member.name ? member.name : member.selectorName;
}

NativeApiSelectorGroupCallTarget selectorGroupMemberForCall(
    id receiver, Class lookupClass, bool receiverIsClass,
    NativeApiSelectorGroupEntry& entry, size_t count) {
  if (!entry.hasMember) {
    return {&entry.selectorName, nullptr, true};
  }
  if (count == 0 && entry.member.property) {
    if (!entry.propertyGetterResolved) {
      entry.propertyGetterSelectorName = selectorGroupPropertyGetterSelector(
          receiver, lookupClass, receiverIsClass, entry.member);
      entry.propertyGetterCanPrepare = selectorGroupCanPrepareSelector(
          receiver, lookupClass, receiverIsClass,
          entry.propertyGetterSelectorName);
      if (entry.propertyGetterSelectorName != entry.member.selectorName) {
        entry.propertyGetterMember = entry.member;
        entry.propertyGetterMember.selectorName =
            entry.propertyGetterSelectorName;
        entry.propertyGetterHasAdjustedMember = true;
      }
      entry.propertyGetterResolved = true;
    }
    return {&entry.propertyGetterSelectorName,
            entry.propertyGetterHasAdjustedMember ? &entry.propertyGetterMember
                                                  : &entry.member,
            entry.propertyGetterCanPrepare};
  }
  return {&entry.selectorName, &entry.member, true};
}

inline NativeApiSelectorGroupCallTarget selectorGroupCallTargetForEntry(
    id receiver, Class lookupClass, bool receiverIsClass,
    NativeApiSelectorGroupEntry& entry, size_t count) {
  if (entry.hasMember && (!entry.member.property || count != 0)) {
    return {&entry.selectorName, &entry.member, true};
  }
  return selectorGroupMemberForCall(receiver, lookupClass, receiverIsClass,
                                    entry, count);
}

const NativeApiMember* selectPropertyMember(
    const std::vector<NativeApiMember>& members, const std::string& property,
    bool staticMethod) {
  for (const auto& member : members) {
    if (!member.property || member.name != property) {
      continue;
    }

    bool memberIsStatic = (member.flags & metagen::mdMemberStatic) != 0;
    if (memberIsStatic == staticMethod) {
      return &member;
    }
  }
  return nullptr;
}

const NativeApiMember* selectWritablePropertyMember(
    const std::vector<NativeApiMember>& members, const std::string& property,
    bool staticMethod) {
  const NativeApiMember* propertyMember = nullptr;
  for (const auto& member : members) {
    if (!member.property || member.name != property) {
      continue;
    }

    bool memberIsStatic = (member.flags & metagen::mdMemberStatic) != 0;
    if (memberIsStatic != staticMethod) {
      continue;
    }

    if (propertyMember == nullptr) {
      propertyMember = &member;
    }
    if (!member.readonly && !member.setterSelectorName.empty()) {
      return &member;
    }
  }
  return propertyMember;
}

void skipMetadataEngineType(MDMetadataReader* metadata, MDSectionOffset* offset);
Protocol* lookupProtocolByNativeName(const std::string& name);
struct NativeApiPreparedObjCInvocation;
bool preparedObjCInvocationIsInit(
    const NativeApiPreparedObjCInvocation& prepared);

inline uintptr_t normalizeRuntimePointer(uintptr_t pointer) {
#if INTPTR_MAX == INT64_MAX
  return pointer & 0x0000FFFFFFFFFFFFULL;
#else
  return pointer;
#endif
}

// One bridge is shared by every Runtime that touches the same process (a
// worklet spins up an additional Runtime on its own thread). Expandos are
// per-runtime: a Value created in one Runtime must never be handed back out
// of a different one, so every expando read/write is keyed on the owning
// runtime's identity, not just the native pointer.
uintptr_t runtimeObjectExpandoKey(Runtime& runtime) {
#if defined(TARGET_ENGINE_V8) || defined(TARGET_ENGINE_JSC) || \
    defined(TARGET_ENGINE_QUICKJS)
  return normalizeRuntimePointer(
      reinterpret_cast<uintptr_t>(runtime.identity()));
#else
  return normalizeRuntimePointer(reinterpret_cast<uintptr_t>(&runtime));
#endif
}

class NativeApiBridge {
  struct NativeApiRoundTripValue {
    std::shared_ptr<Value> value;
    bool stringLikeNative = false;
    bool persistBeyondFrame = true;
    uintptr_t validationKey = 0;
  };
#ifdef TARGET_ENGINE_HERMES
  struct NativeApiWeakRoundTripValue {
    std::shared_ptr<WeakObject> value;
    bool stringLikeNative = false;
    uintptr_t validationKey = 0;
  };
#endif
  using NativeApiRoundTripReleaseList =
      std::vector<NativeApiRoundTripValue>;
  using NativeApiRoundTripFrame =
      std::unordered_map<uintptr_t, NativeApiRoundTripValue>;
  using NativeApiRoundTripFrameStack = std::vector<NativeApiRoundTripFrame>;

  static constexpr size_t kRecentRoundTripValueLimit = 2;

 public:
  explicit NativeApiBridge(const NativeApiConfig& config)
      : metadata_(loadMetadata(config)),
        scheduler_(config.scheduler),
        nativeInvocationInvoker_(config.nativeInvocationInvoker),
        nativeCallbackInvoker_(config.nativeCallbackInvoker),
        runtimeCallbackInvoker_(config.runtimeCallbackInvoker),
        jsThreadCallbackInvoker_(config.jsThreadCallbackInvoker),
        jsThreadAsyncCallbackInvoker_(config.jsThreadAsyncCallbackInvoker),
        callbackInvocationAllowed_(config.callbackInvocationAllowed),
        invokeCallbacksOnNativeCallerThread_(
            config.invokeCallbacksOnNativeCallerThread),
        indexRuntimePointers_(config.indexRuntimePointers) {
    selfDl_ = dlopen(nullptr, RTLD_NOW);
    buildSymbolIndexes();
  }

  ~NativeApiBridge() {
    if (selfDl_ != nullptr) {
      dlclose(selfDl_);
    }
  }

  MDMetadataReader* metadata() const { return metadata_.get(); }

  void* selfDl() const { return selfDl_; }

  const NativeApiSymbol* find(const std::string& name) const {
    auto it = symbolsByName_.find(name);
    return it != symbolsByName_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findClass(const std::string& name) const {
    const NativeApiSymbol* symbol = find(name);
    if (symbol != nullptr && symbol->kind == NativeApiSymbolKind::Class) {
      return symbol;
    }
    auto it = classSymbolsByRuntimeName_.find(name);
    return it != classSymbolsByRuntimeName_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findClassByOffset(MDSectionOffset offset) const {
    auto it = classSymbolsByOffset_.find(offset);
    return it != classSymbolsByOffset_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findClassForRuntimeClass(Class cls) const {
    Class current = cls;
    while (current != Nil) {
      const char* name = class_getName(current);
      if (name != nullptr) {
        if (const NativeApiSymbol* symbol = findClass(name)) {
          return symbol;
        }
      }
      current = class_getSuperclass(current);
    }

    // The class-name walk above found nothing at all: not even a distant
    // ancestor (e.g. NSObject) is known to metadata. As a last resort,
    // identify the object by the most specific conformed protocol metadata
    // does know about (see forEachConformedProtocol for the deterministic
    // traversal/tie-break order), so callers that just need *some* symbol to
    // key off (building a class-identity/prototype value, for instance)
    // still get one instead of nullptr.
    //
    // This is the rarer half of the protocol-method-metadata fallback. The
    // common real-world case -- a class walk that *does* resolve to a known
    // symbol (the object's own class, or an ancestor), but whose
    // metadata-baked member table doesn't carry a method declared solely on
    // a protocol the object conforms to only at the ObjC runtime level (a
    // category/class-extension conformance the metadata generator never
    // parsed, or a fully private concrete class such as the one behind
    // UIViewControllerTransitionCoordinator) -- is handled by
    // protocolMembersForRuntimeClass below, which callers consult once a
    // normal member lookup on this symbol comes up empty.
    const NativeApiSymbol* protocolSymbol = nullptr;
    forEachConformedProtocol(cls, [&](Protocol* protocol) {
      auto it = protocolSymbolsByRuntimePointer_.find(
          normalizeRuntimePointer(reinterpret_cast<uintptr_t>(protocol)));
      if (it != protocolSymbolsByRuntimePointer_.end()) {
        protocolSymbol = &it->second;
        return true;
      }
      return false;
    });
    return protocolSymbol;
  }

  // Invokes `visit` for every protocol that `cls` (or an ancestor in its
  // *runtime* class hierarchy) conforms to, until `visit` returns true.
  // class_copyProtocolList only reports protocols adopted directly at the
  // queried class level -- it does not walk superclasses -- so this walks
  // class_getSuperclass itself (mirroring findClassForRuntimeClass's own
  // walk) to collect the full picture.
  //
  // Traversal order (this is the deterministic tie-break rule for a class
  // that conforms to more than one protocol declaring the same selector):
  // the most-derived class in cls's hierarchy is visited first, then each
  // ancestor in turn; within one class's own adopted-protocol list, in the
  // order class_copyProtocolList returns them (stable per compiled binary --
  // it reflects the order protocols were listed in that class's
  // @interface/category/extension); each protocol's own inherited protocols
  // are expanded depth-first immediately after it, ahead of its next
  // sibling. The first protocol in this order that satisfies the caller's
  // `visit` wins. This mirrors the "closest/most-specific declaration wins"
  // rule the class-hierarchy member composition elsewhere in this file
  // already follows (see appendSurfaceMember).
  template <typename Visitor>
  void forEachConformedProtocol(Class cls, Visitor&& visit) const {
    std::unordered_set<Protocol*> visited;
    for (Class current = cls; current != Nil;
        current = class_getSuperclass(current)) {
      unsigned int count = 0;
      Protocol* __unsafe_unretained* protocols =
          class_copyProtocolList(current, &count);
      if (protocols == nullptr) {
        continue;
      }
      bool stop = false;
      for (unsigned int i = 0; i < count && !stop; i++) {
        stop = visitProtocolDepthFirst(protocols[i], visited, visit);
      }
      free(protocols);
      if (stop) {
        return;
      }
    }
  }

  template <typename Visitor>
  bool visitProtocolDepthFirst(Protocol* protocol,
                               std::unordered_set<Protocol*>& visited,
                               Visitor&& visit) const {
    if (protocol == nullptr || !visited.insert(protocol).second) {
      return false;
    }
    if (visit(protocol)) {
      return true;
    }

    unsigned int count = 0;
    Protocol* __unsafe_unretained* inherited =
        protocol_copyProtocolList(protocol, &count);
    if (inherited == nullptr) {
      return false;
    }
    bool stop = false;
    for (unsigned int i = 0; i < count && !stop; i++) {
      stop = visitProtocolDepthFirst(inherited[i], visited, visit);
    }
    free(inherited);
    return stop;
  }

  // Members contributed by protocols `cls` conforms to at the Objective-C
  // runtime level, restricted to protocols metadata knows about. This is
  // the fallback for a selector declared solely on a protocol -- e.g. every
  // method of UIViewControllerTransitionCoordinator -- where
  // findClassForRuntimeClass's plain class_getSuperclass walk resolves to a
  // real, known class symbol (the object's own class, or some ancestor),
  // but that symbol's own member table doesn't carry the method, because
  // metadata bakes conformance from what each class's *own* processed
  // header declares, not from what the object conforms to at runtime.
  //
  // Cached per runtime Class (not per metadata symbol offset, the way
  // membersForClass/surfaceMembersForClass are): two different runtime
  // classes can both resolve to the same metadata symbol while conforming
  // to different runtime-only protocols, so the metadata-symbol cache would
  // be the wrong granularity here and could leak members across unrelated
  // classes.
  //
  // Callers are expected to consult this only once an ordinary
  // membersForClass/surfaceMembersForClass lookup has already missed, so
  // classes with complete metadata (the common case) pay nothing extra.
  const std::vector<NativeApiMember>& protocolMembersForRuntimeClass(
      Class cls) const {
    static const std::vector<NativeApiMember> kEmptyMembers;
    if (cls == Nil) {
      return kEmptyMembers;
    }

    uintptr_t key = normalizeRuntimePointer(reinterpret_cast<uintptr_t>(cls));
    auto cached = protocolMembersByRuntimeClass_.find(key);
    if (cached != protocolMembersByRuntimeClass_.end()) {
      return cached->second;
    }

    std::vector<NativeApiMember> members;
    forEachConformedProtocol(cls, [&](Protocol* protocol) {
      auto it = protocolSymbolsByRuntimePointer_.find(
          normalizeRuntimePointer(reinterpret_cast<uintptr_t>(protocol)));
      if (it != protocolSymbolsByRuntimePointer_.end()) {
        for (const auto& member : membersForProtocol(it->second)) {
          bool exists = false;
          for (const auto& existing : members) {
            if (sameMemberSlot(existing, member)) {
              exists = true;
              break;
            }
          }
          if (!exists) {
            members.push_back(member);
          }
        }
      }
      return false;  // Keep visiting: collect members from every conformed
                     // protocol, not just the first.
    });

    auto inserted = protocolMembersByRuntimeClass_.emplace(
        key, std::move(members));
    return inserted.first->second;
  }

  const NativeApiSymbol* findClassForRuntimePointer(void* pointer) const {
    if (pointer == nullptr) {
      return nullptr;
    }

    auto it = classSymbolsByRuntimePointer_.find(
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(pointer)));
    return it != classSymbolsByRuntimePointer_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findProtocolForRuntimePointer(void* pointer) const {
    if (pointer == nullptr) {
      return nullptr;
    }

    auto it = protocolSymbolsByRuntimePointer_.find(
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(pointer)));
    return it != protocolSymbolsByRuntimePointer_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findFunction(const std::string& name) const {
    auto it = functionSymbolsByName_.find(name);
    return it != functionSymbolsByName_.end() ? &it->second : nullptr;
  }

  static uintptr_t callbackRoundTripValidationKey(
      const NativeApiType& type) {
    if (type.signatureOffset == 0 ||
        type.signatureOffset == MD_SECTION_OFFSET_NULL) {
      return 0;
    }
    return (static_cast<uintptr_t>(type.signatureOffset) << 8) |
           (static_cast<uintptr_t>(type.kind) & 0xff);
  }

  void rememberRoundTripValue(Runtime& runtime, const void* native,
                              const Value& value,
                              bool stringLikeNative = false,
                              uintptr_t validationKey = 0) {
    if (native == nullptr) {
      return;
    }
    uintptr_t key =
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native));
    NativeApiRoundTripReleaseList releaseAfterUnlock;
    {
      std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
      storeRoundTripEntry(
          roundTripValues_, key,
          NativeApiRoundTripValue{
              std::make_shared<Value>(runtime, value), stringLikeNative, true,
              validationKey},
          releaseAfterUnlock);
      roundTripValuesGeneration_.fetch_add(1, std::memory_order_release);
    }
#ifdef TARGET_ENGINE_HERMES
    rootRoundTripValue(runtime, key, value);
#endif
  }

  // Convenience for preservedNativeApiInitializerSelfReturn: remembers an id
  // (rather than an arbitrary native pointer) with the class-derived
  // validation key, the same key an ordinary object wrapping would use.
  void rememberNativeObjectRoundTripValue(Runtime& runtime, id object,
                                          const Value& value,
                                          bool stringLikeNative = false) {
#ifdef TARGET_ENGINE_HERMES
    rememberScopedRoundTripValue(runtime, object, value, stringLikeNative,
                                 true);
    rememberWeakNativeObjectRoundTripValue(runtime, object, value,
                                           stringLikeNative);
#else
    rememberRoundTripValue(runtime, object, value, stringLikeNative,
                           nativeObjectClassKey(object));
#endif
  }

  // Hermes has no engine-side wrapper cache. Keep a weak identity entry for
  // native object host wrappers so a callback can recover the exact existing
  // JS object without rooting the wrapper (and therefore its retained native
  // object) forever. The wrapper destructor removes this entry.
  void rememberWeakNativeObjectRoundTripValue(
      Runtime& runtime, id object, const Value& value,
      bool stringLikeNative = false) {
#ifdef TARGET_ENGINE_HERMES
    if (object == nil || !value.isObject()) {
      return;
    }
    auto weak =
        std::make_shared<WeakObject>(runtime, value.asObject(runtime));
    uintptr_t key =
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(object));
    {
      std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
      weakRoundTripValues_[key] = NativeApiWeakRoundTripValue{
          std::move(weak), stringLikeNative, nativeObjectClassKey(object)};
      roundTripValuesGeneration_.fetch_add(1, std::memory_order_release);
    }
#else
    (void)runtime;
    (void)object;
    (void)value;
    (void)stringLikeNative;
#endif
  }

  void rememberScopedRoundTripValue(Runtime& runtime, const void* native,
                                    const Value& value,
                                    bool stringLikeNative = false,
                                    bool persistBeyondFrame = true) {
    rememberScopedRoundTripValueWithValidationKey(
        runtime, native, value, stringLikeNative, persistBeyondFrame,
        nativeObjectClassKey(native));
  }

  void rememberScopedRawRoundTripValue(Runtime& runtime, const void* native,
                                       const Value& value,
                                       bool stringLikeNative = false,
                                       bool persistBeyondFrame = true) {
    rememberScopedRoundTripValueWithValidationKey(runtime, native, value,
                                                  stringLikeNative,
                                                  persistBeyondFrame, 0);
  }

  void rememberScopedRoundTripValueWithValidationKey(Runtime& runtime,
                                                     const void* native,
                                                     const Value& value,
                                                     bool stringLikeNative,
                                                     bool persistBeyondFrame,
                                                     uintptr_t validationKey) {
    if (native == nullptr) {
      return;
    }
    uintptr_t key =
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native));
    NativeApiRoundTripValue entry{
        std::make_shared<Value>(runtime, value), stringLikeNative,
        persistBeyondFrame, validationKey};
    NativeApiRoundTripReleaseList releaseAfterUnlock;
    {
      std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
      auto framesIt =
          roundTripCacheFramesByThread_.find(std::this_thread::get_id());
      if (framesIt != roundTripCacheFramesByThread_.end() &&
          !framesIt->second.empty()) {
        storeRoundTripEntry(framesIt->second.back(), key, std::move(entry),
                            releaseAfterUnlock);
      } else if (persistBeyondFrame) {
        rememberRecentRoundTripValue(key, std::move(entry),
                                     releaseAfterUnlock);
      }
      roundTripValuesGeneration_.fetch_add(1, std::memory_order_release);
    }
  }

  Value findRoundTripValue(Runtime& runtime, const void* native,
                           bool* stringLikeNative = nullptr,
                           bool nativeIsObject = false,
                           uintptr_t validationKey = 0) {
    if (stringLikeNative != nullptr) {
      *stringLikeNative = false;
    }
    if (native == nullptr) {
      return Value::undefined();
    }
    uintptr_t key =
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native));
    const uintptr_t expectedValidationKey =
        validationKey != 0
            ? validationKey
            : (nativeIsObject ? nativeObjectClassKey(native) : 0);
    struct RoundTripCacheEntry {
      const NativeApiBridge* bridge = nullptr;
      uintptr_t key = 0;
      uint64_t generation = 0;
      std::weak_ptr<Value> value;
      bool miss = false;
      bool stringLikeNative = false;
      uintptr_t validationKey = 0;
    };
    static thread_local RoundTripCacheEntry cache[4];
    const uint64_t generation =
        roundTripValuesGeneration_.load(std::memory_order_acquire);
    const size_t firstSlot = (key >> 4) & 3;
    for (size_t i = 0; i < 4; i++) {
      RoundTripCacheEntry& entry = cache[(firstSlot + i) & 3];
      if (entry.bridge == this && entry.key == key &&
          entry.generation == generation) {
        if (entry.validationKey != expectedValidationKey) {
          break;
        }
        if (entry.miss) {
          return Value::undefined();
        }
        if (auto cached = entry.value.lock()) {
          if (roundTripValuesGeneration_.load(std::memory_order_acquire) ==
              generation) {
            if (stringLikeNative != nullptr) {
              *stringLikeNative = entry.stringLikeNative;
            }
            return Value(runtime, *cached);
          }
        }
        break;
      }
    }

    std::shared_ptr<Value> storedValue;
#ifdef TARGET_ENGINE_HERMES
    std::shared_ptr<WeakObject> weakValue;
#endif
    bool cachedStringLike = false;
    {
      std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
      auto findEntry = [&](const auto& map) -> const NativeApiRoundTripValue* {
        auto it = map.find(key);
        if (it == map.end() || it->second.value == nullptr) {
          return nullptr;
        }
        if (it->second.validationKey != expectedValidationKey) {
          return nullptr;
        }
        return &it->second;
      };

      const NativeApiRoundTripValue* entry = findEntry(roundTripValues_);
      if (entry == nullptr) {
        auto framesIt =
            roundTripCacheFramesByThread_.find(std::this_thread::get_id());
        if (framesIt != roundTripCacheFramesByThread_.end()) {
          for (auto frame = framesIt->second.rbegin();
               frame != framesIt->second.rend(); ++frame) {
            entry = findEntry(*frame);
            if (entry != nullptr) {
              break;
            }
          }
        }
      }
      if (entry == nullptr) {
        entry = findEntry(recentRoundTripValues_);
      }
      if (entry == nullptr) {
#ifdef TARGET_ENGINE_HERMES
        if (nativeIsObject) {
          auto weakIt = weakRoundTripValues_.find(key);
          if (weakIt != weakRoundTripValues_.end() &&
              weakIt->second.validationKey == expectedValidationKey) {
            weakValue = weakIt->second.value;
            cachedStringLike = weakIt->second.stringLikeNative;
          }
        }
        if (weakValue == nullptr) {
          cache[firstSlot] = RoundTripCacheEntry{
              this, key, generation, {}, true, false, expectedValidationKey};
          return Value::undefined();
        }
#else
        cache[firstSlot] = RoundTripCacheEntry{
            this, key, generation, {}, true, false, expectedValidationKey};
        return Value::undefined();
#endif
      } else {
        storedValue = entry->value;
        cachedStringLike = entry->stringLikeNative;
        cache[firstSlot] = RoundTripCacheEntry{
            this, key, generation, storedValue, false, cachedStringLike,
            entry->validationKey};
      }
    }
#ifdef TARGET_ENGINE_HERMES
    if (weakValue != nullptr) {
      Value locked = weakValue->lock(runtime);
      if (!locked.isUndefined()) {
        if (stringLikeNative != nullptr) {
          *stringLikeNative = cachedStringLike;
        }
        return locked;
      }
      std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
      auto weakIt = weakRoundTripValues_.find(key);
      if (weakIt != weakRoundTripValues_.end() &&
          weakIt->second.value == weakValue) {
        weakRoundTripValues_.erase(weakIt);
        roundTripValuesGeneration_.fetch_add(1, std::memory_order_release);
      }
      return Value::undefined();
    }
#endif
    if (stringLikeNative != nullptr) {
      *stringLikeNative = cachedStringLike;
    }
    return Value(runtime, *storedValue);
  }

  void forgetRoundTripValue(Runtime& runtime, const void* native) {
    if (native == nullptr) {
      return;
    }
    uintptr_t key =
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native));
#ifdef TARGET_ENGINE_HERMES
    bool rooted = false;
    NativeApiRoundTripReleaseList releaseAfterUnlock;
    {
      std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
      eraseRoundTripMapKey(roundTripValues_, key, releaseAfterUnlock);
      eraseRoundTripKeyFromScopedCaches(key, releaseAfterUnlock);
      weakRoundTripValues_.erase(key);
      rooted = rootedRoundTripValues_.erase(key) > 0;
      roundTripValuesGeneration_.fetch_add(1, std::memory_order_release);
    }
    if (rooted) {
      unrootRoundTripValue(runtime, key);
    }
#else
    forgetRoundTripKey(key);
#endif
  }

  void forgetRoundTripKey(uintptr_t key) {
    if (key == 0) {
      return;
    }
    NativeApiRoundTripReleaseList releaseAfterUnlock;
    {
      std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
      eraseRoundTripMapKey(roundTripValues_, key, releaseAfterUnlock);
      eraseRoundTripKeyFromScopedCaches(key, releaseAfterUnlock);
#ifdef TARGET_ENGINE_HERMES
      weakRoundTripValues_.erase(key);
#endif
      roundTripValuesGeneration_.fetch_add(1, std::memory_order_release);
    }
  }

  void forgetRoundTripValue(const void* native) {
    if (native == nullptr) {
      return;
    }
    uintptr_t key =
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native));
    NativeApiRoundTripReleaseList releaseAfterUnlock;
    {
      std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
      eraseRoundTripMapKey(roundTripValues_, key, releaseAfterUnlock);
      eraseRoundTripKeyFromScopedCaches(key, releaseAfterUnlock);
#ifdef TARGET_ENGINE_HERMES
      weakRoundTripValues_.erase(key);
#endif
      roundTripValuesGeneration_.fetch_add(1, std::memory_order_release);
    }
  }

  uint64_t roundTripValuesGeneration() const {
    return roundTripValuesGeneration_.load(std::memory_order_acquire);
  }

  void beginRoundTripCacheFrame() {
    std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
    roundTripCacheFramesByThread_[std::this_thread::get_id()].emplace_back();
  }

  void endRoundTripCacheFrame() {
    NativeApiRoundTripReleaseList releaseAfterUnlock;
    NativeApiRoundTripFrame frame;
    {
      std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
      auto framesIt =
          roundTripCacheFramesByThread_.find(std::this_thread::get_id());
      if (framesIt == roundTripCacheFramesByThread_.end() ||
          framesIt->second.empty()) {
        return;
      }

      auto& frames = framesIt->second;
      frame = std::move(frames.back());
      frames.pop_back();
      if (!frames.empty()) {
        auto& parent = frames.back();
        for (auto& entry : frame) {
          storeRoundTripEntry(parent, entry.first, std::move(entry.second),
                              releaseAfterUnlock);
        }
      } else {
        roundTripCacheFramesByThread_.erase(framesIt);
        for (auto& entry : frame) {
          if (entry.second.persistBeyondFrame) {
            rememberRecentRoundTripValue(entry.first, std::move(entry.second),
                                         releaseAfterUnlock);
          } else {
            releaseAfterUnlock.push_back(std::move(entry.second));
          }
        }
      }
      roundTripValuesGeneration_.fetch_add(1, std::memory_order_release);
    }
  }

  void rememberClassValue(Runtime& runtime, Class cls, const Value& value) {
    if (cls == Nil) {
      return;
    }
    classValues_[normalizeRuntimePointer(reinterpret_cast<uintptr_t>(cls))] =
        std::make_shared<Value>(runtime, value);
  }

  Value findClassValue(Runtime& runtime, Class cls) const {
    if (cls == Nil) {
      return Value::undefined();
    }
    auto it = classValues_.find(
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(cls)));
    if (it == classValues_.end() || it->second == nullptr) {
      return Value::undefined();
    }
    return Value(runtime, *it->second);
  }

  void rememberClassPrototype(Runtime& runtime, Class cls, const Value& value) {
    if (cls == Nil) {
      return;
    }
    classPrototypes_[normalizeRuntimePointer(reinterpret_cast<uintptr_t>(cls))] =
        std::make_shared<Value>(runtime, value);
  }

  Value findClassPrototype(Runtime& runtime, Class cls) const {
    if (cls == Nil) {
      return Value::undefined();
    }
    auto it = classPrototypes_.find(
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(cls)));
    if (it == classPrototypes_.end() || it->second == nullptr) {
      return Value::undefined();
    }
    return Value(runtime, *it->second);
  }

  // Expandos are keyed native-pointer -> property -> owning-runtime, all
  // guarded by objectExpandosMutex_: worklet runtimes run on their own
  // thread but share this bridge, and a host-object dtor releasing its
  // expando owner count can run on either thread relative to a get/set.
  void setObjectExpando(Runtime& runtime, const void* native,
                        const std::string& property, const Value& value) {
    if (native == nullptr || property.empty()) {
      return;
    }
    const uintptr_t key =
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native));
    const uintptr_t runtimeKey = runtimeObjectExpandoKey(runtime);
    {
      std::lock_guard<std::mutex> lock(objectExpandosMutex_);
      objectExpandos_[key][property][runtimeKey] =
          std::make_shared<Value>(runtime, value);
      objectExpandosGeneration_.fetch_add(1, std::memory_order_release);
    }
  }

  void retainObjectExpandoOwner(const void* native) {
    if (native == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(objectExpandosMutex_);
    objectExpandoOwnerCounts_[
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native))] += 1;
  }

  // preserveExpandos keeps the stored values around after the last owner
  // releases (used when a wrapper is being replaced/detached but the
  // underlying native receiver's expando state must survive the swap).
  void releaseObjectExpandoOwner(const void* native,
                                 bool preserveExpandos = false) {
    if (native == nullptr) {
      return;
    }
    uintptr_t key =
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native));
    bool shouldForget = false;
    {
      std::lock_guard<std::mutex> lock(objectExpandosMutex_);
      auto ownerIt = objectExpandoOwnerCounts_.find(key);
      if (ownerIt != objectExpandoOwnerCounts_.end()) {
        if (ownerIt->second > 1) {
          ownerIt->second -= 1;
          return;
        }
        objectExpandoOwnerCounts_.erase(ownerIt);
      }
      shouldForget = !preserveExpandos;
    }
    if (shouldForget) {
      forgetObjectExpandos(native);
    }
  }

  Value findObjectExpando(Runtime& runtime, const void* native,
                          const std::string& property) const {
    if (native == nullptr || property.empty()) {
      return Value::undefined();
    }
    struct ObjectExpandoCacheEntry {
      const NativeApiBridge* bridge = nullptr;
      uintptr_t key = 0;
      uintptr_t runtimeKey = 0;
      uint64_t generation = 0;
      std::string property;
      std::weak_ptr<Value> value;
      bool miss = false;
    };
    static thread_local ObjectExpandoCacheEntry cache[8];
    static thread_local size_t nextSlot = 0;

    const uintptr_t key =
        normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native));
    const uintptr_t runtimeKey = runtimeObjectExpandoKey(runtime);
    const uint64_t generation =
        objectExpandosGeneration_.load(std::memory_order_acquire);
    for (auto& entry : cache) {
      if (entry.bridge == this && entry.key == key &&
          entry.runtimeKey == runtimeKey && entry.generation == generation &&
          entry.property == property) {
        if (entry.miss) {
          return Value::undefined();
        }
        if (auto cached = entry.value.lock()) {
          return Value(runtime, *cached);
        }
        break;
      }
    }

    std::shared_ptr<Value> storedValue;
    const size_t slot = nextSlot++ & 7;
    {
      std::lock_guard<std::mutex> lock(objectExpandosMutex_);
      auto objectIt = objectExpandos_.find(key);
      if (objectIt != objectExpandos_.end()) {
        auto propertyIt = objectIt->second.find(property);
        if (propertyIt != objectIt->second.end()) {
          auto runtimeIt = propertyIt->second.find(runtimeKey);
          if (runtimeIt != propertyIt->second.end() &&
              runtimeIt->second != nullptr) {
            storedValue = runtimeIt->second;
          }
        }
      }
      if (storedValue == nullptr) {
        cache[slot] = ObjectExpandoCacheEntry{
            this, key, runtimeKey, generation, property, {}, true};
        return Value::undefined();
      }
      cache[slot] = ObjectExpandoCacheEntry{
          this, key, runtimeKey, generation, property, storedValue, false};
    }
    return Value(runtime, *storedValue);
  }

  // Erases every runtime's stored values for this native (codex semantics):
  // the native object itself is gone, so no runtime should keep seeing it.
  void forgetObjectExpandos(const void* native) {
    if (native == nullptr) {
      return;
    }
    auto key = normalizeRuntimePointer(reinterpret_cast<uintptr_t>(native));
    {
      std::lock_guard<std::mutex> lock(objectExpandosMutex_);
      objectExpandos_.erase(key);
      objectExpandosGeneration_.fetch_add(1, std::memory_order_release);
    }
  }

  // Per-class cache of resolved metadata property-getter members. Lets the
  // instance property interceptor skip the special-name chain + metadata
  // discovery on every `object.prop` access (the engines without V8's
  // kNonMasking prototype fast path otherwise re-resolve on each access).
  // membersByClassOffset_ vectors are permanent, so the member pointer is
  // stable for the bridge's lifetime.
  struct CachedPropertyGetter {
    const NativeApiMember* member;
    std::string selectorName;
    std::shared_ptr<NativeApiPreparedObjCInvocation> preparedInvocation;
  };
  const CachedPropertyGetter* findCachedPropertyGetter(
      Class cls, const std::string& property) const {
    if (cls == Nil || property.empty()) {
      return nullptr;
    }
    struct PropertyGetterCacheEntry {
      const NativeApiBridge* bridge = nullptr;
      Class cls = Nil;
      uint64_t generation = 0;
      std::string property;
      const CachedPropertyGetter* getter = nullptr;
      bool miss = false;
    };
    static thread_local PropertyGetterCacheEntry cache[8];
    static thread_local size_t nextSlot = 0;

    const uint64_t generation =
        propertyGetterCacheGeneration_.load(std::memory_order_acquire);
    for (auto& entry : cache) {
      if (entry.bridge == this && entry.cls == cls &&
          entry.generation == generation && entry.property == property) {
        return entry.miss ? nullptr : entry.getter;
      }
    }

    auto classIt = propertyGetterCache_.find(cls);
    const size_t slot = nextSlot++ & 7;
    if (classIt == propertyGetterCache_.end()) {
      cache[slot] = PropertyGetterCacheEntry{
          this, cls, generation, property, nullptr, true};
      return nullptr;
    }
    auto propIt = classIt->second.find(property);
    if (propIt == classIt->second.end()) {
      cache[slot] = PropertyGetterCacheEntry{
          this, cls, generation, property, nullptr, true};
      return nullptr;
    }
    cache[slot] =
        PropertyGetterCacheEntry{this, cls, generation, property,
                                 &propIt->second, false};
    return &propIt->second;
  }
  void cachePropertyGetter(Class cls, const std::string& property,
                           const NativeApiMember* member,
                           const std::string& selectorName,
                           std::shared_ptr<NativeApiPreparedObjCInvocation>
                               preparedInvocation = nullptr) {
    propertyGetterCache_[cls][property] =
        CachedPropertyGetter{member, selectorName,
                             std::move(preparedInvocation)};
    propertyGetterCacheGeneration_.fetch_add(1, std::memory_order_release);
  }

  void rememberPointerValue(Runtime& runtime, const void* native,
                            const Value& value) {
    pointerValues_[reinterpret_cast<uintptr_t>(native)] =
        std::make_shared<Value>(runtime, value);
  }

  Value findPointerValue(Runtime& runtime, const void* native) const {
    auto it = pointerValues_.find(reinterpret_cast<uintptr_t>(native));
    if (it == pointerValues_.end() || it->second == nullptr) {
      return Value::undefined();
    }
    return Value(runtime, *it->second);
  }

  void forgetPointerValue(const void* native) {
    if (native == nullptr) {
      return;
    }
    pointerValues_.erase(reinterpret_cast<uintptr_t>(native));
  }

  const NativeApiSymbol* findConstant(const std::string& name) const {
    auto it = constantSymbolsByName_.find(name);
    return it != constantSymbolsByName_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findProtocol(const std::string& name) const {
    const NativeApiSymbol* symbol = find(name);
    if (symbol != nullptr && symbol->kind == NativeApiSymbolKind::Protocol) {
      return symbol;
    }
    auto it = protocolSymbolsByRuntimeName_.find(name);
    return it != protocolSymbolsByRuntimeName_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findEnum(const std::string& name) const {
    auto it = enumSymbolsByName_.find(name);
    return it != enumSymbolsByName_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findStruct(const std::string& name) const {
    auto it = structSymbolsByName_.find(name);
    return it != structSymbolsByName_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findUnion(const std::string& name) const {
    auto it = unionSymbolsByName_.find(name);
    return it != unionSymbolsByName_.end() ? &it->second : nullptr;
  }

  const NativeApiSymbol* findAggregate(const std::string& name) const {
    const NativeApiSymbol* symbol = findStruct(name);
    if (symbol != nullptr) {
      return symbol;
    }
    return findUnion(name);
  }

  size_t classCount() const { return classNames_.size(); }
  size_t functionCount() const { return functionNames_.size(); }
  size_t constantCount() const { return constantNames_.size(); }
  size_t protocolCount() const { return protocolNames_.size(); }
  size_t enumCount() const { return enumNames_.size(); }
  size_t structCount() const { return structNames_.size(); }
  size_t unionCount() const { return unionNames_.size(); }

  const std::vector<std::string>& classNames() const { return classNames_; }
  const std::vector<std::string>& functionNames() const { return functionNames_; }
  const std::vector<std::string>& constantNames() const { return constantNames_; }
  const std::vector<std::string>& protocolNames() const { return protocolNames_; }
  const std::vector<std::string>& enumNames() const { return enumNames_; }
  const std::vector<std::string>& structNames() const { return structNames_; }
  const std::vector<std::string>& unionNames() const { return unionNames_; }
  std::shared_ptr<NativeApiScheduler> scheduler() const { return scheduler_; }
  const std::function<void(std::function<void()>)>& nativeInvocationInvoker()
      const {
    return nativeInvocationInvoker_;
  }
  const std::function<void(std::function<void()>)>& nativeCallbackInvoker()
      const {
    return nativeCallbackInvoker_;
  }
  const std::function<void(std::function<void()>)>& runtimeCallbackInvoker()
      const {
    return runtimeCallbackInvoker_;
  }
  const std::function<void(std::function<void()>)>& jsThreadCallbackInvoker()
      const {
    return jsThreadCallbackInvoker_;
  }
  const std::function<void(std::function<void()>)>&
  jsThreadAsyncCallbackInvoker() const {
    return jsThreadAsyncCallbackInvoker_;
  }
  // Teardown-safety gate: RN sets this so callbacks can be refused once the
  // host is tearing down/reloading, without every call site needing to know
  // why. Both a C++ try and an @try wrap the call — they catch different
  // exception families (std::exception-derived vs. NSException), and either
  // one escaping here would otherwise cross into caller frames that aren't
  // set up to catch it.
  bool callbackInvocationAllowed() const noexcept {
    if (!callbackInvocationAllowed_) {
      return true;
    }
    @try {
    try {
      return callbackInvocationAllowed_();
    } catch (...) {
      return false;
    }
    } @catch (...) {
      return false;
    }
  }
  bool invokeCallbacksOnNativeCallerThread() const {
    return invokeCallbacksOnNativeCallerThread_;
  }

  std::thread::id jsThreadId() const { return jsThreadId_; }

  void retainEngineLifetime(std::shared_ptr<void> lifetime) {
    if (lifetime == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(retainedLifetimesMutex_);
    retainedLifetimes_.push_back(std::move(lifetime));
  }

#ifdef TARGET_ENGINE_HERMES
  std::string roundTripRootKey(uintptr_t key) const {
    char buffer[32] = {};
    snprintf(buffer, sizeof(buffer), "p%llx",
             static_cast<unsigned long long>(key));
    return buffer;
  }

  Object roundTripRootObject(Runtime& runtime) {
    if (roundTripRootCache_) {
      return roundTripRootCache_->asObject(runtime);
    }
    static constexpr const char* kRootName =
        "__nativeScriptNativeApiRoundTripValues";
    Object global = runtime.global();
    if (global.hasProperty(runtime, kRootName)) {
      Value existing = global.getProperty(runtime, kRootName);
      if (existing.isObject()) {
        Object root = existing.asObject(runtime);
        roundTripRootCache_ = std::make_shared<Value>(runtime, root);
        return root;
      }
    }

    Object root(runtime);
    global.setProperty(runtime, kRootName, root);
    roundTripRootCache_ = std::make_shared<Value>(runtime, root);
    return root;
  }

  void rootRoundTripValue(Runtime& runtime, uintptr_t key,
                          const Value& value) {
    roundTripRootObject(runtime)
        .setProperty(runtime, roundTripRootKey(key).c_str(), value);
    std::lock_guard<std::mutex> lock(roundTripValuesMutex_);
    rootedRoundTripValues_.insert(key);
  }

  void unrootRoundTripValue(Runtime& runtime, uintptr_t key) {
    roundTripRootObject(runtime)
        .setProperty(runtime, roundTripRootKey(key).c_str(),
                     Value::undefined());
  }
#endif

  uintptr_t nativeObjectClassKey(const void* native) const {
    if (native == nullptr) {
      return 0;
    }
    return normalizeRuntimePointer(
        reinterpret_cast<uintptr_t>(object_getClass(static_cast<id>(native))));
  }

  void storeRoundTripEntry(
      std::unordered_map<uintptr_t, NativeApiRoundTripValue>& map,
      uintptr_t key, NativeApiRoundTripValue&& entry,
      NativeApiRoundTripReleaseList& releaseAfterUnlock) {
    auto it = map.find(key);
    if (it == map.end()) {
      map.emplace(key, std::move(entry));
      return;
    }

    releaseAfterUnlock.push_back(std::move(it->second));
    it->second = std::move(entry);
  }

  void eraseRoundTripMapKey(
      std::unordered_map<uintptr_t, NativeApiRoundTripValue>& map,
      uintptr_t key, NativeApiRoundTripReleaseList& releaseAfterUnlock) {
    auto it = map.find(key);
    if (it == map.end()) {
      return;
    }

    releaseAfterUnlock.push_back(std::move(it->second));
    map.erase(it);
  }

  void eraseRoundTripKeyFromScopedCaches(
      uintptr_t key, NativeApiRoundTripReleaseList& releaseAfterUnlock) {
    eraseRoundTripMapKey(recentRoundTripValues_, key, releaseAfterUnlock);
    recentRoundTripValueOrder_.erase(
        std::remove(recentRoundTripValueOrder_.begin(),
                    recentRoundTripValueOrder_.end(), key),
        recentRoundTripValueOrder_.end());
    for (auto& stackEntry : roundTripCacheFramesByThread_) {
      for (auto& frame : stackEntry.second) {
        eraseRoundTripMapKey(frame, key, releaseAfterUnlock);
      }
    }
  }

  void rememberRecentRoundTripValue(uintptr_t key,
                                    NativeApiRoundTripValue&& entry,
                                    NativeApiRoundTripReleaseList& releaseAfterUnlock) {
    if (recentRoundTripValues_.find(key) == recentRoundTripValues_.end()) {
      recentRoundTripValueOrder_.push_back(key);
    }
    storeRoundTripEntry(recentRoundTripValues_, key, std::move(entry),
                        releaseAfterUnlock);
    while (recentRoundTripValueOrder_.size() > kRecentRoundTripValueLimit) {
      uintptr_t evicted = recentRoundTripValueOrder_.front();
      recentRoundTripValueOrder_.erase(recentRoundTripValueOrder_.begin());
      eraseRoundTripMapKey(recentRoundTripValues_, evicted,
                           releaseAfterUnlock);
    }
  }

  const std::vector<NativeApiMember>& membersForClass(
      const NativeApiSymbol& symbol) const {
    auto cached = membersByClassOffset_.find(symbol.offset);
    if (cached != membersByClassOffset_.end()) {
      return cached->second;
    }

    auto inserted = membersByClassOffset_.emplace(
        symbol.offset, readMembersForClassHierarchy(symbol));
    return inserted.first->second;
  }

  const std::vector<NativeApiMember>& surfaceMembersForClass(
      const NativeApiSymbol& symbol) const {
    auto cached = surfaceMembersByClassOffset_.find(symbol.offset);
    if (cached != surfaceMembersByClassOffset_.end()) {
      return cached->second;
    }

    auto inserted = surfaceMembersByClassOffset_.emplace(
        symbol.offset, readSurfaceMembersForClass(symbol));
    return inserted.first->second;
  }

  const std::vector<NativeApiMember>& membersForProtocol(
      const NativeApiSymbol& symbol) const {
    auto cached = membersByProtocolOffset_.find(symbol.offset);
    if (cached != membersByProtocolOffset_.end()) {
      return cached->second;
    }

    auto inserted = membersByProtocolOffset_.emplace(
        symbol.offset, readMembersForProtocolHierarchy(symbol.offset));
    return inserted.first->second;
  }

  std::shared_ptr<NativeApiAggregateInfo> aggregateInfoFor(
      MDSectionOffset aggregateOffset, bool isUnion);

  std::shared_ptr<NativeApiAggregateInfo> aggregateInfoFor(
      const NativeApiSymbol& symbol) {
    return aggregateInfoFor(symbol.offset,
                            symbol.kind == NativeApiSymbolKind::Union);
  }

 private:
  static std::unique_ptr<MDMetadataReader> loadMetadataFromFile(
      const char* metadataPath) {
    const char* path = metadataPath != nullptr ? metadataPath : "metadata.nsmd";
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
      throw std::runtime_error(std::string("metadata.nsmd not found: ") + path);
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
      fclose(file);
      throw std::runtime_error(std::string("metadata.nsmd is empty: ") + path);
    }

    void* buffer = malloc(static_cast<size_t>(size));
    if (buffer == nullptr) {
      fclose(file);
      throw std::bad_alloc();
    }

    size_t read = fread(buffer, 1, static_cast<size_t>(size), file);
    fclose(file);
    if (read != static_cast<size_t>(size)) {
      free(buffer);
      throw std::runtime_error(std::string("failed to read metadata: ") + path);
    }

    return std::make_unique<MDMetadataReader>(buffer, true);
  }

  static std::unique_ptr<MDMetadataReader> loadMetadata(
      const NativeApiConfig& config) {
    if (config.metadataPtr != nullptr &&
        *static_cast<const char*>(config.metadataPtr) != '\0') {
#ifdef EMBED_METADATA_SIZE
      return std::make_unique<MDMetadataReader>((void*)embedded_metadata);
#else
      return std::make_unique<MDMetadataReader>(
          const_cast<void*>(config.metadataPtr));
#endif
    }

#ifdef EMBED_METADATA_SIZE
    if (config.metadataPath == nullptr) {
      return std::make_unique<MDMetadataReader>((void*)embedded_metadata);
    }
#endif

    unsigned long segmentSize = 0;
    auto segmentData = getsegmentdata(
        reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0)),
        "__objc_metadata", &segmentSize);
    if (segmentData != nullptr && segmentSize > 0) {
      return std::make_unique<MDMetadataReader>(segmentData);
    }

    return loadMetadataFromFile(config.metadataPath);
  }

  void addSymbol(NativeApiSymbolKind kind, MDSectionOffset offset,
                 const char* name, const char* runtimeName = nullptr,
                 MDSectionOffset superclassOffset = MD_SECTION_OFFSET_NULL) {
    if (name == nullptr || name[0] == '\0') {
      return;
    }

    NativeApiSymbol symbol{
        .kind = kind,
        .offset = offset,
        .superclassOffset = superclassOffset,
        .name = name,
        .runtimeName = runtimeName != nullptr ? runtimeName : name,
    };

    switch (kind) {
      case NativeApiSymbolKind::Class:
        classNames_.push_back(symbol.name);
        break;
      case NativeApiSymbolKind::Function:
        functionNames_.push_back(symbol.name);
        functionSymbolsByName_[symbol.name] = symbol;
        break;
      case NativeApiSymbolKind::Constant:
        constantNames_.push_back(symbol.name);
        constantSymbolsByName_[symbol.name] = symbol;
        break;
      case NativeApiSymbolKind::Protocol:
        protocolNames_.push_back(symbol.name);
        break;
      case NativeApiSymbolKind::Enum:
        enumNames_.push_back(symbol.name);
        enumSymbolsByName_[symbol.name] = symbol;
        break;
      case NativeApiSymbolKind::Struct:
        structNames_.push_back(symbol.name);
        structSymbolsByName_[symbol.name] = symbol;
        break;
      case NativeApiSymbolKind::Union:
        unionNames_.push_back(symbol.name);
        unionSymbolsByName_[symbol.name] = symbol;
        break;
    }

    symbolsByName_[symbol.name] = symbol;
    if (kind == NativeApiSymbolKind::Class) {
      classSymbolsByOffset_[symbol.offset] = symbol;
      classSymbolsByRuntimeName_[symbol.runtimeName] = symbol;
      if (indexRuntimePointers_) {
        Class cls = objc_lookUpClass(symbol.runtimeName.c_str());
        if (cls != Nil) {
          classSymbolsByRuntimePointer_[normalizeRuntimePointer(
              reinterpret_cast<uintptr_t>(cls))] = symbol;
        }
      }
    } else if (kind == NativeApiSymbolKind::Protocol) {
      protocolSymbolsByOffset_[symbol.offset] = symbol;
      protocolSymbolsByRuntimeName_[symbol.runtimeName] = symbol;
      auto rememberProtocolRuntimeName = [&](const std::string& runtimeName) {
        if (runtimeName.empty()) {
          return;
        }
        protocolSymbolsByRuntimeName_[runtimeName] = symbol;
        if (indexRuntimePointers_) {
          Protocol* runtimeProtocol = lookupProtocolByNativeName(runtimeName);
          if (runtimeProtocol != nullptr) {
            protocolSymbolsByRuntimePointer_[normalizeRuntimePointer(
                reinterpret_cast<uintptr_t>(runtimeProtocol))] = symbol;
          }
        }
      };
      if (symbol.name.size() > 9 &&
          std::isdigit(static_cast<unsigned char>(symbol.name.back()))) {
        size_t digitsStart = symbol.name.size();
        while (digitsStart > 0 &&
               std::isdigit(static_cast<unsigned char>(symbol.name[digitsStart - 1]))) {
          digitsStart--;
        }
        constexpr const char* protocolSuffix = "Protocol";
        size_t protocolSuffixLength = std::strlen(protocolSuffix);
        if (digitsStart > protocolSuffixLength &&
            symbol.name.compare(digitsStart - protocolSuffixLength,
                                protocolSuffixLength, protocolSuffix) == 0) {
          rememberProtocolRuntimeName(
              symbol.name.substr(0, digitsStart - protocolSuffixLength));
        }
      }
      if (indexRuntimePointers_) {
        Protocol* protocol = lookupProtocolByNativeName(symbol.runtimeName);
        if (protocol == nullptr && symbol.runtimeName != symbol.name) {
          protocol = lookupProtocolByNativeName(symbol.name);
        }
        if (protocol != nullptr) {
          protocolSymbolsByRuntimePointer_[normalizeRuntimePointer(
              reinterpret_cast<uintptr_t>(protocol))] = symbol;
        }
      }
    } else if (kind == NativeApiSymbolKind::Struct) {
      structSymbolsByOffset_[symbol.offset] = symbol;
    } else if (kind == NativeApiSymbolKind::Union) {
      unionSymbolsByOffset_[symbol.offset] = symbol;
    }
  }

  void addAggregateAliases(NativeApiSymbolKind kind, MDSectionOffset offset,
                           const std::string& name) {
    if (name.empty()) {
      return;
    }

    if (!name.empty() && name[0] == '_') {
      std::string alias = name.substr(1);
      if (!alias.empty() && symbolsByName_.find(alias) == symbolsByName_.end()) {
        addSymbol(kind, offset, alias.c_str(), name.c_str());
      }
    }

    constexpr const char* suffix = "Struct";
    if (name.size() < std::strlen(suffix) ||
        name.compare(name.size() - std::strlen(suffix), std::strlen(suffix),
                     suffix) != 0) {
      std::string alias = name + suffix;
      if (symbolsByName_.find(alias) == symbolsByName_.end()) {
        addSymbol(kind, offset, alias.c_str(), name.c_str());
      }
    }
  }

  void buildSymbolIndexes() {
    if (metadata_ == nullptr) {
      return;
    }

    indexConstants();
    indexEnums();
    indexFunctions();
    indexProtocols();
    indexClasses();
    indexStructs();
    indexUnions();
  }

  static void skipConstantValue(MDMetadataReader* metadata,
                                MDSectionOffset& offset,
                                metagen::MDVariableEvalKind evalKind) {
    switch (evalKind) {
      case metagen::mdEvalNone:
        skipMetadataEngineType(metadata, &offset);
        break;
      case metagen::mdEvalInt64:
        offset += sizeof(int64_t);
        break;
      case metagen::mdEvalDouble:
        offset += sizeof(double);
        break;
      case metagen::mdEvalString:
        offset += sizeof(MDSectionOffset);
        break;
    }
  }

  void indexConstants() {
    MDSectionOffset offset = metadata_->constantsOffset;
    while (offset < metadata_->enumsOffset) {
      MDSectionOffset originalOffset = offset;
      addSymbol(NativeApiSymbolKind::Constant, originalOffset,
                metadata_->getString(offset));
      offset += sizeof(MDSectionOffset);
      auto evalKind = metadata_->getVariableEvalKind(offset);
      offset += sizeof(metagen::MDVariableEvalKind);
      skipConstantValue(metadata_.get(), offset, evalKind);
    }
  }

  void indexEnums() {
    MDSectionOffset offset = metadata_->enumsOffset;
    while (offset < metadata_->signaturesOffset) {
      MDSectionOffset originalOffset = offset;
      addSymbol(NativeApiSymbolKind::Enum, originalOffset,
                metadata_->getString(offset));
      offset += sizeof(MDSectionOffset);

      bool next = true;
      while (next) {
        auto nameOffset = metadata_->getOffset(offset);
        next = (nameOffset & metagen::mdSectionOffsetNext) != 0;
        offset += sizeof(MDSectionOffset);
        offset += sizeof(int64_t);
      }
    }
  }

  void indexFunctions() {
    MDSectionOffset offset = metadata_->functionsOffset;
    while (offset < metadata_->protocolsOffset) {
      MDSectionOffset originalOffset = offset;
      addSymbol(NativeApiSymbolKind::Function, originalOffset,
                metadata_->getString(offset));
      offset += sizeof(MDSectionOffset);
      offset += sizeof(MDSectionOffset);
      offset += sizeof(metagen::MDFunctionFlag);
    }
  }

  void indexProtocols() {
    MDSectionOffset offset = metadata_->protocolsOffset;
    while (offset < metadata_->classesOffset) {
      MDSectionOffset originalOffset = offset;
      auto nameOffset = metadata_->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      bool next = (nameOffset & metagen::mdSectionOffsetNext) != 0;
      nameOffset &= ~metagen::mdSectionOffsetNext;
      addSymbol(NativeApiSymbolKind::Protocol, originalOffset,
                metadata_->resolveString(nameOffset));

      while (next) {
        auto protocolOffset = metadata_->getOffset(offset);
        offset += sizeof(MDSectionOffset);
        next = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
      }

      next = true;
      while (next) {
        auto flags = metadata_->getMemberFlag(offset);
        next = (flags & metagen::mdMemberNext) != 0;
        offset += sizeof(flags);
        if (flags == metagen::mdMemberFlagNull) {
          break;
        }

        skipMember(flags, offset);
      }
    }
  }

  void indexClasses() {
    MDSectionOffset offset = metadata_->classesOffset;
    while (offset < metadata_->structsOffset) {
      MDSectionOffset originalOffset = offset;
      auto nameOffset = metadata_->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      auto runtimeNameOffset = metadata_->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      bool hasProtocols = (nameOffset & metagen::mdSectionOffsetNext) != 0;
      nameOffset &= ~metagen::mdSectionOffsetNext;

      auto name = metadata_->resolveString(nameOffset);
      const char* runtimeName = name;
      if (runtimeNameOffset != MD_SECTION_OFFSET_NULL) {
        runtimeName = metadata_->resolveString(runtimeNameOffset);
      }

      while (hasProtocols) {
        auto protocolOffset = metadata_->getOffset(offset);
        offset += sizeof(MDSectionOffset);
        hasProtocols = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
      }

      auto superclass = metadata_->getOffset(offset);
      offset += sizeof(superclass);
      MDSectionOffset superclassOffset =
          superclass & ~metagen::mdSectionOffsetNext;
      if (superclassOffset != MD_SECTION_OFFSET_NULL) {
        superclassOffset += metadata_->classesOffset;
      }

      addSymbol(NativeApiSymbolKind::Class, originalOffset, name, runtimeName,
                superclassOffset);

      bool next = (superclass & metagen::mdSectionOffsetNext) != 0;
      while (next) {
        auto flags = metadata_->getMemberFlag(offset);
        next = (flags & metagen::mdMemberNext) != 0;
        offset += sizeof(flags);
        skipMember(flags, offset);
      }
    }
  }

  void skipAggregateFields(MDSectionOffset& offset, bool isUnion) const {
    bool next = true;
    while (next) {
      MDSectionOffset nameOffset = metadata_->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      next = (nameOffset & metagen::mdSectionOffsetNext) != 0;
      nameOffset &= ~metagen::mdSectionOffsetNext;
      if (nameOffset == MD_SECTION_OFFSET_NULL) {
        break;
      }
      if (!isUnion) {
        offset += sizeof(uint16_t);
      }
      skipMetadataEngineType(metadata_.get(), &offset);
    }
  }

  void indexStructs() {
    MDSectionOffset offset = metadata_->structsOffset;
    while (offset < metadata_->unionsOffset) {
      if (metadata_->getOffset(offset) == 0) {
        break;
      }
      MDSectionOffset originalOffset = offset;
      const char* name = metadata_->getString(offset);
      offset += sizeof(MDSectionOffset);
      offset += sizeof(uint16_t);
      addSymbol(NativeApiSymbolKind::Struct, originalOffset, name);
      addAggregateAliases(NativeApiSymbolKind::Struct, originalOffset,
                          name != nullptr ? name : "");
      skipAggregateFields(offset, false);
    }
  }

  void indexUnions() {
    MDSectionOffset offset = metadata_->unionsOffset;
    while (metadata_->getOffset(offset) != 0) {
      MDSectionOffset originalOffset = offset;
      const char* name = metadata_->getString(offset);
      offset += sizeof(MDSectionOffset);
      offset += sizeof(uint16_t);
      addSymbol(NativeApiSymbolKind::Union, originalOffset, name);
      addAggregateAliases(NativeApiSymbolKind::Union, originalOffset,
                          name != nullptr ? name : "");
      skipAggregateFields(offset, true);
    }
  }

  void skipMember(MDMemberFlag flags, MDSectionOffset& offset) const {
    if ((flags & metagen::mdMemberProperty) != 0) {
      bool readonly = (flags & metagen::mdMemberReadonly) != 0;
      offset += sizeof(MDSectionOffset);
      offset += sizeof(MDSectionOffset);
      offset += sizeof(MDSectionOffset);
      if (!readonly) {
        offset += sizeof(MDSectionOffset);
        offset += sizeof(MDSectionOffset);
      }
      return;
    }

    offset += sizeof(MDSectionOffset);
    offset += sizeof(MDSectionOffset);
  }

  std::vector<MDSectionOffset> readProtocolOffsetsForClass(
      MDSectionOffset classOffset, MDSectionOffset* memberOffset = nullptr,
      MDSectionOffset* superclassOffsetOut = nullptr) const {
    std::vector<MDSectionOffset> protocols;
    if (metadata_ == nullptr || classOffset == MD_SECTION_OFFSET_NULL) {
      return protocols;
    }

    MDSectionOffset offset = classOffset;
    auto nameOffset = metadata_->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    offset += sizeof(MDSectionOffset);
    bool hasProtocols = (nameOffset & metagen::mdSectionOffsetNext) != 0;

    while (hasProtocols) {
      auto protocolOffset = metadata_->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      hasProtocols = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
      protocolOffset &= ~metagen::mdSectionOffsetNext;
      if (protocolOffset != MD_SECTION_OFFSET_NULL) {
        protocols.push_back(protocolOffset + metadata_->protocolsOffset);
      }
    }

    auto superclass = metadata_->getOffset(offset);
    offset += sizeof(superclass);
    const bool hasMembers = (superclass & metagen::mdSectionOffsetNext) != 0;
    if (superclassOffsetOut != nullptr) {
      MDSectionOffset superclassOffset =
          superclass & ~metagen::mdSectionOffsetNext;
      *superclassOffsetOut =
          superclassOffset != MD_SECTION_OFFSET_NULL
              ? superclassOffset + metadata_->classesOffset
              : MD_SECTION_OFFSET_NULL;
    }
    if (memberOffset != nullptr) {
      *memberOffset = hasMembers ? offset : MD_SECTION_OFFSET_NULL;
    }
    return protocols;
  }

  std::vector<NativeApiMember> readOwnMembersForClass(
      MDSectionOffset classOffset) const {
    std::vector<NativeApiMember> members;
    if (metadata_ == nullptr || classOffset == MD_SECTION_OFFSET_NULL) {
      return members;
    }

    MDSectionOffset memberOffset = MD_SECTION_OFFSET_NULL;
    for (MDSectionOffset protocolOffset :
         readProtocolOffsetsForClass(classOffset, &memberOffset)) {
      auto protocol = protocolSymbolsByOffset_.find(protocolOffset);
      if (protocol == protocolSymbolsByOffset_.end()) {
        continue;
      }
      const auto& protocolMembers = membersForProtocol(protocol->second);
      members.insert(members.end(), protocolMembers.begin(),
                     protocolMembers.end());
    }

    if (memberOffset != MD_SECTION_OFFSET_NULL) {
      std::vector<NativeApiMember> ownMembers =
          readMembersAtOffset(memberOffset);
      members.insert(members.end(), ownMembers.begin(), ownMembers.end());
    }
    return members;
  }

  std::vector<NativeApiMember> readMembersForClass(
      MDSectionOffset classOffset) const {
    std::vector<NativeApiMember> members;
    if (metadata_ == nullptr || classOffset == MD_SECTION_OFFSET_NULL) {
      return members;
    }

    MDSectionOffset offset = classOffset;
    auto nameOffset = metadata_->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    offset += sizeof(MDSectionOffset);
    bool hasProtocols = (nameOffset & metagen::mdSectionOffsetNext) != 0;

    while (hasProtocols) {
      auto protocolOffset = metadata_->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      hasProtocols = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
    }

    auto superclass = metadata_->getOffset(offset);
    offset += sizeof(superclass);

    bool next = (superclass & metagen::mdSectionOffsetNext) != 0;
    while (next) {
      auto flags = metadata_->getMemberFlag(offset);
      next = (flags & metagen::mdMemberNext) != 0;
      offset += sizeof(flags);
      if (flags == metagen::mdMemberFlagNull) {
        break;
      }

      NativeApiMember member;
      member.flags = flags;
      if ((flags & metagen::mdMemberProperty) != 0) {
        member.property = true;
        member.readonly = (flags & metagen::mdMemberReadonly) != 0;
        member.name = metadata_->getString(offset);
        offset += sizeof(MDSectionOffset);
        member.selectorName = metadata_->getString(offset);
        offset += sizeof(MDSectionOffset);
        member.signatureOffset =
            metadata_->signaturesOffset + metadata_->getOffset(offset);
        offset += sizeof(MDSectionOffset);

        if (!member.readonly) {
          member.setterSelectorName = metadata_->getString(offset);
          offset += sizeof(MDSectionOffset);
          member.setterSignatureOffset =
              metadata_->signaturesOffset + metadata_->getOffset(offset);
          offset += sizeof(MDSectionOffset);
        }
      } else {
        member.selectorName = metadata_->getString(offset);
        offset += sizeof(MDSectionOffset);
        member.signatureOffset =
            metadata_->signaturesOffset + metadata_->getOffset(offset);
        offset += sizeof(MDSectionOffset);
        member.name = jsifySelector(member.selectorName.c_str());
      }
      members.push_back(std::move(member));
    }

    return members;
  }

  static bool memberIsStatic(const NativeApiMember& member) {
    return (member.flags & metagen::mdMemberStatic) != 0;
  }

  static bool sameMemberSlot(const NativeApiMember& lhs,
                             const NativeApiMember& rhs) {
    return lhs.name == rhs.name && lhs.property == rhs.property &&
           memberIsStatic(lhs) == memberIsStatic(rhs);
  }

  static bool sameMethodSelector(const NativeApiMember& lhs,
                                 const NativeApiMember& rhs) {
    return !lhs.property && !rhs.property && sameMemberSlot(lhs, rhs) &&
           lhs.selectorName == rhs.selectorName;
  }

  static const NativeApiMember* findPropertyMember(
      const std::vector<NativeApiMember>& members,
      const NativeApiMember& candidate) {
    for (const auto& member : members) {
      if (member.property && sameMemberSlot(member, candidate)) {
        return &member;
      }
    }
    return nullptr;
  }

  static bool selectorExistsInMembers(
      const std::vector<NativeApiMember>& members,
      const NativeApiMember& candidate) {
    for (const auto& member : members) {
      if (sameMethodSelector(member, candidate)) {
        return true;
      }
    }
    return false;
  }

  static bool shouldSkipPropertyOverride(
      const NativeApiMember* inherited, const NativeApiMember& member) {
    if (inherited == nullptr || !inherited->property) {
      return false;
    }

    bool sameGetter = inherited->selectorName == member.selectorName;
    bool sameSetter =
        inherited->setterSelectorName == member.setterSelectorName;
    if ((!inherited->readonly && member.readonly) ||
        (inherited->readonly == member.readonly && sameGetter &&
         (member.readonly || sameSetter))) {
      return true;
    }
    return false;
  }

  static void appendSurfaceMember(
      std::vector<NativeApiMember>& surface,
      const std::vector<NativeApiMember>& inheritedMembers,
      const NativeApiMember& member) {
    if (member.name.empty()) {
      return;
    }

    if (member.property) {
      const NativeApiMember* inherited =
          findPropertyMember(inheritedMembers, member);
      if (shouldSkipPropertyOverride(inherited, member)) {
        return;
      }

      for (auto& existing : surface) {
        if (!existing.property || !sameMemberSlot(existing, member)) {
          continue;
        }
        if (existing.readonly && !member.readonly) {
          existing = member;
        }
        return;
      }
      surface.push_back(member);
      return;
    }

    const bool keepInheritedMethod =
        member.name == "alloc" || member.name == "toString" ||
        member.name == "superclass";
    if (!keepInheritedMethod &&
        selectorExistsInMembers(inheritedMembers, member)) {
      return;
    }
    if (selectorExistsInMembers(surface, member)) {
      return;
    }
    surface.push_back(member);
  }

  std::vector<NativeApiMember> readSurfaceMembersForClass(
      const NativeApiSymbol& symbol) const {
    std::vector<NativeApiMember> inheritedMembers;
    if (symbol.superclassOffset != MD_SECTION_OFFSET_NULL) {
      auto superclass = classSymbolsByOffset_.find(symbol.superclassOffset);
      if (superclass != classSymbolsByOffset_.end()) {
        const auto& inherited = surfaceMembersForClass(superclass->second);
        inheritedMembers.insert(inheritedMembers.end(), inherited.begin(),
                                inherited.end());
      }
    }

    std::vector<NativeApiMember> surface;
    for (const auto& member : readOwnMembersForClass(symbol.offset)) {
      appendSurfaceMember(surface, inheritedMembers, member);
    }
    return surface;
  }

  std::vector<NativeApiMember> readMembersAtOffset(
      MDSectionOffset& offset) const {
    std::vector<NativeApiMember> members;
    bool next = true;
    while (next) {
      auto flags = metadata_->getMemberFlag(offset);
      next = (flags & metagen::mdMemberNext) != 0;
      offset += sizeof(flags);
      if (flags == metagen::mdMemberFlagNull) {
        break;
      }

      NativeApiMember member;
      member.flags = flags;
      if ((flags & metagen::mdMemberProperty) != 0) {
        member.property = true;
        member.readonly = (flags & metagen::mdMemberReadonly) != 0;
        member.name = metadata_->getString(offset);
        offset += sizeof(MDSectionOffset);
        member.selectorName = metadata_->getString(offset);
        offset += sizeof(MDSectionOffset);
        member.signatureOffset =
            metadata_->signaturesOffset + metadata_->getOffset(offset);
        offset += sizeof(MDSectionOffset);

        if (!member.readonly) {
          member.setterSelectorName = metadata_->getString(offset);
          offset += sizeof(MDSectionOffset);
          member.setterSignatureOffset =
              metadata_->signaturesOffset + metadata_->getOffset(offset);
          offset += sizeof(MDSectionOffset);
        }
      } else {
        member.selectorName = metadata_->getString(offset);
        offset += sizeof(MDSectionOffset);
        member.signatureOffset =
            metadata_->signaturesOffset + metadata_->getOffset(offset);
        offset += sizeof(MDSectionOffset);
        member.name = jsifySelector(member.selectorName.c_str());
      }
      members.push_back(std::move(member));
    }
    return members;
  }

  std::vector<NativeApiMember> readMembersForProtocolHierarchy(
      MDSectionOffset protocolOffset) const {
    std::vector<NativeApiMember> members;
    if (metadata_ == nullptr || protocolOffset == MD_SECTION_OFFSET_NULL) {
      return members;
    }

    MDSectionOffset offset = protocolOffset;
    auto nameOffset = metadata_->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    bool hasProtocols = (nameOffset & metagen::mdSectionOffsetNext) != 0;

    while (hasProtocols) {
      auto inheritedOffset = metadata_->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      hasProtocols = (inheritedOffset & metagen::mdSectionOffsetNext) != 0;
      inheritedOffset &= ~metagen::mdSectionOffsetNext;
      if (inheritedOffset == MD_SECTION_OFFSET_NULL) {
        continue;
      }

      MDSectionOffset absoluteOffset =
          inheritedOffset + metadata_->protocolsOffset;
      auto inheritedSymbol = protocolSymbolsByOffset_.find(absoluteOffset);
      if (inheritedSymbol != protocolSymbolsByOffset_.end()) {
        const auto& inheritedMembers =
            membersForProtocol(inheritedSymbol->second);
        members.insert(members.end(), inheritedMembers.begin(),
                       inheritedMembers.end());
      }
    }

    std::vector<NativeApiMember> ownMembers = readMembersAtOffset(offset);
    members.insert(members.end(), ownMembers.begin(), ownMembers.end());
    return members;
  }

  std::vector<NativeApiMember> readMembersForClassHierarchy(
      const NativeApiSymbol& symbol) const {
    std::vector<NativeApiMember> members = readOwnMembersForClass(symbol.offset);
    if (symbol.superclassOffset == MD_SECTION_OFFSET_NULL) {
      return members;
    }

    auto superclass = classSymbolsByOffset_.find(symbol.superclassOffset);
    if (superclass != classSymbolsByOffset_.end()) {
      const auto& inheritedMembers = membersForClass(superclass->second);
      members.insert(members.end(), inheritedMembers.begin(),
                     inheritedMembers.end());
    }
    return members;
  }

  std::unique_ptr<MDMetadataReader> metadata_;
  void* selfDl_ = nullptr;
  std::unordered_map<std::string, NativeApiSymbol> symbolsByName_;
  std::unordered_map<std::string, NativeApiSymbol> functionSymbolsByName_;
  std::unordered_map<std::string, NativeApiSymbol> constantSymbolsByName_;
  std::unordered_map<std::string, NativeApiSymbol> enumSymbolsByName_;
  std::unordered_map<std::string, NativeApiSymbol> structSymbolsByName_;
  std::unordered_map<std::string, NativeApiSymbol> unionSymbolsByName_;
  std::unordered_map<std::string, NativeApiSymbol> classSymbolsByRuntimeName_;
  std::unordered_map<std::string, NativeApiSymbol> protocolSymbolsByRuntimeName_;
  std::unordered_map<uintptr_t, NativeApiSymbol> classSymbolsByRuntimePointer_;
  std::unordered_map<uintptr_t, NativeApiSymbol> protocolSymbolsByRuntimePointer_;
  mutable std::mutex roundTripValuesMutex_;
  std::unordered_map<uintptr_t, NativeApiRoundTripValue> roundTripValues_;
  std::unordered_map<std::thread::id, NativeApiRoundTripFrameStack>
      roundTripCacheFramesByThread_;
  std::unordered_map<uintptr_t, NativeApiRoundTripValue>
      recentRoundTripValues_;
  std::vector<uintptr_t> recentRoundTripValueOrder_;
#ifdef TARGET_ENGINE_HERMES
  std::unordered_map<uintptr_t, NativeApiWeakRoundTripValue>
      weakRoundTripValues_;
  std::unordered_set<uintptr_t> rootedRoundTripValues_;
  std::shared_ptr<Value> roundTripRootCache_;
#endif
  std::atomic<uint64_t> roundTripValuesGeneration_{1};
  std::unordered_map<uintptr_t, std::shared_ptr<Value>> classValues_;
  std::unordered_map<uintptr_t, std::shared_ptr<Value>> classPrototypes_;
  std::unordered_map<uintptr_t, std::shared_ptr<Value>> pointerValues_;
  mutable std::mutex objectExpandosMutex_;
  std::unordered_map<
      uintptr_t,
      std::unordered_map<
          std::string,
          std::unordered_map<uintptr_t, std::shared_ptr<Value>>>>
      objectExpandos_;
  std::unordered_map<uintptr_t, size_t> objectExpandoOwnerCounts_;
  std::atomic<uint64_t> objectExpandosGeneration_{1};
  std::unordered_map<Class, std::unordered_map<std::string, CachedPropertyGetter>>
      propertyGetterCache_;
  std::atomic<uint64_t> propertyGetterCacheGeneration_{1};
  std::unordered_map<MDSectionOffset, NativeApiSymbol> classSymbolsByOffset_;
  std::unordered_map<MDSectionOffset, NativeApiSymbol> protocolSymbolsByOffset_;
  std::vector<std::string> classNames_;
  std::vector<std::string> functionNames_;
  std::vector<std::string> constantNames_;
  std::vector<std::string> protocolNames_;
  std::vector<std::string> enumNames_;
  std::vector<std::string> structNames_;
  std::vector<std::string> unionNames_;
  std::shared_ptr<NativeApiScheduler> scheduler_;
  std::function<void(std::function<void()>)> nativeInvocationInvoker_;
  std::function<void(std::function<void()>)> nativeCallbackInvoker_;
  std::function<void(std::function<void()>)> runtimeCallbackInvoker_;
  std::function<void(std::function<void()>)> jsThreadCallbackInvoker_;
  std::function<void(std::function<void()>)> jsThreadAsyncCallbackInvoker_;
  std::function<bool()> callbackInvocationAllowed_;
  bool invokeCallbacksOnNativeCallerThread_ = false;
  bool indexRuntimePointers_ = true;
  mutable std::unordered_map<MDSectionOffset, std::vector<NativeApiMember>>
      membersByClassOffset_;
  mutable std::unordered_map<MDSectionOffset, std::vector<NativeApiMember>>
      surfaceMembersByClassOffset_;
  mutable std::unordered_map<MDSectionOffset, std::vector<NativeApiMember>>
      membersByProtocolOffset_;
  // Keyed by runtime Class pointer (not metadata offset -- see
  // protocolMembersForRuntimeClass for why). Caches the protocol-conformance
  // method-metadata fallback.
  mutable std::unordered_map<uintptr_t, std::vector<NativeApiMember>>
      protocolMembersByRuntimeClass_;
  std::unordered_map<MDSectionOffset, NativeApiSymbol> structSymbolsByOffset_;
  std::unordered_map<MDSectionOffset, NativeApiSymbol> unionSymbolsByOffset_;
  std::unordered_map<MDSectionOffset, std::shared_ptr<NativeApiAggregateInfo>>
      aggregateInfoByOffset_;
  std::unordered_set<MDSectionOffset> aggregateInfoInProgress_;
  std::thread::id jsThreadId_ = std::this_thread::get_id();
  std::mutex retainedLifetimesMutex_;
  std::vector<std::shared_ptr<void>> retainedLifetimes_;
};

class NativeApiRoundTripCacheFrameGuard final {
 public:
  explicit NativeApiRoundTripCacheFrameGuard(
      const std::shared_ptr<NativeApiBridge>& bridge)
      : bridge_(bridge) {
    if (bridge_ != nullptr) {
      bridge_->beginRoundTripCacheFrame();
    }
  }

  ~NativeApiRoundTripCacheFrameGuard() {
    if (bridge_ != nullptr) {
      bridge_->endRoundTripCacheFrame();
    }
  }

  NativeApiRoundTripCacheFrameGuard(
      const NativeApiRoundTripCacheFrameGuard&) = delete;
  NativeApiRoundTripCacheFrameGuard& operator=(
      const NativeApiRoundTripCacheFrameGuard&) = delete;

 private:
  std::shared_ptr<NativeApiBridge> bridge_;
};

template <typename Invocation>
void performGeneratedObjCInvocation(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    Invocation&& invocation) {
  const auto& invoker = bridge->nativeInvocationInvoker();
  if (invoker || bridge->invokeCallbacksOnNativeCallerThread()) {
    performNativeInvocation(runtime, invoker, [&]() { invocation(); });
  } else {
    invocation();
  }
}

bool nativeObjectReturnMayCoerceToString(const NativeApiType& type) {
  return type.kind == metagen::mdTypeAnyObject ||
         type.kind == metagen::mdTypeNSStringObject;
}

// Guards against treating a misread register value as an object pointer:
// low addresses can't be valid ObjC objects, but a register holding e.g. an
// unboxed integer or a non-object primitive read as `id` can land there.
// Without this guard, dereferencing it (object_getClass, isKindOfClass:,
// etc. below) can crash on garbage. Do not remove — this is what stops
// crashes on AnyObject-typed returns from selectors whose actual return
// isn't an object.
bool nativeObjectPointerMayBeObject(id object) {
  if (object == nil) {
    return false;
  }

  const uintptr_t raw = reinterpret_cast<uintptr_t>(object);
  return raw > 0x1000;
}

bool nativeObjectIsStringLike(id object) {
  if (!nativeObjectPointerMayBeObject(object)) {
    return false;
  }
  Class cls = object_getClass(object);
  struct StringLikeClassCacheEntry {
    Class cls = Nil;
    bool stringLike = false;
  };
  static thread_local StringLikeClassCacheEntry cache[4];
  const size_t firstSlot = (reinterpret_cast<uintptr_t>(cls) >> 4) & 3;
  for (size_t i = 0; i < 4; i++) {
    const auto& entry = cache[(firstSlot + i) & 3];
    if (entry.cls == cls) {
      return entry.stringLike;
    }
  }
  bool stringLike = [object isKindOfClass:[NSString class]];
  cache[firstSlot] = StringLikeClassCacheEntry{cls, stringLike};
  return stringLike;
}

Value findCachedNativeObjectReturn(Runtime& runtime,
                                   const std::shared_ptr<NativeApiBridge>& bridge,
                                   const NativeApiType& type, id object) {
  if (!nativeObjectPointerMayBeObject(object)) {
    return Value::undefined();
  }

  bool roundTripStringLike = false;
  const bool stringReturnCandidate = nativeObjectReturnMayCoerceToString(type);
  // AnyObject/NSString returns intentionally coerce string-like native objects
  // to JS strings, so cached identity is only valid for non-string wrappers.
  if (stringReturnCandidate && nativeObjectIsStringLike(object)) {
    return Value::undefined();
  }
  Value roundTrip = bridge->findRoundTripValue(
      runtime, object, stringReturnCandidate ? &roundTripStringLike : nullptr,
      true);
  if (!roundTrip.isUndefined() && !roundTripStringLike) {
    return roundTrip;
  }
  return Value::undefined();
}

Value makeString(Runtime& runtime, const std::string& value) {
  return String::createFromUtf8(runtime, value);
}

std::string readStringArg(Runtime& runtime, const Value* args, size_t count,
                          size_t index, const char* argumentName) {
  if (index >= count || !args[index].isString()) {
    throw JSError(
        runtime, std::string(argumentName) + " must be a string.");
  }
  return args[index].asString(runtime).utf8(runtime);
}

const char* kindName(NativeApiSymbolKind kind) {
  switch (kind) {
    case NativeApiSymbolKind::Class:
      return "class";
    case NativeApiSymbolKind::Function:
      return "function";
    case NativeApiSymbolKind::Constant:
      return "constant";
    case NativeApiSymbolKind::Protocol:
      return "protocol";
    case NativeApiSymbolKind::Enum:
      return "enum";
    case NativeApiSymbolKind::Struct:
      return "struct";
    case NativeApiSymbolKind::Union:
      return "union";
  }
  return "unknown";
}

Array namesToArray(Runtime& runtime, const std::vector<std::string>& names) {
  Array result(runtime, names.size());
  for (size_t i = 0; i < names.size(); i++) {
    result.setValueAtIndex(runtime, i, makeString(runtime, names[i]));
  }
  return result;
}

void addPropertyName(Runtime& runtime, std::vector<PropNameID>& names,
                     const char* name) {
  names.push_back(PropNameID::forAscii(runtime, name));
}

class NativeApiPointerHostObject;
class NativeApiObjectHostObject;
class NativeApiClassHostObject;
class NativeApiProtocolHostObject;
class NativeApiArgumentFrame;
struct NativeApiPreparedCFunctionInvocation;
struct NativeApiPreparedObjCInvocation;

Value callCFunction(Runtime& runtime,
                    const std::shared_ptr<NativeApiBridge>& bridge,
                    const NativeApiSymbol& symbol, const Value* args,
                    size_t count);
Value callCFunction(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const std::shared_ptr<NativeApiPreparedCFunctionInvocation>& prepared,
    const Value* args, size_t count);

Value callObjCSelector(Runtime& runtime,
                       const std::shared_ptr<NativeApiBridge>& bridge,
                       id receiver, bool receiverIsClass,
                       const std::string& selectorName,
                       const NativeApiMember* member,
                       const Value* args, size_t count,
                       Class dispatchSuperClass = Nil);

std::shared_ptr<NativeApiPreparedObjCInvocation>
prepareNativeApiObjCInvocation(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    Class lookupClass, bool receiverIsClass, const std::string& selectorName,
    const NativeApiMember* member);

Value callPreparedObjCSelector(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, bool receiverIsClass,
    const NativeApiPreparedObjCInvocation& prepared, const Value* args,
    size_t count, Class dispatchSuperClass = Nil);

void* lookupGeneratedEngineObjCGsdInvoker(uint64_t dispatchId);
bool tryCallGeneratedEngineObjCSelector(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const NativeApiPreparedObjCInvocation& prepared,
    const Value* args, size_t count, Class dispatchSuperClass, Value* result);

Function CreateNativeApiSelectorGroupFunction(
    Runtime& runtime, std::shared_ptr<NativeApiBridge> bridge,
    Class lookupClass, bool receiverIsClass,
    std::shared_ptr<std::vector<NativeApiSelectorGroupEntry>> selectors,
    std::shared_ptr<
        std::vector<std::shared_ptr<NativeApiPreparedObjCInvocation>>>
        preparedInvocations);

Function CreateNativeApiBoundSelectorGroupFunction(
    Runtime& runtime, std::shared_ptr<NativeApiBridge> bridge, Class lookupClass,
    std::shared_ptr<NativeApiObjectHostObject> receiverHostObject,
    std::shared_ptr<std::vector<NativeApiSelectorGroupEntry>> selectors,
    std::shared_ptr<
        std::vector<std::shared_ptr<NativeApiPreparedObjCInvocation>>>
        preparedInvocations);

Value makeNativeObjectValue(Runtime& runtime,
                            const std::shared_ptr<NativeApiBridge>& bridge,
                            id object, bool ownsObject,
                            Class superDispatchClass = Nil);

Value makeNativeClassValue(Runtime& runtime,
                           const std::shared_ptr<NativeApiBridge>& bridge,
                           NativeApiSymbol symbol);

Object symbolToObject(Runtime& runtime, const NativeApiSymbol& symbol) {
  Object result(runtime);
  result.setProperty(runtime, "kind", makeString(runtime, kindName(symbol.kind)));
  result.setProperty(runtime, "name", makeString(runtime, symbol.name));
  result.setProperty(runtime, "runtimeName",
                     makeString(runtime, symbol.runtimeName));
  result.setProperty(runtime, "metadataOffset",
                     static_cast<double>(symbol.offset));

  if (symbol.kind == NativeApiSymbolKind::Class) {
    Class cls = objc_lookUpClass(symbol.runtimeName.c_str());
    result.setProperty(runtime, "available", cls != nil);
    if (cls != nil) {
      char address[32] = {};
      snprintf(address, sizeof(address), "%p", cls);
      result.setProperty(runtime, "nativeAddress", makeString(runtime, address));
    }
  } else if (symbol.kind == NativeApiSymbolKind::Struct ||
             symbol.kind == NativeApiSymbolKind::Union) {
    result.setProperty(runtime, "available", true);
  }

  return result;
}

size_t nativeSizeForType(const NativeApiType& type);
std::optional<size_t> parseArrayIndexProperty(const std::string& property);

NativeApiType nativeObjectReturnType(
    MDTypeKind kind = metagen::mdTypeAnyObject) {
  NativeApiType type;
  type.kind = kind;
  type.ffiType = &ffi_type_pointer;
  type.supported = true;
  return type;
}

NativeApiType nativeObjectReturnTypeForClass(Class cls) {
  if (cls != Nil) {
    const char* name = class_getName(cls);
    if (name != nullptr && std::strcmp(name, "NSString") == 0) {
      return nativeObjectReturnType(metagen::mdTypeNSStringObject);
    }
    if (name != nullptr && std::strcmp(name, "NSMutableString") == 0) {
      return nativeObjectReturnType(metagen::mdTypeNSMutableStringObject);
    }
  }
  return nativeObjectReturnType(metagen::mdTypeInstanceObject);
}

Value convertNativeReturnValue(Runtime& runtime,
                               const std::shared_ptr<NativeApiBridge>& bridge,
                               const NativeApiType& type, void* value);
Object createPointer(Runtime& runtime,
                     const std::shared_ptr<NativeApiBridge>& bridge,
                     void* pointer, bool adopted = false,
                     std::shared_ptr<Value> backingValue = nullptr);

NativeApiType primitiveInteropType(MDTypeKind kind);
