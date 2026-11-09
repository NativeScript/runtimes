#include "Class.h"
#include "ClassBuilder.h"
#include "ClassMember.h"
#include "Interop.h"
#include "Metadata.h"
#include "MetadataReader.h"
#include "ObjCBridge.h"
#include "Protocol.h"
#include "Util.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "node_api_util.h"

#import <Foundation/Foundation.h>
#include <cstring>
#include <string>

namespace nativescript {

napi_value JS_NSObject_alloc(napi_env env, napi_callback_info cbinfo);

inline bool tryGetInteropPointerArg(napi_env env, napi_value value, void** out) {
  if (out == nullptr || value == nullptr) {
    return false;
  }

  *out = nullptr;

  napi_valuetype argType = napi_undefined;
  napi_typeof(env, value, &argType);

  if (argType == napi_external) {
    return napi_get_value_external(env, value, out) == napi_ok && *out != nullptr;
  }

  if (argType == napi_bigint) {
    uint64_t raw = 0;
    bool lossless = false;
    if (napi_get_value_bigint_uint64(env, value, &raw, &lossless) != napi_ok) {
      return false;
    }
    *out = reinterpret_cast<void*>(raw);
    return *out != nullptr;
  }

  if (argType != napi_object) {
    return false;
  }

  if (Pointer::isInstance(env, value)) {
    Pointer* ptr = Pointer::unwrap(env, value);
    *out = ptr != nullptr ? ptr->data : nullptr;
    return *out != nullptr;
  }

  if (Reference::isInstance(env, value)) {
    Reference* ref = Reference::unwrap(env, value);
    *out = ref != nullptr ? ref->data : nullptr;
    return *out != nullptr;
  }

  return false;
}

void ObjCBridgeState::registerClassGlobals(napi_env env, napi_value global) {
  MDSectionOffset offset = metadata->classesOffset;
  while (offset < metadata->structsOffset) {
    MDSectionOffset originalOffset = offset;
    auto nameOffset = metadata->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    auto runtimeNameOffset = metadata->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    bool hasProtocols = (nameOffset & mdSectionOffsetNext) != 0;
    nameOffset &= ~mdSectionOffsetNext;
    auto name = metadata->resolveString(nameOffset);
    auto runtimeName = name;
    if (runtimeNameOffset != MD_SECTION_OFFSET_NULL) {
      runtimeName = metadata->resolveString(runtimeNameOffset);
    }
    while (hasProtocols) {
      auto protocolOffset = metadata->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      hasProtocols = (protocolOffset & mdSectionOffsetNext) != 0;
    }

    auto superclass = metadata->getOffset(offset);
    offset += sizeof(superclass);

    bool next = (superclass & mdSectionOffsetNext) != 0;

    while (next) {
      auto flags = metadata->getMemberFlag(offset);
      next = (flags & mdMemberNext) != 0;
      offset += sizeof(flags);

      if ((flags & mdMemberProperty) != 0) {
        bool readonly = (flags & mdMemberReadonly) != 0;
        offset += sizeof(MDSectionOffset);  // name
        offset += sizeof(MDSectionOffset);  // getterSelector
        offset += sizeof(MDSectionOffset);  // getterSignature
        if (!readonly) {
          offset += sizeof(MDSectionOffset);  // setterSelector
          offset += sizeof(MDSectionOffset);  // setterSignature
        }
      } else {
        offset += sizeof(MDSectionOffset);  // selector
        offset += sizeof(MDSectionOffset);  // signature
      }
    }

    auto nativeClass = objc_getClass(runtimeName);
    if (nativeClass != nil) {
      mdClassesByPointer[nativeClass] = originalOffset;
    }

    napi_property_descriptor prop = {
        .utf8name = name,
        .method = nullptr,
        .getter = JS_classGetter,
        .setter = nullptr,
        .value = nullptr,
        .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
        .data = (void*)((size_t)originalOffset),
    };

    napi_define_properties(env, global, 1, &prop);
  }
}

NAPI_FUNCTION(registerClass) {
  NAPI_CALLBACK_BEGIN(1)

  // In case no arguments are passed, we just return the NativeObject class.
  // This is to support both @NativeClass and @NativeClass() syntaxes.
  // Maybe we should support more in future like
  // @NativeClass(NSApplicationDelegate).
  if (argc == 0) {
    napi_value func;
    napi_create_function(env, "NativeClass", NAPI_AUTO_LENGTH, JS_registerClass, nullptr, &func);
    return func;
  }

  auto bridgeState = ObjCBridgeState::InstanceData(env);
  // bridgeState->registerClass(env, argv[0]);

  ClassBuilder* builder = new ClassBuilder(env, argv[0]);
  // It gets lazily built when a static method is called.
  // builder->build();
  bridgeState->registerRuntimeClass(builder, builder->nativeClass);

  return nullptr;
}

const char* ObjCClassDecorator = R"(
globalThis.ObjCClass = function ObjCClass(...protocols) {
  return function (target) {
    if (target.ObjCProtocols) {
      target.ObjCProtocols.push(...protocols);
    } else {
      target.ObjCProtocols = protocols;
    }

    return globalThis.NativeClass(target);
  };
};
)";

