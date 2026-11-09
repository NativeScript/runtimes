#include "Object.h"
#include <cstring>
#include "Interop.h"
#include "JSObject.h"
#include "ObjCBridge.h"
#include "js_native_api.h"
#include "node_api_util.h"

#import <Foundation/Foundation.h>
#include <objc/runtime.h>

static SEL JSWrapperObjectAssociationKey = @selector(JSWrapperObjectAssociationKey);
static SEL ObjCLifecycleAssociationKey = @selector(ObjCLifecycleAssociationKey);

@interface JSWrapperObjectAssociation : NSObject

@property(nonatomic) napi_env env;
@property(nonatomic) napi_ref ref;
@property(nonatomic) nativescript::ObjCBridgeState* bridgeState;
@property(nonatomic) uint64_t bridgeStateToken;

+ (void)transferOwnership:(napi_env)env of:(napi_value)value toNative:(id)object;

+ (instancetype)associationFor:(id)object;

- (instancetype)initWithEnv:(napi_env)env ref:(napi_ref)ref;

@end

@interface ObjCLifecycleAssociation : NSObject {
  nativescript::ObjCBridgeState* _bridgeState;
  uint64_t _bridgeStateToken;
  uintptr_t _objectAddress;
}

- (instancetype)initWithBridgeState:(nativescript::ObjCBridgeState*)bridgeState object:(id)object;

@end

@implementation JSWrapperObjectAssociation

- (instancetype)initWithEnv:(napi_env)env ref:(napi_ref)ref {
  self = [super init];
  if (self) {
    self.env = env;
    self.ref = ref;
    self.bridgeState = nativescript::ObjCBridgeState::InstanceData(env);
    self.bridgeStateToken = self.bridgeState != nullptr ? self.bridgeState->lifetimeToken : 0;
  }
  return self;
}

+ (void)transferOwnership:(napi_env)env of:(napi_value)value toNative:(id)object {
  napi_ref ref = nativescript::make_ref(env, value);
  JSWrapperObjectAssociation* association = [[JSWrapperObjectAssociation alloc] initWithEnv:env
                                                                                        ref:ref];
  objc_setAssociatedObject(object, JSWrapperObjectAssociationKey, association,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  [association release];
}

+ (instancetype)associationFor:(id)object {
  return objc_getAssociatedObject(object, JSWrapperObjectAssociationKey);
}

- (void)dealloc {
  if (self.env != nullptr && self.ref != nullptr &&
      (self.bridgeState == nullptr ||
       nativescript::IsBridgeStateLive(self.bridgeState, self.bridgeStateToken))) {
    nativescript::DeleteReferenceOnOwningThread(self.env, self.bridgeState,
                                                self.bridgeStateToken, self.ref);
    self.ref = nullptr;
  }
  self.bridgeState = nullptr;
  self.bridgeStateToken = 0;
  self.env = nullptr;
  [super dealloc];
}

@end

@implementation ObjCLifecycleAssociation

- (instancetype)initWithBridgeState:(nativescript::ObjCBridgeState*)bridgeState object:(id)object {
  self = [super init];
  if (self) {
    _bridgeState = bridgeState;
    _bridgeStateToken = bridgeState != nullptr ? bridgeState->lifetimeToken : 0;
    _objectAddress = (uintptr_t)object;
  }

  return self;
}

- (void)dealloc {
  nativescript::ObjCBridgeState* bridgeState = _bridgeState;
  uint64_t bridgeStateToken = _bridgeStateToken;
  uintptr_t objectAddress = _objectAddress;
  if (nativescript::IsBridgeStateLive(bridgeState, bridgeStateToken)) {
    bridgeState->detachObject((id)objectAddress);
  }

  [super dealloc];
}

@end

napi_value JS_transferOwnershipToNative(napi_env env, napi_callback_info cbinfo) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, cbinfo, &argc, &arg, nullptr, nullptr) != napi_ok || argc == 0 ||
      arg == nullptr) {
    return nullptr;
  }

  id obj = nil;
  if (napi_unwrap(env, arg, (void**)&obj) != napi_ok || obj == nil) {
    return nullptr;
  }

  [JSWrapperObjectAssociation transferOwnership:env of:arg toNative:obj];

  return nullptr;
}

