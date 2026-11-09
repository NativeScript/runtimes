#include "ClassMember.h"
#import <Foundation/Foundation.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
#include "Block.h"
#include "CallbackThreading.h"
#include "Class.h"
#include "ClassBuilder.h"
#include "Closure.h"
#include "Interop.h"
#include "MetadataReader.h"
#include "ObjCBridge.h"
#include "SignatureDispatch.h"
#include "TypeConv.h"
#include "Util.h"
#include "runtime/apple/NativeScriptException.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "node_api_util.h"

namespace nativescript {

namespace {
constexpr const char* kNativePointerProperty = "__ns_native_ptr";
}

napi_value JS_NSObject_alloc(napi_env env, napi_callback_info cbinfo) {
  napi_value jsThis;
  ObjCClassMember* method = nullptr;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &jsThis, (void**)&method);

  id self = nil;
  ObjCBridgeState* state = ObjCBridgeState::InstanceData(env);
  if (state != nullptr && jsThis != nullptr) {
    state->tryResolveBridgedTypeConstructor(env, jsThis, &self);
  }

  napi_status unwrapStatus = self != nil ? napi_ok : napi_unwrap(env, jsThis, (void**)&self);
  if ((unwrapStatus != napi_ok || self == nil) && method != nullptr && method->cls != nullptr &&
      method->cls->nativeClass != nil) {
    bool canFallbackToMethodClass = true;
    napi_valuetype jsType = napi_undefined;
    if (jsThis != nullptr && napi_typeof(env, jsThis, &jsType) == napi_ok &&
        jsType == napi_function) {
      napi_value definingConstructor = get_ref_value(env, method->cls->constructor);
      if (definingConstructor != nullptr) {
        bool isSameConstructor = false;
        if (napi_strict_equals(env, jsThis, definingConstructor, &isSameConstructor) == napi_ok &&
            !isSameConstructor) {
          canFallbackToMethodClass = false;
        }
      }
    }

    if (canFallbackToMethodClass) {
      self = (id)method->cls->nativeClass;
    }
  }
  if (self == nil) {
    napi_throw_error(env, "NativeScriptException",
                     "There was no native class counterpart to the JavaScript constructor.");
    return nullptr;
  }

  bool supercall = class_conformsToProtocol((Class)self, @protocol(ObjCBridgeClassBuilderProtocol));

  if (supercall) {
    ObjCBridgeState* state = ObjCBridgeState::InstanceData(env);
    ClassBuilder* builder = (ClassBuilder*)state->classesByPointer[self];
    if (!builder->isFinal) builder->build();
  }

  id result = [self alloc];
  return ObjCBridgeState::InstanceData(env)->getObject(env, result, jsThis, kOwnedObject);
}

void ObjCClassMember::defineMembers(napi_env env, ObjCClassMemberMap& memberMap,
                                    MDSectionOffset offset, napi_value constructor,
                                    ObjCClass* cls) {
  auto bridgeState = ObjCBridgeState::InstanceData(env);

  napi_value prototype;
  napi_get_named_property(env, constructor, "prototype", &prototype);

  bool next = true;

  auto hasOwnNamedProperty = [&](napi_value object, const char* propertyName) {
    napi_value propertyKey = nullptr;
    napi_create_string_utf8(env, propertyName, NAPI_AUTO_LENGTH, &propertyKey);
    bool hasOwn = false;
    napi_has_own_property(env, object, propertyKey, &hasOwn);
    return hasOwn;
  };

  auto tryGetSuperclassMember = [&](const std::string& name, bool isStatic) -> ObjCClassMember* {
    if (cls == nullptr || cls->superclass == nullptr) {
      return nullptr;
    }
    auto it = cls->superclass->members.find({name, isStatic});
    if (it == cls->superclass->members.end()) {
      return nullptr;
    }
    return &it->second;
  };

  while (next) {
    auto flags = bridgeState->metadata->getMemberFlag(offset);

    if (flags == mdMemberFlagNull) break;

    next = (flags & mdMemberNext) != 0;
    offset += sizeof(flags);

    const bool isStaticMember = (flags & mdMemberStatic) != 0;
    napi_value jsObject;
    if (isStaticMember) {
      jsObject = constructor;
    } else {
      jsObject = prototype;
    }

    if ((flags & mdMemberProperty) != 0) {
      bool readonly = (flags & mdMemberReadonly) != 0;
      const char* name = bridgeState->metadata->getString(offset);
      offset += sizeof(MDSectionOffset);  // name

      MDSectionOffset getterSignature, setterSignature;

      const char* getterSelector = bridgeState->metadata->getString(offset);

      offset += sizeof(MDSectionOffset);  // getterSelector

      getterSignature = bridgeState->metadata->getOffset(offset);
      offset += sizeof(MDSectionOffset);  // getterSignature

      const char* setterSelector = nullptr;
      if (!readonly) {
        setterSelector = bridgeState->metadata->getString(offset);
        offset += sizeof(MDSectionOffset);  // setterSelector

        setterSignature = bridgeState->metadata->getOffset(offset);
        offset += sizeof(MDSectionOffset);  // setterSignature
      }

      bool hasProperty = false;
      napi_has_named_property(env, jsObject, name, &hasProperty);
      bool hasOwnProperty = hasOwnNamedProperty(jsObject, name);
      bool inheritedProperty = hasProperty && !hasOwnProperty;

      ObjCClassMember* superMember = tryGetSuperclassMember(name, isStaticMember);

      if (inheritedProperty && superMember != nullptr && superMember->methodOrGetter.isProperty) {
        bool superReadonly = superMember->setter.selector == nullptr;
        SEL getterSel = sel_registerName(getterSelector);
        SEL setterSel = readonly ? nullptr : sel_registerName(setterSelector);
        bool sameGetter = superMember->methodOrGetter.selector == getterSel;
        bool sameSetter = superMember->setter.selector == setterSel;

        if ((!superReadonly && readonly) ||
            (superReadonly == readonly && sameGetter && (readonly || sameSetter))) {
          continue;
        }
      }

      auto updatedMember = ObjCClassMember(
          bridgeState, sel_registerName(getterSelector),
          !readonly ? sel_registerName(setterSelector) : nullptr,
          getterSignature + bridgeState->metadata->signaturesOffset,
          !readonly ? setterSignature + bridgeState->metadata->signaturesOffset : 0, flags);
      auto memberIt = memberMap.find({name, isStaticMember});
      if (memberIt != memberMap.end()) {
        memberIt->second = updatedMember;
      } else {
        const auto& inserted =
            memberMap.emplace(ObjCClassMemberKey{name, isStaticMember}, updatedMember);
        memberIt = inserted.first;
      }

      napi_property_descriptor property = {
          .utf8name = name,
          .name = nil,
          .method = nil,
          .getter = jsGetter,
          .setter = readonly ? jsReadOnlySetter : jsSetter,
          .value = nil,
          .attributes = (napi_property_attributes)(napi_configurable | napi_enumerable),
          .data = &memberIt->second,
      };

      napi_define_properties(env, jsObject, 1, &property);
    } else {
      auto selector = bridgeState->metadata->getString(offset);
      offset += sizeof(MDSectionOffset);  // selector
      auto signature = bridgeState->metadata->getOffset(offset);
      offset += sizeof(MDSectionOffset);  // signature

      auto name = jsifySelector(selector);
      bool hasProperty = false;
      napi_has_named_property(env, jsObject, name.c_str(), &hasProperty);
      bool hasOwnProperty = hasOwnNamedProperty(jsObject, name.c_str());
      bool inheritedProperty = hasProperty && !hasOwnProperty && name != "init";

      SEL methodSelector = sel_registerName(selector);
      MDSectionOffset signatureOffset = signature + bridgeState->metadata->signaturesOffset;
      ObjCClassMember* superMember = tryGetSuperclassMember(name, isStaticMember);
      bool selectorExistsInSuper = false;
      if (superMember != nullptr && !superMember->methodOrGetter.isProperty) {
        selectorExistsInSuper = superMember->methodOrGetter.selector == methodSelector;
        if (!selectorExistsInSuper) {
          for (const auto& overload : superMember->overloads) {
            if (overload.method.selector == methodSelector) {
              selectorExistsInSuper = true;
              break;
            }
          }
        }
      }

      const bool keepInheritedMethod =
          name == "alloc" || name == "toString" || name == "superclass";
      if (inheritedProperty && selectorExistsInSuper && !keepInheritedMethod) {
        continue;
      }

      const ObjCClassMemberKey memberKey{name, isStaticMember};
      auto memberIt = memberMap.find(memberKey);
      ObjCClassMember* member = nullptr;
      if (memberIt != memberMap.end()) {
        if (memberIt->second.methodOrGetter.isProperty) {
          continue;
        }
        memberIt->second.addOverload(methodSelector, signatureOffset,
                                     (flags & metagen::mdMemberReturnOwned) != 0 ? 1 : 0);
        member = &memberIt->second;
      } else if (inheritedProperty && superMember != nullptr &&
                 !superMember->methodOrGetter.isProperty) {
        const auto& inserted = memberMap.emplace(
            memberKey, ObjCClassMember(bridgeState, superMember->methodOrGetter.selector,
                                       superMember->methodOrGetter.signatureOffset, flags));
        member = &inserted.first->second;
        for (const auto& overload : superMember->overloads) {
          member->addOverload(overload.method.selector, overload.method.signatureOffset,
                              overload.method.dispatchFlags);
        }
        member->addOverload(methodSelector, signatureOffset,
                            (flags & metagen::mdMemberReturnOwned) != 0 ? 1 : 0);
      } else {
        const auto& inserted = memberMap.emplace(
            memberKey, ObjCClassMember(bridgeState, methodSelector, signatureOffset, flags));
        member = &inserted.first->second;
      }

      if (member == nullptr) {
        continue;
      }

      if (cls != nullptr) {
        member->cls = cls;
      }

      if (hasOwnProperty && name != "init") {
        if ((flags & mdMemberIsInit) != 0) {
          member->cls = cls;
        }
        continue;
      }

      if (inheritedProperty && !selectorExistsInSuper && superMember == nullptr && name != "init") {
        continue;
      }

      napi_property_descriptor property = {
          .utf8name = name.c_str(),
          .name = nil,
          .method = (flags & metagen::mdMemberIsInit) != 0 ? jsCallInit : jsCall,
          .getter = nil,
          .setter = nil,
          .value = nil,
          .attributes =
              (napi_property_attributes)(napi_configurable | napi_writable | napi_enumerable),
          .data = member,
      };

      if ((flags & mdMemberIsInit) != 0) {
        member->cls = cls;
      }

      if (name == "alloc") {
        property.method = JS_NSObject_alloc;
      }

      napi_define_properties(env, jsObject, 1, &property);
    }
  }
}

