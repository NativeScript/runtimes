#include "Interop.h"
#include "ClassBuilder.h"
#include "Metadata.h"
#include "ObjCBridge.h"
#include "Util.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "node_api_util.h"

#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>

namespace nativescript {

namespace {
constexpr const char* kPointerMarker = "__ns_pointer";
constexpr const char* kNativePointerProperty = "__ns_native_ptr";
constexpr const char* kReferenceMarker = "__ns_reference";
constexpr const char* kFunctionReferenceMarker = "__ns_function_reference";
constexpr const char* kFunctionReferenceDataProperty = "__ns_function_reference_data";
constexpr const char* kReferenceInitValueSymbolKey = "__ns_reference_init_value";

inline uintptr_t pointerKey(void* data) { return reinterpret_cast<uintptr_t>(data); }

inline napi_value referenceInitValueSymbol(napi_env env) {
  return jsSymbolFor(env, kReferenceInitValueSymbolKey);
}

inline napi_value getReferenceInitValueProperty(napi_env env, napi_value value) {
  if (value == nullptr) {
    return nullptr;
  }

  napi_value initValue = nullptr;
  napi_get_property(env, value, referenceInitValueSymbol(env), &initValue);
  return initValue;
}

inline void setReferenceInitValueProperty(napi_env env, napi_value value, napi_value initValue) {
  if (value == nullptr || initValue == nullptr) {
    return;
  }

  napi_set_property(env, value, referenceInitValueSymbol(env), initValue);
}

inline void clearReferenceInitValueProperty(napi_env env, napi_value value) {
  if (value == nullptr) {
    return;
  }

  bool deleted = false;
  napi_delete_property(env, value, referenceInitValueSymbol(env), &deleted);
}

inline bool isInteropTypeCode(int32_t value) {
  switch (value) {
    case mdTypeVoid:
    case mdTypeBool:
    case mdTypeChar:
    case mdTypeUInt8:
    case mdTypeSShort:
    case mdTypeUShort:
    case mdTypeUnichar:
    case mdTypeSInt:
    case mdTypeUInt:
    case mdTypeSInt64:
    case mdTypeUInt64:
    case mdTypeFloat:
    case mdTypeDouble:
    case mdTypeString:
    case mdTypeAnyObject:
    case mdTypePointer:
    case mdTypeSelector:
      return true;
    default:
      return false;
  }
}

inline size_t referenceElementSize(Reference* ref) {
  if (ref == nullptr || ref->type == nullptr || ref->type->type == nullptr ||
      ref->type->type->size == 0) {
    return sizeof(void*);
  }

  return ref->type->type->size;
}

inline bool isArrayIndexProperty(napi_env env, napi_value property, uint32_t* index) {
  if (index == nullptr) {
    return false;
  }

  napi_valuetype propertyType = napi_undefined;
  napi_typeof(env, property, &propertyType);

  if (propertyType == napi_number) {
    double number = 0;
    if (napi_get_value_double(env, property, &number) != napi_ok) {
      return false;
    }

    if (!std::isfinite(number) || number < 0 || floor(number) != number ||
        number > static_cast<double>(UINT32_MAX)) {
      return false;
    }

    *index = static_cast<uint32_t>(number);
    return true;
  }

  if (propertyType != napi_string) {
    return false;
  }

  char chars[32];
  size_t length = 0;
  if (napi_get_value_string_utf8(env, property, chars, sizeof(chars), &length) != napi_ok ||
      length == 0 || length >= sizeof(chars)) {
    return false;
  }

  for (size_t i = 0; i < length; i++) {
    if (!std::isdigit(static_cast<unsigned char>(chars[i]))) {
      return false;
    }
  }

  char* end = nullptr;
  unsigned long long parsed = std::strtoull(chars, &end, 10);
  if (end == nullptr || *end != '\0' || parsed > static_cast<unsigned long long>(UINT32_MAX)) {
    return false;
  }

  *index = static_cast<uint32_t>(parsed);
  return true;
}

inline napi_value referenceValueAtIndex(napi_env env, Reference* ref, uint32_t index) {
  napi_value undefined;
  napi_get_undefined(env, &undefined);

  if (ref == nullptr || ref->type == nullptr || ref->data == nullptr) {
    return undefined;
  }

  auto slot =
      static_cast<uint8_t*>(ref->data) + (static_cast<size_t>(index) * referenceElementSize(ref));
  napi_value value = ref->type->toJS(env, slot);
  napi_valuetype valueType = napi_undefined;
  napi_typeof(env, value, &valueType);
  if (valueType == napi_boolean) {
    bool boolValue = false;
    napi_get_value_bool(env, value, &boolValue);
    napi_create_uint32(env, boolValue ? 1 : 0, &value);
  }
  return value;
}

inline bool referenceSetValueAtIndex(napi_env env, Reference* ref, uint32_t index,
                                     napi_value value) {
  if (ref == nullptr || ref->type == nullptr || ref->data == nullptr) {
    napi_throw_error(env, nullptr, "Reference is not initialized");
    return false;
  }

  auto slot =
      static_cast<uint8_t*>(ref->data) + (static_cast<size_t>(index) * referenceElementSize(ref));
  bool shouldFree = false;
  ref->type->toNative(env, value, slot, &shouldFree, &shouldFree);

  if (ConsumeNapiArgumentConversionFailure(env)) {
    return false;
  }
  bool hasPendingException = false;
  napi_is_exception_pending(env, &hasPendingException);
  return !hasPendingException;
}

napi_value referenceProxyGetter(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value target = argc > 0 ? argv[0] : nullptr;
  napi_value property = argc > 1 ? argv[1] : nullptr;

  uint32_t index = 0;
  if (property != nullptr && isArrayIndexProperty(env, property, &index)) {
    return referenceValueAtIndex(env, Reference::unwrap(env, target), index);
  }

  napi_value result;
  if (napi_get_property(env, target, property, &result) != napi_ok) {
    return nullptr;
  }

  napi_valuetype resultType = napi_undefined;
  napi_typeof(env, result, &resultType);
  if (resultType == napi_function) {
    napi_value bind;
    if (napi_get_named_property(env, result, "bind", &bind) == napi_ok) {
      napi_value bound;
      napi_value bindArgs[1] = {target};
      if (napi_call_function(env, result, bind, 1, bindArgs, &bound) == napi_ok) {
        return bound;
      }
    }
  }

  return result;
}

napi_value referenceProxySetter(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value target = argc > 0 ? argv[0] : nullptr;
  napi_value property = argc > 1 ? argv[1] : nullptr;
  napi_value value = argc > 2 ? argv[2] : nullptr;

  uint32_t index = 0;
  bool ok = true;
  if (property != nullptr && isArrayIndexProperty(env, property, &index)) {
    ok = referenceSetValueAtIndex(env, Reference::unwrap(env, target), index, value);
  } else {
    ok = napi_set_property(env, target, property, value) == napi_ok;
  }

  napi_value result;
  napi_get_boolean(env, ok, &result);
  return result;
}

inline napi_value createReferenceProxy(napi_env env, napi_value target, Reference* reference) {
  napi_value global;
  napi_get_global(env, &global);

  napi_value proxyCtor;
  if (napi_get_named_property(env, global, "Proxy", &proxyCtor) != napi_ok) {
    return target;
  }

  napi_value handler;
  napi_create_object(env, &handler);
  const napi_property_descriptor handlerProperties[] = {
      {
          .utf8name = "get",
          .method = referenceProxyGetter,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_default,
          .data = nullptr,
      },
      {
          .utf8name = "set",
          .method = referenceProxySetter,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_default,
          .data = nullptr,
      },
  };
  napi_define_properties(env, handler, 2, handlerProperties);

  napi_value proxyArgs[2] = {target, handler};
  napi_value proxy;
  if (napi_new_instance(env, proxyCtor, 2, proxyArgs, &proxy) != napi_ok) {
    return target;
  }

  napi_wrap(env, proxy, reference, nullptr, nullptr, nullptr);

  return proxy;
}

void finalizeReferenceNow(napi_env env, void* data, void* hint) {
  (void)env;
  (void)hint;
  Reference* ref = (Reference*)data;
  delete ref;
}

void finalizeFunctionReferenceNow(napi_env env, void* data, void* hint) {
  (void)env;
  (void)hint;
  FunctionReference* ref = (FunctionReference*)data;
  delete ref;
}

void finalizePointerNow(napi_env env, void* data, void* hint) {
  (void)env;
  (void)hint;
  Pointer* ptr = (Pointer*)data;
  if (ptr == nullptr) {
    return;
  }
  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState != nullptr && ptr->cached) {
    auto& cache = bridgeState->pointerCache;
    auto it = cache.find(pointerKey(ptr->data));
    if (it != cache.end() && it->second.owner == ptr) {
      napi_ref ref = it->second.ref;
      cache.erase(it);
      napi_delete_reference(env, ref);
    }
  }
  delete ptr;
}

inline bool getCachedPointer(napi_env env, void* data, napi_value* value) {
  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr) {
    return false;
  }