namespace nativescript {

napi_value findConstructorForObject(napi_env env, ObjCBridgeState* bridgeState, id object,
                                    Class cls = nil);

void transferOwnershipToNative(napi_env env, napi_value value, id object) {
  if (env == nullptr || value == nullptr || object == nil) {
    return;
  }

  [JSWrapperObjectAssociation transferOwnership:env of:value toNative:object];
}

namespace {
constexpr const char* kNativePointerProperty = "__ns_native_ptr";

napi_value findConstructorForClassObject(napi_env env, ObjCBridgeState* bridgeState, Class cls,
                                         napi_value fallback = nullptr) {
  if (bridgeState == nullptr || cls == nil) {
    return fallback;
  }

  auto bridgedClass = bridgeState->classesByPointer.find(cls);
  if (bridgedClass != bridgeState->classesByPointer.end() && bridgedClass->second != nullptr) {
    return get_ref_value(env, bridgedClass->second->constructor);
  }

  auto constructedClass = bridgeState->constructorsByPointer.find(cls);
  if (constructedClass != bridgeState->constructorsByPointer.end()) {
    napi_value constructor = get_ref_value(env, constructedClass->second);
    if (constructor != nullptr) {
      return constructor;
    }
  }

  auto metadataClass = bridgeState->mdClassesByPointer.find(cls);
  if (metadataClass != bridgeState->mdClassesByPointer.end()) {
    auto bridgedMetadataClass = bridgeState->getClass(env, metadataClass->second);
    if (bridgedMetadataClass != nullptr) {
      return get_ref_value(env, bridgedMetadataClass->constructor);
    }
  }

  const char* runtimeName = class_getName(cls);
  if (runtimeName != nullptr && runtimeName[0] != '\0') {
    napi_value global = nullptr;
    napi_value constructor = nullptr;
    bool hasGlobal = false;
    napi_get_global(env, &global);
    if (napi_has_named_property(env, global, runtimeName, &hasGlobal) == napi_ok && hasGlobal &&
        napi_get_named_property(env, global, runtimeName, &constructor) == napi_ok &&
        constructor != nullptr) {
      return constructor;
    }
  }

  napi_value resolved = findConstructorForObject(env, bridgeState, (id)cls, cls);
  return resolved != nullptr ? resolved : fallback;
}
}  // namespace

const char* nativeObjectProxySource = R"(
  (function (object, isArray, transferOwnershipToNative) {
    let isTransfered = false;
    const boundMethods = Object.create(null);
    let objectAtIndexMethod;
    let addObjectMethod;
    let removeObjectAtIndexMethod;
    let setObjectAtIndexedSubscriptMethod;

    function bindTargetMethod(target, name) {
      const cached = boundMethods[name];
      if (cached !== undefined) {
        return cached;
      }

      const value = target[name];
      if (typeof value !== "function") {
        return value;
      }

      if (value.__ns_proxy_bound === true) {
        boundMethods[name] = value;
        return value;
      }

      const wrapper = value.bind(target);
      Object.defineProperty(wrapper, "__ns_proxy_bound", { value: true });
      boundMethods[name] = wrapper;
      try {
        Object.defineProperty(target, name, {
          value: wrapper,
          configurable: true,
          writable: true
        });
      } catch (_) {
      }
      return wrapper;
    }

    return new Proxy(object, {
      get (target, name, receiver) {
        if (name === "superclass" && typeof target.class === "function") {
          return target.class().superclass();
        }

        if (isArray && name === "count") {
          return target.count;
        }

        if (isArray) {
          switch (name) {
            case "objectAtIndex":
              return objectAtIndexMethod !== undefined
                ? objectAtIndexMethod
                : (objectAtIndexMethod = bindTargetMethod(target, name));
            case "addObject":
              return addObjectMethod !== undefined
                ? addObjectMethod
                : (addObjectMethod = bindTargetMethod(target, name));
            case "removeObjectAtIndex":
              return removeObjectAtIndexMethod !== undefined
                ? removeObjectAtIndexMethod
                : (removeObjectAtIndexMethod = bindTargetMethod(target, name));
            case "setObjectAtIndexedSubscript":
              return setObjectAtIndexedSubscriptMethod !== undefined
                ? setObjectAtIndexedSubscriptMethod
                : (setObjectAtIndexedSubscriptMethod = bindTargetMethod(target, name));
          }
        }

        if (typeof name === "string") {
          const boundMethod = boundMethods[name];
          if (boundMethod !== undefined) {
            return boundMethod;
          }
        }

        const value = target[name];
        if (value !== undefined || name in target) {
          if (typeof value === "function" && name !== "constructor") {
            if (value.__ns_proxy_bound === true) {
              return value;
            }

            let wrapper;
            if ((name === "isKindOfClass" || name === "isMemberOfClass")) {
              wrapper = function (cls, a1, a2, a3) {
                let resolvedClass = cls;
                const resolvedClassType = typeof resolvedClass;
                if (resolvedClass != null &&
                    (resolvedClassType === "object" || resolvedClassType === "function")) {
                  try {
                    const runtimeName = typeof NSStringFromClass === "function"
                      ? NSStringFromClass(resolvedClass)
                      : null;
	                    if (typeof runtimeName === "string" && runtimeName.length > 0) {
	                      const registry = globalThis.__nsConstructorsByObjCClassName;
	                      if (registry && registry[runtimeName]) {
	                        resolvedClass = registry[runtimeName];
	                      } else if (typeof globalThis[runtimeName] !== "undefined") {
	                        resolvedClass = globalThis[runtimeName];
	                      }
	                    }
                  } catch (_) {
                  }
                }

                switch (arguments.length) {
                  case 0:
                  case 1:
                    return value.call(target, resolvedClass);
                  case 2:
                    return value.call(target, resolvedClass, a1);
                  case 3:
                    return value.call(target, resolvedClass, a1, a2);
                  case 4:
                    return value.call(target, resolvedClass, a1, a2, a3);
                  default: {
                    const args = Array.prototype.slice.call(arguments);
                    args[0] = resolvedClass;
                    return Reflect.apply(value, target, args);
                  }
                }
              };
            } else {
              wrapper = value.bind(target);
            }

            Object.defineProperty(wrapper, "__ns_proxy_bound", { value: true });
            if (typeof name === "string") {
              boundMethods[name] = wrapper;
            }
            try {
              Object.defineProperty(target, name, {
                value: wrapper,
                configurable: true,
                writable: true
              });
            } catch (_) {
            }
            return wrapper;
          }
          return value;
        }

        if (typeof name === 'symbol') {
          return undefined;
        }

        if (isArray) {
          const index = Number(name);
          if (!isNaN(index)) {
            return target.objectAtIndex(index);
          }
        }
      },

      set (target, name, value) {
        const isInternalProperty = typeof name === 'string' &&
          (name === 'napi_external' || name === 'napi_typetag' || name === '__ns_native_ptr');

        if (typeof name === 'symbol') {
          target[name] = value;
          return true;
        }

        if (typeof name === "string" && boundMethods[name] !== undefined) {
          delete boundMethods[name];
        }

        if (isArray) {
          switch (name) {
            case "objectAtIndex":
              objectAtIndexMethod = undefined;
              break;
            case "addObject":
              addObjectMethod = undefined;
              break;
            case "removeObjectAtIndex":
              removeObjectAtIndexMethod = undefined;
              break;
            case "setObjectAtIndexedSubscript":
              setObjectAtIndexedSubscriptMethod = undefined;
              break;
          }
        }

        if (isArray) {
          const index = Number(name);
          if (!isNaN(index)) {
            target.setObjectAtIndexedSubscript(value, index);
            return true;
          }
        }

        if (!isInternalProperty && !(name in target) && !isTransfered) {
          isTransfered = true;
          transferOwnershipToNative(target);
        }

        target[name] = value;

        return true;
      },
    });
  })
)";