void setupObjCClassDecorator(napi_env env) {
  napi_value global, script;
  napi_get_global(env, &global);
  napi_create_string_utf8(env, ObjCClassDecorator, NAPI_AUTO_LENGTH, &script);
  napi_run_script(env, script, &global);
}

char class_name[256];

// Get a Bridged Class by metadata offset, creating it if it doesn't exist.
// This is used to cache ObjCClass instances.
ObjCClass* ObjCBridgeState::getClass(napi_env env, MDSectionOffset offset) {
  auto find = this->classes[offset];
  if (find != nullptr) {
    return find;
  }

  auto cls = new ObjCClass(env, offset);
  this->classes[offset] = cls;

  return cls;
}

NAPI_FUNCTION(import) {
  NAPI_CALLBACK_BEGIN(1)

  NAPI_GUARD(napi_get_value_string_utf8(env, argv[0], class_name, 256, nullptr)) {
    NAPI_THROW_LAST_ERROR
    return nullptr;
  }

  std::string name = class_name;

  if (!name.starts_with("/")) {
    name = "/System/Library/Frameworks/" + name + ".framework";
  }

  NSBundle* bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:name.c_str()]];
  if (bundle == nil) {
    NSLog(@"Could not find bundle: %s", name.c_str());
    return nullptr;
  }

  bool loaded = [bundle load];

  if (!loaded) {
    NSLog(@"Could not load bundle: %s", name.c_str());
    return nullptr;
  }

  return nullptr;
}

NAPI_FUNCTION(classGetter) {
  void* data;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, nullptr, &data);
  MDSectionOffset offset = (MDSectionOffset)((size_t)data);
  auto bridgeState = ObjCBridgeState::InstanceData(env);

  auto cached = bridgeState->mdValueCache[offset];
  if (cached != nullptr) {
    return get_ref_value(env, cached);
  }

  // std::string name = bridgeState->metadata->getString(offset);
  auto cls = bridgeState->getClass(env, offset);

  if (cls != nullptr) {
    bridgeState->mdValueCache[offset] = cls->constructor;
  } else {
    return nullptr;
  }

  return get_ref_value(env, cls->constructor);
}

NAPI_FUNCTION(classSuperclassFallback) {
  napi_value jsThis;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &jsThis, nullptr);

  Class currentClass = nil;
  auto bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState != nullptr && jsThis != nullptr) {
    bridgeState->tryResolveBridgedClassConstructor(env, jsThis, &currentClass);
  }
  if (currentClass == nil) {
    napi_unwrap(env, jsThis, (void**)&currentClass);
  }
  if (currentClass == nil) {
    return nullptr;
  }

  Class superClass = class_getSuperclass(currentClass);
  if (superClass == nil) {
    return nullptr;
  }

  auto find = bridgeState->classesByPointer.find(superClass);
  if (find != bridgeState->classesByPointer.end()) {
    return get_ref_value(env, find->second->constructor);
  }

  auto mdClass = bridgeState->mdClassesByPointer.find(superClass);
  if (mdClass != bridgeState->mdClassesByPointer.end()) {
    auto bridgedClass = bridgeState->getClass(env, mdClass->second);
    return get_ref_value(env, bridgedClass->constructor);
  }

  const char* runtimeName = class_getName(superClass);
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

  return bridgeState->getObject(env, (id)superClass, kUnownedObject, 0, nullptr);
}