  auto& cache = bridgeState->pointerCache;
  auto it = cache.find(pointerKey(data));
  if (it == cache.end()) {
    return false;
  }

  *value = get_ref_value(env, it->second.ref);
  if (*value == nullptr) {
    napi_delete_reference(env, it->second.ref);
    cache.erase(it);
    return false;
  }

  napi_valuetype valueType = napi_undefined;
  if (napi_typeof(env, *value, &valueType) != napi_ok || valueType != napi_object) {
    napi_delete_reference(env, it->second.ref);
    cache.erase(it);
    *value = nullptr;
    return false;
  }

  Pointer* ptr = Pointer::unwrap(env, *value);
  if (ptr == nullptr || ptr->data != data) {
    napi_delete_reference(env, it->second.ref);
    cache.erase(it);
    *value = nullptr;
    return false;
  }

  return true;
}

inline bool cachePointer(napi_env env, Pointer* owner, napi_value value) {
  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr) {
    return false;
  }

  auto& cache = bridgeState->pointerCache;
  const uintptr_t key = pointerKey(owner->data);
  if (cache.find(key) != cache.end()) {
    return false;
  }
  napi_ref ref = nullptr;
  if (napi_create_reference(env, value, 0, &ref) != napi_ok) {
    return false;
  }
  cache[key] = {owner, ref};
  return true;
}

inline std::string pointerHexString(void* data) {
  uintptr_t value = reinterpret_cast<uintptr_t>(data);
  if (value == 0) {
    return "0x0";
  }

  char hex[2 + sizeof(uintptr_t) * 2 + 1];
#if UINTPTR_MAX == 0xffffffff
  snprintf(hex, sizeof(hex), "0x%x", static_cast<unsigned int>(value));
#else
  snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(value));
#endif
  return std::string(hex);
}

inline void* lookupSymbolByName(ObjCBridgeState* bridgeState, const char* symbolName) {
  void* fn = dlsym(bridgeState->self_dl, symbolName);
  if (fn == nullptr) {
    fn = dlsym(RTLD_DEFAULT, symbolName);
  }
  if (fn == nullptr) {
    std::string underscored = "_";
    underscored += symbolName;
    fn = dlsym(bridgeState->self_dl, underscored.c_str());
    if (fn == nullptr) {
      fn = dlsym(RTLD_DEFAULT, underscored.c_str());
    }
  }
  return fn;
}

inline bool unwrapKnownNativeHandleImpl(napi_env env, napi_value value, void** out) {
  if (env == nullptr || value == nullptr || out == nullptr) {
    return false;
  }
  *out = nullptr;

  napi_valuetype valueType = napi_undefined;
  if (napi_typeof(env, value, &valueType) != napi_ok) {
    return false;
  }

  if (valueType == napi_bigint) {
    uint64_t raw = 0;
    bool lossless = false;
    if (napi_get_value_bigint_uint64(env, value, &raw, &lossless) != napi_ok) {
      return false;
    }
    *out = reinterpret_cast<void*>(static_cast<uintptr_t>(raw));
    return true;
  }

  if (valueType == napi_external) {
    return napi_get_value_external(env, value, out) == napi_ok;
  }

  if (valueType != napi_object && valueType != napi_function) {
    return false;
  }

  bool isArray = false;
  if (valueType == napi_object && napi_is_array(env, value, &isArray) == napi_ok && isArray) {
    return false;
  }

  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState != nullptr) {
    id bridgedType = nil;
    if (bridgeState->tryResolveBridgedTypeConstructor(env, value, &bridgedType) &&
        bridgedType != nil) {
      *out = (void*)bridgedType;
      return true;
    }
  }

  if (Pointer::isInstance(env, value)) {
    Pointer* ptr = Pointer::unwrap(env, value);
    *out = ptr != nullptr ? ptr->data : nullptr;
    return true;
  }

  if (Reference::isInstance(env, value)) {
    Reference* ref = Reference::unwrap(env, value);
    *out = ref != nullptr ? ref->data : nullptr;
    return true;
  }

  if (valueType == napi_object && StructObject::isInstance(env, value)) {
    StructObject* object = StructObject::unwrap(env, value);
    *out = object != nullptr ? object->data : nullptr;
    return object != nullptr;
  }

  bool hasNativePointer = false;
  napi_has_named_property(env, value, kNativePointerProperty, &hasNativePointer);
  if (hasNativePointer) {
    napi_value nativePointerValue = nullptr;
    if (napi_get_named_property(env, value, kNativePointerProperty, &nativePointerValue) ==
        napi_ok) {
      if (Pointer::isInstance(env, nativePointerValue)) {
        Pointer* pointer = Pointer::unwrap(env, nativePointerValue);
        if (pointer != nullptr && pointer->data != nullptr) {
          *out = pointer->data;
          return true;
        }
      } else {
        void* nativePointer = nullptr;
        if (napi_get_value_external(env, nativePointerValue, &nativePointer) == napi_ok &&
            nativePointer != nullptr) {
          *out = nativePointer;
          return true;
        }
      }
    }
  }

  if (valueType != napi_function) {
    return false;
  }

  void* wrapped = nullptr;
  if (napi_unwrap(env, value, &wrapped) != napi_ok || wrapped == nullptr) {
    return false;
  }

  bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState != nullptr) {
    for (const auto& entry : bridgeState->classes) {
      if (entry.second == wrapped) {
        *out = (void*)entry.second->nativeClass;
        return true;
      }
    }

    for (const auto& entry : bridgeState->protocols) {
      if (entry.second == wrapped) {
        *out = (void*)objc_getProtocol(entry.second->name.c_str());
        return true;
      }
    }

    for (const auto& entry : bridgeState->cFunctionCache) {
      if (entry.second == wrapped) {
        *out = entry.second->fnptr;
        return true;
      }
    }
  }

  return false;
}

inline bool resolveNativePointerFromRegisteredFunction(napi_env env, napi_value value, void** out) {
  auto bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr || bridgeState->metadata == nullptr) {
    return false;
  }

  napi_value global;
  if (napi_get_global(env, &global) != napi_ok) {
    return false;
  }

  MDSectionOffset offset = bridgeState->metadata->functionsOffset;
  while (offset < bridgeState->metadata->protocolsOffset) {
    MDSectionOffset originalOffset = offset;
    const char* functionName = bridgeState->metadata->getString(offset);
    offset += sizeof(MDSectionOffset);
    offset += sizeof(MDSectionOffset);
    offset += sizeof(MDFunctionFlag);

    napi_value globalFn;
    if (napi_get_named_property(env, global, functionName, &globalFn) != napi_ok) {
      continue;
    }

    bool isSameFunction = false;
    napi_strict_equals(env, value, globalFn, &isSameFunction);
    if (!isSameFunction) {
      continue;
    }

    auto cFunction = bridgeState->getCFunction(env, originalOffset);
    if (cFunction != nullptr && cFunction->fnptr != nullptr) {
      *out = cFunction->fnptr;
      return true;
    }

    void* fn = lookupSymbolByName(bridgeState, functionName);
    if (fn != nullptr) {
      *out = fn;
      return true;
    }
    return false;
  }

  return false;
}