void ObjCClassMember::addOverload(SEL selector, MDSectionOffset offset, uint8_t dispatchFlags) {
  if (methodOrGetter.selector == selector) {
    return;
  }

  for (const auto& overload : overloads) {
    if (overload.method.selector == selector) {
      return;
    }
  }

  overloads.emplace_back(selector, offset, dispatchFlags);
}

inline bool tryObjCNapiDispatch(napi_env env, Cif* cif, id self, bool classMethod, SEL selector,
                                MethodDescriptor* descriptor, uint8_t dispatchFlags,
                                const napi_value* argv, void* rvalue, bool* didInvoke) {
  if (didInvoke != nullptr) {
    *didInvoke = false;
  }

  if (cif == nullptr || cif->signatureHash == 0) {
    return true;
  }

  Class receiverClass = classMethod ? (Class)self : object_getClass(self);
  const bool supercall =
      receiverClass != nil &&
      class_conformsToProtocol(receiverClass, @protocol(ObjCBridgeClassBuilderProtocol));
  if (supercall) {
    return true;
  }

  if (descriptor != nullptr) {
    if (!descriptor->dispatchLookupCached ||
        descriptor->dispatchLookupSignatureHash != cif->signatureHash ||
        descriptor->dispatchLookupFlags != dispatchFlags) {
      descriptor->dispatchLookupSignatureHash = cif->signatureHash;
      descriptor->dispatchLookupFlags = dispatchFlags;
      descriptor->dispatchId = composeSignatureDispatchId(
          cif->signatureHash, SignatureCallKind::ObjCMethod, dispatchFlags);
      descriptor->preparedInvoker =
          reinterpret_cast<void*>(lookupObjCPreparedInvoker(descriptor->dispatchId));
      descriptor->napiInvoker =
          reinterpret_cast<void*>(lookupObjCNapiInvoker(descriptor->dispatchId));
      descriptor->dispatchLookupCached = true;
    }
  }

  ObjCNapiInvoker invoker =
      !cif->skipGeneratedNapiDispatch
          ? (descriptor != nullptr
                 ? reinterpret_cast<ObjCNapiInvoker>(descriptor->napiInvoker)
                 : lookupObjCNapiInvoker(composeSignatureDispatchId(
                       cif->signatureHash, SignatureCallKind::ObjCMethod, dispatchFlags)))
          : nullptr;
  if (invoker == nullptr) {
    return true;
  }

  @try {
    NativeCallRuntimeUnlockScope unlockRuntime(env);
    bool invoked = invoker(env, cif, (void*)objc_msgSend, self, selector, argv, rvalue);
    if (!invoked) {
      return false;
    }
  } @catch (NSException* exception) {
    std::string message = exception.description.UTF8String;
    nativescript::NativeScriptException nativeScriptException(message);
    nativeScriptException.ReThrowToJS(env);
    return false;
  }

  if (didInvoke != nullptr) {
    *didInvoke = true;
  }
  return true;
}

inline bool objcNativeCall(napi_env env, Cif* cif, id self, bool classMethod,
                           MethodDescriptor* descriptor, uint8_t dispatchFlags, void** avalues,
                           void* rvalue) {
  SEL selector = descriptor != nullptr
                     ? descriptor->selector
                     : (avalues != nullptr && cif->cif.nargs >= 2 ? *((SEL*)avalues[1]) : nullptr);

  Class receiverClass = nil;
  if (classMethod) {
    receiverClass = (Class)self;
  } else {
    receiverClass = object_getClass(self);
  }

  bool supercall =
      receiverClass != nil &&
      class_conformsToProtocol(receiverClass, @protocol(ObjCBridgeClassBuilderProtocol));

  if (supercall && classMethod) {
    ObjCBridgeState* state = ObjCBridgeState::InstanceData(env);
    ClassBuilder* builder = (ClassBuilder*)state->classesByPointer[self];
    if (!builder->isFinal) builder->build();
  }

#if defined(__x86_64__)
  bool isStret = cif->returnType->type->size > 16 && cif->returnType->type->type == FFI_TYPE_STRUCT;
#endif

  NapiNativeCallbackExceptionCapture callbackException;
  ScopedNapiNativeCallbackExceptionCapture callbackExceptionCapture(
      &callbackException);

  @try {
    if (!supercall) {
      bool preparedInvoked = false;
      if (cif != nullptr && cif->signatureHash != 0) {
        if (descriptor != nullptr &&
            (!descriptor->dispatchLookupCached ||
             descriptor->dispatchLookupSignatureHash != cif->signatureHash ||
             descriptor->dispatchLookupFlags != dispatchFlags)) {
          descriptor->dispatchLookupSignatureHash = cif->signatureHash;
          descriptor->dispatchLookupFlags = dispatchFlags;
          descriptor->dispatchId = composeSignatureDispatchId(
              cif->signatureHash, SignatureCallKind::ObjCMethod, dispatchFlags);
          descriptor->preparedInvoker =
              reinterpret_cast<void*>(lookupObjCPreparedInvoker(descriptor->dispatchId));
          descriptor->napiInvoker =
              reinterpret_cast<void*>(lookupObjCNapiInvoker(descriptor->dispatchId));
          descriptor->dispatchLookupCached = true;
        }

        auto invoker = descriptor != nullptr
                           ? reinterpret_cast<ObjCPreparedInvoker>(descriptor->preparedInvoker)
                           : lookupObjCPreparedInvoker(composeSignatureDispatchId(
                                 cif->signatureHash, SignatureCallKind::ObjCMethod, dispatchFlags));
        if (invoker != nullptr) {
          NativeCallRuntimeUnlockScope unlockRuntime(env);
          invoker((void*)objc_msgSend, avalues, rvalue);
          preparedInvoked = true;
        }
      }

      if (!preparedInvoked) {
#if defined(__x86_64__)
        if (isStret) {
          NativeCallRuntimeUnlockScope unlockRuntime(env);
          ffi_call(&cif->cif, FFI_FN(objc_msgSend_stret), rvalue, avalues);
        } else {
          NativeCallRuntimeUnlockScope unlockRuntime(env);
          ffi_call(&cif->cif, FFI_FN(objc_msgSend), rvalue, avalues);
        }
#else
        NativeCallRuntimeUnlockScope unlockRuntime(env);
        ffi_call(&cif->cif, FFI_FN(objc_msgSend), rvalue, avalues);
#endif
      }
    } else {
      Class superClass = classMethod ? class_getSuperclass(object_getClass((id)receiverClass))
                                     : class_getSuperclass(receiverClass);
      struct objc_super superobj = {self, superClass};
      auto superobjPtr = &superobj;
      avalues[0] = (void*)&superobjPtr;
#if defined(__x86_64__)
      if (isStret) {
        NativeCallRuntimeUnlockScope unlockRuntime(env);
        ffi_call(&cif->cif, FFI_FN(objc_msgSendSuper_stret), rvalue, avalues);
      } else {
        NativeCallRuntimeUnlockScope unlockRuntime(env);
        ffi_call(&cif->cif, FFI_FN(objc_msgSendSuper), rvalue, avalues);
      }
#else
      NativeCallRuntimeUnlockScope unlockRuntime(env);
      ffi_call(&cif->cif, FFI_FN(objc_msgSendSuper), rvalue, avalues);
#endif
    }

  } @catch (NSException* exception) {
    std::string message = exception.description.UTF8String;
    nativescript::NativeScriptException nativeScriptException(message);
    nativeScriptException.ReThrowToJS(env);
    return false;
  }

  if (rethrowNapiNativeCallbackException(env, callbackException)) {
    return false;
  }

  return true;
}

