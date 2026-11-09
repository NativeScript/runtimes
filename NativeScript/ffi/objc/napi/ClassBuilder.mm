#include "ClassBuilder.h"
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <mutex>
#include "Closure.h"
#include "Interop.h"
#include "Metadata.h"
#include "ObjCBridge.h"
#include "Protocol.h"
#include "TypeConv.h"
#include "Util.h"
#include "js_native_api.h"
#include "node_api_util.h"

namespace nativescript {
namespace {
std::unordered_map<std::string, MethodDescriptor> gKnownExposedMethods;
std::mutex gClassBuilderEnvMutex;
std::unordered_map<Class, napi_env> gClassBuilderEnvs;

void registerClassBuilderEnv(Class nativeClass, napi_env env) {
  if (nativeClass == Nil || env == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(gClassBuilderEnvMutex);
  gClassBuilderEnvs[nativeClass] = env;
}

void unregisterClassBuilderEnv(Class nativeClass) {
  if (nativeClass == Nil) {
    return;
  }

  std::lock_guard<std::mutex> lock(gClassBuilderEnvMutex);
  gClassBuilderEnvs.erase(nativeClass);
}

napi_env resolveClassBuilderEnv(id self) {
  if (self == nil) {
    return nullptr;
  }

  Class currentClass = object_getClass(self);
  if (currentClass == Nil) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(gClassBuilderEnvMutex);
  while (currentClass != Nil) {
    auto find = gClassBuilderEnvs.find(currentClass);
    if (find != gClassBuilderEnvs.end()) {
      return find->second;
    }
    currentClass = class_getSuperclass(currentClass);
  }

  return nullptr;
}

const char* kInstallSuperAccessorSource = R"(
  (function (prototype, basePrototype) {
    const superCacheKey = Symbol.for("__ns_super_proxy__");
    Object.defineProperty(prototype, "super", {
      configurable: true,
      enumerable: false,
      get: function () {
        const self = this;
        if (self == null) {
          return undefined;
        }

        const existing = self[superCacheKey];
        if (existing && existing.base === basePrototype) {
          return existing.proxy;
        }

        const proxy = new Proxy(basePrototype, {
          get(target, key) {
            let current = target;
            while (current != null) {
              const descriptor = Object.getOwnPropertyDescriptor(current, key);
              if (descriptor) {
                if (typeof descriptor.get === "function") {
                  return descriptor.get.call(self);
                }
                const value = descriptor.value;
                if (typeof value === "function") {
                  return value.bind(self);
                }
                return value;
              }
              current = Object.getPrototypeOf(current);
            }
            return undefined;
          },
          set(target, key, value) {
            let current = target;
            while (current != null) {
              const descriptor = Object.getOwnPropertyDescriptor(current, key);
              if (descriptor) {
                if (typeof descriptor.set === "function") {
                  descriptor.set.call(self, value);
                  return true;
                }
                if ("value" in descriptor && descriptor.writable) {
                  self[key] = value;
                  return true;
                }
                return false;
              }
              current = Object.getPrototypeOf(current);
            }
            self[key] = value;
            return true;
          }
        });

        Object.defineProperty(self, superCacheKey, {
          configurable: true,
          enumerable: false,
          writable: true,
          value: { base: basePrototype, proxy }
        });

        return proxy;
      }
    });
  })
	)";

const char* kInstallClassFromStringAliasSource = R"(
  (function (global) {
    if (global.__nsClassFromStringAliasInstalled === true) {
      return;
    }

    const original = global.NSClassFromString;
    if (typeof original !== "function") {
      return;
    }

    Object.defineProperty(global, "__nsOriginalNSClassFromString", {
      configurable: true,
      enumerable: false,
      writable: false,
      value: original
    });

    Object.defineProperty(global, "NSClassFromString", {
      configurable: true,
      enumerable: true,
      writable: true,
      value: function (name) {
        const cls = original.call(this, name);
        if (cls != null) {
          try {
            const registry = global.__nsConstructorsByObjCClassName;
            const stringFromClass = global.NSStringFromClass;
            if (registry != null && typeof stringFromClass === "function") {
              const runtimeName = stringFromClass(cls);
              const constructor = registry[runtimeName];
              if (constructor != null) {
                return constructor;
              }
            }
          } catch (_) {
          }
        }

        return cls;
      }
    });

    Object.defineProperty(global, "__nsClassFromStringAliasInstalled", {
      configurable: true,
      enumerable: false,
      writable: false,
      value: true
    });
  })
)";

const char* NSFastEnumerationMethodEncoding() {
  static const char* encoding = nullptr;
  if (encoding == nullptr) {
    struct objc_method_description desc = protocol_getMethodDescription(
        @protocol(NSFastEnumeration), @selector(countByEnumeratingWithState:objects:count:), YES,
        YES);
    encoding = desc.types;
  }
  return encoding;
}

void clearPendingException(napi_env env) {
  napi_value ignored = nullptr;
  napi_get_and_clear_last_exception(env, &ignored);
}

Protocol* resolveRuntimeProtocol(napi_env env, napi_value protocolValue) {
  if (protocolValue == nullptr) {
    return nullptr;
  }

  ObjCProtocol* wrappedProtocol = nullptr;
  if (napi_unwrap(env, protocolValue, (void**)&wrappedProtocol) == napi_ok &&
      wrappedProtocol != nullptr) {
    Protocol* runtimeProtocol = objc_getProtocol(wrappedProtocol->name.c_str());
    if (runtimeProtocol != nullptr) {
      return runtimeProtocol;
    }
  }

  if (Pointer::isInstance(env, protocolValue)) {
    Pointer* pointer = Pointer::unwrap(env, protocolValue);
    if (pointer != nullptr && pointer->data != nullptr) {
      Protocol* runtimeProtocol = (Protocol*)pointer->data;
      if (protocol_getName(runtimeProtocol) != nullptr) {
        return runtimeProtocol;
      }
    }
  }

  auto protocolFromName = [](const std::string& protocolName) -> Protocol* {
    if (protocolName.empty()) {
      return nullptr;
    }

    Protocol* runtimeProtocol = objc_getProtocol(protocolName.c_str());
    if (runtimeProtocol != nullptr) {
      return runtimeProtocol;
    }

    const std::string suffix = "Protocol";
    if (protocolName.length() > suffix.length() &&
        protocolName.rfind(suffix) == protocolName.length() - suffix.length()) {
      std::string strippedName = protocolName.substr(0, protocolName.length() - suffix.length());
      return objc_getProtocol(strippedName.c_str());
    }

    return nullptr;
  };

  napi_valuetype valueType = napi_undefined;
  napi_typeof(env, protocolValue, &valueType);
  if (valueType == napi_string) {
    char protocolNameBuffer[512];
    size_t protocolNameLength = 0;
    napi_get_value_string_utf8(env, protocolValue, protocolNameBuffer, sizeof(protocolNameBuffer),
                               &protocolNameLength);
    return protocolFromName(std::string(protocolNameBuffer, protocolNameLength));
  }

  if (valueType == napi_object || valueType == napi_function) {
    bool hasName = false;
    napi_has_named_property(env, protocolValue, "name", &hasName);
    if (hasName) {
      napi_value protocolNameValue = nullptr;
      napi_get_named_property(env, protocolValue, "name", &protocolNameValue);
      napi_valuetype protocolNameType = napi_undefined;
      napi_typeof(env, protocolNameValue, &protocolNameType);
      if (protocolNameType == napi_string) {
        char protocolNameBuffer[512];
        size_t protocolNameLength = 0;
        napi_get_value_string_utf8(env, protocolNameValue, protocolNameBuffer,
                                   sizeof(protocolNameBuffer), &protocolNameLength);
        return protocolFromName(std::string(protocolNameBuffer, protocolNameLength));
      }
    }
  }

  return nullptr;
}

napi_value JS_classConformsToProtocolSafe(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value jsThis = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &jsThis, nullptr);