inline bool resolveNativePointerFromFunctionName(napi_env env, napi_value value, void** out) {
  napi_valuetype type = napi_undefined;
  napi_typeof(env, value, &type);
  if (type != napi_function) {
    return false;
  }

  napi_value nameValue;
  if (napi_get_named_property(env, value, "name", &nameValue) != napi_ok) {
    return false;
  }

  napi_valuetype nameType = napi_undefined;
  napi_typeof(env, nameValue, &nameType);
  if (nameType != napi_string) {
    return false;
  }

  char name[512];
  size_t len = 0;
  if (napi_get_value_string_utf8(env, nameValue, name, sizeof(name), &len) != napi_ok || len == 0) {
    return resolveNativePointerFromRegisteredFunction(env, value, out);
  }

  auto bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr) {
    return resolveNativePointerFromRegisteredFunction(env, value, out);
  }
  void* fn = lookupSymbolByName(bridgeState, name);
  if (fn == nullptr) {
    return resolveNativePointerFromRegisteredFunction(env, value, out);
  }

  *out = fn;
  return true;
}

inline void setObjectPrototype(napi_env env, napi_value object, napi_value prototype) {
  napi_value global;
  napi_get_global(env, &global);

  napi_value jsObject;
  napi_get_named_property(env, global, "Object", &jsObject);

  napi_value setPrototypeOf;
  napi_get_named_property(env, jsObject, "setPrototypeOf", &setPrototypeOf);

  napi_value argv[2] = {object, prototype};
  napi_call_function(env, jsObject, setPrototypeOf, 2, argv, nullptr);
}
}  // namespace

bool unwrapKnownNativeHandle(napi_env env, napi_value value, void** out) {
  return unwrapKnownNativeHandleImpl(env, value, out);
}

inline napi_value createJSNumber(napi_env env, int32_t ival) {
  napi_value value;
  napi_create_int32(env, ival, &value);
  return value;
}

napi_value __extends(napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value constructor = argv[0];
  napi_value superConstructor = argv[1];

  napi_inherits(env, constructor, superConstructor);

  Class superClassNative = nullptr;
  napi_unwrap(env, superConstructor, (void**)&superClassNative);
  if (superClassNative != nullptr) {
    ClassBuilder* builder = new ClassBuilder(env, constructor);
    ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
    bridgeState->registerRuntimeClass(builder, builder->nativeClass);
  }

  return nullptr;
}

const char* jsHelpersSource = R"(
  if (typeof globalThis.__decorate !== "function") {
    globalThis.__decorate = function(decorators, target, key, desc) {
        var c = arguments.length, r = c < 3 ? target : desc === null ? desc = Object.getOwnPropertyDescriptor(target, key) : desc, d;
        if (typeof Reflect === "object" && typeof Reflect.decorate === "function") r = Reflect.decorate(decorators, target, key, desc);
        else for(var i = decorators.length - 1; i >= 0; i--)if (d = decorators[i]) r = (c < 3 ? d(r) : c > 3 ? d(target, key, r) : d(target, key)) || r;
        return c > 3 && r && Object.defineProperty(target, key, r), r;
    };
  }

  if (typeof globalThis.__param !== "function") {
    globalThis.__param = function(paramIndex, decorator) {
      return function(target, key) { decorator(target, key, paramIndex); };
    };
  }

  globalThis.ObjCClass = function ObjCClass(...protocols) {
    return function(constructor) {
      if (constructor.ObjCProtocols) {
        constructor.ObjCProtocols.push(...protocols);
      } else {
        constructor.ObjCProtocols = protocols;
      }

      if (typeof globalThis.NativeClass === "function") {
        return globalThis.NativeClass(constructor);
      }

      return constructor;
    };
  };

  if (typeof WeakRef === "function") {
    WeakRef.prototype.get = WeakRef.prototype.deref;
    if (!WeakRef.prototype.clear) {
      WeakRef.prototype.clear = function() {
        console.warn("WeakRef.clear() is non-standard and has been deprecated. It does nothing and the call can be safely removed.");
      };
    }
  }
)";

void registerInterop(napi_env env, napi_value global) {
  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);

  const napi_property_descriptor __extendsProperty = {
      .utf8name = "__extends",
      .method = __extends,
      .getter = nullptr,
      .setter = nullptr,
      .value = nullptr,
      .attributes = napi_enumerable,
      .data = nullptr,
  };

  napi_define_properties(env, global, 1, &__extendsProperty);

  napi_value jsHelpers;
  napi_create_string_utf8(env, jsHelpersSource, NAPI_AUTO_LENGTH, &jsHelpers);
  napi_value result;
  napi_run_script(env, jsHelpers, &result);

  napi_value interop;
  napi_create_object(env, &interop);

  napi_value types;
  napi_create_object(env, &types);

  napi_set_named_property(env, types, "void", createJSNumber(env, mdTypeVoid));
  napi_set_named_property(env, types, "bool", createJSNumber(env, mdTypeBool));
  napi_set_named_property(env, types, "int8", createJSNumber(env, mdTypeChar));
  napi_set_named_property(env, types, "uint8", createJSNumber(env, mdTypeUInt8));
  napi_set_named_property(env, types, "int16", createJSNumber(env, mdTypeSShort));
  napi_set_named_property(env, types, "uint16", createJSNumber(env, mdTypeUShort));
  napi_set_named_property(env, types, "int32", createJSNumber(env, mdTypeSInt));
  napi_set_named_property(env, types, "uint32", createJSNumber(env, mdTypeUInt));
  napi_set_named_property(env, types, "int64", createJSNumber(env, mdTypeSInt64));
  napi_set_named_property(env, types, "uint64", createJSNumber(env, mdTypeUInt64));
  napi_set_named_property(env, types, "float", createJSNumber(env, mdTypeFloat));
  napi_set_named_property(env, types, "double", createJSNumber(env, mdTypeDouble));
  napi_set_named_property(env, types, "UTF8CString", createJSNumber(env, mdTypeString));
  napi_set_named_property(env, types, "unichar", createJSNumber(env, mdTypeUnichar));
  napi_set_named_property(env, types, "id", createJSNumber(env, mdTypeAnyObject));
  napi_set_named_property(env, types, "protocol", createJSNumber(env, mdTypePointer));
  napi_set_named_property(env, types, "class", createJSNumber(env, mdTypeAnyObject));
  napi_set_named_property(env, types, "SEL", createJSNumber(env, mdTypeSelector));
  napi_set_named_property(env, types, "selector", createJSNumber(env, mdTypeSelector));
  napi_set_named_property(env, types, "pointer", createJSNumber(env, mdTypePointer));

  napi_value Pointer = Pointer::defineJSClass(env);
  bridgeState->pointerClass = make_ref(env, Pointer);

  napi_value Reference = Reference::defineJSClass(env);
  bridgeState->referenceClass = make_ref(env, Reference);

  napi_value FunctionReference = FunctionReference::defineJSClass(env);
  bridgeState->functionReferenceClass = make_ref(env, FunctionReference);

  const napi_property_descriptor properties[] = {
      {
          .utf8name = "Pointer",
          .method = nullptr,
          .getter = nullptr,
          .setter = nullptr,
          .value = Pointer,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "Reference",
          .method = nullptr,
          .getter = nullptr,
          .setter = nullptr,
          .value = Reference,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "FunctionReference",
          .method = nullptr,
          .getter = nullptr,
          .setter = nullptr,
          .value = FunctionReference,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "types",
          .method = nullptr,
          .getter = nullptr,
          .setter = nullptr,
          .value = types,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "adopt",
          .method = interop_adopt,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "addMethod",
          .method = interop_addMethod,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "addProtocol",
          .method = interop_addProtocol,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "free",
          .method = interop_free,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "sizeof",
          .method = interop_sizeof,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "alloc",
          .method = interop_alloc,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "handleof",
          .method = interop_handleof,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "bufferFromData",
          .method = interop_bufferFromData,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "stringFromCString",
          .method = interop_stringFromCString,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
  };

  napi_define_properties(env, interop, 13, properties);

  napi_set_named_property(env, global, "interop", interop);
}