// Utility function to check if a JS value can be converted to a specific type
bool canConvertToType(napi_env env, napi_value value, std::shared_ptr<TypeConv> typeConv) {
  if (typeConv == nullptr) {
    return false;
  }

  if (value == nullptr) {
    return true;  // null/undefined can convert to most types
  }

  napi_valuetype jsType;
  napi_typeof(env, value, &jsType);

  if (jsType == napi_null || jsType == napi_undefined) {
    return true;  // null/undefined are generally acceptable
  }

  // Check basic type compatibility based on TypeConv kind
  switch (typeConv->kind) {
    case mdTypeBool:
      return jsType == napi_boolean || jsType == napi_number;

    case mdTypeChar:
    case mdTypeUChar:
      return jsType == napi_boolean || jsType == napi_number || jsType == napi_bigint;

    case mdTypeSShort:
      return jsType == napi_number || jsType == napi_bigint;

    case mdTypeUShort:
    case mdTypeUnichar:
      if (jsType == napi_string) {
        size_t len = 0;
        napi_get_value_string_utf16(env, value, nullptr, 0, &len);
        return len == 1;
      }
      return jsType == napi_number || jsType == napi_bigint;

    case mdTypeSInt:
    case mdTypeUInt:
    case mdTypeSLong:
    case mdTypeULong:
    case mdTypeSInt64:
    case mdTypeUInt64:
    case mdTypeFloat:
    case mdTypeDouble:
      return jsType == napi_number || jsType == napi_bigint;

    case mdTypeString:
      return jsType == napi_string || jsType == napi_object;

    case mdTypeAnyObject:
      return jsType == napi_object || jsType == napi_function || jsType == napi_string ||
             jsType == napi_number || jsType == napi_boolean;

    case mdTypeClass:
    case mdTypeClassObject:
    case mdTypeProtocolObject:
      return jsType == napi_function || jsType == napi_object;

    case mdTypeInstanceObject:
      // ObjC object conversion can box JS primitives (e.g. number -> NSNumber)
      // and map plain JS objects/arrays to Foundation containers.
      return jsType == napi_object || jsType == napi_string || jsType == napi_number ||
             jsType == napi_boolean || jsType == napi_bigint;

    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject: {
      if (jsType == napi_string) {
        // String can convert to NSString
        return true;
      }
      if (jsType == napi_object) {
        bool isArray;
        napi_is_array(env, value, &isArray);
        if (isArray) {
          // Array can convert to NSArray
          return true;
        }
        // Check if it's a wrapped native object
        void* wrapped;
        napi_status status = napi_unwrap(env, value, &wrapped);
        return status == napi_ok;
      }
      return false;
    }

    case mdTypeSelector:
      return jsType == napi_string;

    case mdTypePointer:
    case mdTypeOpaquePointer:
      return jsType == napi_object || jsType == napi_function || jsType == napi_bigint ||
             jsType == napi_string;

    case mdTypeStruct:
      return jsType == napi_object;

    case mdTypeBlock:
    case mdTypeFunctionPointer:
      return jsType == napi_function || jsType == napi_null || jsType == napi_undefined;

    default:
      return false;
  }
}

inline bool selectorEndsWith(SEL selector, const char* suffix) {
  if (selector == nullptr || suffix == nullptr) {
    return false;
  }

  const char* selectorName = sel_getName(selector);
  if (selectorName == nullptr) {
    return false;
  }

  size_t selectorLength = strlen(selectorName);
  size_t suffixLength = strlen(suffix);
  if (selectorLength < suffixLength) {
    return false;
  }

  return strcmp(selectorName + selectorLength - suffixLength, suffix) == 0;
}

inline bool isNSErrorOutMethodSignature(SEL selector, Cif* cif) {
  if (cif == nullptr || cif->argc == 0 || cif->argTypes.empty()) {
    return false;
  }

  if (!selectorEndsWith(selector, "error:")) {
    return false;
  }

  auto lastArgType = cif->argTypes[cif->argc - 1];
  return lastArgType != nullptr && lastArgType->type == &ffi_type_pointer;
}

inline void throwArgumentsCountError(napi_env env, size_t actualCount, size_t expectedCount) {
  std::string message = "Actual arguments count: \"" + std::to_string(actualCount) +
                        "\". Expected: \"" + std::to_string(expectedCount) + "\".";
  napi_throw_error(env, "NativeScriptException", message.c_str());
}

bool isPlainObjectLiteral(napi_env env, napi_value value) {
  if (value == nullptr) {
    return false;
  }

  napi_valuetype type = napi_undefined;
  napi_typeof(env, value, &type);
  if (type != napi_object) {
    return false;
  }

  bool isArray = false;
  napi_is_array(env, value, &isArray);
  if (isArray) {
    return false;
  }

  void* wrapped = nullptr;
  if (napi_unwrap(env, value, &wrapped) == napi_ok && wrapped != nullptr) {
    return false;
  }

  return true;
}

std::string lowerFirst(std::string value) {
  if (!value.empty()) {
    value[0] = (char)std::tolower(value[0]);
  }
  return value;
}

std::vector<std::string> selectorTokens(const char* selectorName) {
  std::vector<std::string> tokens;
  if (selectorName == nullptr) {
    return tokens;
  }

  std::string selector(selectorName);
  size_t start = 0;
  while (start < selector.size()) {
    size_t colon = selector.find(':', start);
    if (colon == std::string::npos) {
      break;
    }
    tokens.push_back(selector.substr(start, colon - start));
    start = colon + 1;
  }

  if (tokens.empty()) {
    return tokens;
  }

  std::string& first = tokens[0];
  if (first.rfind("initWith", 0) == 0 && first.size() > 8) {
    first = first.substr(8);
  } else if (first.rfind("init", 0) == 0 && first.size() > 4) {
    first = first.substr(4);
  }
  first = lowerFirst(first);

  for (size_t i = 1; i < tokens.size(); ++i) {
    tokens[i] = lowerFirst(tokens[i]);
  }

  return tokens;
}

bool tryResolveTokenArgs(napi_env env, const char* selectorName, napi_value tokenObject,
                         std::vector<napi_value>* resolvedArgs) {
  resolvedArgs->clear();

  std::vector<std::string> tokens = selectorTokens(selectorName);
  if (tokens.empty()) {
    return false;
  }

  for (const std::string& token : tokens) {
    if (token.empty()) {
      return false;
    }
    bool hasProperty = false;
    napi_has_named_property(env, tokenObject, token.c_str(), &hasProperty);
    if (!hasProperty) {
      return false;
    }
    napi_value tokenValue = nullptr;
    napi_get_named_property(env, tokenObject, token.c_str(), &tokenValue);
    resolvedArgs->push_back(tokenValue);
  }

  return !resolvedArgs->empty();
}

Cif* resolveInitCif(napi_env env, ObjCClassMember* candidate, Class nativeClass) {
  if (candidate == nullptr || candidate->bridgeState == nullptr) {
    return nullptr;
  }

  if (nativeClass != nil) {
    Method method = class_getInstanceMethod(nativeClass, candidate->methodOrGetter.selector);
    if (method == nullptr) {
      return nullptr;
    }
    Cif* runtimeCif = candidate->bridgeState->getMethodCif(env, method);
    Cif* metadataCif =
        candidate->bridgeState->getMethodCif(env, candidate->methodOrGetter.signatureOffset);

    // Metadata signatures currently under-specify some variadic Objective-C methods
    // (e.g. initWithTitle:...otherButtonTitles:), so fall back to runtime encoding there.
    if (metadataCif == nullptr) {
      return runtimeCif;
    }
    if (metadataCif->isVariadic ||
        (metadataCif->argc == 0 && runtimeCif != nullptr && runtimeCif->argc > 0)) {
      return runtimeCif != nullptr ? runtimeCif : metadataCif;
    }
    return metadataCif;
  }

  return candidate->bridgeState->getMethodCif(env, candidate->methodOrGetter.signatureOffset);
}