  Class nativeClass = nullptr;
  napi_unwrap(env, jsThis, (void**)&nativeClass);

  bool conforms = false;
  if (nativeClass != nullptr && argc > 0) {
    Protocol* runtimeProtocol = resolveRuntimeProtocol(env, argv[0]);
    if (runtimeProtocol != nullptr) {
      conforms = class_conformsToProtocol(nativeClass, runtimeProtocol);
    }
  }

  napi_value result = nullptr;
  napi_get_boolean(env, conforms, &result);
  return result;
}

NSUInteger JS_SymbolIteratorCountByEnumerating(id self, SEL _cmd, NSFastEnumerationState* state,
                                               id __unsafe_unretained stackbuf[], NSUInteger len) {
  if (len == 0) {
    return 0;
  }

  napi_env env = resolveClassBuilderEnv(self);
  if (env == nullptr) {
    return 0;
  }

  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr) {
    return 0;
  }

  napi_value jsSelf = bridgeState->getObject(env, self, kUnownedObject);
  if (jsSelf == nullptr) {
    return 0;
  }

  napi_value global = nullptr;
  napi_value symbolCtor = nullptr;
  napi_value symbolIterator = nullptr;
  if (napi_get_global(env, &global) != napi_ok ||
      napi_get_named_property(env, global, "Symbol", &symbolCtor) != napi_ok ||
      napi_get_named_property(env, symbolCtor, "iterator", &symbolIterator) != napi_ok) {
    clearPendingException(env);
    return 0;
  }

  napi_value iteratorMethod = nullptr;
  if (napi_get_property(env, jsSelf, symbolIterator, &iteratorMethod) != napi_ok) {
    clearPendingException(env);
    return 0;
  }

  napi_valuetype iteratorMethodType = napi_undefined;
  if (napi_typeof(env, iteratorMethod, &iteratorMethodType) != napi_ok ||
      iteratorMethodType != napi_function) {
    return 0;
  }

  napi_value iterator = nullptr;
  if (napi_call_function(env, jsSelf, iteratorMethod, 0, nullptr, &iterator) != napi_ok) {
    clearPendingException(env);
    return 0;
  }

  napi_valuetype iteratorType = napi_undefined;
  if (napi_typeof(env, iterator, &iteratorType) != napi_ok ||
      (iteratorType != napi_object && iteratorType != napi_function)) {
    return 0;
  }

  napi_value nextMethod = nullptr;
  if (napi_get_named_property(env, iterator, "next", &nextMethod) != napi_ok) {
    clearPendingException(env);
    return 0;
  }

  napi_valuetype nextType = napi_undefined;
  if (napi_typeof(env, nextMethod, &nextType) != napi_ok || nextType != napi_function) {
    return 0;
  }

  auto callNext = [&](napi_value* nextResult) -> bool {
    if (napi_call_function(env, iterator, nextMethod, 0, nullptr, nextResult) != napi_ok) {
      clearPendingException(env);
      return false;
    }
    return true;
  };

  auto readDoneFlag = [&](napi_value nextResult, bool* done) -> bool {
    napi_value doneValue = nullptr;
    if (napi_get_named_property(env, nextResult, "done", &doneValue) != napi_ok) {
      clearPendingException(env);
      return false;
    }

    bool isDone = false;
    if (napi_get_value_bool(env, doneValue, &isDone) != napi_ok) {
      clearPendingException(env);
      return false;
    }

    *done = isDone;
    return true;
  };

  // Rebuild the iterator from the beginning and skip already yielded values.
  // This keeps state handling simple and avoids retaining JS iterators in ObjC.
  for (unsigned long skipped = 0; skipped < state->state; skipped++) {
    napi_value skippedResult = nullptr;
    if (!callNext(&skippedResult)) {
      return 0;
    }

    bool done = false;
    if (!readDoneFlag(skippedResult, &done) || done) {
      return 0;
    }
  }

  const char* objectEncoding = "@";
  auto objectConverter = TypeConv::Make(env, &objectEncoding);

  NSUInteger count = 0;
  while (count < len) {
    napi_value nextResult = nullptr;
    if (!callNext(&nextResult)) {
      break;
    }

    bool done = false;
    if (!readDoneFlag(nextResult, &done)) {
      break;
    }

    if (done) {
      break;
    }

    napi_value value = nullptr;
    if (napi_get_named_property(env, nextResult, "value", &value) != napi_ok) {
      clearPendingException(env);
      break;
    }

    id nativeValue = nil;
    bool shouldFree = false;
    bool shouldFreeAny = false;
    objectConverter->toNative(env, value, &nativeValue, &shouldFree, &shouldFreeAny);
    if (ConsumeNapiArgumentConversionFailure(env)) {
      break;
    }
    stackbuf[count++] = nativeValue;
  }

  state->itemsPtr = stackbuf;
  state->mutationsPtr = &state->extra[0];
  state->extra[0] = 0;
  state->state += count;

  return count;
}
}  // namespace