napi_value interop_addMethod(napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  ClassBuilder* builder = nullptr;
  napi_unwrap(env, argv[0], (void**)&builder);
  if (builder == nullptr) {
    napi_throw_error(env, nullptr, "Invalid class");
    return nullptr;
  }
  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  builder = (ClassBuilder*)bridgeState->classesByPointer[(id)builder];

  napi_value name;
  napi_get_named_property(env, argv[1], "name", &name);

  if (builder->isFinal) {
    static char funcNameBuf[512];
    napi_get_value_string_utf8(env, name, funcNameBuf, 512, nullptr);
    std::string funcName = funcNameBuf;

    MethodDescriptor* desc = builder->lookupMethodDescriptor(funcName);
    if (desc == nullptr) {
      napi_throw_error(env, nullptr, "Invalid method, descriptor not found");
      return nullptr;
    }

    builder->addMethod(funcName, desc, name, argv[1]);
  } else {
    napi_set_property(env, get_ref_value(env, builder->prototype), name, argv[1]);
  }

  return nullptr;
}

napi_value interop_addProtocol(napi_env env, napi_callback_info info) {
  napi_value argv[2];
  size_t argc = 2;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  ClassBuilder* builder = nullptr;
  napi_unwrap(env, argv[0], (void**)&builder);
  if (builder == nullptr) {
    napi_throw_error(env, nullptr, "Invalid class");
    return nullptr;
  }
  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  builder = (ClassBuilder*)bridgeState->classesByPointer[(id)builder];

  ObjCProtocol* proto = nullptr;
  napi_unwrap(env, argv[1], (void**)&proto);
  if (proto == nullptr) {
    napi_throw_error(env, nullptr, "Invalid protocol");
    return nullptr;
  }

  builder->addProtocol(proto);

  return nullptr;
}

napi_value interop_adopt(napi_env env, napi_callback_info info) {
  napi_value arg;
  size_t argc = 1;
  napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);

  Pointer* ptr;
  napi_unwrap(env, arg, (void**)&ptr);

  napi_value adopted;
  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  napi_new_instance(env, get_ref_value(env, bridgeState->pointerClass), 1, &arg, &adopted);
  Pointer* adoptedPtr;
  napi_unwrap(env, adopted, (void**)&adoptedPtr);
  adoptedPtr->adopted = true;

  return adopted;
}

napi_value interop_free(napi_env env, napi_callback_info info) {
  napi_value arg;
  size_t argc = 1;
  napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);

  Pointer* ptr = nullptr;
  napi_unwrap(env, arg, (void**)&ptr);

  if (ptr != nullptr && ptr->data != nullptr) {
    ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
    if (bridgeState != nullptr) {
      auto& cache = bridgeState->pointerCache;
      auto it = cache.find(pointerKey(ptr->data));
      if (it != cache.end() && it->second.owner == ptr) {
        napi_delete_reference(env, it->second.ref);
        cache.erase(it);
      }
    }
    ptr->cached = false;
    free(ptr->data);
    ptr->data = nullptr;
  }

  return nullptr;
}

napi_value interop_stringFromCString(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_value arg = argv[0];

  napi_valuetype type;
  napi_typeof(env, arg, &type);

  if (type == napi_string) {
    return arg;
  }

  if (type != napi_object) {
    napi_throw_type_error(env, "TypeError", "Expected an object");
    return nullptr;
  }

  void* data = nullptr;

  if (Pointer::isInstance(env, arg)) {
    Pointer* ptr = Pointer::unwrap(env, arg);
    data = ptr->data;
  }

  if (Reference::isInstance(env, arg)) {
    Reference* ref = Reference::unwrap(env, arg);
    data = ref->data;
  }

  napi_value result;

  if (data == nullptr) {
    napi_get_null(env, &result);
  } else {
    size_t length = NAPI_AUTO_LENGTH;
    if (argc >= 2 && argv[1] != nullptr) {
      napi_valuetype lengthType = napi_undefined;
      napi_typeof(env, argv[1], &lengthType);
      if (lengthType != napi_undefined && lengthType != napi_null) {
        int64_t explicitLength = 0;
        napi_get_value_int64(env, argv[1], &explicitLength);
        if (explicitLength < 0) {
          explicitLength = 0;
        }
        length = static_cast<size_t>(explicitLength);
      }
    }
    napi_create_string_utf8(env, (const char*)data, length, &result);
  }

  return result;
}

size_t jsSizeof(napi_env env, napi_value arg) {
  napi_valuetype type;
  napi_typeof(env, arg, &type);

  switch (type) {
    case napi_number: {
      int32_t ival;
      napi_get_value_int32(env, arg, &ival);

      switch (ival) {
        case mdTypeVoid:
          return 0;

        case mdTypeBool:
        case mdTypeChar:
        case mdTypeUInt8:
          return sizeof(uint8_t);

        case mdTypeSShort:
        case mdTypeUShort:
        case mdTypeUnichar:
          return sizeof(uint16_t);

        case mdTypeSInt:
        case mdTypeUInt:
          return sizeof(uint32_t);

        case mdTypeSInt64:
        case mdTypeUInt64:
          return sizeof(uint64_t);

        case mdTypeFloat:
          return sizeof(float);

        case mdTypeDouble:
          return sizeof(double);

        case mdTypeString:
          return sizeof(char*);

        case mdTypeAnyObject:
          return sizeof(id);

        case mdTypePointer:
          return sizeof(void*);

        case mdTypeSelector:
          return sizeof(SEL);

        default:
          napi_throw_type_error(env, "TypeError",
                                "Invalid type number for sizeof. Use interop.types.*");
      }
    }

    case napi_object:
    case napi_function: {
      napi_value symbolSizeof = jsSymbolFor(env, "sizeof");
      napi_value result;
      if (napi_get_property(env, arg, symbolSizeof, &result) == napi_ok) {
        napi_valuetype resultType;
        napi_typeof(env, result, &resultType);
        if (resultType == napi_number) {
          int32_t size;
          napi_get_value_int32(env, result, &size);
          return size;
        }
      }

      void* nativeHandle = nullptr;
      if (unwrapKnownNativeHandle(env, arg, &nativeHandle)) {
        return sizeof(void*);
      }
      if (resolveNativePointerFromFunctionName(env, arg, &nativeHandle)) {
        return sizeof(void*);
      }

      napi_throw_type_error(env, "TypeError", "Invalid type for sizeof");
    }

    default:
      napi_throw_type_error(env, "TypeError", "Invalid type for sizeof");
  }

  return -1;
}

napi_value interop_sizeof(napi_env env, napi_callback_info info) {
  napi_valuetype type;
  napi_value arg;
  size_t argc = 1;
  napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);

  size_t size = jsSizeof(env, arg);
  napi_value result;
  napi_create_int32(env, size, &result);

  return result;
}

napi_value interop_alloc(napi_env env, napi_callback_info info) {
  napi_value arg;
  size_t argc = 1;
  napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);

  int64_t size;
  napi_get_value_int64(env, arg, &size);

  void* data = calloc(1, size);
  return Pointer::create(env, data);
}