void initProxyFactory(napi_env env, ObjCBridgeState* state) {
  napi_value script, result;
  napi_create_string_utf8(env, nativeObjectProxySource, NAPI_AUTO_LENGTH, &script);
  napi_run_script(env, script, &result);
  state->createNativeProxy = make_ref(env, result);

  napi_value transferOwnershipToNative;
  napi_create_function(env, "transferOwnershipToNative", NAPI_AUTO_LENGTH,
                       JS_transferOwnershipToNative, nullptr, &transferOwnershipToNative);
  state->transferOwnershipToNative = make_ref(env, transferOwnershipToNative);
}

void attachObjectLifecycleAssociation(napi_env env, id object) {
  if (object == nil) {
    return;
  }

  auto bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr) {
    return;
  }

  if (objc_getAssociatedObject(object, ObjCLifecycleAssociationKey) != nil) {
    return;
  }

  ObjCLifecycleAssociation* association =
      [[ObjCLifecycleAssociation alloc] initWithBridgeState:bridgeState object:object];
  objc_setAssociatedObject(object, ObjCLifecycleAssociationKey, association,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  [association release];
}

namespace {
void finalize_objc_object_now(napi_env env, void* data, void* hint) {
  (void)hint;
  JSObjectFinalizerContext* context = static_cast<JSObjectFinalizerContext*>(data);
  if (context == nullptr) {
    return;
  }

  ObjCBridgeState* bridgeState = context->bridgeState;
  if (IsBridgeStateLive(bridgeState, context->bridgeStateToken)) {
    bridgeState->unregisterObjectIfRefMatches(context->object, context->ref);
  }

#if defined(TARGET_ENGINE_HERMES)
  if (env != nullptr && context->ref != nullptr) {
    napi_delete_reference(env, context->ref);
    context->ref = nullptr;
  }
#endif

  delete context;
}
}  // namespace