NAPI_FUNCTION(classHasInstance) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value jsThis = nullptr;
  napi_get_cb_info(env, cbinfo, &argc, argv, &jsThis, nullptr);

  Class expectedClass = nil;
  auto bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState != nullptr && jsThis != nullptr) {
    bridgeState->tryResolveBridgedClassConstructor(env, jsThis, &expectedClass);
  }
  if (expectedClass == nil) {
    napi_unwrap(env, jsThis, (void**)&expectedClass);
  }

  bool isInstance = false;
  if (expectedClass != nil && argc > 0 && argv[0] != nullptr) {
    napi_valuetype valueType = napi_undefined;
    if (napi_typeof(env, argv[0], &valueType) == napi_ok &&
        (valueType == napi_object || valueType == napi_function)) {
      id instance = nil;
      napi_status unwrapStatus = napi_invalid_arg;
      if (Pointer::isInstance(env, argv[0])) {
        Pointer* pointer = Pointer::unwrap(env, argv[0]);
        instance = pointer != nullptr ? static_cast<id>(pointer->data) : nil;
        unwrapStatus = instance != nil ? napi_ok : napi_invalid_arg;
      } else if (Reference::isInstance(env, argv[0])) {
        Reference* reference = Reference::unwrap(env, argv[0]);
        instance = reference != nullptr ? static_cast<id>(reference->data) : nil;
        unwrapStatus = instance != nil ? napi_ok : napi_invalid_arg;
      } else {
        unwrapStatus = napi_unwrap(env, argv[0], (void**)&instance);
      }

      if (unwrapStatus != napi_ok || instance == nil) {
        napi_value nativePointer = nullptr;
        if (napi_get_named_property(env, argv[0], "__ns_native_ptr", &nativePointer) == napi_ok &&
            Pointer::isInstance(env, nativePointer)) {
          Pointer* pointer = Pointer::unwrap(env, nativePointer);
          instance = pointer != nullptr ? static_cast<id>(pointer->data) : nil;
        }
      }

      if (instance != nil) {
        Class currentClass = object_getClass(instance);
        while (currentClass != nil) {
          if (currentClass == expectedClass) {
            isInstance = true;
            break;
          }
          currentClass = class_getSuperclass(currentClass);
        }
      }
    }
  }

  napi_value result = nullptr;
  napi_get_boolean(env, isInstance, &result);
  return result;
}