napi_value interop_handleof(napi_env env, napi_callback_info info) {
  napi_value value, result;
  size_t argc = 1;
  napi_get_cb_info(env, info, &argc, &value, nullptr, nullptr);

  napi_valuetype valueType = napi_undefined;
  napi_typeof(env, value, &valueType);
  if (valueType == napi_null || valueType == napi_undefined) {
    napi_get_null(env, &result);
    return result;
  }

  if (valueType == napi_string) {
    size_t len = 0;
    napi_get_value_string_utf8(env, value, nullptr, 0, &len);
    char* str = static_cast<char*>(malloc(len + 1));
    napi_get_value_string_utf8(env, value, str, len + 1, &len);
    str[len] = '\0';
    return Pointer::create(env, str);
  }

  if (Pointer::isInstance(env, value)) {
    return value;
  } else if (Reference::isInstance(env, value)) {
    Reference* ref = Reference::unwrap(env, value);
    if (ref == nullptr || ref->data == nullptr) {
      napi_throw_error(env, nullptr, "Reference is not initialized");
      return nullptr;
    }
    return Pointer::create(env, ref->data);
  }

  if (FunctionReference::isInstance(env, value)) {
    FunctionReference* reference = FunctionReference::unwrap(env, value);
    if (reference == nullptr || reference->closure == nullptr ||
        reference->closure->fnptr == nullptr) {
      napi_throw_error(env, nullptr, "FunctionReference is not initialized");
      return nullptr;
    }
    return Pointer::create(env, reference->closure->fnptr);
  }

  void* data = nullptr;
  if (unwrapKnownNativeHandle(env, value, &data) && data != nullptr) {
    if (valueType == napi_object && !Pointer::isInstance(env, value) &&
        !Reference::isInstance(env, value) && !FunctionReference::isInstance(env, value)) {
      ObjCBridgeState::InstanceData(env)->cacheHandleObject(env, data, value);
    }
    return Pointer::create(env, data);
  }
  if (resolveNativePointerFromFunctionName(env, value, &data) && data != nullptr) {
    return Pointer::create(env, data);
  }

  napi_get_null(env, &result);
  return result;
}

napi_value interop_bufferFromData(napi_env env, napi_callback_info info) {
  napi_value arg;
  size_t argc = 1;
  napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);

  bool isArrayBuffer = false;
  napi_is_arraybuffer(env, arg, &isArrayBuffer);
  if (isArrayBuffer) {
    return arg;
  }

  NSData* data = nil;
  if (napi_unwrap(env, arg, (void**)&data) != napi_ok || data == nil) {
    void* native = nullptr;
    if (!unwrapKnownNativeHandle(env, arg, &native) || native == nullptr) {
      napi_throw_error(env, nullptr, "Invalid data");
      return nullptr;
    }

    id candidate = (id)native;
    if (![candidate isKindOfClass:[NSData class]]) {
      napi_throw_error(env, nullptr, "Invalid data");
      return nullptr;
    }
    data = (NSData*)candidate;
  }

  if (data == nil) {
    napi_throw_error(env, nullptr, "Invalid data");
    return nullptr;
  }

  napi_value result = nullptr;
  void* bufferData = nullptr;
  napi_status status =
      napi_create_arraybuffer(env, static_cast<size_t>(data.length), &bufferData, &result);
  if (status != napi_ok || result == nullptr) {
    napi_throw_error(env, nullptr, "Failed to create ArrayBuffer");
    return nullptr;
  }

  if (data.length > 0 && bufferData != nullptr) {
    std::memcpy(bufferData, data.bytes, static_cast<size_t>(data.length));
  }

  return result;
}

napi_value Pointer::defineJSClass(napi_env env) {
  const napi_property_descriptor properties[] = {
      {
          .utf8name = "add",
          .method = Pointer::add,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "subtract",
          .method = Pointer::subtract,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "toNumber",
          .method = Pointer::toNumber,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "toBigInt",
          .method = Pointer::toBigInt,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "toHexString",
          .method = Pointer::toHexString,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "toDecimalString",
          .method = Pointer::toDecimalString,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "toString",
          .method = Pointer::toString,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = nullptr,
          .name = jsSymbolFor(env, "nodejs.util.inspect.custom"),
          .method = Pointer::customInspect,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
  };

  napi_value constructor;
  napi_define_class(env, "Pointer", NAPI_AUTO_LENGTH, Pointer::constructor, nullptr, 8, properties,
                    &constructor);

  napi_value symbolSizeof = jsSymbolFor(env, "sizeof");
  napi_value sizeValue;
  napi_create_int32(env, sizeof(void*), &sizeValue);
  napi_set_property(env, constructor, symbolSizeof, sizeValue);

  napi_value prototype;
  napi_get_named_property(env, constructor, "prototype", &prototype);
  napi_set_property(env, prototype, symbolSizeof, sizeValue);

  napi_value marker;
  napi_get_boolean(env, true, &marker);
  napi_property_descriptor markerProp = {
      .utf8name = kPointerMarker,
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = marker,
      .attributes = napi_configurable,
      .data = nullptr,
  };
  napi_define_properties(env, prototype, 1, &markerProp);

  return constructor;
}

bool Pointer::isInstance(napi_env env, napi_value value) {
  napi_valuetype valueType = napi_undefined;
  napi_typeof(env, value, &valueType);
  if (valueType != napi_object && valueType != napi_function) {
    return false;
  }

  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr || bridgeState->pointerClass == nullptr) {
    return false;
  }

  napi_value constructor = get_ref_value(env, bridgeState->pointerClass);
  bool result = false;
  if (constructor == nullptr ||
      napi_instanceof(env, value, constructor, &result) != napi_ok) {
    return false;
  }
  return result;
}

napi_value Pointer::create(napi_env env, void* data) {
  napi_value cached;
  if (getCachedPointer(env, data, &cached)) {
    return cached;
  }

  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  napi_value jsPointer = get_ref_value(env, bridgeState->pointerClass);
  napi_value argv[1];
  napi_create_bigint_uint64(env, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data)),
                            &argv[0]);
  napi_value result;
  napi_status status = napi_new_instance(env, jsPointer, 1, argv, &result);

  if (status != napi_ok) {
    return nullptr;
  }
  return result;
}

Pointer* Pointer::unwrap(napi_env env, napi_value value) {
  if (value == nullptr) {
    return nullptr;
  }

  napi_valuetype valueType = napi_undefined;
  if (napi_typeof(env, value, &valueType) != napi_ok || valueType != napi_object) {
    return nullptr;
  }

  Pointer* ptr = nullptr;
  if (napi_unwrap(env, value, (void**)&ptr) != napi_ok) {
    return nullptr;
  }
  return ptr;
}