void finalize_objc_object(napi_env env, void* data, void* hint) {
  if (PostFinalizer(env, finalize_objc_object_now, data, hint)) {
    return;
  }

  finalize_objc_object_now(env, data, hint);
}

napi_value ObjCBridgeState::getObject(napi_env env, id obj, napi_value constructor,
                                      ObjectOwnership ownership) {
  if (obj == nil) {
    return nullptr;
  }

  NS_OBJC_NAPI_PREAMBLE

  Class cls = object_getClass(obj);

  if (cls == nullptr) {
    return nullptr;
  }

  if (class_isMetaClass(cls)) {
    return findConstructorForClassObject(env, this, (Class)obj, constructor);
  }

  napi_value resolvedConstructor = constructor;
  if (resolvedConstructor != nullptr) {
    Class hintedClass = nil;
    tryResolveBridgedClassConstructor(env, resolvedConstructor, &hintedClass);
  }

  napi_value actualConstructor = findConstructorForObject(env, this, obj, cls);
  if (actualConstructor != nullptr) {
    resolvedConstructor = actualConstructor;
  }

  if (napi_value cached = findCachedObjectWrapper(env, obj); cached != nullptr) {
    return cached;
  }

  napi_value result = nil;
  napi_value prototype;
  NAPI_GUARD(napi_get_named_property(env, resolvedConstructor, "prototype", &prototype)) {
    NAPI_THROW_LAST_ERROR
    return nullptr;
  }

  NAPI_GUARD(napi_create_object(env, &result)) {
    NAPI_THROW_LAST_ERROR
    return nullptr;
  }

  napi_value global;
  napi_value objectCtor;
  napi_value setPrototypeOf;
  napi_value argv[2] = {result, prototype};
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Object", &objectCtor);
  napi_get_named_property(env, objectCtor, "setPrototypeOf", &setPrototypeOf);

  NAPI_GUARD(napi_call_function(env, objectCtor, setPrototypeOf, 2, argv, nullptr)) {
    NAPI_THROW_LAST_ERROR
    return nullptr;
  }

  napi_wrap(env, result, obj, nullptr, nullptr, nullptr);
  napi_value nativePointer = Pointer::create(env, obj);
  if (nativePointer != nullptr) {
    napi_set_named_property(env, result, kNativePointerProperty, nativePointer);
  }

  if (ownership == kUnownedObject) {
    [obj retain];
  }

  result = proxyNativeObject(env, result, obj);

  // #if DEBUG
  // napi_value global, Error, error, stack;
  // napi_get_global(env, &global);
  // napi_get_named_property(env, global, "Error", &Error);
  // napi_new_instance(env, Error, 0, nullptr, &error);
  // napi_get_named_property(env, error, "stack", &stack);

  // size_t stackSize;
  // napi_get_value_string_utf8(env, stack, nullptr, 0, &stackSize);
  // char *stackStr = new char[stackSize + 1];
  // napi_get_value_string_utf8(env, stack, stackStr, stackSize + 1, nullptr);

  // NSString *str = [NSString stringWithFormat:@"Wrapped object <%s: %p> @ %ld # %s",
  //       class_getName(cls), obj, [obj retainCount], stackStr];
  // dbglog([str UTF8String]);

  // delete[] stackStr;
  // #endif

  return result;
}