NAPI_FUNCTION(BridgedConstructor) {
  NAPI_CALLBACK_BEGIN(16)

  napi_value newTarget = nullptr;
  napi_get_new_target(env, cbinfo, &newTarget);

  napi_valuetype thisType = napi_undefined;
  if (jsThis == nullptr || napi_typeof(env, jsThis, &thisType) != napi_ok ||
      (thisType != napi_object && thisType != napi_function)) {
    napi_create_object(env, &jsThis);

    napi_value prototypeOwner = newTarget;
    if (prototypeOwner == nullptr || napi_typeof(env, prototypeOwner, &thisType) != napi_ok ||
        (thisType != napi_function && thisType != napi_object)) {
      prototypeOwner = nullptr;
    }

    if (prototypeOwner != nullptr) {
      napi_value prototype = nullptr;
      if (napi_get_named_property(env, prototypeOwner, "prototype", &prototype) == napi_ok &&
          prototype != nullptr) {
        napi_value global = nullptr;
        napi_value objectCtor = nullptr;
        napi_value setPrototypeOf = nullptr;
        napi_get_global(env, &global);
        napi_get_named_property(env, global, "Object", &objectCtor);
        napi_get_named_property(env, objectCtor, "setPrototypeOf", &setPrototypeOf);
        napi_value setPrototypeArgs[2] = {jsThis, prototype};
        napi_call_function(env, objectCtor, setPrototypeOf, 2, setPrototypeArgs, nullptr);
      }
    }
  }

  napi_valuetype jsType = napi_undefined;
  if (argc > 0) {
    napi_typeof(env, argv[0], &jsType);
  }

  id object = nil;

  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  auto ensureWrappedThis = [&](id nativeObject) {
    if (jsThis == nullptr || nativeObject == nil) {
      return;
    }

    if (newTarget != nullptr) {
      // Fresh constructor receivers should not need an unwrap probe here.
      // On V8, reentrant constructor calls from ObjC->JS block callbacks can
      // stall inside napi_unwrap() on that receiver.
      napi_wrap(env, jsThis, nativeObject, nullptr, nullptr, nullptr);
      return;
    }

    void* existingWrapped = nullptr;
    napi_status unwrapStatus = napi_unwrap(env, jsThis, &existingWrapped);
    if (unwrapStatus == napi_ok && existingWrapped != nullptr) {
      return;
    }

    napi_wrap(env, jsThis, nativeObject, nullptr, nullptr, nullptr);
  };

  Class cls = (Class)data;

  napi_value constructor = newTarget;
  if (constructor == nullptr && jsThis != nullptr) {
    napi_get_named_property(env, jsThis, "constructor", &constructor);
  }

  if (constructor != nullptr) {
    Class newTargetCls = nil;
    if (!(bridgeState != nullptr &&
          bridgeState->tryResolveBridgedClassConstructor(env, constructor, &newTargetCls))) {
      napi_unwrap(env, constructor, (void**)&newTargetCls);
    }

    if (newTargetCls != nil) {
      cls = newTargetCls;
    }
  }

  if (jsType == napi_external) {
    return jsThis;
  } else {
    // Backward compatibility: allow `new Class(pointer)` to wrap an existing
    // native instance instead of running initializer resolution.
    if (argc == 1 && argv[0] != nullptr) {
      void* rawPointer = nullptr;
      if (tryGetInteropPointerArg(env, argv[0], &rawPointer) && rawPointer != nullptr) {
        if (napi_value cached = bridgeState->getCachedHandleObject(env, rawPointer);
            cached != nullptr) {
          return cached;
        }

        object = (id)rawPointer;
        if (napi_value existing = bridgeState->findCachedObjectWrapper(env, object);
            existing != nullptr) {
          return existing;
        }

        ensureWrappedThis(object);
        jsThis = bridgeState->proxyNativeObject(env, jsThis, object);
        napi_wrap(env, jsThis, object, nullptr, nullptr, nullptr);
        return jsThis;
      }
    }

    bool supercall = class_conformsToProtocol(cls, @protocol(ObjCBridgeClassBuilderProtocol));

    if (supercall) {
      ClassBuilder* builder = (ClassBuilder*)bridgeState->classesByPointer[cls];
      if (!builder->isFinal) builder->build();
    }

    // Allocate first, then run initializer resolution through the bridged
    // JS "init" method so constructor arguments participate in selector
    // matching (including Swift-style token objects).
    object = [cls alloc];
    ensureWrappedThis(object);
    jsThis = bridgeState->proxyNativeObject(env, jsThis, object);
  }

  napi_wrap(env, jsThis, object, nullptr, nullptr, nullptr);

  napi_value initMethod;
  napi_status initStatus = napi_get_named_property(env, jsThis, "init", &initMethod);
  if (initStatus != napi_ok || initMethod == nullptr) {
    return jsThis;
  }

  napi_value initResult;
  initStatus = napi_call_function(env, jsThis, initMethod, argc, argv, &initResult);
  if (initStatus != napi_ok) {
    return nullptr;
  }

  return initResult != nullptr ? initResult : jsThis;
}

// Used to display the description of a native object in console.log.
// It's just implementation of nodejs.util.inspect.custom.
NAPI_FUNCTION(CustomInspect) {
  napi_value jsThis;
  void* data;
  size_t argc = 0;

  napi_get_cb_info(env, cbinfo, &argc, nil, &jsThis, &data);

  id self = nil;
  napi_unwrap(env, jsThis, (void**)&self);

  if (self == nil) {
    napi_value result;
    napi_create_string_utf8(env, "(nil)", NAPI_AUTO_LENGTH, &result);
    return result;
  }

  auto description = [self description];
  auto descriptionString = [description UTF8String];

  napi_value result;
  napi_create_string_utf8(env, descriptionString, NAPI_AUTO_LENGTH, &result);

  return result;
}

// Used for Symbol.dispose (using statement support).
NAPI_FUNCTION(releaseObject) {
  napi_value jsThis;
  void* data;
  napi_get_cb_info(env, cbinfo, nil, nil, &jsThis, &data);
  id self;
  napi_unwrap(env, jsThis, (void**)&self);
  auto bridgeState = ObjCBridgeState::InstanceData(env);
  bridgeState->unregisterObject(self);
  return nullptr;
}