napi_value Pointer::constructor(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, &jsThis, nullptr);

  napi_valuetype thisType = napi_undefined;
  if (jsThis == nullptr || napi_typeof(env, jsThis, &thisType) != napi_ok ||
      (thisType != napi_object && thisType != napi_function)) {
    napi_create_object(env, &jsThis);

    auto bridgeState = ObjCBridgeState::InstanceData(env);
    if (bridgeState != nullptr && bridgeState->pointerClass != nullptr) {
      napi_value pointerCtor = get_ref_value(env, bridgeState->pointerClass);
      napi_value pointerPrototype = nullptr;
      if (pointerCtor != nullptr &&
          napi_get_named_property(env, pointerCtor, "prototype", &pointerPrototype) == napi_ok &&
          pointerPrototype != nullptr) {
        napi_value global = nullptr;
        napi_value objectCtor = nullptr;
        napi_value setPrototypeOf = nullptr;
        napi_get_global(env, &global);
        napi_get_named_property(env, global, "Object", &objectCtor);
        napi_get_named_property(env, objectCtor, "setPrototypeOf", &setPrototypeOf);
        napi_value setPrototypeArgs[2] = {jsThis, pointerPrototype};
        napi_call_function(env, objectCtor, setPrototypeOf, 2, setPrototypeArgs, nullptr);
      }
    }
  }

  napi_value arg;
  if (argc == 0) {
    napi_get_undefined(env, &arg);
  } else {
    arg = argv[0];
  }

  napi_valuetype type = napi_undefined;
  napi_typeof(env, arg, &type);

  void* data;

  switch (type) {
    case napi_number: {
      double number = 0;
      napi_get_value_double(env, arg, &number);
      if (!std::isfinite(number)) {
        napi_throw_error(env, nullptr, "Invalid type");
        return nullptr;
      }
      data = (void*)((intptr_t)number);
      break;
    }

    case napi_bigint: {
      uint64_t value = 0;
      bool lossless = false;
      napi_get_value_bigint_uint64(env, arg, &value, &lossless);
      data = reinterpret_cast<void*>(static_cast<uintptr_t>(value));
      break;
    }

    case napi_object: {
      bool isInstance = Pointer::isInstance(env, arg);
      if (isInstance) {
        Pointer* ptr;
        napi_unwrap(env, arg, (void**)&ptr);
        if (ptr == nullptr) {
          napi_throw_error(env, nullptr, "Invalid type");
          return nullptr;
        }
        return arg;
      } else {
        napi_value numberValue;
        napi_coerce_to_number(env, arg, &numberValue);
        double number = 0;
        napi_get_value_double(env, numberValue, &number);
        if (!std::isfinite(number)) {
          napi_throw_error(env, nullptr, "Invalid type");
          return nullptr;
        }
        data = (void*)((intptr_t)number);
      }
      break;
    }

    case napi_null:
    case napi_undefined: {
      data = nullptr;
      break;
    }

    default:
      napi_throw_error(env, nullptr, "Invalid type");
      return nullptr;
  }

  napi_value cached;
  if (getCachedPointer(env, data, &cached)) {
    return cached;
  }

  Pointer* ptr = new Pointer(data);
  napi_wrap(env, jsThis, ptr, Pointer::finalize, nullptr, nullptr);
  ptr->cached = cachePointer(env, ptr, jsThis);

  return jsThis;
}

napi_value Pointer::add(napi_env env, napi_callback_info info) {
  napi_value arg, jsThis;
  size_t argc = 1;
  napi_get_cb_info(env, info, &argc, &arg, &jsThis, nullptr);

  Pointer* ptr = Pointer::unwrap(env, jsThis);

  int64_t ival;
  napi_get_value_int64(env, arg, &ival);

  void* newData = (void*)((intptr_t)((intptr_t)ptr->data + (intptr_t)ival));
  return Pointer::create(env, newData);
}

napi_value Pointer::subtract(napi_env env, napi_callback_info info) {
  napi_value arg, jsThis;
  size_t argc = 1;
  napi_get_cb_info(env, info, &argc, &arg, &jsThis, nullptr);

  Pointer* ptr = Pointer::unwrap(env, jsThis);

  int64_t ival;
  napi_get_value_int64(env, arg, &ival);

  void* newData = (void*)((intptr_t)((intptr_t)ptr->data - (intptr_t)ival));
  return Pointer::create(env, newData);
}

napi_value Pointer::toNumber(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Pointer* ptr = Pointer::unwrap(env, jsThis);

  napi_value result;
  double value = static_cast<double>(reinterpret_cast<uintptr_t>(ptr->data));
  napi_create_double(env, value, &result);
  return result;
}

napi_value Pointer::toBigInt(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Pointer* ptr = Pointer::unwrap(env, jsThis);

  napi_value result;
  napi_create_bigint_uint64(env, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr->data)),
                            &result);
  return result;
}

napi_value Pointer::toHexString(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Pointer* ptr = Pointer::unwrap(env, jsThis);
  std::string hex = pointerHexString(ptr->data);

  napi_value result;
  napi_create_string_utf8(env, hex.c_str(), NAPI_AUTO_LENGTH, &result);
  return result;
}

napi_value Pointer::toDecimalString(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Pointer* ptr = Pointer::unwrap(env, jsThis);
  std::string decimal;
  if (sizeof(void*) == 4) {
    decimal = std::to_string(static_cast<int32_t>(reinterpret_cast<intptr_t>(ptr->data)));
  } else {
    decimal = std::to_string(static_cast<int64_t>(reinterpret_cast<intptr_t>(ptr->data)));
  }

  napi_value result;
  napi_create_string_utf8(env, decimal.c_str(), NAPI_AUTO_LENGTH, &result);
  return result;
}

napi_value Pointer::toString(napi_env env, napi_callback_info info) {
  return Pointer::customInspect(env, info);
}

napi_value Pointer::customInspect(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Pointer* ptr = Pointer::unwrap(env, jsThis);
  std::string str = "<Pointer: " + pointerHexString(ptr->data) + ">";

  napi_value result;
  napi_create_string_utf8(env, str.c_str(), NAPI_AUTO_LENGTH, &result);

  return result;
}

void Pointer::finalize(napi_env env, void* data, void* hint) {
  if (PostFinalizer(env, finalizePointerNow, data, hint)) {
    return;
  }

  finalizePointerNow(env, data, hint);
}

Pointer::Pointer(void* data) { this->data = data; }

Pointer::~Pointer() {
  if (adopted && data != nullptr) {
    free(data);
  }
}