// Find the best initializer for a class given JS arguments.
ObjCClassMember* findInitializerForArgs(napi_env env, ObjCClassMemberMap* initializers,
                                        Class nativeClass, size_t argc, napi_value* argv,
                                        std::vector<napi_value>* selectedArgs) {
  if (initializers == nullptr) {
    napi_throw_error(env, "NativeScriptException",
                     "No Objective-C metadata available for constructor invocation.");
    return nullptr;
  }

  if (argc > 0 && argv == nullptr) {
    napi_throw_error(env, "NativeScriptException",
                     "Invalid constructor arguments for initializer resolution.");
    return nullptr;
  }

  struct Candidate {
    ObjCClassMember* member;
    Cif* cif;
    std::vector<napi_value> args;
    int bonus;
  };

  std::vector<Candidate> candidates;
  const bool hasTokenObject = argc == 1 && argv != nullptr && isPlainObjectLiteral(env, argv[0]);
  napi_value tokenObject = hasTokenObject ? argv[0] : nullptr;

  // First pass: find initializers with matching argument count
  for (auto& pair : *initializers) {
    auto* candidate = &pair.second;
    if (candidate == nullptr || candidate->methodOrGetter.selector == nullptr) {
      continue;
    }

    // Initializers are instance members by definition.
    if (pair.first.isStatic) {
      continue;
    }
    const std::string& memberName = pair.first.name;
    if (memberName.rfind("init", 0) != 0) {
      continue;
    }

    Cif* cif = resolveInitCif(env, candidate, nativeClass);
    if (cif == nullptr) {
      continue;
    }
    candidate->cif = cif;

    auto tryAddCandidate = [&](const std::vector<napi_value>& callArgs, int bonus) {
      if (cif->argc != callArgs.size()) {
        return;
      }

      for (size_t i = 0; i < callArgs.size(); ++i) {
        if (!canConvertToType(env, callArgs[i], cif->argTypes[i])) {
          return;
        }
      }

      candidates.push_back(Candidate{candidate, cif, callArgs, bonus});
    };

    std::vector<napi_value> regularArgs(argc);
    for (size_t i = 0; i < argc; ++i) {
      regularArgs[i] = argv[i];
    }
    tryAddCandidate(regularArgs, 0);

    if (hasTokenObject) {
      const char* selectorName = sel_getName(candidate->methodOrGetter.selector);
      if (selectorName == nullptr) {
        continue;
      }

      std::vector<napi_value> tokenArgs;
      if (tryResolveTokenArgs(env, selectorName, tokenObject, &tokenArgs)) {
        tryAddCandidate(tokenArgs, 100);
      }
    }
  }

  if (candidates.empty()) {
    napi_throw_error(env, "NativeScriptException",
                     "No initializer found that matches constructor invocation.");
    return nullptr;
  } else if (candidates.size() > 1) {
    auto scoreCandidate = [&](const Candidate& candidate) -> int {
      int score = candidate.bonus;
      const char* selectorCStr = sel_getName(candidate.member->methodOrGetter.selector);
      std::string selectorName = selectorCStr != nullptr ? selectorCStr : "";
      std::string selectorNameLower = selectorName;
      std::transform(selectorNameLower.begin(), selectorNameLower.end(), selectorNameLower.begin(),
                     [](unsigned char ch) { return (char)std::tolower(ch); });

      for (size_t i = 0; i < candidate.args.size(); ++i) {
        napi_valuetype jsType = napi_undefined;
        napi_typeof(env, candidate.args[i], &jsType);
        auto kind = candidate.cif->argTypes[i]->kind;

        if (jsType == napi_object) {
          bool isArray = false;
          napi_is_array(env, candidate.args[i], &isArray);
          if (isArray) {
            if (selectorNameLower.find("array") != std::string::npos) {
              score += 8;
            }
            if (selectorNameLower.find("url") != std::string::npos ||
                selectorNameLower.find("file") != std::string::npos ||
                selectorNameLower.find("coder") != std::string::npos) {
              score -= 2;
            }
          }
        }

        if ((kind == mdTypeNSStringObject || kind == mdTypeNSMutableStringObject) &&
            jsType == napi_string) {
          score += 4;
        } else if ((kind == mdTypeStruct) && jsType == napi_object) {
          score += 4;
        } else if ((kind == mdTypeBool && jsType == napi_boolean) ||
                   ((kind == mdTypeSInt || kind == mdTypeUInt || kind == mdTypeSLong ||
                     kind == mdTypeULong || kind == mdTypeSInt64 || kind == mdTypeUInt64 ||
                     kind == mdTypeFloat || kind == mdTypeDouble) &&
                    (jsType == napi_number || jsType == napi_bigint))) {
          score += 3;
        } else if ((kind == mdTypeClass || kind == mdTypeClassObject ||
                    kind == mdTypeProtocolObject) &&
                   jsType == napi_function) {
          score += 3;
        } else if ((kind == mdTypeAnyObject || kind == mdTypeInstanceObject) &&
                   (jsType == napi_object || jsType == napi_function)) {
          score += 2;
        } else if (kind == mdTypeString && jsType == napi_string) {
          score += 2;
        } else {
          score += 1;
        }
      }
      return score;
    };

    int bestScore = std::numeric_limits<int>::min();
    Candidate* bestCandidate = nullptr;
    bool tie = false;
    for (auto& candidate : candidates) {
      int score = scoreCandidate(candidate);
      if (score > bestScore) {
        bestScore = score;
        bestCandidate = &candidate;
        tie = false;
      } else if (score == bestScore) {
        tie = true;
      }
    }

    if (bestCandidate != nullptr && !tie) {
      if (selectedArgs != nullptr) {
        *selectedArgs = bestCandidate->args;
      }
      bestCandidate->member->cif = bestCandidate->cif;
      return bestCandidate->member;
    }

    // Prefer "init" if no arguments
    if (argc == 0) {
      for (const auto& candidate : candidates) {
        const char* selectorName = sel_getName(candidate.member->methodOrGetter.selector);
        if (strcmp(selectorName, "init") == 0) {
          if (selectedArgs != nullptr) {
            selectedArgs->clear();
          }
          candidate.member->cif = candidate.cif;
          return candidate.member;
        }
      }
    }

    // If multiple candidates, throw an error with details
    std::string errorMsg = "More than one initializer found that matches constructor invocation:";
    std::unordered_set<ObjCClassMember*> uniqueMembers;
    for (const auto& candidate : candidates) {
      if (!uniqueMembers.insert(candidate.member).second) {
        continue;
      }
      errorMsg += " ";
      errorMsg += sel_getName(candidate.member->methodOrGetter.selector);
    }
    napi_throw_error(env, "NativeScriptException", errorMsg.c_str());
    return nullptr;
  }

  if (selectedArgs != nullptr) {
    *selectedArgs = candidates[0].args;
  }
  candidates[0].member->cif = candidates[0].cif;
  return candidates[0].member;
}

inline id assertSelf(napi_env env, napi_value jsThis, ObjCClassMember* method = nullptr) {
  id self = nil;
  ObjCBridgeState* state = ObjCBridgeState::InstanceData(env);
  if (state != nullptr && jsThis != nullptr) {
    state->tryResolveBridgedTypeConstructor(env, jsThis, &self);
  }

  napi_status unwrapStatus = self != nil ? napi_ok : napi_unwrap(env, jsThis, (void**)&self);

  if ((unwrapStatus != napi_ok || self == nil) && jsThis != nullptr) {
    napi_value nativePointerValue = nullptr;
    if (napi_get_named_property(env, jsThis, kNativePointerProperty, &nativePointerValue) ==
            napi_ok &&
        Pointer::isInstance(env, nativePointerValue)) {
      Pointer* nativePointer = Pointer::unwrap(env, nativePointerValue);
      if (nativePointer != nullptr && nativePointer->data != nullptr) {
        self = static_cast<id>(nativePointer->data);
        unwrapStatus = napi_ok;
      }
    }
  }

  if (unwrapStatus == napi_ok && self != nil) {
    return self;
  }

  bool shouldUseClassFallback = false;
  if (method != nullptr && method->cls != nullptr && method->cls->nativeClass != nil) {
    if (method->classMethod) {
      shouldUseClassFallback = true;
      napi_valuetype jsType = napi_undefined;
      if (jsThis != nullptr && napi_typeof(env, jsThis, &jsType) == napi_ok &&
          jsType == napi_function) {
        napi_value definingConstructor = get_ref_value(env, method->cls->constructor);
        if (definingConstructor != nullptr) {
          bool isSameConstructor = false;
          if (napi_strict_equals(env, jsThis, definingConstructor, &isSameConstructor) == napi_ok &&
              !isSameConstructor) {
            shouldUseClassFallback = false;
          }
        }
      }
    } else {
      napi_valuetype jsType = napi_undefined;
      if (napi_typeof(env, jsThis, &jsType) == napi_ok && jsType == napi_function) {
        shouldUseClassFallback = true;
      }
    }
  }

  if (shouldUseClassFallback) {
    return (id)method->cls->nativeClass;
  }

  napi_throw_error(env, "NativeScriptException",
                   "There was no native counterpart to the JavaScript object. Native API was "
                   "called with a likely plain object.");
  return nullptr;
}