napi_value ObjCBridgeState::findCachedObjectWrapper(napi_env env, id obj) {
  if (obj == nil) {
    return nullptr;
  }

  if (napi_value jsObject = idToJsObject(env, obj); jsObject != nullptr) {
    return jsObject;
  }

  auto roundTrip = getRoundTripObject(env, obj);
  if (roundTrip != nullptr) {
    return roundTrip;
  }

  if (napi_value recentWrapper = getRecentObjectWrapper(env, obj); recentWrapper != nullptr) {
    return recentWrapper;
  }

  if (napi_value handleCached = getCachedHandleObject(env, (void*)obj); handleCached != nullptr) {
    bool isRawHandleRoundTrip = ownsCachedHandleObjectRef((void*)obj);
    void* wrapped = nullptr;
    if (napi_unwrap(env, handleCached, &wrapped) == napi_ok &&
        NormalizeHandleKey(wrapped) == NormalizeHandleKey((void*)obj)) {
      return handleCached;
    }

    bool hasNativePointer = false;
    if (napi_has_named_property(env, handleCached, kNativePointerProperty, &hasNativePointer) ==
            napi_ok &&
        hasNativePointer) {
      napi_value nativePointerValue = nullptr;
      if (napi_get_named_property(env, handleCached, kNativePointerProperty, &nativePointerValue) ==
              napi_ok &&
          Pointer::isInstance(env, nativePointerValue)) {
        Pointer* pointer = Pointer::unwrap(env, nativePointerValue);
        if (pointer != nullptr &&
            NormalizeHandleKey(pointer->data) == NormalizeHandleKey((void*)obj)) {
          return handleCached;
        }
      }
    }

    if (isRawHandleRoundTrip) {
      return handleCached;
    }

    removeCachedHandleObject(env, (void*)obj);
  }

  if (napi_value existing = getNormalizedObjectRef(env, obj); existing != nullptr) {
    return existing;
  }

  JSWrapperObjectAssociation* association = [JSWrapperObjectAssociation associationFor:obj];
  if (association != nil && association.env == env && association.bridgeState == this &&
      IsBridgeStateLive(association.bridgeState, association.bridgeStateToken)) {
    napi_value jsObject = get_ref_value(env, association.ref);
    if (jsObject != nullptr) {
      bool isArrayBuffer = false;
      bool isTypedArray = false;
      bool isDataView = false;
      if ((napi_is_arraybuffer(env, jsObject, &isArrayBuffer) == napi_ok && isArrayBuffer) ||
          (napi_is_typedarray(env, jsObject, &isTypedArray) == napi_ok && isTypedArray) ||
          (napi_is_dataview(env, jsObject, &isDataView) == napi_ok && isDataView)) {
        return jsObject;
      }

      [obj retain];
      return proxyNativeObject(env, jsObject, obj);
    }
  }

  return nullptr;
}