napi_value Reference::defineJSClass(napi_env env) {
  const napi_property_descriptor properties[] = {
      {
          .utf8name = "value",
          .method = nullptr,
          .getter = Reference::get_value,
          .setter = Reference::set_value,
          .value = nullptr,
          .attributes = (napi_property_attributes)(napi_enumerable | napi_writable),
          .data = nullptr,
      },
      {
          .utf8name = nullptr,
          .name = jsSymbolFor(env, "nodejs.util.inspect.custom"),
          .method = Reference::customInspect,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
      {
          .utf8name = "toString",
          .method = Reference::customInspect,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
  };

  napi_value constructor;
  napi_define_class(env, "Reference", NAPI_AUTO_LENGTH, Reference::constructor, nullptr, 3,
                    properties, &constructor);

  napi_value symbolSizeof = jsSymbolFor(env, "sizeof");
  napi_value sizeValue;
  napi_create_int32(env, sizeof(void*), &sizeValue);
  napi_set_property(env, constructor, symbolSizeof, sizeValue);

  napi_value prototype;
  napi_get_named_property(env, constructor, "prototype", &prototype);
  napi_set_property(env, prototype, symbolSizeof, sizeValue);

  napi_value marker;
  napi_get_boolean(env, true, &marker);
  napi_property_descriptor markerProp = {
      .utf8name = kReferenceMarker,
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = marker,
      .attributes = napi_configurable,
      .data = nullptr,
  };
  napi_define_properties(env, prototype, 1, &markerProp);

  return constructor;
}

bool Reference::isInstance(napi_env env, napi_value value) {
  napi_valuetype valueType = napi_undefined;
  napi_typeof(env, value, &valueType);
  if (valueType != napi_object && valueType != napi_function) {
    return false;
  }

  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr || bridgeState->referenceClass == nullptr) {
    return false;
  }

  napi_value constructor = get_ref_value(env, bridgeState->referenceClass);
  bool result = false;
  if (constructor == nullptr ||
      napi_instanceof(env, value, constructor, &result) != napi_ok) {
    return false;
  }
  return result;
}

napi_value Reference::create(napi_env env, std::shared_ptr<TypeConv> type, void* data,
                             bool ownsData) {
  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  napi_value referenceCtor = get_ref_value(env, bridgeState->referenceClass);
  napi_value jsReference = nullptr;
  if (napi_new_instance(env, referenceCtor, 0, nullptr, &jsReference) != napi_ok) {
    return nullptr;
  }

  Reference* reference = Reference::unwrap(env, jsReference);
  if (reference == nullptr) {
    return nullptr;
  }

  reference->env = env;
  reference->bridgeState = ObjCBridgeState::InstanceData(env);
  reference->bridgeStateToken =
      reference->bridgeState != nullptr ? reference->bridgeState->lifetimeToken : 0;
  reference->type = std::move(type);
  reference->data = data;
  reference->ownsData = ownsData;

  return jsReference;
}

Reference* Reference::unwrap(napi_env env, napi_value value) {
  Reference* ref = nullptr;
  if (napi_unwrap(env, value, (void**)&ref) != napi_ok) {
    return nullptr;
  }
  return ref;
}

napi_value Reference::getInitValue(napi_env env, napi_value value, Reference* ref) {
  napi_value initValue = getReferenceInitValueProperty(env, value);
  napi_valuetype initType = napi_undefined;
  if (initValue != nullptr && napi_typeof(env, initValue, &initType) == napi_ok &&
      initType != napi_undefined) {
    return initValue;
  }

  if (ref != nullptr && ref->initValue != nullptr) {
    return get_ref_value(env, ref->initValue);
  }

  return nullptr;
}

void Reference::setInitValue(napi_env env, napi_value value, Reference* ref, napi_value initValue) {
  setReferenceInitValueProperty(env, value, initValue);

  if (ref == nullptr) {
    return;
  }

  if (ref->initValue != nullptr) {
    napi_delete_reference(env, ref->initValue);
    ref->initValue = nullptr;
  }

  if (initValue != nullptr) {
    napi_create_reference(env, initValue, 1, &ref->initValue);
  }
}

void Reference::clearInitValue(napi_env env, napi_value value, Reference* ref) {
  clearReferenceInitValueProperty(env, value);

  if (ref != nullptr && ref->initValue != nullptr) {
    napi_delete_reference(env, ref->initValue);
    ref->initValue = nullptr;
  }
}

napi_value Reference::constructor(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, &jsThis, nullptr);

  napi_valuetype thisType = napi_undefined;
  if (jsThis == nullptr || napi_typeof(env, jsThis, &thisType) != napi_ok ||
      (thisType != napi_object && thisType != napi_function)) {
    napi_create_object(env, &jsThis);

    auto bridgeState = ObjCBridgeState::InstanceData(env);
    if (bridgeState != nullptr && bridgeState->referenceClass != nullptr) {
      napi_value referenceCtor = get_ref_value(env, bridgeState->referenceClass);
      napi_value referencePrototype = nullptr;
      if (referenceCtor != nullptr &&
          napi_get_named_property(env, referenceCtor, "prototype", &referencePrototype) ==
              napi_ok &&
          referencePrototype != nullptr) {
        napi_value global = nullptr;
        napi_value objectCtor = nullptr;
        napi_value setPrototypeOf = nullptr;
        napi_get_global(env, &global);
        napi_get_named_property(env, global, "Object", &objectCtor);
        napi_get_named_property(env, objectCtor, "setPrototypeOf", &setPrototypeOf);
        napi_value setPrototypeArgs[2] = {jsThis, referencePrototype};
        napi_call_function(env, objectCtor, setPrototypeOf, 2, setPrototypeArgs, nullptr);
      }
    }
  }

  Reference* reference = new Reference();
  reference->env = env;
  reference->bridgeState = ObjCBridgeState::InstanceData(env);
  reference->bridgeStateToken =
      reference->bridgeState != nullptr ? reference->bridgeState->lifetimeToken : 0;

  if (argc == 0) {
    // Leave it uninitialized. It can be materialized later by pointer marshalling.
  } else if (argc == 1) {
    napi_valuetype argType = napi_undefined;
    napi_typeof(env, argv[0], &argType);

    bool isTypeArg = false;
    if (argType == napi_function) {
      isTypeArg = true;
    } else if (argType == napi_number) {
      double numericValue = 0;
      if (napi_get_value_double(env, argv[0], &numericValue) == napi_ok &&
          std::isfinite(numericValue) && floor(numericValue) == numericValue &&
          numericValue >= static_cast<double>(INT32_MIN) &&
          numericValue <= static_cast<double>(INT32_MAX)) {
        int32_t typeCode = static_cast<int32_t>(numericValue);
        if (isInteropTypeCode(typeCode)) {
          isTypeArg = true;
        }
      }
    }

    if (isTypeArg) {
      std::string typeEncoding = getEncodedType(env, argv[0]);
      const char* typestr = typeEncoding.c_str();
      reference->type = TypeConv::Make(env, &typestr);

      size_t size = reference->type != nullptr && reference->type->type != nullptr &&
                            reference->type->type->size > 0
                        ? reference->type->type->size
                        : sizeof(void*);
      reference->data = calloc(1, size);
      reference->ownsData = true;
    } else {
      Reference::setInitValue(env, jsThis, reference, argv[0]);
    }
  } else if (argc == 2) {
    std::string type = getEncodedType(env, argv[0]);
    const char* typestr = type.c_str();
    reference->type = TypeConv::Make(env, &typestr);

    napi_valuetype argtype;
    napi_typeof(env, argv[1], &argtype);

    if (argtype == napi_object && Pointer::isInstance(env, argv[1])) {
      reference->data = Pointer::unwrap(env, argv[1])->data;
      reference->ownsData = false;
    } else if (reference->type != nullptr && reference->type->kind == mdTypeStruct &&
               argtype == napi_object && StructObject::isInstance(env, argv[1])) {
      StructObject* structObject = StructObject::unwrap(env, argv[1]);
      if (structObject != nullptr) {
        reference->data = structObject->data;
        reference->ownsData = false;
      }
    } else if (argtype == napi_object && Reference::isInstance(env, argv[1])) {
      Reference* other = Reference::unwrap(env, argv[1]);
      if (other != nullptr && other->data != nullptr) {
        reference->data = other->data;
        reference->ownsData = false;
      } else if (other != nullptr) {
        size_t size = reference->type != nullptr && reference->type->type != nullptr &&
                              reference->type->type->size > 0
                          ? reference->type->type->size
                          : sizeof(void*);
        reference->data = calloc(1, size);
        reference->ownsData = true;
        bool shouldFree = false;
        napi_value initValue = Reference::getInitValue(env, argv[1], other);
        if (initValue != nullptr) {
          reference->type->toNative(env, initValue, reference->data, &shouldFree, &shouldFree);
          if (ConsumeNapiArgumentConversionFailure(env)) {
            delete reference;
            return nullptr;
          }
        }
      }
    } else {
      size_t size = reference->type != nullptr && reference->type->type != nullptr &&
                            reference->type->type->size > 0
                        ? reference->type->type->size
                        : sizeof(void*);
      reference->data = malloc(size);
      reference->ownsData = true;
      bool shouldFree;
      reference->type->toNative(env, argv[1], reference->data, &shouldFree, &shouldFree);
      if (ConsumeNapiArgumentConversionFailure(env)) {
        delete reference;
        return nullptr;
      }
    }
  } else {
    napi_throw_error(env, nullptr, "Invalid number of arguments");
    return nullptr;
  }

  napi_wrap(env, jsThis, reference, Reference::finalize, nullptr, nullptr);

  return createReferenceProxy(env, jsThis, reference);
}