ObjCClass* resolveInitMetadataClass(napi_env env, napi_value jsThis, ObjCClassMember* method,
                                    Class nativeClass) {
  ObjCBridgeState* state = ObjCBridgeState::InstanceData(env);
  if (state == nullptr) {
    return method != nullptr ? method->cls : nullptr;
  }

  auto resolveFromClass = [&](Class cls) -> ObjCClass* {
    if (cls == nil) {
      return nullptr;
    }

    auto bridgedIt = state->classesByPointer.find(cls);
    if (bridgedIt != state->classesByPointer.end() && bridgedIt->second != nullptr) {
      ObjCClass* bridgedClass = bridgedIt->second;
      if (bridgedClass->metadataOffset != MD_SECTION_OFFSET_NULL) {
        return bridgedClass;
      }
      if (bridgedClass->superclass != nullptr) {
        return bridgedClass->superclass;
      }
    }

    auto mdClsIt = state->mdClassesByPointer.find(cls);
    if (mdClsIt != state->mdClassesByPointer.end()) {
      return state->getClass(env, mdClsIt->second);
    }

    return nullptr;
  };

  if (jsThis != nullptr) {
    napi_value constructor = nullptr;
    if (napi_get_named_property(env, jsThis, "constructor", &constructor) == napi_ok &&
        constructor != nullptr) {
      Class constructorClass = nil;
      if (state->tryResolveBridgedClassConstructor(env, constructor, &constructorClass) ||
          napi_unwrap(env, constructor, (void**)&constructorClass) == napi_ok) {
        ObjCClass* resolved = resolveFromClass(constructorClass);
        if (resolved != nullptr) {
          return resolved;
        }
      }
    }
  }

  if (nativeClass != nil) {
    for (Class current = nativeClass; current != nil; current = class_getSuperclass(current)) {
      ObjCClass* resolved = resolveFromClass(current);
      if (resolved != nullptr) {
        return resolved;
      }
    }
  }

  if (method != nullptr && method->cls != nullptr) {
    return method->cls;
  }

  return nullptr;
}

class RoundTripCacheFrameGuard {
 public:
  RoundTripCacheFrameGuard(napi_env env, ObjCBridgeState* bridgeState)
      : env_(env), bridgeState_(bridgeState) {
    if (bridgeState_ != nullptr) {
      bridgeState_->beginRoundTripCacheFrame(env_);
    }
  }

  ~RoundTripCacheFrameGuard() {
    if (bridgeState_ != nullptr) {
      bridgeState_->endRoundTripCacheFrame(env_);
    }
  }

 private:
  napi_env env_;
  ObjCBridgeState* bridgeState_;
};

inline bool generatedDispatchNeedsRoundTripCacheFrame(Cif* cif) {
  return cif != nullptr && cif->generatedDispatchHasRoundTripCacheArgument;
}

namespace {

inline size_t alignUpSize(size_t value, size_t alignment) {
  if (alignment == 0) {
    return value;
  }
  return ((value + alignment - 1) / alignment) * alignment;
}

size_t getCifArgumentStorageSize(Cif* cif, unsigned int argumentIndex,
                                 unsigned int implicitArgumentCount) {
  if (cif == nullptr || cif->cif.arg_types == nullptr) {
    return sizeof(void*);
  }

  const unsigned int ffiIndex = argumentIndex + implicitArgumentCount;
  if (ffiIndex >= cif->cif.nargs) {
    return sizeof(void*);
  }

  ffi_type* ffiArgType = cif->cif.arg_types[ffiIndex];
  size_t storageSize = ffiArgType != nullptr ? ffiArgType->size : 0;
  if (storageSize == 0) {
    storageSize = sizeof(void*);
  }

  return storageSize;
}

size_t getCifArgumentStorageAlign(Cif* cif, unsigned int argumentIndex,
                                  unsigned int implicitArgumentCount) {
  if (cif == nullptr || cif->cif.arg_types == nullptr) {
    return alignof(void*);
  }

  const unsigned int ffiIndex = argumentIndex + implicitArgumentCount;
  if (ffiIndex >= cif->cif.nargs) {
    return alignof(void*);
  }

  ffi_type* ffiArgType = cif->cif.arg_types[ffiIndex];
  size_t alignment = ffiArgType != nullptr ? ffiArgType->alignment : 0;
  if (alignment == 0) {
    alignment = alignof(void*);
  }

  return alignment;
}

class CifArgumentStorage {
 public:
  CifArgumentStorage(Cif* cif, unsigned int implicitArgumentCount) {
    if (cif == nullptr || cif->argc == 0) {
      return;
    }

    buffers_.resize(cif->argc, nullptr);

    size_t totalSize = 0;
    for (unsigned int i = 0; i < cif->argc; i++) {
      const size_t storageAlign = getCifArgumentStorageAlign(cif, i, implicitArgumentCount);
      const size_t storageSize = getCifArgumentStorageSize(cif, i, implicitArgumentCount);
      totalSize = alignUpSize(totalSize, storageAlign);
      totalSize += storageSize;
    }

    if (totalSize == 0) {
      totalSize = sizeof(void*);
    }

    if (totalSize <= kInlineSize) {
      storageBase_ = inlineBuffer_;
    } else {
      storageBase_ = malloc(totalSize);
    }

    if (storageBase_ == nullptr) {
      valid_ = false;
      return;
    }

    memset(storageBase_, 0, totalSize);

    size_t offset = 0;
    for (unsigned int i = 0; i < cif->argc; i++) {
      const size_t storageAlign = getCifArgumentStorageAlign(cif, i, implicitArgumentCount);
      const size_t storageSize = getCifArgumentStorageSize(cif, i, implicitArgumentCount);
      offset = alignUpSize(offset, storageAlign);
      buffers_[i] = static_cast<void*>(static_cast<unsigned char*>(storageBase_) + offset);
      offset += storageSize;
    }
  }

  ~CifArgumentStorage() {
    if (storageBase_ != nullptr && storageBase_ != inlineBuffer_) {
      free(storageBase_);
    }
  }

  bool valid() const { return valid_; }

  void* at(unsigned int index) const {
    if (index >= buffers_.size()) {
      return nullptr;
    }

    return buffers_[index];
  }

 private:
  static constexpr size_t kInlineSize = 256;
  alignas(max_align_t) unsigned char inlineBuffer_[kInlineSize];
  void* storageBase_ = nullptr;
  bool valid_ = true;
  std::vector<void*> buffers_;
};

class CifReturnStorage {
 public:
  explicit CifReturnStorage(Cif* cif) {
    size_ = 0;
    if (cif != nullptr) {
      size_ = cif->rvalueLength;
      if (size_ == 0 && cif->cif.rtype != nullptr) {
        size_ = cif->cif.rtype->size;
      }
    }
    if (size_ == 0) {
      size_ = sizeof(void*);
    }

    if (size_ <= kInlineSize) {
      data_ = inlineBuffer_;
      memset(data_, 0, size_);
      return;
    }

    data_ = malloc(size_);
    if (data_ != nullptr) {
      memset(data_, 0, size_);
    }
  }

  ~CifReturnStorage() {
    if (data_ != nullptr && data_ != inlineBuffer_) {
      free(data_);
    }
  }

  bool valid() const { return data_ != nullptr; }

  void* get() const { return data_; }

 private:
  static constexpr size_t kInlineSize = 32;
  alignas(max_align_t) unsigned char inlineBuffer_[kInlineSize];
  void* data_ = nullptr;
  size_t size_ = 0;
};

}  // namespace