ClassBuilder::ClassBuilder(napi_env env, napi_value constructor, Class explicitSuperClass) {
  this->env = env;
  bridgeState = ObjCBridgeState::InstanceData(env);

  metadataOffset = MD_SECTION_OFFSET_NULL;

  Class superClassNative = explicitSuperClass;
  if (superClassNative == nullptr) {
    napi_value superConstructor;
    napi_get_prototype(env, constructor, &superConstructor);
    napi_unwrap(env, superConstructor, (void**)&superClassNative);
  }

  if (superClassNative == nullptr) {
    // If the class does not inherit from a native class,
    // by default it inherits from NSObject.
    superClassNative = [NSObject class];
  }

  auto superClassIt = bridgeState->classesByPointer.find(superClassNative);
  if (superClassIt != bridgeState->classesByPointer.end()) {
    superclass = superClassIt->second;
  } else {
    auto mdClassIt = bridgeState->mdClassesByPointer.find(superClassNative);
    if (mdClassIt != bridgeState->mdClassesByPointer.end()) {
      superclass = bridgeState->getClass(env, mdClassIt->second);
    } else {
      superclass = nullptr;
    }
  }

  napi_value className;
  bool hasNativeClassName = false;
  napi_has_named_property(env, constructor, "ObjCClassName", &hasNativeClassName);
  if (hasNativeClassName) {
    napi_get_named_property(env, constructor, "ObjCClassName", &className);
  } else {
    napi_get_named_property(env, constructor, "name", &className);
  }
  static char classNameBuf[512];
  napi_get_value_string_utf8(env, className, classNameBuf, 512, nullptr);

  name = classNameBuf;
  if (objc_lookUpClass(name.c_str()) != nullptr) {
    std::string baseName = name;
    do {
      name = baseName + "_" + std::to_string(rand());
    } while (objc_lookUpClass(name.c_str()) != nullptr);
  }
  nativeClass = objc_allocateClassPair(superClassNative, name.c_str(), 0);

  if (nativeClass == nullptr) {
    napi_throw_error(env, nullptr, "Failed to allocate class");
    return;
  }

  class_addProtocol(nativeClass, @protocol(ObjCBridgeClassBuilderProtocol));

  objc_registerClassPair(nativeClass);
  registerClassBuilderEnv(nativeClass, env);

  napi_remove_wrap(env, constructor, nullptr);

  napi_wrap(env, constructor, (void*)nativeClass, nullptr, nullptr, nullptr);

  napi_value prototype;
  napi_get_named_property(env, constructor, "prototype", &prototype);

  this->constructor = make_ref(env, constructor);
  this->prototype = make_ref(env, prototype);
}