// Implemented in JS to minimize calls into native. Fast enumeration
// makes use of buffers, so we only fill up the buffer once via
// native call and only do it again if needed via _fillStack.
static const char* FastEnumerationIteratorFactorySource = R"(
  (function () {
    return {
      stack: new Array(16),
      stacklen: -1,
      stackptr: -1,
      done: false,

      next() {
        if (this.stackptr < 0 && !this.done) {
          this.stacklen = this._fillStack(this.stack);
          if (this.stacklen == 0) {
            this.done = true;
            this.stackptr = -1;
          } else {
            this.stackptr = 0;
          }
        }

        if (this.done) {
          return { done: true };
        }

        const result = { value: this.stack[this.stackptr++], done: false };
        if (this.stackptr >= this.stacklen) {
          this.stackptr = -1;
        }

        return result;
      },
    };
  })
)";

void initFastEnumeratorIteratorFactory(napi_env env, ObjCBridgeState* bridgeState) {
  napi_value result, script;
  napi_create_string_utf8(env, FastEnumerationIteratorFactorySource, NAPI_AUTO_LENGTH, &script);
  napi_run_script(env, script, &result);
  bridgeState->createFastEnumeratorIterator = make_ref(env, result);
}

class FastEnumerationIterator {
 public:
  FastEnumerationIterator(id<NSFastEnumeration> collection) : collection(collection) {}

  static void finalize(napi_env env, void* data, void* hint) {
    FastEnumerationIterator* iterator = (FastEnumerationIterator*)data;
    delete iterator;
  }

  static napi_value fillStack(napi_env env, napi_callback_info cbinfo) {
    ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);

    napi_value jsThis;
    void* data;
    size_t argc = 1;
    napi_value stackArray;

    napi_get_cb_info(env, cbinfo, &argc, &stackArray, &jsThis, &data);

    FastEnumerationIterator* self = nil;
    napi_unwrap(env, jsThis, (void**)&self);

    NSUInteger count = [self->collection countByEnumeratingWithState:&self->state
                                                             objects:self->stackbuf
                                                               count:16];

    for (uint32_t index = 0; index < static_cast<uint32_t>(count); index++) {
      id obj = self->state.itemsPtr[index];
      napi_value jsObj = bridgeState->getObject(env, obj);
      napi_set_element(env, stackArray, index, jsObj);
    }

    napi_value result;
    napi_create_uint32(env, static_cast<uint32_t>(count), &result);

    return result;
  }

  napi_value toJS(napi_env env) {
    ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);

    napi_value createIterator = get_ref_value(env, bridgeState->createFastEnumeratorIterator);

    napi_value result;
    napi_call_function(env, createIterator, createIterator, 0, nullptr, &result);

    napi_property_descriptor fillStack = {
        .utf8name = "_fillStack",
        .name = nil,
        .method = FastEnumerationIterator::fillStack,
        .getter = nil,
        .setter = nil,
        .value = nil,
        .attributes = napi_enumerable,
        .data = nil,
    };

    napi_define_properties(env, result, 1, &fillStack);

    napi_ref ref;
    napi_wrap(env, result, this, FastEnumerationIterator::finalize, nullptr, &ref);

    return result;
  }

  id<NSFastEnumeration> collection;
  NSFastEnumerationState state = {0};
  id stackbuf[16];
  BOOL firstLoop = YES;
  long mutationsPtrValue;
};

NAPI_FUNCTION(fastEnumeration) {
  napi_value jsThis;
  void* data;
  size_t argc = 0;

  napi_get_cb_info(env, cbinfo, &argc, nil, &jsThis, &data);

  id self = nil;
  napi_unwrap(env, jsThis, (void**)&self);

  if (self == nil) {
    napi_value result;
    napi_create_string_utf8(env, "(nil)", NAPI_AUTO_LENGTH, &result);
    return result;
  }

  if (![self conformsToProtocol:@protocol(NSFastEnumeration)]) {
    napi_throw_error(env, nil, "Object does not conform to NSFastEnumeration");
    return nullptr;
  }

  auto iterator = new FastEnumerationIterator((id<NSFastEnumeration>)self);
  return iterator->toJS(env);
}