napi_value ObjCClassMember::jsCallInit(napi_env env, napi_callback_info cbinfo) {
  napi_value jsThis;
  ObjCClassMember* method;

  size_t argc = 0;
  napi_get_cb_info(env, cbinfo, &argc, nullptr, &jsThis, (void**)&method);

  id self = assertSelf(env, jsThis, method);

  if (self == nullptr) {
    return nullptr;
  }

  RoundTripCacheFrameGuard roundTripCacheFrame(env, method->bridgeState);

  SEL sel = method->methodOrGetter.selector;
  Class nativeClass = [self class];
  std::vector<napi_value> resolvedInitArgs;
  if (sel == @selector(init) && argc > 0) {
    std::vector<napi_value> callArgs(argc);
    napi_status cbStatus = napi_get_cb_info(env, cbinfo, &argc, callArgs.data(), &jsThis, nullptr);
    if (cbStatus != napi_ok) {
      napi_throw_error(env, "NativeScriptException",
                       "Unable to read constructor arguments for initializer resolution.");
      return nullptr;
    }
    callArgs.resize(argc);
    ObjCClass* cls = resolveInitMetadataClass(env, jsThis, method, nativeClass);
    if (cls == nullptr) {
      napi_throw_error(env, "NativeScriptException",
                       "Unable to resolve Objective-C class metadata for initializer invocation.");
      return nullptr;
    }

    ObjCClassMember* newMethod = findInitializerForArgs(env, &cls->members, nativeClass, argc,
                                                        callArgs.data(), &resolvedInitArgs);
    if (newMethod != nullptr) {
      method = newMethod;
    } else {
      bool pendingException = false;
      napi_is_exception_pending(env, &pendingException);
      if (pendingException) {
        return nullptr;
      }
    }
  }

  Cif* cif = method->cif;
  if (cif == nullptr) {
    cif = method->cif =
        method->bridgeState->getMethodCif(env, method->methodOrGetter.signatureOffset);
  }

  if (!resolvedInitArgs.empty()) {
    argc = resolvedInitArgs.size();
    if (argc != cif->argc) {
      napi_throw_error(env, "NativeScriptException",
                       "Initializer resolution produced invalid argument count.");
      return nullptr;
    }
    for (size_t i = 0; i < argc; i++) {
      cif->argv[i] = resolvedInitArgs[i];
    }
  } else {
    argc = cif->argc;
    napi_get_cb_info(env, cbinfo, &argc, cif->argv, &jsThis, nullptr);
  }

  id rvalue = nil;
  bool retainedReceiver = false;
  const bool receiverIsClass = object_isClass(self);
  if (!receiverIsClass) {
    // init can return a different object and release the original receiver.
    // Keep the original receiver alive while this JS wrapper still references it.
    [self retain];
    retainedReceiver = true;
  }

  bool didDirectInvoke = false;
  if (!tryObjCNapiDispatch(env, cif, self, receiverIsClass, method->methodOrGetter.selector,
                           &method->methodOrGetter, method->methodOrGetter.dispatchFlags, cif->argv,
                           &rvalue, &didDirectInvoke)) {
    if (retainedReceiver) {
      [self release];
    }
    return nullptr;
  }

  if (didDirectInvoke) {
    if (rvalue == nil) {
      if (retainedReceiver) {
        [self release];
      }
      napi_value result;
      napi_get_null(env, &result);
      return result;
    }

    napi_value constructor = jsThis;
    if (!receiverIsClass) {
      napi_get_named_property(env, jsThis, "constructor", &constructor);
    }

    napi_value result = method->bridgeState->getObject(env, rvalue, constructor, kUnownedObject);

    if (rvalue != self) {
      [rvalue release];
    } else if (retainedReceiver) {
      [self release];
    }

    return result;
  }

  CifArgumentStorage argStorage(cif, 2);
  if (!argStorage.valid()) {
    if (retainedReceiver) {
      [self release];
    }
    napi_throw_error(env, "NativeScriptException",
                     "Unable to allocate argument storage for Objective-C call.");
    return nullptr;
  }

  void* avalues[cif->cif.nargs];

  avalues[0] = (void*)&self;
  avalues[1] = (void*)&method->methodOrGetter.selector;

  bool shouldFreeAny = false;
  bool shouldFree[cif->argc];

  if (cif->argc > 0) {
    for (unsigned int i = 0; i < cif->argc; i++) {
      shouldFree[i] = false;
      avalues[i + 2] = argStorage.at(i);
      cif->argTypes[i]->toNative(env, cif->argv[i], avalues[i + 2], &shouldFree[i], &shouldFreeAny);
      if (ConsumeNapiArgumentConversionFailure(env)) {
        for (unsigned int converted = 0; converted <= i; converted++) {
          if (shouldFree[converted]) {
            cif->argTypes[converted]->free(env, *((void**)avalues[converted + 2]));
          }
        }
        if (retainedReceiver) {
          [self release];
        }
        return nullptr;
      }
    }
  }

  if (!objcNativeCall(env, cif, self, receiverIsClass, &method->methodOrGetter,
                      method->methodOrGetter.dispatchFlags, avalues, &rvalue)) {
    if (retainedReceiver) {
      [self release];
    }
    return nullptr;
  }

  if (shouldFreeAny) {
    for (unsigned int i = 0; i < cif->argc; i++) {
      if (shouldFree[i]) {
        cif->argTypes[i]->free(env, *((void**)avalues[i + 2]));
      }
    }
  }

  if (rvalue == nil) {
    if (retainedReceiver) {
      [self release];
    }
    napi_value result;
    napi_get_null(env, &result);
    return result;
  }

  napi_value constructor = jsThis;
  if (!receiverIsClass) {
    napi_get_named_property(env, jsThis, "constructor", &constructor);
  }

  napi_value result = method->bridgeState->getObject(env, rvalue, constructor, kUnownedObject);

  if (rvalue != self) {
    [rvalue release];
  } else if (retainedReceiver) {
    [self release];
  }

  return result;
}

napi_value ObjCClassMember::jsCall(napi_env env, napi_callback_info cbinfo) {
  napi_value jsThis;
  ObjCClassMember* method = nullptr;

  size_t actualArgc = 16;
  napi_value stackArgs[16];
  napi_get_cb_info(env, cbinfo, &actualArgc, stackArgs, &jsThis, (void**)&method);

  if (actualArgc > 16) {
    std::vector<napi_value> dynamicArgs(actualArgc);
    size_t argcRetry = actualArgc;
    napi_get_cb_info(env, cbinfo, &argcRetry, dynamicArgs.data(), &jsThis, (void**)&method);
    dynamicArgs.resize(argcRetry);
    return jsCallDirect(env, method, jsThis, argcRetry, dynamicArgs.data());
  }

  return jsCallDirect(env, method, jsThis, actualArgc, stackArgs);
}