ClassBuilder::~ClassBuilder() {
  unregisterClassBuilderEnv(nativeClass);

  if (nativeClass != nullptr) {
    objc_disposeClassPair(nativeClass);
    napi_delete_reference(env, constructor);
    napi_delete_reference(env, prototype);
  }
}

void ClassBuilder::addProtocol(ObjCProtocol* protocol) {
  if (!protocols.contains(protocol)) {
    protocols.emplace(protocol);
    // Not always there is a `Protocol` object for the protocol, so we
    // need to check if it exists first.
    Protocol* proto = objc_getProtocol(protocol->name.c_str());
    if (proto != nullptr) {
      class_addProtocol(nativeClass, proto);
    }
  }
}

std::vector<MethodDescriptor*> ClassBuilder::lookupMethodDescriptors(std::string& name,
                                                                     bool setter) {
  std::vector<MethodDescriptor*> descriptors;
  std::unordered_set<SEL> selectors;

  auto appendDescriptor = [&](MethodDescriptor* descriptor) {
    if (descriptor == nullptr || descriptor->selector == nullptr) {
      return;
    }
    if (!selectors.insert(descriptor->selector).second) {
      return;
    }
    descriptors.emplace_back(descriptor);
  };

  // 1. First we look up if there was a custom definition for the method
  // in ObjCExposedMethods static of the custom class.
  if (!setter) {
    auto findExposedMethod = exposedMethods.find(name);
    if (findExposedMethod != exposedMethods.end()) {
      appendDescriptor(&findExposedMethod->second);
      return descriptors;
    }

    auto findKnownExposedMethod = gKnownExposedMethods.find(name);
    if (findKnownExposedMethod != gKnownExposedMethods.end()) {
      appendDescriptor(&findKnownExposedMethod->second);
      return descriptors;
    }
  }

  auto appendMemberDescriptors = [&](ObjCClassMember& member) {
    if (setter) {
      if (member.methodOrGetter.isProperty && member.setter.selector != nullptr) {
        appendDescriptor(&member.setter);
      }
      return;
    }

    appendDescriptor(&member.methodOrGetter);
    for (auto& overload : member.overloads) {
      appendDescriptor(&overload.method);
    }
  };

  // 2. Then walk through the class hierarchy and see if we can find the
  // method in the superclass chain.
  ObjCClass* currentClass = superclass;
  while (currentClass != nullptr) {
    // JS extend() overrides target instance members; fall back to a static
    // twin only when no instance member exists under this name.
    auto findMethod = currentClass->members.find({name, false});
    if (findMethod == currentClass->members.end()) {
      findMethod = currentClass->members.find({name, true});
    }
    if (findMethod != currentClass->members.end()) {
      appendMemberDescriptors(findMethod->second);
      if (!descriptors.empty()) {
        return descriptors;
      }
    }
    currentClass = currentClass->superclass;
  }

  // 3. And finally, look into all protocols implemented (directly or
  // indirectly) and try to find the method there.
  std::function<void(std::unordered_set<ObjCProtocol*>&)> processProtocols =
      [&](std::unordered_set<ObjCProtocol*>& protocols) {
        for (auto protocol : protocols) {
          auto findMethod = protocol->members.find({name, false});
          if (findMethod == protocol->members.end()) {
            findMethod = protocol->members.find({name, true});
          }
          if (findMethod != protocol->members.end()) {
            appendMemberDescriptors(findMethod->second);
          }
          processProtocols(protocol->protocols);
        }
      };

  processProtocols(protocols);
  return descriptors;
}