napi_value findConstructorForObject(napi_env env, ObjCBridgeState* bridgeState, id object,
                                    Class cls) {
  if (cls == nil) {
    cls = object_getClass(object);
  }

  // Look up if there is a custom class for it already
  {
    auto find = bridgeState->classesByPointer.find(cls);
    if (find != bridgeState->classesByPointer.end()) {
      return get_ref_value(env, find->second->constructor);
    }
  }

  // Look up if there is a custom constructor for it already
  {
    auto find = bridgeState->constructorsByPointer.find(cls);
    if (find != bridgeState->constructorsByPointer.end()) {
      return get_ref_value(env, find->second);
    }
  }

  // Look up if there is a metadata-defined class
  {
    auto find = bridgeState->mdClassesByPointer.find(cls);
    if (find != bridgeState->mdClassesByPointer.end()) {
      auto cls = bridgeState->getClass(env, find->second);
      return get_ref_value(env, cls->constructor);
    }
  }

  Class superclass = class_getSuperclass(cls);
  if (superclass != nullptr) {
    napi_value superclassConstructor =
        findConstructorForObject(env, bridgeState, object, superclass);
    if (superclassConstructor != nullptr) {
      return superclassConstructor;
    }
  }

  // Look up the protocols implemented by this class if no class-based
  // constructor could be resolved. For private runtime subclasses we prefer
  // inheriting the public superclass surface over exposing a protocol-only
  // shell that drops concrete class members.
  {
    unsigned int count;
    auto protocols = class_copyProtocolList(cls, &count);
    std::unordered_set<ObjCProtocol*> impls;

    std::function<void(Protocol**, unsigned int)> processProtocolList = [&](Protocol** list,
                                                                            unsigned int count) {
      for (unsigned int i = 0; i < count; i++) {
        auto protocol = list[i];
        auto find = bridgeState->mdProtocolsByPointer.find(protocol);
        if (find != bridgeState->mdProtocolsByPointer.end()) {
          impls.insert(bridgeState->getProtocol(env, find->second));
        }
        list = protocol_copyProtocolList(protocol, &count);
        processProtocolList(list, count);
      }
    };

    processProtocolList(protocols, count);

    if (!impls.empty()) {
      napi_value constructor;
      napi_define_class(env, class_getName(cls), NAPI_AUTO_LENGTH, ObjCProtocol::jsConstructor,
                        nullptr, 0, nullptr, &constructor);
      for (auto impl : impls) {
        ObjCClassMember::defineMembers(env, impl->members, impl->membersOffset, constructor);
      }

      bridgeState->constructorsByPointer[cls] = make_ref(env, constructor);

      return constructor;
    }
  }

  return nullptr;
}