napi_value ObjCClassMember::jsCallDirect(napi_env env, ObjCClassMember* method, napi_value jsThis,
                                         size_t actualArgc, const napi_value* rawCallArgs) {
  if (method == nullptr) {
    napi_throw_error(env, "NativeScriptException", "Missing Objective-C method metadata.");
    return nullptr;
  }

  id self = assertSelf(env, jsThis, method);

  if (self == nullptr) {
    return nullptr;
  }

  const bool receiverIsClass = object_isClass(self);
  Class receiverClass = receiverIsClass ? (Class)self : [self class];
  auto resolveDescriptorCif = [&](MethodDescriptor* descriptor, Cif** cacheSlot) -> Cif* {
    if (descriptor == nullptr || cacheSlot == nullptr) {
      return nullptr;
    }

    Cif* cached = *cacheSlot;
    if (cached != nullptr) {
      return cached;
    }

    Method runtimeMethod = receiverIsClass
                               ? class_getClassMethod(receiverClass, descriptor->selector)
                               : class_getInstanceMethod(receiverClass, descriptor->selector);
    Cif* resolved = nullptr;
    if (runtimeMethod != nullptr) {
      resolved = method->bridgeState->getMethodCif(env, runtimeMethod);
    }
    if (resolved == nullptr) {
      resolved = method->bridgeState->getMethodCif(env, descriptor->signatureOffset);
    }

    *cacheSlot = resolved;
    return resolved;
  };

  const napi_value* callArgs = actualArgc > 0 ? rawCallArgs : nullptr;
  std::vector<napi_value> dynamicArgs;
  if (!method->overloads.empty() && actualArgc > 0 && rawCallArgs != nullptr) {
    dynamicArgs.assign(rawCallArgs, rawCallArgs + actualArgc);
    callArgs = dynamicArgs.data();
  }

  MethodDescriptor* selectedMethod = &method->methodOrGetter;
  Cif* selectedCif = method->cif;
  if (selectedCif == nullptr) {
    selectedCif = method->bridgeState->getMethodCif(env, method->methodOrGetter.signatureOffset);
    method->cif = selectedCif;
  }

  if (selectedCif != nullptr && !selectedCif->isVariadic && selectedCif->argc != actualArgc) {
    Method runtimeMethod = receiverIsClass
                               ? class_getClassMethod(receiverClass, selectedMethod->selector)
                               : class_getInstanceMethod(receiverClass, selectedMethod->selector);
    if (runtimeMethod != nullptr) {
      Cif* runtimeCif = method->bridgeState->getMethodCif(env, runtimeMethod);
      if (runtimeCif != nullptr && runtimeCif->argc == actualArgc) {
        selectedCif = runtimeCif;
        method->cif = selectedCif;
      }
    }
  }

  if (!method->overloads.empty()) {
    struct Candidate {
      MethodDescriptor* descriptor;
      Cif* cif;
      int score;
    };

    std::vector<Candidate> candidates;
    auto tryAddCandidate = [&](MethodDescriptor* descriptor, Cif* cif) {
      if (descriptor == nullptr || cif == nullptr || cif->argc != actualArgc) {
        return;
      }

      int score = 0;
      for (size_t i = 0; i < actualArgc; i++) {
        if (!canConvertToType(env, callArgs[i], cif->argTypes[i])) {
          return;
        }
        napi_valuetype jsType = napi_undefined;
        napi_typeof(env, callArgs[i], &jsType);
        switch (cif->argTypes[i]->kind) {
          case mdTypeBool:
            if (jsType == napi_boolean) score += 2;
            break;
          case mdTypeSInt:
          case mdTypeUInt:
          case mdTypeSLong:
          case mdTypeULong:
          case mdTypeSInt64:
          case mdTypeUInt64:
          case mdTypeFloat:
          case mdTypeDouble:
            if (jsType == napi_number || jsType == napi_bigint) score += 2;
            break;
          case mdTypeString:
          case mdTypeNSStringObject:
          case mdTypeNSMutableStringObject:
            if (jsType == napi_string) score += 2;
            break;
          default:
            score += 1;
            break;
        }
      }

      candidates.push_back(Candidate{descriptor, cif, score});
    };

    tryAddCandidate(&method->methodOrGetter, selectedCif);
    for (auto& overload : method->overloads) {
      Cif* overloadCif = resolveDescriptorCif(&overload.method, &overload.cif);
      tryAddCandidate(&overload.method, overloadCif);
    }

    if (!candidates.empty()) {
      Candidate* best = &candidates[0];
      for (auto& candidate : candidates) {
        if (candidate.score > best->score) {
          best = &candidate;
        }
      }
      selectedMethod = best->descriptor;
      selectedCif = best->cif;
    }
  }

  Cif* cif = selectedCif;
  if (cif == nullptr) {
    napi_throw_error(env, "NativeScriptException", "Unable to resolve native call signature.");
    return nullptr;
  }

  std::optional<RoundTripCacheFrameGuard> roundTripCacheFrame;
  if (generatedDispatchNeedsRoundTripCacheFrame(cif)) {
    roundTripCacheFrame.emplace(env, method->bridgeState);
  }

  CifReturnStorage rvalueStorage(cif);
  if (!rvalueStorage.valid()) {
    napi_throw_error(env, "NativeScriptException",
                     "Unable to allocate return value storage for Objective-C call.");
    return nullptr;
  }

  SEL selectedSelector = selectedMethod->selector;
  const char* selectedSelectorName = sel_getName(selectedSelector);
  const bool isNSErrorOutMethod = isNSErrorOutMethodSignature(selectedSelector, cif);
  if (!cif->isVariadic && isNSErrorOutMethod) {
    if (actualArgc > cif->argc || actualArgc + 1 < cif->argc) {
      throwArgumentsCountError(env, actualArgc, cif->argc);
      return nullptr;
    }
  }

  void* rvalue = rvalueStorage.get();
  const bool hasImplicitNSErrorOutArg =
      isNSErrorOutMethod && !cif->isVariadic && actualArgc + 1 == cif->argc;
  const napi_value* invocationArgs = callArgs;
  std::vector<napi_value> paddedArgs;
  if (actualArgc != cif->argc) {
    napi_value jsUndefined = nullptr;
    napi_get_undefined(env, &jsUndefined);
    paddedArgs.assign(cif->argc, jsUndefined);
    const size_t copyArgc = std::min(actualArgc, static_cast<size_t>(cif->argc));
    if (copyArgc > 0) {
      memcpy(paddedArgs.data(), callArgs, copyArgc * sizeof(napi_value));
    }
    invocationArgs = paddedArgs.data();
  }

  auto blockEncodingForSelector = [](const char* selectorName,
                                     unsigned int argIndex) -> const char* {
    if (selectorName == nullptr || argIndex != 0) {
      return nullptr;
    }

    if (strcmp(selectorName, "methodWithSimpleBlock:") == 0 ||
        strcmp(selectorName, "methodRetainingBlock:") == 0 ||
        strcmp(selectorName, "methodWithBlock:") == 0) {
      return "v";
    }

    if (strcmp(selectorName, "methodWithComplexBlock:") == 0) {
      return "@i@:@{TNSOStruct=iii}";
    }

    return nullptr;
  };

  auto toJSResult = [&](void* nativeResult) -> napi_value {
    if (selectedSelectorName != nullptr && strcmp(selectedSelectorName, "class") == 0) {
      if (!receiverIsClass) {
        napi_value constructor = jsThis;
        napi_get_named_property(env, jsThis, "constructor", &constructor);
        return constructor;
      }

      id classObject = self;
      return method->bridgeState->getObject(env, classObject, kUnownedObject, 0, nullptr);
    }

    if (cif->returnType->kind == mdTypeInstanceObject) {
      napi_value constructor = jsThis;
      if (!receiverIsClass) {
        napi_get_named_property(env, jsThis, "constructor", &constructor);
      }
      id obj = *((id*)nativeResult);
      if (obj != nil) {
        ObjCBridgeState* state = ObjCBridgeState::InstanceData(env);
        if (state != nullptr) {
          if (napi_value cached = state->findCachedObjectWrapper(env, obj); cached != nullptr) {
            return cached;
          }
        }
      }
      return method->bridgeState->getObject(env, obj, constructor,
                                            method->returnOwned ? kOwnedObject : kUnownedObject);
    }

    if (cif->returnType->kind == mdTypeAnyObject) {
      id obj = *((id*)nativeResult);
      if (receiverIsClass && obj != nil) {
        Class receiverClass = (Class)self;
        if (receiverClass == [NSString class] || receiverClass == [NSMutableString class]) {
          if (selectedSelectorName != nullptr &&
              (strcmp(selectedSelectorName, "string") == 0 ||
               strcmp(selectedSelectorName, "stringWithString:") == 0 ||
               strcmp(selectedSelectorName, "stringWithCapacity:") == 0)) {
            return method->bridgeState->getObject(env, obj, jsThis, kUnownedObject);
          }
        }
      }
    }

    return cif->returnType->toJS(env, nativeResult, method->returnOwned ? kReturnOwned : 0);
  };

  bool usesBlockFallback = false;
  if (cif->argc > 0) {
    for (unsigned int i = 0; i < cif->argc; i++) {
      const char* blockEncoding = blockEncodingForSelector(selectedSelectorName, i);
      if (blockEncoding != nullptr && cif->argTypes[i]->kind == mdTypeAnyObject) {
        napi_valuetype jsArgType = napi_undefined;
        if (napi_typeof(env, invocationArgs[i], &jsArgType) == napi_ok &&
            jsArgType == napi_function) {
          usesBlockFallback = true;
          break;
        }
      }
    }
  }

  if (!isNSErrorOutMethod && !usesBlockFallback) {
    bool didDirectInvoke = false;
    if (!tryObjCNapiDispatch(env, cif, self, receiverIsClass, selectedSelector, selectedMethod,
                             selectedMethod->dispatchFlags, invocationArgs, rvalue,
                             &didDirectInvoke)) {
      return nullptr;
    }

    if (didDirectInvoke) {
      return toJSResult(rvalue);
    }
  }

  CifArgumentStorage argStorage(cif, 2);
  if (!argStorage.valid()) {
    napi_throw_error(env, "NativeScriptException",
                     "Unable to allocate argument storage for Objective-C call.");
    return nullptr;
  }

  void* avalues[cif->cif.nargs];
  avalues[0] = (void*)&self;
  avalues[1] = (void*)&selectedSelector;

  bool shouldFreeAny = false;
  bool shouldFree[cif->argc];
  memset(shouldFree, 0, sizeof(shouldFree));
  std::vector<id> fallbackBlocksToRelease;
  NSError* implicitNSError = nil;

  auto cleanupArguments = [&]() {
    for (id block : fallbackBlocksToRelease) {
      [block release];
    }
    fallbackBlocksToRelease.clear();

    if (!shouldFreeAny) {
      return;
    }
    for (unsigned int i = 0; i < cif->argc; i++) {
      if (shouldFree[i]) {
        cif->argTypes[i]->free(env, *reinterpret_cast<void**>(avalues[i + 2]));
      }
    }
  };

  if (cif->argc > 0) {
    for (unsigned int i = 0; i < cif->argc; i++) {
      avalues[i + 2] = argStorage.at(i);
      const char* blockEncoding = blockEncodingForSelector(selectedSelectorName, i);

      if (hasImplicitNSErrorOutArg && i == cif->argc - 1) {
        NSError** implicitNSErrorOutArg = &implicitNSError;
        *reinterpret_cast<void**>(avalues[i + 2]) = implicitNSErrorOutArg;
        continue;
      }

      bool convertedViaBlockFallback = false;
      if (blockEncoding != nullptr && cif->argTypes[i]->kind == mdTypeAnyObject) {
        napi_valuetype jsArgType = napi_undefined;
        if (napi_typeof(env, invocationArgs[i], &jsArgType) == napi_ok &&
            jsArgType == napi_function) {
          auto closure = new Closure(env, std::string(blockEncoding), true);
          id block = registerBlock(env, closure, invocationArgs[i]);
          *((void**)avalues[i + 2]) = (void*)block;
          fallbackBlocksToRelease.push_back(block);
          convertedViaBlockFallback = true;
        }
      }

      if (!convertedViaBlockFallback) {
        cif->argTypes[i]->toNative(env, invocationArgs[i], avalues[i + 2], &shouldFree[i],
                                   &shouldFreeAny);
        bool conversionFailed = false;
        if (ConsumeNapiArgumentConversionFailure(env) ||
            napi_is_exception_pending(env, &conversionFailed) != napi_ok || conversionFailed) {
          cleanupArguments();
          return nullptr;
        }
      }
    }
  }

  // NSLog(@"objcNativeCall: %p, %@", self, NSStringFromSelector(method->methodOrGetter.selector));

  if (!objcNativeCall(env, cif, self, receiverIsClass, selectedMethod,
                      selectedMethod->dispatchFlags, avalues, rvalue)) {
    cleanupArguments();
    return nullptr;
  }

  cleanupArguments();

  if (hasImplicitNSErrorOutArg && implicitNSError != nil) {
    const char* errorMessage = [[implicitNSError description] UTF8String];
    NativeScriptException nativeScriptException(errorMessage != nullptr ? errorMessage
                                                                        : "Unknown NSError");
    nativeScriptException.ReThrowToJS(env);
    return nullptr;
  }

  return toJSResult(rvalue);
}