MethodDescriptor* ClassBuilder::lookupMethodDescriptor(std::string& name, bool setter) {
  auto descriptors = lookupMethodDescriptors(name, setter);
  return descriptors.empty() ? nullptr : descriptors.front();
}

void ClassBuilder::addMethod(std::string& name, MethodDescriptor* desc, napi_value key,
                             napi_value func) {
  switch (desc->kind) {
    case kMethodDescEncoding: {
      const char* encoding = desc->encoding.c_str();
      auto closure = new Closure(env, encoding, false, true);
      if (func != nullptr)
        closure->func = make_ref(env, func);
      else
        closure->propertyName = name;
      closure->selector = desc->selector;
      closure->thisConstructor = constructor;
      class_replaceMethod(nativeClass, desc->selector, (IMP)closure->fnptr, encoding);
      break;
    }

    case kMethodDescSignatureOffset: {
      std::string encoding;
      auto closure = new Closure(env, bridgeState->metadata, desc->signatureOffset, false,
                                 &encoding, true, desc->isProperty);
      if (func != nullptr)
        closure->func = make_ref(env, func);
      else
        closure->propertyName = name;
      closure->selector = desc->selector;
      closure->thisConstructor = constructor;
      class_replaceMethod(nativeClass, desc->selector, (IMP)closure->fnptr, encoding.c_str());
      break;
    }
  }
}