napi_value Reference::get_value(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Reference* ref = Reference::unwrap(env, jsThis);

  if (ref == nullptr) {
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  if (ref->data == nullptr) {
    napi_value initValue = Reference::getInitValue(env, jsThis, ref);
    if (initValue != nullptr) {
      return initValue;
    }

    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  return ref->type->toJS(env, ref->data);
}

napi_value Reference::set_value(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  size_t argc = 1;
  napi_value arg;
  napi_get_cb_info(env, info, &argc, &arg, &jsThis, nullptr);

  Reference* ref = Reference::unwrap(env, jsThis);

  if (ref == nullptr) {
    return nullptr;
  }

  if (ref->data == nullptr) {
    if (ref->type == nullptr) {
      Reference::setInitValue(env, jsThis, ref, arg);
      return nullptr;
    }

    size_t size = ref->type->type != nullptr && ref->type->type->size > 0 ? ref->type->type->size
                                                                          : sizeof(void*);
    ref->data = calloc(1, size);
    ref->ownsData = true;
  }

  bool shouldFree = false;
  ref->type->toNative(env, arg, ref->data, &shouldFree, &shouldFree);
  if (ConsumeNapiArgumentConversionFailure(env)) {
    return nullptr;
  }
  Reference::clearInitValue(env, jsThis, ref);

  return nullptr;
}

napi_value Reference::customInspect(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Reference* ref = Reference::unwrap(env, jsThis);

  napi_value result;
  if (ref == nullptr) {
    napi_create_string_utf8(env, "<Reference: 0x0>", NAPI_AUTO_LENGTH, &result);
    return result;
  }
  std::string str = "<Reference: " + pointerHexString(ref->data) + ">";
  napi_create_string_utf8(env, str.c_str(), NAPI_AUTO_LENGTH, &result);

  return result;
}

void Reference::finalize(napi_env env, void* data, void* hint) {
  if (PostFinalizer(env, finalizeReferenceNow, data, hint)) {
    return;
  }

  finalizeReferenceNow(env, data, hint);
}

Reference::~Reference() {
  if (initValue != nullptr && env != nullptr &&
      (bridgeState == nullptr || IsBridgeStateLive(bridgeState, bridgeStateToken))) {
    DeleteReferenceOnOwningThread(env, bridgeState, bridgeStateToken, initValue);
    initValue = nullptr;
  }
  bridgeState = nullptr;
  bridgeStateToken = 0;
  if (data != nullptr && ownsData) {
    free(data);
    data = nullptr;
  }
}

napi_value FunctionReference::defineJSClass(napi_env env) {
  napi_value constructor;
  napi_define_class(env, "FunctionReference", NAPI_AUTO_LENGTH, FunctionReference::constructor,
                    nullptr, 0, nullptr, &constructor);

  napi_value symbolSizeof = jsSymbolFor(env, "sizeof");
  napi_value sizeValue;
  napi_create_int32(env, sizeof(void*), &sizeValue);
  napi_set_property(env, constructor, symbolSizeof, sizeValue);

  napi_value prototype;
  napi_get_named_property(env, constructor, "prototype", &prototype);
  napi_set_property(env, prototype, symbolSizeof, sizeValue);

  return constructor;
}

bool FunctionReference::isInstance(napi_env env, napi_value value) {
  napi_valuetype valueType = napi_undefined;
  napi_typeof(env, value, &valueType);
  if (valueType != napi_object && valueType != napi_function) {
    return false;
  }

  bool hasMarker = false;
  bool hasNativeRef = false;
  if (napi_has_named_property(env, value, kFunctionReferenceMarker, &hasMarker) != napi_ok ||
      !hasMarker ||
      napi_has_named_property(env, value, kFunctionReferenceDataProperty, &hasNativeRef) !=
          napi_ok ||
      !hasNativeRef) {
    return false;
  }

  napi_value marker = nullptr;
  napi_value nativeRef = nullptr;
  bool markerValue = false;
  void* nativeData = nullptr;
  if (napi_get_named_property(env, value, kFunctionReferenceMarker, &marker) != napi_ok ||
      napi_get_value_bool(env, marker, &markerValue) != napi_ok || !markerValue ||
      napi_get_named_property(env, value, kFunctionReferenceDataProperty, &nativeRef) != napi_ok ||
      napi_get_value_external(env, nativeRef, &nativeData) != napi_ok) {
    return false;
  }
  return nativeData != nullptr;
}

FunctionReference* FunctionReference::unwrap(napi_env env, napi_value value) {
  FunctionReference* ref = nullptr;
  if (napi_unwrap(env, value, (void**)&ref) == napi_ok && ref != nullptr) {
    return ref;
  }

  bool hasNativeRef = false;
  napi_has_named_property(env, value, kFunctionReferenceDataProperty, &hasNativeRef);
  if (!hasNativeRef) {
    return nullptr;
  }

  napi_value nativeRefValue;
  if (napi_get_named_property(env, value, kFunctionReferenceDataProperty, &nativeRefValue) !=
      napi_ok) {
    return nullptr;
  }

  void* nativeRef = nullptr;
  if (napi_get_value_external(env, nativeRefValue, &nativeRef) != napi_ok) {
    return nullptr;
  }

  return static_cast<FunctionReference*>(nativeRef);
}

void FunctionReference::finalize(napi_env env, void* data, void* hint) {
  if (PostFinalizer(env, finalizeFunctionReferenceNow, data, hint)) {
    return;
  }

  finalizeFunctionReferenceNow(env, data, hint);
}

FunctionReference::FunctionReference(napi_env env, napi_ref ref) : env(env), ref(ref) {
  bridgeState = ObjCBridgeState::InstanceData(env);
  bridgeStateToken = bridgeState != nullptr ? bridgeState->lifetimeToken : 0;
}

napi_value FunctionReference::constructor(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  size_t argc = 1;
  napi_value argv[1];
  napi_value newTarget = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &jsThis, nullptr);
  napi_get_new_target(env, info, &newTarget);

  napi_value arg;
  if (argc == 0) {
    napi_throw_type_error(env, nullptr, "FunctionReference constructor expects a function");
    return nullptr;
  }
  arg = argv[0];

  napi_valuetype argType = napi_undefined;
  napi_typeof(env, arg, &argType);
  if (argType != napi_function) {
    napi_throw_type_error(env, nullptr, "FunctionReference constructor expects a function");
    return nullptr;
  }

  if (FunctionReference::isInstance(env, arg)) {
    return arg;
  }

  FunctionReference* reference = new FunctionReference(env, make_ref(env, arg));
  napi_value nativeRef;
  napi_create_external(env, reference, nullptr, nullptr, &nativeRef);
  napi_property_descriptor nativeRefProp = {
      .utf8name = kFunctionReferenceDataProperty,
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = nativeRef,
      .attributes = napi_configurable,
      .data = nullptr,
  };
  napi_define_properties(env, arg, 1, &nativeRefProp);
  napi_ref finalizerRef = nullptr;
  napi_add_finalizer(env, arg, reference, FunctionReference::finalize, nullptr, &finalizerRef);

  napi_value marker;
  napi_get_boolean(env, true, &marker);
  napi_property_descriptor markerProp = {
      .utf8name = kFunctionReferenceMarker,
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = marker,
      .attributes = napi_configurable,
      .data = nullptr,
  };
  napi_define_properties(env, arg, 1, &markerProp);

  // Keep function prototype chain untouched to avoid side effects on JS function
  // semantics; expose sizeof directly on the function instance instead.
  napi_value symbolSizeof = jsSymbolFor(env, "sizeof");
  napi_value sizeValue;
  napi_create_int32(env, sizeof(void*), &sizeValue);
  napi_set_property(env, arg, symbolSizeof, sizeValue);

  (void)newTarget;
  return arg;
}

FunctionReference::~FunctionReference() {
  // If closure is already created, it shares the same JS function ref.
  // Clear it there and delete exactly once here.
  if (closure != nullptr && closure->func == ref) {
    closure->func = nullptr;
  }
  if (ref != nullptr && env != nullptr &&
      (bridgeState == nullptr || IsBridgeStateLive(bridgeState, bridgeStateToken))) {
    DeleteReferenceOnOwningThread(env, bridgeState, bridgeStateToken, ref);
    ref = nullptr;
  }
  bridgeState = nullptr;
  bridgeStateToken = 0;
}

void* FunctionReference::getFunctionPointer(MDSectionOffset offset, bool isBlock) {
  if (closure == nullptr) {
    closure = std::make_shared<Closure>(env, ObjCBridgeState::InstanceData(env)->metadata, offset,
                                        isBlock);
    closure->func = ref;
  }

  return closure->fnptr;
}

}  // namespace nativescript