napi_value ObjCClassMember::jsGetter(napi_env env, napi_callback_info cbinfo) {
  napi_value jsThis;
  ObjCClassMember* method = nullptr;

  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &jsThis, (void**)&method);

  return jsGetterDirect(env, method, jsThis);
}

napi_value ObjCClassMember::jsGetterDirect(napi_env env, ObjCClassMember* method,
                                           napi_value jsThis) {
  if (method == nullptr) {
    napi_throw_error(env, "NativeScriptException", "Missing Objective-C getter metadata.");
    return nullptr;
  }

  id self = assertSelf(env, jsThis, method);

  if (self == nullptr) {
    return nullptr;
  }

  const bool receiverIsClass = object_isClass(self);

  Cif* cif = method->cif;
  if (cif == nullptr) {
    cif = method->cif =
        method->bridgeState->getMethodCif(env, method->methodOrGetter.signatureOffset);
  }

  CifReturnStorage rvalueStorage(cif);
  if (!rvalueStorage.valid()) {
    napi_throw_error(env, "NativeScriptException",
                     "Unable to allocate return value storage for Objective-C getter call.");
    return nullptr;
  }

  void* avalues[2] = {&self, &method->methodOrGetter.selector};
  void* rvalue = rvalueStorage.get();

  bool didDirectInvoke = false;
  if (!tryObjCNapiDispatch(env, cif, self, receiverIsClass, method->methodOrGetter.selector,
                           &method->methodOrGetter, method->methodOrGetter.dispatchFlags, nullptr,
                           rvalue, &didDirectInvoke)) {
    return nullptr;
  }

  if (!didDirectInvoke) {
    // NSLog(@"objcNativeCall: %p, %@", self,
    // NSStringFromSelector(method->methodOrGetter.selector));

    if (!objcNativeCall(env, cif, self, receiverIsClass, &method->methodOrGetter,
                        method->methodOrGetter.dispatchFlags, avalues, rvalue)) {
      return nullptr;
    }
  }

  const char* selectorName = sel_getName(method->methodOrGetter.selector);
  if (strcmp(selectorName, "class") == 0) {
    id classObject = receiverIsClass ? self : (id)object_getClass(self);
    return method->bridgeState->getObject(env, classObject, kUnownedObject, 0, nullptr);
  }

  if (cif->returnType->kind == mdTypeInstanceObject) {
    napi_value constructor = jsThis;
    if (!receiverIsClass) {
      napi_get_named_property(env, jsThis, "constructor", &constructor);
    }
    id obj = *((id*)rvalue);
    if (obj != nil) {
      ObjCBridgeState* state = ObjCBridgeState::InstanceData(env);
      if (state != nullptr) {
        if (napi_value cached = state->findCachedObjectWrapper(env, obj); cached != nullptr) {
          return cached;
        }
      }
    }
    return method->bridgeState->getObject(env, obj, constructor,
                                          method->returnOwned ? kOwnedObject : kUnownedObject);
  }

  return cif->returnType->toJS(env, rvalue, 0);
}

napi_value ObjCClassMember::jsReadOnlySetter(napi_env env, napi_callback_info cbinfo) {
  return jsReadOnlySetterDirect(env);
}

napi_value ObjCClassMember::jsReadOnlySetterDirect(napi_env env) {
  napi_throw_error(env, nullptr, "Attempted to assign to readonly property.");
  return nullptr;
}

napi_value ObjCClassMember::jsSetter(napi_env env, napi_callback_info cbinfo) {
  napi_value jsThis, argv;
  size_t argc = 1;
  ObjCClassMember* method = nullptr;

  napi_get_cb_info(env, cbinfo, &argc, &argv, &jsThis, (void**)&method);

  return jsSetterDirect(env, method, jsThis, argv);
}

napi_value ObjCClassMember::jsSetterDirect(napi_env env, ObjCClassMember* method, napi_value jsThis,
                                           napi_value value) {
  if (method == nullptr) {
    napi_throw_error(env, "NativeScriptException", "Missing Objective-C setter metadata.");
    return nullptr;
  }

  id self = assertSelf(env, jsThis, method);

  if (self == nullptr) {
    return nullptr;
  }

  const bool receiverIsClass = object_isClass(self);

  Cif* cif = method->setterCif;
  if (cif == nullptr) {
    cif = method->setterCif =
        method->bridgeState->getMethodCif(env, method->setter.signatureOffset);
  }

  std::optional<RoundTripCacheFrameGuard> roundTripCacheFrame;
  if (generatedDispatchNeedsRoundTripCacheFrame(cif)) {
    roundTripCacheFrame.emplace(env, method->bridgeState);
  }

  if (cif->argc > 0) {
    cif->argv[0] = value;
  }

  bool didDirectInvoke = false;
  if (!tryObjCNapiDispatch(env, cif, self, receiverIsClass, method->setter.selector,
                           &method->setter, method->setter.dispatchFlags, cif->argv, nullptr,
                           &didDirectInvoke)) {
    return nullptr;
  }

  if (didDirectInvoke) {
    return nullptr;
  }

  CifArgumentStorage argStorage(cif, 2);
  if (!argStorage.valid()) {
    napi_throw_error(env, "NativeScriptException",
                     "Unable to allocate argument storage for Objective-C setter call.");
    return nullptr;
  }

  void* avalues[3] = {&self, &method->setter.selector, argStorage.at(0)};
  void* rvalue = nullptr;

  bool shouldFree = false;
  cif->argTypes[0]->toNative(env, value, avalues[2], &shouldFree, &shouldFree);
  if (ConsumeNapiArgumentConversionFailure(env)) {
    if (shouldFree) {
      cif->argTypes[0]->free(env, *((void**)avalues[2]));
    }
    return nullptr;
  }

  if (!objcNativeCall(env, cif, self, receiverIsClass, &method->setter,
                      method->setter.dispatchFlags, avalues, rvalue)) {
    return nullptr;
  }

  if (shouldFree) {
    cif->argTypes[0]->free(env, *((void**)avalues[2]));
  }

  return nullptr;
}

}  // namespace nativescript