void ClassBuilder::build() {
  if (isFinal) return;

  isFinal = true;

  napi_value constructor = get_ref_value(env, this->constructor),
             prototype = get_ref_value(env, this->prototype);

  // 1. Extract method definitions from ObjCExposedMethods static

  napi_value exposedMethods, exposedMethodNames;
  bool hasExposedMethods = false;
  napi_has_named_property(env, constructor, "ObjCExposedMethods", &hasExposedMethods);

  if (hasExposedMethods) {
    napi_get_named_property(env, constructor, "ObjCExposedMethods", &exposedMethods);

    napi_get_all_property_names(env, exposedMethods, napi_key_own_only, napi_key_skip_symbols,
                                napi_key_numbers_to_strings, &exposedMethodNames);

    uint32_t exposedMethodCount = 0;
    napi_get_array_length(env, exposedMethodNames, &exposedMethodCount);

    for (uint32_t i = 0; i < exposedMethodCount; i++) {
      napi_value exposedMethodName;
      napi_get_element(env, exposedMethodNames, i, &exposedMethodName);
      static char exposedMethodNameBuf[512];
      napi_get_value_string_utf8(env, exposedMethodName, exposedMethodNameBuf, 512, nullptr);
      std::string name = exposedMethodNameBuf;
      SEL selector = sel_registerName(name.c_str());
      std::string encoding;

      napi_value def, params = nullptr, returns;
      napi_get_named_property(env, exposedMethods, name.c_str(), &def);
      napi_get_named_property(env, def, "returns", &returns);

      uint32_t paramCount = 0;
      bool hasParams = false;
      napi_has_named_property(env, def, "params", &hasParams);
      if (hasParams) {
        napi_get_named_property(env, def, "params", &params);
        napi_valuetype paramsType = napi_undefined;
        napi_typeof(env, params, &paramsType);
        if (paramsType == napi_object) {
          bool isArray = false;
          napi_is_array(env, params, &isArray);
          if (isArray) {
            napi_get_array_length(env, params, &paramCount);
          } else {
            napi_throw_type_error(env, nullptr,
                                  "ObjCExposedMethods params must be an array when provided");
            return;
          }
        } else if (paramsType != napi_undefined && paramsType != napi_null) {
          napi_throw_type_error(env, nullptr,
                                "ObjCExposedMethods params must be an array when provided");
          return;
        }
      }

      encoding += getEncodedType(env, returns);
      encoding += "@:";

      for (uint32_t j = 0; j < paramCount; j++) {
        napi_value param;
        napi_get_element(env, params, j, &param);
        std::string enctype = getEncodedType(env, param);
        if (enctype == "v") {
          napi_throw_error(env, nullptr, "Void type not allowed in method params");
          return;
        }
        encoding += enctype;
      }

      auto descriptor = MethodDescriptor(selector, encoding);
      this->exposedMethods[name] = descriptor;
      gKnownExposedMethods[name] = descriptor;
      gKnownExposedMethods[jsifySelector(name)] = descriptor;
    }
  }

  // 2. Extract the protocol references from ObjCProtocols static

  napi_value protocols, protocol;
  bool hasProtocols = false;
  napi_has_named_property(env, constructor, "ObjCProtocols", &hasProtocols);

  if (hasProtocols) {
    napi_get_named_property(env, constructor, "ObjCProtocols", &protocols);

    uint32_t i = 0;
    uint32_t length = 0;
    napi_get_array_length(env, protocols, &length);

    while (i < length) {
      napi_get_element(env, protocols, i, &protocol);
      static char protocolNameBuf[512];
      napi_get_value_string_utf8(env, protocol, protocolNameBuf, 512, nullptr);
      ObjCProtocol* proto = nullptr;
      napi_unwrap(env, protocol, (void**)&proto);
      if (proto != nullptr) addProtocol(proto);
      i++;
    }
  }

  // 3. Walk over methods defined on the prototype and add them to the ObjC
  // class if we can find the corresponding method descriptor.

  napi_value properties;

  napi_get_all_property_names(env, prototype, napi_key_own_only, napi_key_skip_symbols,
                              napi_key_numbers_to_strings, &properties);

  uint32_t propertyCount = 0;
  napi_get_array_length(env, properties, &propertyCount);

  napi_value global, objectCtor, getOwnPropertyDescriptor;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Object", &objectCtor);
  napi_get_named_property(env, objectCtor, "getOwnPropertyDescriptor", &getOwnPropertyDescriptor);

  bool hasIteratorMethod = false;
  napi_value symbolCtor = nullptr;
  napi_value symbolIterator = nullptr;
  napi_get_named_property(env, global, "Symbol", &symbolCtor);
  napi_get_named_property(env, symbolCtor, "iterator", &symbolIterator);
  napi_has_own_property(env, prototype, symbolIterator, &hasIteratorMethod);
  if (hasIteratorMethod) {
    class_addProtocol(nativeClass, @protocol(NSFastEnumeration));
    const char* fastEnumerationEncoding = NSFastEnumerationMethodEncoding();
    if (fastEnumerationEncoding != nullptr) {
      class_replaceMethod(nativeClass, @selector(countByEnumeratingWithState:objects:count:),
                          (IMP)JS_SymbolIteratorCountByEnumerating, fastEnumerationEncoding);
    }
  }

  uint32_t i = 0;
  while (i < propertyCount) {
    napi_value property;
    napi_get_element(env, properties, i, &property);
    static char propertyNameBuf[512];
    napi_get_value_string_utf8(env, property, propertyNameBuf, 512, nullptr);
    std::string name = propertyNameBuf;
    napi_value descriptorArgs[] = {prototype, property};
    napi_value propertyDescriptor;
    napi_call_function(env, objectCtor, getOwnPropertyDescriptor, 2, descriptorArgs,
                       &propertyDescriptor);

    napi_valuetype descriptorType;
    napi_typeof(env, propertyDescriptor, &descriptorType);
    if (descriptorType == napi_undefined || descriptorType == napi_null) {
      i++;
      continue;
    }

    bool hasValue = false;
    napi_has_named_property(env, propertyDescriptor, "value", &hasValue);
    if (hasValue) {
      napi_value methodFunc;
      napi_get_named_property(env, propertyDescriptor, "value", &methodFunc);
      napi_valuetype funcType = napi_undefined;
      napi_typeof(env, methodFunc, &funcType);
      if (funcType == napi_function) {
        auto descriptors = lookupMethodDescriptors(name);
        for (MethodDescriptor* descriptor : descriptors) {
          if (descriptor != nullptr) {
            addMethod(name, descriptor, property, methodFunc);
          }
        }
      }
    }

    bool hasGetter = false;
    napi_has_named_property(env, propertyDescriptor, "get", &hasGetter);
    if (hasGetter) {
      napi_value getterFunc;
      napi_get_named_property(env, propertyDescriptor, "get", &getterFunc);
      napi_valuetype getterType = napi_undefined;
      napi_typeof(env, getterFunc, &getterType);
      if (getterType == napi_function) {
        auto getterDescs = lookupMethodDescriptors(name);
        for (MethodDescriptor* getterDesc : getterDescs) {
          if (getterDesc != nullptr) {
            addMethod(name, getterDesc, property, getterFunc);
          }
        }
      }
    }

    bool hasSetter = false;
    napi_has_named_property(env, propertyDescriptor, "set", &hasSetter);
    if (hasSetter) {
      napi_value setterFunc;
      napi_get_named_property(env, propertyDescriptor, "set", &setterFunc);
      napi_valuetype setterType = napi_undefined;
      napi_typeof(env, setterFunc, &setterType);
      if (setterType == napi_function) {
        auto setterDescs = lookupMethodDescriptors(name, true);
        for (MethodDescriptor* setterDesc : setterDescs) {
          if (setterDesc != nullptr) {
            addMethod(name, setterDesc, property, setterFunc);
          }
        }
      }
    }
    i++;
  }
}