// Get a napi_value for an Objective-C object, creating it if it doesn't exist.
// Here we also ensure that the native object always points to the same
// JS object, this makes sure that we only ever finalize it once.
// Might want to consider using associated objects instead of a hashtable.
napi_value ObjCBridgeState::getObject(napi_env env, id obj, ObjectOwnership ownership,
                                      MDSectionOffset classOffset,
                                      std::vector<MDSectionOffset>* protocolOffsets) {
  NS_OBJC_NAPI_PREAMBLE

  if (obj == nullptr) {
    return nullptr;
  }

  Class objectClass = object_getClass(obj);
  if (objectClass != nil && class_isMetaClass(objectClass)) {
    return findConstructorForClassObject(env, this, (Class)obj, nullptr);
  }

  auto roundTrip = getRoundTripObject(env, obj);
  if (roundTrip != nullptr) {
    return roundTrip;
  }

  if (napi_value existing = getNormalizedObjectRef(env, obj); existing != nullptr) {
    return existing;
  }

  auto findClass = classesByPointer.find(obj);
  if (findClass != classesByPointer.end()) {
    return get_ref_value(env, findClass->second->constructor);
  }

  auto mdFindClassByPointer = mdClassesByPointer.find((Class)obj);
  if (mdFindClassByPointer != mdClassesByPointer.end()) {
    auto bridgedClass = getClass(env, mdFindClassByPointer->second);
    return bridgedClass != nullptr ? get_ref_value(env, bridgedClass->constructor) : nullptr;
  }

  auto cls = objectClass;

  auto mdFindByPointer = mdClassesByPointer.find(cls);
  if (mdFindByPointer != mdClassesByPointer.end()) {
    classOffset = mdFindByPointer->second;
  }

  auto findByPointer = classesByPointer.find(cls);
  if (findByPointer != classesByPointer.end()) {
    return getObject(env, obj, get_ref_value(env, findByPointer->second->constructor), ownership);
  }

  napi_value constructor = nullptr;
  if (classOffset != 0) {
    auto bridgedCls = getClass(env, classOffset);

    if (bridgedCls == nullptr) {
      return nullptr;
    }

    constructor = get_ref_value(env, bridgedCls->constructor);
  } else if (protocolOffsets != nullptr && !protocolOffsets->empty()) {
    auto proto = getProtocol(env, protocolOffsets->front());

    if (proto == nullptr) {
      return nullptr;
    }

    constructor = get_ref_value(env, proto->constructor);
  } else {
    constructor = findConstructorForObject(env, this, obj, cls);
  }

  if (constructor == nullptr) {
    return nullptr;
  }

  return getObject(env, obj, constructor, ownership);
}

void ObjCBridgeState::unregisterObject(id object) noexcept {
  // #if DEBUG
  // NSString *string = [NSString stringWithFormat: @"Unregistering object <%s: %p> @ %ld # success:
  // %d, finalized: %d",
  //     class_getName(object_getClass(object)), object, [object retainCount],
  //     (int)objectRefs.contains(object), (int)finalized];

  // dbglog([string UTF8String]);
  // #endif

  if (takeObjectRef(object) != nullptr) {
    removeRecentObjectWrapper(env, object);
    removeCachedHandleObject(env, (void*)object);
    [object release];
  }
}

bool ObjCBridgeState::unregisterObjectIfRefMatches(id object, napi_ref ref) noexcept {
  if (takeObjectRef(object, ref) == nullptr) {
    return false;
  }

  removeRecentObjectWrapper(env, object);
  removeCachedHandleObject(env, (void*)object);
  [object release];
  return true;
}

void ObjCBridgeState::detachObject(id object) noexcept {
  takeObjectRef(object);
  removeRoundTripObject(object);
  removeRecentObjectWrapper(env, object);
  removeCachedHandleObject(env, (void*)object);

  if (object == nil) {
    return;
  }

  NSMutableSet* trackedObjectTable = static_cast<NSMutableSet*>(trackedObjectLiveness);
  if (trackedObjectTable == nil) {
    return;
  }

  NSNumber* objectKey = [NSNumber numberWithUnsignedLongLong:NormalizeHandleKey((void*)object)];
  std::lock_guard<std::mutex> lock(objectRefsMutex);
  [trackedObjectTable removeObject:objectKey];
}

}  // namespace nativescript