std::string NativeObjectName = "NativeObject";

// Bridge an Objective-C class to JavaScript on the fly. Runtime introspection
// is used to determine the class's properties and methods.
// In an overview, we define two versions of same class. One is the "normal"
// one, and the other is the "supercall" one. The supercall one is used to call
// superclasses' methods. The normal one is used to call the class's own
// methods. The supercall one is used automatically when the normal one is
// extended by a JS class.
// Every Bridged Class extends the NativeObject class.

void defineProtocolMembers(napi_env env, ObjCClassMemberMap& members, napi_value constructor,
                           ObjCProtocol* protocol, ObjCClass* cls) {
  ObjCClassMember::defineMembers(env, members, protocol->membersOffset, constructor, cls);
  for (auto protocol : protocol->protocols) {
    defineProtocolMembers(env, members, constructor, protocol, cls);
  }
}

ObjCClass::ObjCClass(napi_env env, MDSectionOffset offset) {
  NS_OBJC_NAPI_PREAMBLE

  this->env = env;

  metadataOffset = offset;

  bridgeState = ObjCBridgeState::InstanceData(env);
  bridgeStateToken = bridgeState != nullptr ? bridgeState->lifetimeToken : 0;

  bool isNativeObject = offset == MD_SECTION_OFFSET_NULL;

  std::vector<MDSectionOffset> protocolOffsets;

  MDSectionOffset superClassOffset = MD_SECTION_OFFSET_NULL;
  bool hasMembers = false;
  std::string jsConstructorName;
  if (isNativeObject) {
    name = NativeObjectName;
    jsConstructorName = name;
    nativeClass = nil;
  } else {
    auto nameOffset = bridgeState->metadata->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    auto runtimeNameOffset = bridgeState->metadata->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    bool hasProtocols = (nameOffset & mdSectionOffsetNext) != 0;
    nameOffset &= ~mdSectionOffsetNext;
    name = bridgeState->metadata->resolveString(nameOffset);
    const char* runtimeName = name.c_str();
    if (runtimeNameOffset != MD_SECTION_OFFSET_NULL) {
      runtimeName = bridgeState->metadata->resolveString(runtimeNameOffset);
    }
    jsConstructorName = runtimeName;
    if (jsConstructorName.empty()) {
      jsConstructorName = name;
    }
    nativeClass = objc_getClass(runtimeName);
    while (hasProtocols) {
      auto protocolOffset = bridgeState->metadata->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      hasProtocols = (protocolOffset & mdSectionOffsetNext) != 0;
      protocolOffset &= ~mdSectionOffsetNext;
      if (protocolOffset != MD_SECTION_OFFSET_NULL) protocolOffsets.push_back(protocolOffset);
    }
    superClassOffset = bridgeState->metadata->getOffset(offset);
    hasMembers = (superClassOffset & mdSectionOffsetNext) != 0;
    superClassOffset &= ~mdSectionOffsetNext;
    offset += sizeof(MDSectionOffset);
  }

  napi_value constructor, prototype;

  napi_define_class(env, jsConstructorName.c_str(), jsConstructorName.length(),
                    JS_BridgedConstructor, (void*)nativeClass, 0, nil, &constructor);

  if (nativeClass != nil) {
    napi_wrap(env, constructor, (void*)nativeClass, nil, nil, nil);
    bridgeState->registerRuntimeClass(this, nativeClass);
  }

  napi_get_named_property(env, constructor, "prototype", &prototype);

  napi_value superConstructor = nil, superPrototype = nil;

  // If the class requested isn't NativeObject - the class which every bridged
  // class extends - we need to find the super class and inherit from it, if it
  // exists.
  if (!isNativeObject) {
    if (superClassOffset != MD_SECTION_OFFSET_NULL) {
      superClassOffset += bridgeState->metadata->classesOffset;
    }

    // If class offset is 0, it means that the class doesn't have a super class.
    // But we just inherit NativeObject class in that case.
    superclass = bridgeState->getClass(env, superClassOffset);
    if (superclass != nil) {
      superConstructor = get_ref_value(env, superclass->constructor);
      superPrototype = get_ref_value(env, superclass->prototype);
      napi_inherits(env, constructor, superConstructor);
    }

    for (auto protocolOffset : protocolOffsets) {
      auto protocol =
          bridgeState->getProtocol(env, protocolOffset + bridgeState->metadata->protocolsOffset);
      if (protocol == nil) continue;
      defineProtocolMembers(env, members, constructor, protocol, this);
    }
  } else {
    superclass = nullptr;
  }

  napi_value constructorNameValue = nullptr;
  napi_create_string_utf8(env, jsConstructorName.c_str(), jsConstructorName.length(),
                          &constructorNameValue);
  napi_property_descriptor constructorNameProp = {
      .utf8name = "name",
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = constructorNameValue,
      .attributes = napi_default,
      .data = nullptr,
  };
  napi_define_properties(env, constructor, 1, &constructorNameProp);

  this->constructor = make_ref(env, constructor);
  this->prototype = make_ref(env, prototype);

  if (isNativeObject) {
    napi_value global, Symbol, SymbolDispose, SymbolIterator;
    napi_get_global(env, &global);
    napi_get_named_property(env, global, "Symbol", &Symbol);
    napi_get_named_property(env, Symbol, "iterator", &SymbolIterator);
    napi_get_named_property(env, Symbol, "dispose", &SymbolDispose);
    napi_valuetype type;
    napi_typeof(env, SymbolDispose, &type);

    napi_value sizeofValue;
    napi_create_int32(env, sizeof(id), &sizeofValue);

    napi_property_descriptor properties[] = {
        {
            .utf8name = nil,
            .name = jsSymbolFor(env, "nodejs.util.inspect.custom"),
            .method = JS_CustomInspect,
            .getter = nil,
            .setter = nil,
            .value = nil,
            .attributes = napi_enumerable,
            .data = nil,
        },
        {
            .utf8name = "toString",
            .name = nil,
            .method = JS_CustomInspect,
            .getter = nil,
            .setter = nil,
            .value = nil,
            .attributes = napi_enumerable,
            .data = nil,
        },
        {
            .utf8name = nil,
            .name = jsSymbolFor(env, "sizeof"),
            .method = nil,
            .getter = nil,
            .setter = nil,
            .value = sizeofValue,
            .attributes = napi_enumerable,
            .data = nil,
        },
        {
            .utf8name = nil,
            .name = SymbolIterator,
            .method = JS_fastEnumeration,
            .getter = nil,
            .setter = nil,
            .value = nil,
            .attributes = napi_enumerable,
            .data = nil,
        }};

    napi_define_properties(env, prototype, 4, properties);

    napi_define_properties(env, constructor, 1, &properties[2]);

    bool hasSuperclass = false;
    napi_value superclassKey = nullptr;
    napi_create_string_utf8(env, "superclass", NAPI_AUTO_LENGTH, &superclassKey);
    napi_has_own_property(env, constructor, superclassKey, &hasSuperclass);
    if (!hasSuperclass) {
      napi_property_descriptor superclassProperty = {
          .utf8name = "superclass",
          .name = nil,
          .method = JS_classSuperclassFallback,
          .getter = nil,
          .setter = nil,
          .value = nil,
          .attributes =
              (napi_property_attributes)(napi_configurable | napi_writable | napi_enumerable),
          .data = nil,
      };
      napi_define_properties(env, constructor, 1, &superclassProperty);
    }

    if (type == napi_symbol) {
      properties[0].name = SymbolDispose;
      properties[0].method = JS_releaseObject;

      napi_define_properties(env, prototype, 1, properties);
    }

    return;
  }

  // Add the 'extend' static method to all native classes (not NativeObject)
  if (!isNativeObject) {
    napi_value extendMethod;
    napi_create_function(env, "extend", NAPI_AUTO_LENGTH, ClassBuilder::ExtendCallback, nullptr,
                         &extendMethod);
    napi_set_named_property(env, constructor, "extend", extendMethod);
  }

  if (!hasMembers) return;

  ObjCClassMember::defineMembers(env, members, offset, constructor, this);

  auto hasOwnNamedProperty = [&](napi_value object, const char* propertyName) {
    napi_value key = nullptr;
    napi_create_string_utf8(env, propertyName, NAPI_AUTO_LENGTH, &key);
    bool hasOwn = false;
    napi_has_own_property(env, object, key, &hasOwn);
    return hasOwn;
  };

  if (!hasOwnNamedProperty(constructor, "name")) {
    napi_value classNameValue = nullptr;
    napi_create_string_utf8(env, jsConstructorName.c_str(), NAPI_AUTO_LENGTH, &classNameValue);
    napi_property_descriptor nameProperty = {
        .utf8name = "name",
        .name = nil,
        .method = nil,
        .getter = nil,
        .setter = nil,
        .value = classNameValue,
        .attributes = (napi_property_attributes)(napi_configurable),
        .data = nil,
    };
    napi_define_properties(env, constructor, 1, &nameProperty);
  }

  if (!hasOwnNamedProperty(constructor, "length")) {
    napi_value zeroValue = nullptr;
    napi_create_int32(env, 0, &zeroValue);
    napi_property_descriptor lengthProperty = {
        .utf8name = "length",
        .name = nil,
        .method = nil,
        .getter = nil,
        .setter = nil,
        .value = zeroValue,
        .attributes = (napi_property_attributes)(napi_configurable),
        .data = nil,
    };
    napi_define_properties(env, constructor, 1, &lengthProperty);
  }

  if (!hasOwnNamedProperty(constructor, "alloc")) {
    napi_property_descriptor allocProperty = {
        .utf8name = "alloc",
        .name = nil,
        .method = JS_NSObject_alloc,
        .getter = nil,
        .setter = nil,
        .value = nil,
        .attributes =
            (napi_property_attributes)(napi_configurable | napi_writable | napi_enumerable),
        .data = nil,
    };
    napi_define_properties(env, constructor, 1, &allocProperty);
  }

  if (!hasOwnNamedProperty(constructor, "arguments") ||
      !hasOwnNamedProperty(constructor, "caller")) {
    napi_value undefinedValue = nullptr;
    napi_get_undefined(env, &undefinedValue);
    napi_property_descriptor slots[] = {
        {
            .utf8name = "arguments",
            .name = nil,
            .method = nil,
            .getter = nil,
            .setter = nil,
            .value = undefinedValue,
            .attributes = (napi_property_attributes)(napi_configurable | napi_writable),
            .data = nil,
        },
        {
            .utf8name = "caller",
            .name = nil,
            .method = nil,
            .getter = nil,
            .setter = nil,
            .value = undefinedValue,
            .attributes = (napi_property_attributes)(napi_configurable | napi_writable),
            .data = nil,
        },
    };
    napi_define_properties(env, constructor, 2, slots);
  }

  napi_value global = nullptr;
  napi_value symbolCtor = nullptr;
  napi_value hasInstanceSymbol = nullptr;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Symbol", &symbolCtor);
  napi_get_named_property(env, symbolCtor, "hasInstance", &hasInstanceSymbol);
  bool hasOwnHasInstance = false;
  napi_has_own_property(env, constructor, hasInstanceSymbol, &hasOwnHasInstance);
  if (!hasOwnHasInstance) {
    napi_property_descriptor hasInstanceProperty = {
        .utf8name = nil,
        .name = hasInstanceSymbol,
        .method = JS_classHasInstance,
        .getter = nil,
        .setter = nil,
        .value = nil,
        .attributes = (napi_property_attributes)(napi_configurable),
        .data = nil,
    };
    napi_define_properties(env, constructor, 1, &hasInstanceProperty);
  }

  if (!hasOwnNamedProperty(prototype, "toString")) {
    napi_property_descriptor toStringProperty = {
        .utf8name = "toString",
        .name = nil,
        .method = JS_CustomInspect,
        .getter = nil,
        .setter = nil,
        .value = nil,
        .attributes =
            (napi_property_attributes)(napi_configurable | napi_writable | napi_enumerable),
        .data = nil,
    };
    napi_define_properties(env, prototype, 1, &toStringProperty);
  }
}

ObjCClass::~ObjCClass() {
  if (env != nullptr &&
      (bridgeState == nullptr || IsBridgeStateLive(bridgeState, bridgeStateToken))) {
    DeleteReferenceOnOwningThread(env, bridgeState, bridgeStateToken, constructor);
    DeleteReferenceOnOwningThread(env, bridgeState, bridgeStateToken, prototype);
  }
  constructor = nullptr;
  prototype = nullptr;
  bridgeState = nullptr;
  bridgeStateToken = 0;
}

}  // namespace nativescript