// Static method to extend native classes with JavaScript-defined methods
napi_value ClassBuilder::ExtendCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_value thisArg;

  napi_get_cb_info(env, info, &argc, args, &thisArg, nullptr);

  if (argc < 1) {
    napi_throw_error(env, nullptr,
                     "extend() requires at least one parameter with method definitions");
    return nullptr;
  }

  // Validate that the first argument is an object
  napi_valuetype argType;
  napi_typeof(env, args[0], &argType);
  if (argType != napi_object) {
    napi_throw_error(env, nullptr, "extend() first parameter must be an object");
    return nullptr;
  }

  // Get the native class from 'this' (the constructor function)
  Class baseNativeClass = nullptr;
  auto bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState != nullptr) {
    bridgeState->tryResolveBridgedClassConstructor(env, thisArg, &baseNativeClass);
  }
  if (baseNativeClass == nullptr) {
    napi_unwrap(env, thisArg, (void**)&baseNativeClass);
  }

  if (baseNativeClass == nullptr) {
    napi_throw_error(env, nullptr, "extend() can only be called on native class constructors");
    return nullptr;
  }

  if (class_conformsToProtocol(baseNativeClass, @protocol(ObjCBridgeClassBuilderProtocol))) {
    napi_throw_error(env, nullptr, "Cannot extend an already extended class.");
    return nullptr;
  }

  napi_value options = nullptr;
  bool hasOptionsObject = false;
  if (argc >= 2) {
    napi_valuetype secondArgType;
    napi_typeof(env, args[1], &secondArgType);
    hasOptionsObject = secondArgType == napi_object;
    if (hasOptionsObject) {
      options = args[1];
    }
  }

  // Create a class name.
  napi_value baseClassName;
  napi_get_named_property(env, thisArg, "name", &baseClassName);
  static char baseClassNameBuf[512];
  napi_get_value_string_utf8(env, baseClassName, baseClassNameBuf, 512, nullptr);

  std::string requestedName;
  if (hasOptionsObject) {
    bool hasCustomName = false;
    napi_has_named_property(env, options, "name", &hasCustomName);
    if (hasCustomName) {
      napi_value customNameValue = nullptr;
      napi_get_named_property(env, options, "name", &customNameValue);
      napi_valuetype customNameType = napi_undefined;
      napi_typeof(env, customNameValue, &customNameType);
      if (customNameType == napi_string) {
        static char customNameBuf[512];
        size_t customNameLen = 0;
        napi_get_value_string_utf8(env, customNameValue, customNameBuf, sizeof(customNameBuf),
                                   &customNameLen);
        if (customNameLen > 0) {
          requestedName.assign(customNameBuf, customNameLen);
        }
      }
    }
  }

  std::string newClassName;
  bool shouldAliasRequestedName = false;
  if (!requestedName.empty()) {
    newClassName = requestedName;
    Class existingClass = objc_lookUpClass(newClassName.c_str());
    auto nextAvailableClassName = [](const std::string& baseName) {
      size_t suffix = 1;
      std::string candidate;
      do {
        candidate = baseName + std::to_string(suffix++);
      } while (objc_lookUpClass(candidate.c_str()) != nullptr);
      return candidate;
    };
    if (existingClass != nullptr) {
      newClassName = nextAvailableClassName(requestedName);
      shouldAliasRequestedName =
          !class_conformsToProtocol(existingClass, @protocol(ObjCBridgeClassBuilderProtocol));
    }
  } else {
    newClassName = baseClassNameBuf;
    newClassName += "_Extended_";
    newClassName += std::to_string(rand());
  }
  // Create the new constructor function that extends the base
  napi_value newConstructor;
  napi_define_class(env, newClassName.c_str(), newClassName.length(), JS_BridgedConstructor,
                    nullptr, 0, nullptr, &newConstructor);

  // Set up JavaScript inheritance from the base class
  napi_inherits(env, newConstructor, thisArg);

  napi_value safeConformsToProtocol = nullptr;
  napi_create_function(env, "conformsToProtocol", NAPI_AUTO_LENGTH, JS_classConformsToProtocolSafe,
                       nullptr, &safeConformsToProtocol);
  napi_set_named_property(env, newConstructor, "conformsToProtocol", safeConformsToProtocol);

  // Get prototype for adding methods
  napi_value newPrototype, basePrototype;
  napi_get_named_property(env, newConstructor, "prototype", &newPrototype);
  napi_get_named_property(env, thisArg, "prototype", &basePrototype);

  // Copy methods and accessors preserving descriptors.
  napi_value global, objectCtor, getOwnPropertyDescriptors, defineProperties;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Object", &objectCtor);
  napi_get_named_property(env, objectCtor, "getOwnPropertyDescriptors", &getOwnPropertyDescriptors);
  napi_get_named_property(env, objectCtor, "defineProperties", &defineProperties);

  napi_value descriptors;
  napi_call_function(env, objectCtor, getOwnPropertyDescriptors, 1, args, &descriptors);

  napi_value defineArgs[] = {newPrototype, descriptors};
  napi_call_function(env, objectCtor, defineProperties, 2, defineArgs, nullptr);

  napi_value installSuperAccessor = nullptr;
  napi_value installSuperScript = nullptr;
  napi_create_string_utf8(env, kInstallSuperAccessorSource, NAPI_AUTO_LENGTH, &installSuperScript);
  napi_run_script(env, installSuperScript, &installSuperAccessor);
  napi_value superArgs[] = {newPrototype, basePrototype};
  napi_call_function(env, global, installSuperAccessor, 2, superArgs, nullptr);

  // Handle optional second parameter for metadata/options
  if (hasOptionsObject) {
    napi_value exposedMethods;
    bool hasExposedMethods = false;
    napi_has_named_property(env, options, "exposedMethods", &hasExposedMethods);

    if (hasExposedMethods) {
      napi_get_named_property(env, options, "exposedMethods", &exposedMethods);
      napi_set_named_property(env, newConstructor, "ObjCExposedMethods", exposedMethods);
    }

    napi_value protocols;
    bool hasProtocols = false;
    napi_has_named_property(env, options, "protocols", &hasProtocols);

    if (hasProtocols) {
      napi_get_named_property(env, options, "protocols", &protocols);
      napi_set_named_property(env, newConstructor, "ObjCProtocols", protocols);
    }
  }

  napi_value classNameValue = nullptr;
  napi_create_string_utf8(env, newClassName.c_str(), newClassName.length(), &classNameValue);
  napi_set_named_property(env, newConstructor, "ObjCClassName", classNameValue);

  napi_value registryGlobal = nullptr;
  napi_value classRegistry = nullptr;
  bool hasClassRegistry = false;
  napi_get_global(env, &registryGlobal);
  napi_has_named_property(env, registryGlobal, "__nsConstructorsByObjCClassName",
                          &hasClassRegistry);
  if (!hasClassRegistry) {
    napi_create_object(env, &classRegistry);
    napi_set_named_property(env, registryGlobal, "__nsConstructorsByObjCClassName", classRegistry);
  } else {
    napi_get_named_property(env, registryGlobal, "__nsConstructorsByObjCClassName", &classRegistry);
  }

  napi_value installClassFromStringAliasScript = nullptr;
  napi_value installClassFromStringAlias = nullptr;
  if (napi_create_string_utf8(env, kInstallClassFromStringAliasSource, NAPI_AUTO_LENGTH,
                              &installClassFromStringAliasScript) == napi_ok &&
      napi_run_script(env, installClassFromStringAliasScript, &installClassFromStringAlias) ==
          napi_ok &&
      installClassFromStringAlias != nullptr) {
    napi_value aliasArgs[] = {registryGlobal};
    if (napi_call_function(env, registryGlobal, installClassFromStringAlias, 1, aliasArgs,
                           nullptr) != napi_ok) {
      clearPendingException(env);
    }
  } else {
    clearPendingException(env);
  }

  if (classRegistry != nullptr) {
    napi_set_named_property(env, classRegistry, newClassName.c_str(), newConstructor);
    if (shouldAliasRequestedName && !requestedName.empty()) {
      napi_set_named_property(env, classRegistry, requestedName.c_str(), newConstructor);
    }
  }

  // Use ClassBuilder to create the native class and bridge the methods
  ClassBuilder* builder = new ClassBuilder(env, newConstructor, baseNativeClass);
  builder->build();

  // Register the builder in the bridge state
  bridgeState = ObjCBridgeState::InstanceData(env);
  bridgeState->registerRuntimeClass(builder, builder->nativeClass);

  return newConstructor;
}

}  // namespace nativescript
