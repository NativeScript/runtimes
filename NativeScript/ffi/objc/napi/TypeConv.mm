#include "TypeConv.h"
#include "Block.h"
#include "Class.h"
#include "Closure.h"
#include "Interop.h"
#include "JSObject.h"
#include "Metadata.h"
#include "MetadataReader.h"
#include "ObjCBridge.h"
#include "ffi.h"
#include "Struct.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "node_api_util.h"

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#if defined(__has_include)
#if __has_include(<ptrauth.h>)
#include <ptrauth.h>
#endif
#endif
#include <stdbool.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

@interface JSWrapperObjectAssociation : NSObject
+ (void)transferOwnership:(napi_env)env of:(napi_value)value toNative:(id)object;
@end

namespace {

static napi_value findRegisteredClassConstructor(napi_env env, Class cls) {
  if (env == nullptr || cls == nil) {
    return nullptr;
  }

  const char* runtimeName = class_getName(cls);
  if (runtimeName == nullptr || runtimeName[0] == '\0') {
    return nullptr;
  }

  napi_value global = nullptr;
  napi_value classRegistry = nullptr;
  bool hasClassRegistry = false;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_has_named_property(env, global, "__nsConstructorsByObjCClassName",
                              &hasClassRegistry) != napi_ok ||
      !hasClassRegistry ||
      napi_get_named_property(env, global, "__nsConstructorsByObjCClassName",
                              &classRegistry) != napi_ok ||
      classRegistry == nullptr) {
    return nullptr;
  }

  bool hasConstructor = false;
  napi_value constructor = nullptr;
  if (napi_has_named_property(env, classRegistry, runtimeName, &hasConstructor) == napi_ok &&
      hasConstructor &&
      napi_get_named_property(env, classRegistry, runtimeName, &constructor) == napi_ok &&
      constructor != nullptr) {
    return constructor;
  }

  return nullptr;
}

static size_t getBufferElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 1;
  }
}

static bool getJSBufferData(napi_env env, napi_value value, void** data, size_t* byteLength) {
  if (data == nullptr || byteLength == nullptr) {
    return false;
  }

  bool isArrayBuffer = false;
  if (napi_is_arraybuffer(env, value, &isArrayBuffer) == napi_ok && isArrayBuffer) {
    return napi_get_arraybuffer_info(env, value, data, byteLength) == napi_ok;
  }

  bool isTypedArray = false;
  if (napi_is_typedarray(env, value, &isTypedArray) == napi_ok && isTypedArray) {
    napi_typedarray_type type;
    napi_value arrayBuffer;
    size_t byteOffset = 0;
    size_t elementLength = 0;
    if (napi_get_typedarray_info(env, value, &type, &elementLength, data, &arrayBuffer,
                                 &byteOffset) != napi_ok) {
      return false;
    }

    *byteLength = elementLength * getBufferElementSize(type);
    return true;
  }

  bool isDataView = false;
  if (napi_is_dataview(env, value, &isDataView) == napi_ok && isDataView) {
    napi_value arrayBuffer;
    size_t byteOffset = 0;
    return napi_get_dataview_info(env, value, byteLength, data, &arrayBuffer, &byteOffset) ==
           napi_ok;
  }

  return false;
}

struct ActiveObjectConversion {
  napi_env env;
  napi_value value;
};

thread_local std::vector<ActiveObjectConversion> activeObjectConversions;
thread_local napi_env failedObjectConversionEnv = nullptr;

class ScopedObjectConversion {
 public:
  ScopedObjectConversion(napi_env env, napi_value value) {
    for (const auto& active : activeObjectConversions) {
      if (active.env != env) {
        continue;
      }

      bool isSameObject = false;
      status_ = napi_strict_equals(env, active.value, value, &isSameObject);
      if (status_ != napi_ok) {
        return;
      }

      if (isSameObject) {
        failedObjectConversionEnv = env;
        napi_throw_error(
            env, nullptr,
            "Circular JavaScript object graphs cannot be converted to Objective-C collections.");
        return;
      }
    }

    activeObjectConversions.push_back({env, value});
    entered_ = true;
  }

  ~ScopedObjectConversion() {
    if (entered_) {
      activeObjectConversions.pop_back();
    }
  }

  bool entered() const { return entered_; }
  napi_status status() const { return status_; }

 private:
  bool entered_ = false;
  napi_status status_ = napi_ok;
};

static bool hasPendingException(napi_env env) {
  if (failedObjectConversionEnv == env) {
    return true;
  }
  bool pending = false;
  return napi_is_exception_pending(env, &pending) == napi_ok && pending;
}

static uint16_t encodeFloat16(double value) {
  if (std::isnan(value)) {
    return 0x7e00;
  }

  if (std::isinf(value)) {
    return std::signbit(value) ? 0xfc00 : 0x7c00;
  }

  union {
    float f;
    uint32_t bits;
  } input = {static_cast<float>(value)};

  const uint32_t sign = (input.bits >> 16) & 0x8000;
  uint32_t exponent = (input.bits >> 23) & 0xff;
  uint32_t mantissa = input.bits & 0x007fffff;

  if (exponent == 0) {
    return static_cast<uint16_t>(sign);
  }

  int32_t halfExponent = static_cast<int32_t>(exponent) - 127 + 15;
  if (halfExponent >= 0x1f) {
    return static_cast<uint16_t>(sign | 0x7c00);
  }

  if (halfExponent <= 0) {
    if (halfExponent < -10) {
      return static_cast<uint16_t>(sign);
    }

    mantissa |= 0x00800000;
    const uint32_t shift = static_cast<uint32_t>(14 - halfExponent);
    uint32_t halfMantissa = mantissa >> shift;
    if (((mantissa >> (shift - 1)) & 1u) != 0) {
      halfMantissa += 1;
    }
    return static_cast<uint16_t>(sign | halfMantissa);
  }

  uint32_t halfMantissa = mantissa >> 13;
  if ((mantissa & 0x00001000) != 0) {
    halfMantissa += 1;
    if ((halfMantissa & 0x00000400) != 0) {
      halfMantissa = 0;
      halfExponent += 1;
      if (halfExponent >= 0x1f) {
        return static_cast<uint16_t>(sign | 0x7c00);
      }
    }
  }

  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(halfExponent) << 10) |
                               (halfMantissa & 0x03ff));
}

static double decodeFloat16(uint16_t bits) {
  const uint32_t sign = (bits & 0x8000u) << 16;
  const uint32_t exponent = (bits >> 10) & 0x1fu;
  const uint32_t mantissa = bits & 0x03ffu;

  union {
    uint32_t bits;
    float f;
  } output = {0};

  if (exponent == 0) {
    if (mantissa == 0) {
      output.bits = sign;
      return static_cast<double>(output.f);
    }

    uint32_t normalizedMantissa = mantissa;
    int32_t normalizedExponent = -14;
    while ((normalizedMantissa & 0x0400u) == 0) {
      normalizedMantissa <<= 1;
      normalizedExponent -= 1;
    }
    normalizedMantissa &= 0x03ffu;
    output.bits =
        sign | (static_cast<uint32_t>(normalizedExponent + 127) << 23) | (normalizedMantissa << 13);
    return static_cast<double>(output.f);
  }

  if (exponent == 0x1fu) {
    output.bits = sign | 0x7f800000u | (mantissa << 13);
    return static_cast<double>(output.f);
  }

  output.bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
  return static_cast<double>(output.f);
}

static id resolveCachedHandleObject(napi_env env, void* handle) {
  if (env == nullptr || handle == nullptr) {
    return nil;
  }

  auto bridgeState = nativescript::ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr) {
    return nil;
  }

  napi_value cachedValue = bridgeState->getCachedHandleObject(env, handle);
  if (cachedValue == nullptr) {
    return nil;
  }

  void* wrapped = nullptr;
  if (napi_unwrap(env, cachedValue, &wrapped) == napi_ok && wrapped != nullptr) {
    bridgeState->cacheRoundTripObject(env, static_cast<id>(wrapped), cachedValue);
    return static_cast<id>(wrapped);
  }

  bool hasNativePointer = false;
  if (napi_has_named_property(env, cachedValue, "__ns_native_ptr", &hasNativePointer) == napi_ok &&
      hasNativePointer) {
    napi_value nativePointerValue = nullptr;
    if (napi_get_named_property(env, cachedValue, "__ns_native_ptr", &nativePointerValue) ==
        napi_ok) {
      if (nativescript::Pointer::isInstance(env, nativePointerValue)) {
        nativescript::Pointer* pointer = nativescript::Pointer::unwrap(env, nativePointerValue);
        if (pointer != nullptr && pointer->data != nullptr) {
          bridgeState->cacheRoundTripObject(env, static_cast<id>(pointer->data), cachedValue);
          return static_cast<id>(pointer->data);
        }
      } else {
        void* nativePointer = nullptr;
        if (napi_get_value_external(env, nativePointerValue, &nativePointer) == napi_ok &&
            nativePointer != nullptr) {
          bridgeState->cacheRoundTripObject(env, static_cast<id>(nativePointer), cachedValue);
          return static_cast<id>(nativePointer);
        }
      }
    }
  }

  return nil;
}

}  // namespace

namespace nativescript {

namespace {
constexpr const char* kProtocolSuffix = "Protocol";

NSData* createNSDataWrapper(napi_env env, napi_value value, ObjCBridgeState* bridgeState) {
  void* data = nullptr;
  size_t byteLength = 0;
  if (!getJSBufferData(env, value, &data, &byteLength)) {
    return nil;
  }

  NSData* wrappedData = [NSData dataWithBytes:data length:byteLength];
  if (wrappedData == nil) {
    return nil;
  }

  if (bridgeState != nullptr && bridgeState->hasRoundTripCacheFrame()) {
    bridgeState->cacheRoundTripObject(env, wrappedData, value);
  }

  return wrappedData;
}

inline size_t alignUp(size_t value, size_t alignment) {
  if (alignment == 0) {
    return value;
  }
  return ((value + alignment - 1) / alignment) * alignment;
}

inline uintptr_t normalizeRuntimePointer(uintptr_t ptr) {
#if INTPTR_MAX == INT64_MAX
  return ptr & 0x0000FFFFFFFFFFFFULL;
#else
  return ptr;
#endif
}

inline bool isKindOfClassFast(id obj, Class expectedClass) {
  if (obj == nil || expectedClass == Nil) {
    return false;
  }

  return [obj isKindOfClass:expectedClass];
}

bool stripProtocolSuffix(const char* name, std::string* out) {
  if (name == nullptr || out == nullptr) {
    return false;
  }

  const size_t nameLen = std::strlen(name);
  const size_t suffixLen = std::strlen(kProtocolSuffix);
  if (nameLen <= suffixLen) {
    return false;
  }

  if (std::strcmp(name + (nameLen - suffixLen), kProtocolSuffix) != 0) {
    return false;
  }

  *out = std::string(name, nameLen - suffixLen);
  return !out->empty();
}

bool protocolNamesMatch(const char* metadataName, const char* runtimeName) {
  if (metadataName == nullptr || runtimeName == nullptr) {
    return false;
  }

  if (std::strcmp(metadataName, runtimeName) == 0) {
    return true;
  }

  std::string metadataBase(metadataName);
  std::string runtimeBase(runtimeName);
  stripProtocolSuffix(metadataName, &metadataBase);
  stripProtocolSuffix(runtimeName, &runtimeBase);

  return metadataBase == runtimeBase;
}

MDSectionOffset findProtocolMetadataOffset(MDMetadataReader* metadata, const char* protocolName) {
  if (metadata == nullptr || protocolName == nullptr) {
    return MD_SECTION_OFFSET_NULL;
  }

  MDSectionOffset offset = metadata->protocolsOffset;
  while (offset < metadata->classesOffset) {
    MDSectionOffset originalOffset = offset;

    auto nameOffset = metadata->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    bool next = (nameOffset & mdSectionOffsetNext) != 0;
    nameOffset &= ~mdSectionOffsetNext;

    auto name = metadata->resolveString(nameOffset);
    if (protocolNamesMatch(name, protocolName)) {
      return originalOffset;
    }

    while (next) {
      auto protocolImpl = metadata->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      next = (protocolImpl & mdSectionOffsetNext) != 0;
    }

    next = true;
    while (next) {
      auto flags = metadata->getMemberFlag(offset);
      next = (flags & mdMemberNext) != 0;
      offset += sizeof(flags);

      if (flags == mdMemberFlagNull) {
        break;
      }

      if ((flags & mdMemberProperty) != 0) {
        bool readonly = (flags & mdMemberReadonly) != 0;
        offset += sizeof(MDSectionOffset);  // name
        offset += sizeof(MDSectionOffset);  // getter selector
        offset += sizeof(MDSectionOffset);  // getter signature
        if (!readonly) {
          offset += sizeof(MDSectionOffset);  // setter selector
          offset += sizeof(MDSectionOffset);  // setter signature
        }
      } else {
        offset += sizeof(MDSectionOffset);  // selector
        offset += sizeof(MDSectionOffset);  // signature
      }
    }
  }

  return MD_SECTION_OFFSET_NULL;
}
}  // namespace

// Forward declaration
class StructTypeConv;

struct StructTypeCaches {
  std::unordered_set<MDSectionOffset> processingStructs;
  std::unordered_set<std::string> processingEncodingStructs;
  std::unordered_map<MDSectionOffset, ffi_type*> forwardDeclaredStructs;
  std::unordered_map<std::string, ffi_type*> forwardDeclaredEncodingStructs;
  std::unordered_map<MDSectionOffset, std::shared_ptr<TypeConv>> structTypes;
  std::unordered_map<std::string, std::shared_ptr<TypeConv>> encodingStructTypes;
};

thread_local std::unordered_map<napi_env, StructTypeCaches> structTypeCachesByEnv;

inline StructTypeCaches& structTypeCaches(napi_env env) {
  return structTypeCachesByEnv[env];
}

ffi_type* typeFromStruct(napi_env env, const char** encoding, bool* ownsType,
                         std::vector<std::shared_ptr<TypeConv>>* retainedElementTypes) {
  auto& caches = structTypeCaches(env);
  auto& processingEncodingStructs = caches.processingEncodingStructs;
  auto& forwardDeclaredEncodingStructs = caches.forwardDeclaredEncodingStructs;
  *ownsType = false;
  // Extract struct name for cycle detection
  std::string structname;
  const char* nameStart = *encoding + 1;  // skip '{'
  const char* c = nameStart;
  while (*c != '\0' && *c != '=') {
    structname += *c;
    c++;
  }
  if (*c != '=') {
    // Malformed struct encoding. Advance to the end of this token and
    // fallback to pointer conversion to avoid reading past the buffer.
    while (**encoding != '\0' && **encoding != '}') {
      (*encoding)++;
    }
    if (**encoding == '}') {
      (*encoding)++;
    }
    return &ffi_type_pointer;
  }

  // Reuse a single placeholder when a recursive layout references the same
  // struct more than once.
  auto existingForwardIt = forwardDeclaredEncodingStructs.find(structname);
  if (existingForwardIt != forwardDeclaredEncodingStructs.end()) {
    (*encoding)++;  // skip '{'
    while (**encoding != '\0' && **encoding != '}') {
      (*encoding)++;
    }
    if (**encoding == '}') {
      (*encoding)++;
    }
    return existingForwardIt->second;
  }

  // Check if we're already processing this struct (cycle detection)
  if (processingEncodingStructs.find(structname) != processingEncodingStructs.end()) {
    // Create a forward declaration placeholder
    ffi_type* forwardType = new ffi_type;
    forwardType->type = FFI_TYPE_STRUCT;
    forwardType->size = 0;
    forwardType->alignment = 0;
    forwardType->elements = nullptr;

    // Cache this forward declaration for later resolution
    forwardDeclaredEncodingStructs[structname] = forwardType;

    // Skip the struct encoding
    (*encoding)++;  // skip '{'
    while (**encoding != '\0' && **encoding != '}') {
      (*encoding)++;
    }
    if (**encoding == '}') {
      (*encoding)++;  // skip '}'
    }

    return forwardType;
  }

  // Mark this struct as being processed
  processingEncodingStructs.insert(structname);

  ffi_type* type = new ffi_type;
  type->type = FFI_TYPE_STRUCT;
  type->size = 0;
  type->alignment = 0;
  type->elements = nullptr;

  std::vector<ffi_type*> elements;

  (*encoding)++;  // skip '{'

  while (**encoding != '\0' && **encoding != '=') {
    (*encoding)++;
  }  // skip name
  if (**encoding == '\0') {
    processingEncodingStructs.erase(structname);
    delete type;
    return &ffi_type_pointer;
  }

  (*encoding)++;  // skip '='

  while (**encoding != '\0' && **encoding != '}') {
    auto elementType = TypeConv::Make(env, encoding);
    elements.push_back(elementType->type);
    retainedElementTypes->push_back(std::move(elementType));
  }

  if (**encoding == '}') {
    (*encoding)++;  // skip '}'
  }

  type->elements = (ffi_type**)malloc(sizeof(ffi_type*) * (elements.size() + 1));
  for (int i = 0; i < elements.size(); i++) {
    type->elements[i] = elements[i];
  }
  // null-terminate the array
  type->elements[elements.size()] = nullptr;

  // If this was a forward declaration, update it with the real layout
  auto resolvedForwardIt = forwardDeclaredEncodingStructs.find(structname);
  if (resolvedForwardIt != forwardDeclaredEncodingStructs.end()) {
    ffi_type* forwardType = resolvedForwardIt->second;
    forwardType->type = type->type;
    forwardType->size = type->size;
    forwardType->alignment = type->alignment;
    forwardType->elements = type->elements;

    // Clean up the temporary type and use the forward declaration
    delete type;
    type = forwardType;
    forwardDeclaredEncodingStructs.erase(resolvedForwardIt);
  }

  // Remove from processing set
  processingEncodingStructs.erase(structname);
  *ownsType = true;

  return type;
}

ffi_type* typeFromStruct(napi_env env, MDMetadataReader* reader, MDSectionOffset structOffset,
                         bool isUnion, bool* ownsType,
                         std::vector<std::shared_ptr<TypeConv>>* retainedElementTypes) {
  auto& caches = structTypeCaches(env);
  auto& processingStructs = caches.processingStructs;
  auto& forwardDeclaredStructs = caches.forwardDeclaredStructs;
  *ownsType = false;
  auto existingForwardIt = forwardDeclaredStructs.find(structOffset);
  if (existingForwardIt != forwardDeclaredStructs.end()) {
    return existingForwardIt->second;
  }

  // Check if we're already processing this struct (cycle detection)
  if (processingStructs.find(structOffset) != processingStructs.end()) {
    // Create a forward declaration placeholder
    ffi_type* forwardType = new ffi_type;
    forwardType->type = FFI_TYPE_STRUCT;
    forwardType->size = 0;
    forwardType->alignment = 0;
    forwardType->elements = nullptr;

    // Cache this forward declaration for later resolution
    forwardDeclaredStructs[structOffset] = forwardType;
    return forwardType;
  }

  // Mark this struct as being processed
  processingStructs.insert(structOffset);

  ffi_type* type = new ffi_type;
  type->type = FFI_TYPE_STRUCT;
  type->size = 0;
  type->alignment = 0;
  type->elements = nullptr;

  MDSectionOffset nameOffset = reader->getOffset(structOffset);
  auto name = reader->resolveString(nameOffset);
  bool next = true;
  MDSectionOffset currentOffset = structOffset + sizeof(MDSectionOffset);  // skip name
  currentOffset += sizeof(uint16_t);                                       // skip size

  std::vector<ffi_type*> elements;

  while (next) {
    nameOffset = reader->getOffset(currentOffset);
    next = nameOffset & mdSectionOffsetNext;
    nameOffset &= ~mdSectionOffsetNext;
    if (nameOffset == MD_SECTION_OFFSET_NULL) {
      break;
    }
    currentOffset += sizeof(MDSectionOffset);         // skip name
    if (!isUnion) currentOffset += sizeof(uint16_t);  // skip offset
    auto elementType = TypeConv::Make(env, reader, &currentOffset, 1);
    elements.push_back(elementType->type);
    retainedElementTypes->push_back(std::move(elementType));
  }

  type->elements = (ffi_type**)malloc(sizeof(ffi_type*) * (elements.size() + 1));
  for (int i = 0; i < elements.size(); i++) {
    type->elements[i] = elements[i];
  }
  // null-terminate the array
  type->elements[elements.size()] = nullptr;

  // If this was a forward declaration, update it with the real layout
  auto resolvedForwardIt = forwardDeclaredStructs.find(structOffset);
  if (resolvedForwardIt != forwardDeclaredStructs.end()) {
    ffi_type* forwardType = resolvedForwardIt->second;
    forwardType->type = type->type;
    forwardType->size = type->size;
    forwardType->alignment = type->alignment;
    forwardType->elements = type->elements;

    // Clean up the temporary type and use the forward declaration
    delete type;
    type = forwardType;
    forwardDeclaredStructs.erase(resolvedForwardIt);
  }

  // Remove from processing set
  processingStructs.erase(structOffset);
  *ownsType = true;

  return type;
}

static inline size_t getTypedArrayUnitLength(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 0;
  }
}

class VoidTypeConv : public TypeConv {
 public:
  VoidTypeConv() {
    type = &ffi_type_void;
    kind = mdTypeVoid;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    napi_get_null(env, &result);
    return result;
  }

  void encode(std::string* encoding) override { *encoding += "v"; }
};

static const std::shared_ptr<VoidTypeConv> voidTypeConv = std::make_shared<VoidTypeConv>();

class SCharTypeConv : public TypeConv {
 public:
  SCharTypeConv() {
    type = &ffi_type_schar;
    kind = mdTypeChar;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    int8_t raw = *(int8_t*)value;
    if (raw == 0 || raw == 1) {
      napi_value result;
      napi_get_boolean(env, raw == 1, &result);
      return result;
    }

    napi_value result;
    napi_create_int32(env, raw, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    int32_t val;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_int32(env, value, &val);
    *(int8_t*)result = val;
  }

  void encode(std::string* encoding) override { *encoding += "c"; }
};

static const std::shared_ptr<SCharTypeConv> scharTypeConv = std::make_shared<SCharTypeConv>();

class UCharTypeConv : public TypeConv {
 public:
  UCharTypeConv() {
    type = &ffi_type_uchar;
    kind = mdTypeUChar;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    uint8_t raw = *(uint8_t*)value;
    if (raw == 0 || raw == 1) {
      napi_value result;
      napi_get_boolean(env, raw == 1, &result);
      return result;
    }

    napi_value result;
    napi_create_uint32(env, raw, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    uint32_t val;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_uint32(env, value, &val);
    *(uint8_t*)result = val;
  }

  void encode(std::string* encoding) override { *encoding += "C"; }
};

static const std::shared_ptr<UCharTypeConv> ucharTypeConv = std::make_shared<UCharTypeConv>();

class UInt8TypeConv : public TypeConv {
 public:
  UInt8TypeConv() {
    type = &ffi_type_uint8;
    kind = mdTypeUInt8;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    uint8_t raw = *(uint8_t*)value;
    if (raw == 0 || raw == 1) {
      napi_value result;
      napi_get_boolean(env, raw == 1, &result);
      return result;
    }

    napi_value result;
    napi_create_uint32(env, raw, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    uint32_t val;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_uint32(env, value, &val);
    *(uint8_t*)result = val;
  }

  void encode(std::string* encoding) override { *encoding += "C"; }
};

static const std::shared_ptr<UInt8TypeConv> uint8TypeConv = std::make_shared<UInt8TypeConv>();

class SInt16TypeConv : public TypeConv {
 public:
  SInt16TypeConv() {
    type = &ffi_type_sshort;
    kind = mdTypeSShort;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    napi_create_int32(env, *(int16_t*)value, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    int32_t val;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_int32(env, value, &val);
    *(int16_t*)result = val;
  }

  void encode(std::string* encoding) override { *encoding += "s"; }
};

static const std::shared_ptr<SInt16TypeConv> sint16TypeConv = std::make_shared<SInt16TypeConv>();

class UInt16TypeConv : public TypeConv {
 public:
  UInt16TypeConv() {
    type = &ffi_type_ushort;
    kind = mdTypeUShort;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    napi_create_uint32(env, *(uint16_t*)value, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    napi_valuetype valueType = napi_undefined;
    napi_typeof(env, value, &valueType);

    if (valueType == napi_string) {
      size_t strLen = 0;
      napi_get_value_string_utf16(env, value, nullptr, 0, &strLen);
      if (strLen != 1) {
        napi_throw_type_error(env, nullptr, "Expected a single-character string.");
        *(uint16_t*)result = 0;
        return;
      }

      char16_t chars[2] = {0, 0};
      napi_get_value_string_utf16(env, value, chars, 2, &strLen);
      *(uint16_t*)result = static_cast<uint16_t>(chars[0]);
      return;
    }

    uint32_t val = 0;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_uint32(env, value, &val);
    *(uint16_t*)result = val;
  }

  void encode(std::string* encoding) override { *encoding += "S"; }
};

static const std::shared_ptr<UInt16TypeConv> uint16TypeConv = std::make_shared<UInt16TypeConv>();

// unichar/UniChar (mdTypeUnichar): u16 width, but projected to JS as a
// single-character string for any code unit — not just printable ASCII.
class UnicharTypeConv : public UInt16TypeConv {
 public:
  UnicharTypeConv() { kind = mdTypeUnichar; }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    char16_t unit = *(char16_t*)value;
    napi_value result;
    napi_create_string_utf16(env, &unit, 1, &result);
    return result;
  }
};

static const std::shared_ptr<UnicharTypeConv> unicharTypeConv = std::make_shared<UnicharTypeConv>();

class SInt32TypeConv : public TypeConv {
 public:
  SInt32TypeConv() {
    type = &ffi_type_sint;
    kind = mdTypeSInt;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    napi_create_int32(env, *(int32_t*)value, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    int32_t val;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_int32(env, value, &val);
    *(int32_t*)result = val;
  }

  void encode(std::string* encoding) override { *encoding += "i"; }
};

static const std::shared_ptr<SInt32TypeConv> sint32TypeConv = std::make_shared<SInt32TypeConv>();

class UInt32TypeConv : public TypeConv {
 public:
  UInt32TypeConv() {
    type = &ffi_type_uint;
    kind = mdTypeUInt;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    napi_create_uint32(env, *(uint32_t*)value, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    uint32_t val;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_uint32(env, value, &val);
    *(uint32_t*)result = val;
  }

  void encode(std::string* encoding) override { *encoding += "I"; }
};

static const std::shared_ptr<UInt32TypeConv> uint32TypeConv = std::make_shared<UInt32TypeConv>();

class SInt64TypeConv : public TypeConv {
 public:
  SInt64TypeConv() {
    type = &ffi_type_sint64;
    kind = mdTypeSInt64;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    int64_t val = *(int64_t*)value;
    constexpr int64_t kMaxSafeInteger = 9007199254740991LL;
    if (val > kMaxSafeInteger || val < -kMaxSafeInteger) {
      napi_create_bigint_int64(env, val, &result);
    } else {
      napi_create_int64(env, val, &result);
    }
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    napi_valuetype valuetype;
    napi_typeof(env, value, &valuetype);

    switch (valuetype) {
      case napi_number:
        napi_get_value_int64(env, value, (int64_t*)result);
        break;
      case napi_bigint: {
        bool lossless;
        napi_get_value_bigint_int64(env, value, (int64_t*)result, &lossless);
        break;
      }
      case napi_undefined:
      case napi_null:
        *(int64_t*)result = 0;
        break;
      case napi_string:
        *(int64_t*)result = 0;
        break;
      default:
        napi_throw_type_error(env, nullptr, "Expected a number or bigint");
        break;
    }
  }

  void encode(std::string* encoding) override { *encoding += "q"; }
};

static const std::shared_ptr<SInt64TypeConv> sint64TypeConv = std::make_shared<SInt64TypeConv>();

class UInt64TypeConv : public TypeConv {
 public:
  UInt64TypeConv() {
    type = &ffi_type_uint64;
    kind = mdTypeUInt64;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    uint64_t val = *(uint64_t*)value;
    constexpr uint64_t kMaxSafeInteger = 9007199254740991ULL;
    if (val > kMaxSafeInteger) {
      napi_create_bigint_uint64(env, val, &result);
    } else {
      napi_create_int64(env, static_cast<int64_t>(val), &result);
    }
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    napi_valuetype valuetype;
    napi_typeof(env, value, &valuetype);

    switch (valuetype) {
      case napi_number:
        napi_get_value_int64(env, value, (int64_t*)result);
        break;
      case napi_bigint: {
        bool lossless;
        napi_get_value_bigint_uint64(env, value, (uint64_t*)result, &lossless);
        break;
      }
      case napi_undefined:
      case napi_null:
        *(int64_t*)result = 0;
        break;
      default:
        napi_throw_type_error(env, nullptr, "Expected a number or bigint");
        break;
    }
  }

  void encode(std::string* encoding) override { *encoding += "Q"; }
};

static const std::shared_ptr<UInt64TypeConv> uint64TypeConv = std::make_shared<UInt64TypeConv>();

class UInt128TypeConv : public TypeConv {
 private:
  ffi_type _type = {.size = 0,
                    .alignment = 0,
                    .type = FFI_TYPE_STRUCT,
                    .elements = (ffi_type*[]){
                        &ffi_type_uint64,
                        &ffi_type_uint64,
                        nullptr,
                    }};

 public:
  UInt128TypeConv() {
    type = &_type;
    kind = mdTypeUInt128;
  }

  // TODO

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    uint64_t val = *(uint64_t*)value;
    napi_create_int64(env, (int64_t)val, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    napi_valuetype valuetype;
    napi_typeof(env, value, &valuetype);

    switch (valuetype) {
      case napi_number:
        napi_get_value_int64(env, value, (int64_t*)result);
        break;
      case napi_bigint: {
        bool lossless;
        napi_get_value_bigint_uint64(env, value, (uint64_t*)result, &lossless);
        break;
      }
      default:
        napi_throw_type_error(env, nullptr, "Expected a number or bigint");
        break;
    }
  }
};

static const std::shared_ptr<UInt128TypeConv> uint128TypeConv = std::make_shared<UInt128TypeConv>();

class Float32TypeConv : public TypeConv {
 public:
  Float32TypeConv() {
    type = &ffi_type_float;
    kind = mdTypeFloat;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    napi_create_double(env, *(float*)value, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    double val;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_double(env, value, &val);
    *(float*)result = val;
  }

  void encode(std::string* encoding) override { *encoding += "f"; }
};

static const std::shared_ptr<Float32TypeConv> float32TypeConv = std::make_shared<Float32TypeConv>();

class Float16TypeConv : public TypeConv {
 public:
  Float16TypeConv() {
    type = &ffi_type_uint16;
    kind = mdTypeF16;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    napi_create_double(env, decodeFloat16(*(uint16_t*)value), &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    double val = 0;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_double(env, value, &val);
    *(uint16_t*)result = encodeFloat16(val);
  }

  void encode(std::string* encoding) override { *encoding += "H"; }
};

static const std::shared_ptr<Float16TypeConv> float16TypeConv = std::make_shared<Float16TypeConv>();

class Float64TypeConv : public TypeConv {
 public:
  Float64TypeConv() {
    type = &ffi_type_double;
    kind = mdTypeDouble;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    napi_create_double(env, *(double*)value, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    double val;
    napi_coerce_to_number(env, value, &value);
    napi_get_value_double(env, value, &val);
    if (std::isnan(val) || std::isinf(val)) {
      val = 0.0;
    }
    *(double*)result = val;
  }

  void encode(std::string* encoding) override { *encoding += "d"; }
};

static const std::shared_ptr<Float64TypeConv> float64TypeConv = std::make_shared<Float64TypeConv>();

class BoolTypeConv : public TypeConv {
 public:
  BoolTypeConv() {
    type = &ffi_type_uint8;
    kind = mdTypeBool;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    uint8_t raw = *(uint8_t*)value;
    if (raw == 0 || raw == 1) {
      napi_value result;
      napi_get_boolean(env, raw == 1, &result);
      return result;
    }

    napi_value result;
    napi_create_uint32(env, raw, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    napi_valuetype valueType = napi_undefined;
    napi_typeof(env, value, &valueType);

    if (valueType == napi_number) {
      uint32_t val = 0;
      napi_coerce_to_number(env, value, &value);
      napi_get_value_uint32(env, value, &val);
      *(uint8_t*)result = static_cast<uint8_t>(val);
      return;
    }

    if (valueType == napi_bigint) {
      uint64_t val = 0;
      bool lossless = false;
      napi_get_value_bigint_uint64(env, value, &val, &lossless);
      *(uint8_t*)result = static_cast<uint8_t>(val);
      return;
    }

    bool val = false;
    napi_coerce_to_bool(env, value, &value);
    napi_get_value_bool(env, value, &val);
    *(uint8_t*)result = static_cast<uint8_t>(val ? 1 : 0);
  }

  void encode(std::string* encoding) override { *encoding += "B"; }
};

static const std::shared_ptr<BoolTypeConv> boolTypeConv = std::make_shared<BoolTypeConv>();

class PointerTypeConv : public TypeConv {
 public:
  std::shared_ptr<TypeConv> pointeeType = nullptr;

  PointerTypeConv() {
    type = &ffi_type_pointer;
    kind = mdTypePointer;
  }

  PointerTypeConv(std::shared_ptr<TypeConv> pointeeType) : pointeeType(pointeeType) {
    type = &ffi_type_pointer;
    kind = mdTypePointer;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    void* raw = *((void**)value);
    if (raw == nullptr) {
      napi_value nullValue;
      napi_get_null(env, &nullValue);
      return nullValue;
    }

    auto normalizePtr = [](void* ptr) -> uintptr_t {
#if INTPTR_MAX == INT64_MAX
      // Objective-C pointers may carry auth/tag bits on some runtimes.
      // Compare using canonical lower bits for stable lookups.
      return reinterpret_cast<uintptr_t>(ptr) & 0x0000FFFFFFFFFFFFULL;
#else
      return reinterpret_cast<uintptr_t>(ptr);
#endif
    };

    auto bridgeState = ObjCBridgeState::InstanceData(env);
    if (bridgeState != nullptr) {
      auto classIt = bridgeState->mdClassesByPointer.find((Class)raw);
      if (classIt != bridgeState->mdClassesByPointer.end()) {
        auto cls = bridgeState->getClass(env, classIt->second);
        if (cls != nullptr) {
          return get_ref_value(env, cls->constructor);
        }
      } else {
        const uintptr_t rawNormalized = normalizePtr(raw);
        for (const auto& entry : bridgeState->mdClassesByPointer) {
          if (normalizePtr((void*)entry.first) != rawNormalized) {
            continue;
          }

          auto cls = bridgeState->getClass(env, entry.second);
          if (cls != nullptr) {
            return get_ref_value(env, cls->constructor);
          }
        }
      }

      auto protocolIt = bridgeState->mdProtocolsByPointer.find((Protocol*)raw);
      if (protocolIt != bridgeState->mdProtocolsByPointer.end()) {
        auto proto = bridgeState->getProtocol(env, protocolIt->second);
        if (proto != nullptr) {
          return get_ref_value(env, proto->constructor);
        }
      } else {
        const uintptr_t rawNormalized = normalizePtr(raw);
        for (const auto& entry : bridgeState->mdProtocolsByPointer) {
          if (normalizePtr((void*)entry.first) != rawNormalized) {
            continue;
          }

          auto proto = bridgeState->getProtocol(env, entry.second);
          if (proto != nullptr) {
            return get_ref_value(env, proto->constructor);
          }
        }

        // Some protocol pointers come from compile-time @protocol() references
        // and don't always match objc_getProtocol() pointer identity.
        // Resolve them by scanning runtime protocol list and matching by address.
        unsigned int protocolCount = 0;
        Protocol** protocols = objc_copyProtocolList(&protocolCount);
        if (protocols != nullptr) {
          for (unsigned int i = 0; i < protocolCount; i++) {
            Protocol* runtimeProto = protocols[i];
            if (normalizePtr((void*)runtimeProto) != rawNormalized) {
              continue;
            }

            const char* runtimeName = protocol_getName(runtimeProto);
            MDSectionOffset metadataOffset =
                findProtocolMetadataOffset(bridgeState->metadata, runtimeName);
            if (metadataOffset != MD_SECTION_OFFSET_NULL) {
              bridgeState->registerProtocolMetadata(runtimeProto, metadataOffset);
              auto proto = bridgeState->getProtocol(env, metadataOffset);
              bridgeState->registerRuntimeProtocol(proto, runtimeProto);
              if (proto != nullptr) {
                ::free(protocols);
                return get_ref_value(env, proto->constructor);
              }
            }

            break;
          }
          ::free(protocols);
        }
      }
    }

    if (pointeeType != nullptr && pointeeType->kind != mdTypeVoid) {
      napi_value referenceValue = Reference::create(env, pointeeType, raw, false);
      if (referenceValue != nullptr) {
        return referenceValue;
      }
    }

    return Pointer::create(env, raw);
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    void** res = (void**)result;

    napi_valuetype type;
    napi_typeof(env, value, &type);

    switch (type) {
      case napi_null:
      case napi_undefined:
        *res = nullptr;
        return;

      case napi_bigint: {
        uint64_t val = 0;
        bool lossless = false;
        NAPI_GUARD(napi_get_value_bigint_uint64(env, value, &val, &lossless)) {
          NAPI_THROW_LAST_ERROR
          *res = nullptr;
          return;
        }
        *res = (void*)val;
        return;
      }

      case napi_string: {
        size_t len = 0;
        NAPI_GUARD(napi_get_value_string_utf8(env, value, nullptr, len, &len)) {
          NAPI_THROW_LAST_ERROR
          return;
        }

        char* str = (char*)malloc(len + 1);

        NAPI_GUARD(napi_get_value_string_utf8(env, value, str, len + 1, &len)) {
          NAPI_THROW_LAST_ERROR
          ::free(str);
          return;
        }

        str[len] = '\0';

        bool shouldCreateCFString =
            pointeeType != nullptr && (pointeeType->kind == mdTypeNSStringObject ||
                                       pointeeType->kind == mdTypeNSMutableStringObject);

        if (shouldCreateCFString) {
          CFStringRef cfStr =
              CFStringCreateWithCString(kCFAllocatorDefault, str, kCFStringEncodingUTF8);
          ::free(str);
          *res = (void*)cfStr;
          *shouldFree = true;
          *shouldFreeAny = true;
        } else {
          *res = (void*)str;
          *shouldFree = true;
          *shouldFreeAny = true;
        }
        return;
      }

      case napi_external: {
        NAPI_GUARD(napi_get_value_external(env, value, res)) {
          NAPI_THROW_LAST_ERROR
          *res = nullptr;
          return;
        }
        return;
      }

      case napi_object: {
        auto bridgeState = ObjCBridgeState::InstanceData(env);
        if (bridgeState != nullptr) {
          id bridgedType = nil;
          if (bridgeState->tryResolveBridgedTypeConstructor(env, value, &bridgedType) &&
              bridgedType != nil) {
            *res = (void*)bridgedType;
            return;
          }
        }
        if (Pointer::isInstance(env, value)) {
          Pointer* ptr = Pointer::unwrap(env, value);
          *res = ptr->data;
          return;
        }

        if (Reference::isInstance(env, value)) {
          Reference* ref = Reference::unwrap(env, value);
          if (ref == nullptr) {
            napi_throw_error(env, nullptr, "Invalid Reference");
            *res = nullptr;
            return;
          }
          if (ref->data == nullptr) {
            std::shared_ptr<TypeConv> resolvedType = pointeeType;
            if (resolvedType == nullptr) {
              resolvedType = ref->type;
            }

            napi_value pendingInitValue = Reference::getInitValue(env, value, ref);
            napi_valuetype pendingInitType = napi_undefined;
            if (pendingInitValue != nullptr) {
              napi_typeof(env, pendingInitValue, &pendingInitType);
            }
            if (resolvedType == nullptr && pendingInitValue != nullptr) {
              if (pendingInitValue != nullptr) {
                napi_valuetype initType = napi_undefined;
                if (napi_typeof(env, pendingInitValue, &initType) == napi_ok) {
                  auto makeStructType = [&](StructInfo* info) -> std::shared_ptr<TypeConv> {
                    if (info == nullptr || info->name == nullptr) {
                      return nullptr;
                    }

                    std::string encoding = "{";
                    encoding += info->name;
                    encoding += "=";
                    for (const auto& field : info->fields) {
                      if (field.type == nullptr) {
                        return nullptr;
                      }
                      field.type->encode(&encoding);
                    }
                    encoding += "}";

                    const char* encodingPtr = encoding.c_str();
                    return TypeConv::Make(env, &encodingPtr);
                  };

                  if (initType == napi_object) {
                    if (StructObject::isInstance(env, pendingInitValue)) {
                      StructObject* structObj = StructObject::unwrap(env, pendingInitValue);
                      if (structObj != nullptr) {
                        resolvedType = makeStructType(structObj->info);
                      }
                    }

                    if (resolvedType == nullptr) {
                      auto bridgeState = ObjCBridgeState::InstanceData(env);
                      if (bridgeState != nullptr) {
                        bool isArray = false;
                        bool isTypedArray = false;
                        bool isArrayBuffer = false;
                        bool isDataView = false;
                        napi_is_array(env, pendingInitValue, &isArray);
                        napi_is_typedarray(env, pendingInitValue, &isTypedArray);
                        napi_is_arraybuffer(env, pendingInitValue, &isArrayBuffer);
                        napi_is_dataview(env, pendingInitValue, &isDataView);
                        if (!isArray && !isTypedArray && !isArrayBuffer && !isDataView) {
                          napi_value propertyNames = nullptr;
                          if (napi_get_property_names(env, pendingInitValue, &propertyNames) ==
                                  napi_ok &&
                              propertyNames != nullptr) {
                            uint32_t propertyCount = 0;
                            napi_get_array_length(env, propertyNames, &propertyCount);
                            std::unordered_set<std::string> keys;
                            std::unordered_map<std::string, bool> keyIsInteger;
                            keys.reserve(propertyCount);
                            for (uint32_t i = 0; i < propertyCount; i++) {
                              napi_value keyValue = nullptr;
                              if (napi_get_element(env, propertyNames, i, &keyValue) != napi_ok ||
                                  keyValue == nullptr) {
                                continue;
                              }
                              napi_valuetype keyType = napi_undefined;
                              if (napi_typeof(env, keyValue, &keyType) != napi_ok ||
                                  keyType != napi_string) {
                                continue;
                              }
                              size_t keyLength = 0;
                              if (napi_get_value_string_utf8(env, keyValue, nullptr, 0,
                                                             &keyLength) != napi_ok) {
                                continue;
                              }
                              std::vector<char> keyBuffer(keyLength + 1, '\0');
                              if (napi_get_value_string_utf8(env, keyValue, keyBuffer.data(),
                                                             keyBuffer.size(),
                                                             &keyLength) != napi_ok) {
                                continue;
                              }
                              std::string key(keyBuffer.data(), keyLength);
                              keys.insert(key);

                              napi_value propertyValue = nullptr;
                              if (napi_get_property(env, pendingInitValue, keyValue,
                                                    &propertyValue) == napi_ok &&
                                  propertyValue != nullptr) {
                                napi_valuetype propertyType = napi_undefined;
                                if (napi_typeof(env, propertyValue, &propertyType) == napi_ok) {
                                  bool isInteger = false;
                                  if (propertyType == napi_bigint) {
                                    isInteger = true;
                                  } else if (propertyType == napi_number) {
                                    double numericValue = 0;
                                    if (napi_get_value_double(env, propertyValue, &numericValue) ==
                                        napi_ok) {
                                      int64_t truncated = static_cast<int64_t>(numericValue);
                                      isInteger = static_cast<double>(truncated) == numericValue;
                                    }
                                  }
                                  keyIsInteger[key] = isInteger;
                                }
                              }
                            }

                            if (!keys.empty()) {
                              auto isIntegerKind = [](MDTypeKind kind) -> bool {
                                switch (kind) {
                                  case mdTypeChar:
                                  case mdTypeSInt:
                                  case mdTypeSShort:
                                  case mdTypeSLong:
                                  case mdTypeSInt64:
                                  case mdTypeUChar:
                                  case mdTypeUInt:
                                  case mdTypeUShort:
                                  case mdTypeUnichar:
                                  case mdTypeULong:
                                  case mdTypeUInt64:
                                  case mdTypeUInt8:
                                  case mdTypeBool:
                                    return true;
                                  default:
                                    return false;
                                }
                              };

                              auto isFloatingKind = [](MDTypeKind kind) -> bool {
                                return kind == mdTypeFloat || kind == mdTypeDouble ||
                                       kind == mdTypeLongDouble || kind == mdTypeF16;
                              };

                              StructInfo* bestMatch = nullptr;
                              int bestScore = std::numeric_limits<int>::min();
                              uint16_t bestSize = std::numeric_limits<uint16_t>::max();

                              for (const auto& entry : bridgeState->structOffsets) {
                                StructInfo* info = bridgeState->getStructInfo(env, entry.second);
                                if (info == nullptr || info->fields.size() != keys.size()) {
                                  continue;
                                }

                                bool match = true;
                                for (const auto& field : info->fields) {
                                  if (field.name == nullptr ||
                                      keys.find(field.name) == keys.end()) {
                                    match = false;
                                    break;
                                  }
                                }
                                if (!match) {
                                  continue;
                                }

                                int score = 0;
                                bool hasOnlyNumericFields = true;
                                for (const auto& field : info->fields) {
                                  if (field.type == nullptr) {
                                    hasOnlyNumericFields = false;
                                    break;
                                  }

                                  MDTypeKind fieldKind = field.type->kind;
                                  if (isIntegerKind(fieldKind)) {
                                    auto integerEntry =
                                        keyIsInteger.find(field.name != nullptr ? field.name : "");
                                    score +=
                                        (integerEntry != keyIsInteger.end() && integerEntry->second)
                                            ? 3
                                            : 1;
                                  } else if (isFloatingKind(fieldKind)) {
                                    score += 2;
                                  } else {
                                    hasOnlyNumericFields = false;
                                    break;
                                  }
                                }

                                if (!hasOnlyNumericFields) {
                                  continue;
                                }

                                if (score > bestScore ||
                                    (score == bestScore && info->size < bestSize)) {
                                  bestScore = score;
                                  bestSize = info->size;
                                  bestMatch = info;
                                }
                              }

                              if (bestMatch != nullptr) {
                                resolvedType = makeStructType(bestMatch);
                              }
                            }
                          }
                        }
                      }
                    }
                  }

                  if (resolvedType == nullptr) {
                    const char* inferredEncoding = "@";
                    if (initType == napi_number || initType == napi_bigint) {
                      inferredEncoding = "q";
                    } else if (initType == napi_boolean) {
                      inferredEncoding = "B";
                    }
                    resolvedType = TypeConv::Make(env, &inferredEncoding);
                  }
                }
              }
            }

            if (resolvedType == nullptr) {
              const char* defaultEncoding = "@";
              resolvedType = TypeConv::Make(env, &defaultEncoding);
            }

            ref->type = resolvedType;
            size_t pointeeSize = sizeof(void*);
            if (resolvedType != nullptr && resolvedType->type != nullptr &&
                resolvedType->type->size > 0) {
              pointeeSize = resolvedType->type->size;
            }
            ref->data = calloc(1, pointeeSize);
            if (ref->data == nullptr) {
              napi_throw_error(env, nullptr, "Out of memory while allocating out parameter");
              *res = nullptr;
              return;
            }
            ref->ownsData = true;
            napi_value initValue = Reference::getInitValue(env, value, ref);
            if (initValue != nullptr) {
              bool shouldFree;
              ref->type->toNative(env, initValue, ref->data, &shouldFree, &shouldFree);
              Reference::clearInitValue(env, value, ref);
            }
          }
          *res = ref->data;
          return;
        }

        if (StructObject::isInstance(env, value)) {
          StructObject* structObj = StructObject::unwrap(env, value);
          if (structObj != nullptr) {
            *res = structObj->data;
          } else
            *res = nullptr;
          return;
        }

        bool isTypedArray = false;
        napi_is_typedarray(env, value, &isTypedArray);
        if (isTypedArray) {
          void* data;
          size_t length = 0;
          napi_typedarray_type type;
          NAPI_GUARD(
              napi_get_typedarray_info(env, value, &type, &length, &data, nullptr, nullptr)) {
            NAPI_THROW_LAST_ERROR
            *res = nullptr;
            return;
          }

          *res = data;
          return;
        }

        bool isArrayBuffer = false;
        napi_is_arraybuffer(env, value, &isArrayBuffer);
        if (isArrayBuffer) {
          void* data = nullptr;
          size_t byteLength = 0;
          napi_get_arraybuffer_info(env, value, &data, &byteLength);
          *res = data;
          return;
        }
        if (unwrapKnownNativeHandle(env, value, res)) {
          return;
        }
        break;
      }

      case napi_function: {
        if (unwrapKnownNativeHandle(env, value, res)) {
          return;
        }
        break;
      }

      default:
        napi_throw_error(env, nullptr, "Invalid pointer type");
        *res = nullptr;
        return;
    }

    napi_throw_error(env, nullptr, "Invalid pointer type");
    *res = nullptr;
  }

  void free(napi_env env, void* value) override {
    if (value == nullptr) {
      return;
    }

    bool isCFString = pointeeType != nullptr && (pointeeType->kind == mdTypeNSStringObject ||
                                                 pointeeType->kind == mdTypeNSMutableStringObject);

    if (isCFString) {
      CFRelease((CFStringRef)value);
    } else {
      ::free(value);
    }
  }

  void encode(std::string* encoding) override { *encoding += "^v"; }
};

static const std::shared_ptr<PointerTypeConv> pointerTypeConv = std::make_shared<PointerTypeConv>();

class BlockTypeConv : public TypeConv {
 public:
  MDSectionOffset signatureOffset;

  BlockTypeConv(MDSectionOffset signatureOffset) : signatureOffset(signatureOffset) {
    type = &ffi_type_pointer;
    kind = mdTypeBlock;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    void* fn = *((void**)value);
    if (fn == nullptr) {
      napi_value nullValue;
      napi_get_null(env, &nullValue);
      return nullValue;
    }
    return FunctionPointer::wrap(env, fn, signatureOffset, true);
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    void** res = (void**)result;

    napi_valuetype type;
    napi_typeof(env, value, &type);

    switch (type) {
      case napi_null:
      case napi_undefined:
        *res = nullptr;
        return;

      case napi_bigint: {
        uint64_t val = 0;
        bool lossless = false;
        NAPI_GUARD(napi_get_value_bigint_uint64(env, value, &val, &lossless)) {
          NAPI_THROW_LAST_ERROR
          *res = nullptr;
          return;
        }
        *res = (void*)val;
        return;
      }

      case napi_external: {
        NAPI_GUARD(napi_get_value_external(env, value, res)) {
          NAPI_THROW_LAST_ERROR
          *res = nullptr;
          return;
        }
        return;
      }

      case napi_object: {
        NAPI_GUARD(napi_unwrap(env, value, res)) {
          NAPI_THROW_LAST_ERROR
          *res = nullptr;
          return;
        }
        return;
      }

      case napi_function: {
        if (FunctionReference::isInstance(env, value)) {
          FunctionReference* ref = FunctionReference::unwrap(env, value);
          if (ref == nullptr) {
            napi_throw_error(env, nullptr, "Invalid FunctionReference");
            *res = nullptr;
            return;
          }
          *res = ref->getFunctionPointer(signatureOffset, true);
          return;
        }

        void* wrapped;
        status = napi_unwrap(env, value, &wrapped);
        if (status == napi_ok) {
          *res = wrapped;
          return;
        }

        auto bridgeState = ObjCBridgeState::InstanceData(env);
        auto closure = new Closure(env, bridgeState->metadata, signatureOffset, true);
        id block = registerBlock(env, closure, value);
        *res = (void*)block;
        *shouldFree = true;
        *shouldFreeAny = true;
        return;
      }

      default:
        napi_throw_error(env, nullptr, "Invalid block pointer type");
        *res = nullptr;
        return;
    }
  }

  void free(napi_env env, void* value) override {
    if (value != nullptr) {
      [(id)value release];
    }
  }

  void encode(std::string* encoding) override { *encoding += "^v"; }
};

namespace {
void function_pointer_finalize_now(napi_env env, void* finalize_data, void* finalize_hint) {
  Closure* closure = static_cast<Closure*>(finalize_hint);
  if (closure != nullptr) {
    Closure::destroyOnOwningThread(closure);
  }
}
}  // namespace

void function_pointer_finalize(napi_env env, void* finalize_data, void* finalize_hint) {
  if (PostFinalizer(env, function_pointer_finalize_now, finalize_data, finalize_hint)) {
    return;
  }

  function_pointer_finalize_now(env, finalize_data, finalize_hint);
}

class FunctionPointerTypeConv : public TypeConv {
 public:
  MDSectionOffset signatureOffset;

  FunctionPointerTypeConv(MDSectionOffset signatureOffset) : signatureOffset(signatureOffset) {
    type = &ffi_type_pointer;
    kind = mdTypeFunctionPointer;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    void* fn = *((void**)value);
    if (fn == nullptr) {
      napi_value nullValue;
      napi_get_null(env, &nullValue);
      return nullValue;
    }
    return FunctionPointer::wrap(env, fn, signatureOffset, false);
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    void** res = (void**)result;

    napi_valuetype type;
    napi_typeof(env, value, &type);

    switch (type) {
      case napi_null:
      case napi_undefined:
        *res = nullptr;
        return;

      case napi_bigint: {
        uint64_t val = 0;
        bool lossless = false;
        NAPI_GUARD(napi_get_value_bigint_uint64(env, value, &val, &lossless)) {
          NAPI_THROW_LAST_ERROR
          *res = nullptr;
          return;
        }
        *res = (void*)val;
        return;
      }

      case napi_external: {
        NAPI_GUARD(napi_get_value_external(env, value, res)) {
          NAPI_THROW_LAST_ERROR
          *res = nullptr;
          return;
        }
        return;
      }

      case napi_object: {
        if (Pointer::isInstance(env, value)) {
          Pointer* ptr = Pointer::unwrap(env, value);
          *res = ptr->data;
        } else if (Reference::isInstance(env, value)) {
          Reference* ref = Reference::unwrap(env, value);
          *res = ref->data;
        } else if (FunctionReference::isInstance(env, value)) {
          FunctionReference* ref = FunctionReference::unwrap(env, value);
          if (ref == nullptr) {
            napi_throw_error(env, nullptr, "Invalid FunctionReference");
            *res = nullptr;
            return;
          }
          *res = ref->getFunctionPointer(signatureOffset, false);
        } else {
          napi_throw_error(env, nullptr, "Invalid function pointer object");
          *res = nullptr;
        }
        return;
      }

      case napi_function: {
        if (FunctionReference::isInstance(env, value)) {
          FunctionReference* ref = FunctionReference::unwrap(env, value);
          if (ref == nullptr) {
            napi_throw_error(env, nullptr, "Invalid FunctionReference");
            *res = nullptr;
            return;
          }
          *res = ref->getFunctionPointer(signatureOffset, false);
          return;
        }

        void* wrapped;
        status = napi_unwrap(env, value, &wrapped);
        if (status == napi_ok) {
          *res = wrapped;
          return;
        }

        auto bridgeState = ObjCBridgeState::InstanceData(env);
        auto closure = new Closure(env, bridgeState->metadata, signatureOffset, false);
        closure->func = make_ref(env, value);
        napi_remove_wrap(env, value, nullptr);
        napi_ref ref;
        napi_wrap(env, value, closure->fnptr, function_pointer_finalize, closure, &ref);
        *res = (void*)closure->fnptr;
        return;
      }

      default:
        napi_throw_error(env, nullptr, "Invalid block pointer type");
        *res = nullptr;
        return;
    }
  }

  void encode(std::string* encoding) override { *encoding += "^v"; }
};

class StringTypeConv : public TypeConv {
 public:
  StringTypeConv() {
    type = &ffi_type_pointer;
    kind = mdTypeString;
  }

  napi_value toJS(napi_env env, void* cont, uint32_t flags) override {
    void* value = *((void**)cont);
    if (value == nullptr) {
      napi_value null;
      napi_get_null(env, &null);
      return null;
    }

    if ((flags & kCStringAsReference) != 0) {
      return Reference::create(env, scharTypeConv, value, false);
    }

    napi_value result;
    napi_create_string_utf8(env, (char*)value, NAPI_AUTO_LENGTH, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    napi_valuetype valuetype;
    napi_typeof(env, value, &valuetype);

    if (valuetype == napi_null || valuetype == napi_undefined) {
      *(char**)result = nullptr;
      *shouldFree = false;
      *shouldFreeAny = false;
      return;
    }

    if (valuetype == napi_object) {
      if (Pointer::isInstance(env, value)) {
        Pointer* ptr = Pointer::unwrap(env, value);
        *(char**)result = (char*)ptr->data;
        *shouldFree = false;
        *shouldFreeAny = false;
        return;
      }

      if (Reference::isInstance(env, value)) {
        Reference* ref = Reference::unwrap(env, value);
        *(char**)result = (char*)ref->data;
        *shouldFree = false;
        *shouldFreeAny = false;
        return;
      }

      bool isTypedArray = false;
      napi_is_typedarray(env, value, &isTypedArray);
      if (isTypedArray) {
        void* data = nullptr;
        size_t length = 0;
        napi_typedarray_type typedArrayType;
        napi_get_typedarray_info(env, value, &typedArrayType, &length, &data, nullptr, nullptr);
        *(char**)result = static_cast<char*>(data);
        *shouldFree = false;
        *shouldFreeAny = false;
        return;
      }

      bool isArrayBuffer = false;
      napi_is_arraybuffer(env, value, &isArrayBuffer);
      if (isArrayBuffer) {
        void* data = nullptr;
        size_t byteLength = 0;
        napi_get_arraybuffer_info(env, value, &data, &byteLength);
        *(char**)result = static_cast<char*>(data);
        *shouldFree = false;
        *shouldFreeAny = false;
        return;
      }

      bool isDataView = false;
      napi_is_dataview(env, value, &isDataView);
      if (isDataView) {
        void* data = nullptr;
        size_t byteLength = 0;
        napi_value arrayBuffer = nullptr;
        size_t byteOffset = 0;
        napi_get_dataview_info(env, value, &byteLength, &data, &arrayBuffer, &byteOffset);
        *(char**)result = static_cast<char*>(data);
        *shouldFree = false;
        *shouldFreeAny = false;
        return;
      }

      *(char**)result = nullptr;
      *shouldFree = false;
      *shouldFreeAny = false;
      return;
    }

    char** res = (char**)result;

    *res = nullptr;
    size_t len = 0;

    NAPI_GUARD(napi_get_value_string_utf8(env, value, nullptr, len, &len)) {
      NAPI_THROW_LAST_ERROR
      return;
    }

    *res = (char*)malloc(len + 1);

    NAPI_GUARD(napi_get_value_string_utf8(env, value, *res, len + 1, &len)) {
      NAPI_THROW_LAST_ERROR
      ::free(*res);
      return;
    }

    (*res)[len] = '\0';

    *shouldFree = true;
    *shouldFreeAny = true;
  }

  void free(napi_env env, void* value) override { ::free(value); }

  void encode(std::string* encoding) override { *encoding += "*"; }
};

static const std::shared_ptr<StringTypeConv> stringTypeConv = std::make_shared<StringTypeConv>();

class ObjCObjectTypeConv : public TypeConv {
 public:
  MDSectionOffset classOffset = 0;
  std::vector<MDSectionOffset> protocolOffsets;

  ObjCObjectTypeConv() {
    type = &ffi_type_pointer;
    kind = mdTypeAnyObject;
  }

  ObjCObjectTypeConv(MDSectionOffset classOffset, std::vector<MDSectionOffset> protocolOffsets)
      : classOffset(classOffset), protocolOffsets(protocolOffsets) {
    type = &ffi_type_pointer;
    if (classOffset != 0) {
      kind = mdTypeClassObject;
    } else {
      kind = protocolOffsets.empty() ? mdTypeAnyObject : mdTypeProtocolObject;
    }
  }

  ObjCObjectTypeConv(std::vector<MDSectionOffset> protocolOffsets)
      : protocolOffsets(protocolOffsets) {
    type = &ffi_type_pointer;
    kind = protocolOffsets.empty() ? mdTypeAnyObject : mdTypeProtocolObject;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    void* rawPtr = *((void**)value);
    id obj = (__bridge id)rawPtr;

    if (obj == nil) {
      napi_value null;
      napi_get_null(env, &null);
      return null;
    }

    auto bridgeState = ObjCBridgeState::InstanceData(env);

    if (bridgeState != nullptr) {
      if (object_isClass(obj)) {
        if (napi_value constructor = findRegisteredClassConstructor(env, (Class)obj);
            constructor != nullptr) {
          return constructor;
        }
      }

      auto normalizePtr = [](void* ptr) -> uintptr_t {
        return normalizeRuntimePointer(reinterpret_cast<uintptr_t>(ptr));
      };

      auto protocolIt = bridgeState->mdProtocolsByPointer.find((Protocol*)obj);
      if (protocolIt != bridgeState->mdProtocolsByPointer.end()) {
        auto proto = bridgeState->getProtocol(env, protocolIt->second);
        if (proto != nullptr) {
          return get_ref_value(env, proto->constructor);
        }
      } else {
        const uintptr_t objNormalized = normalizePtr((void*)obj);
        for (const auto& entry : bridgeState->mdProtocolsByPointer) {
          if (normalizePtr((void*)entry.first) != objNormalized) {
            continue;
          }

          auto proto = bridgeState->getProtocol(env, entry.second);
          if (proto != nullptr) {
            return get_ref_value(env, proto->constructor);
          }
        }
      }
    }

    // Always unbox NSNull and CFBoolean/NSNumber values (except NSDecimalNumber),
    // so primitive round-trips match historical runtime behavior.
    if (isKindOfClassFast(obj, [NSNull class])) {
      napi_value null;
      napi_get_null(env, &null);
      return null;
    }

    if (isKindOfClassFast(obj, [NSNumber class]) &&
        !isKindOfClassFast(obj, [NSDecimalNumber class])) {
      if (CFGetTypeID((CFTypeRef)obj) == CFBooleanGetTypeID()) {
        napi_value result;
        napi_get_boolean(env, [obj boolValue], &result);
        return result;
      }

      napi_value result;
      napi_create_double(env, [obj doubleValue], &result);
      return result;
    }

    // Untyped id values that are actually Objective-C blocks should be
    // callable from JS. Preserve callback identity when we already have one.
    if (isObjCBlockObject(obj)) {
      napi_value cached = getCachedBlockCallback(env, (void*)obj);
      if (cached != nullptr) {
        return cached;
      }

      const char* signature = getObjCBlockSignature((void*)obj);
      if (signature != nullptr) {
        return FunctionPointer::wrapWithEncoding(env, (void*)obj, signature, true);
      }
    }

    // Auto-unbox plain id string values.
    const bool isUntypedObject = classOffset == 0 && protocolOffsets.empty();
    if (isUntypedObject && isKindOfClassFast(obj, [NSString class])) {
      NSUInteger length = [obj length];
      std::vector<char16_t> chars(length > 0 ? length : 1);
      if (length > 0) {
        [((NSString*)obj) getCharacters:(unichar*)chars.data() range:NSMakeRange(0, length)];
      }
      napi_value result;
      napi_create_string_utf16(env, length > 0 ? chars.data() : nullptr, length, &result);
      return result;
    }

    if (bridgeState == nullptr) {
      return Pointer::create(env, (void*)obj);
    }

    if (napi_value existing = bridgeState->findCachedObjectWrapper(env, obj); existing != nullptr) {
      return existing;
    }

    ObjectOwnership ownership;
    if ((flags & kReturnOwned) != 0) {
      ownership = kOwnedObject;
    } else {
      ownership = kUnownedObject;
    }

    auto object = bridgeState->getObject(env, obj, ownership, classOffset, &protocolOffsets);
    if (object == nullptr) {
      napi_value null;
      napi_get_null(env, &null);
      return null;
    }

    return object;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    id* res = (id*)result;

    napi_valuetype type;
    napi_typeof(env, value, &type);

    switch (type) {
      case napi_null:
      case napi_undefined:
        *res = nil;
        return;

      case napi_string: {
        size_t len = 0;
        NAPI_GUARD(napi_get_value_string_utf8(env, value, nullptr, 0, &len)) {
          NAPI_THROW_LAST_ERROR
          return;
        }

        std::vector<char> chars(len + 1);
        NAPI_GUARD(napi_get_value_string_utf8(env, value, chars.data(), len + 1, &len)) {
          NAPI_THROW_LAST_ERROR
          return;
        }

        *res = [[[NSString alloc] initWithBytes:chars.data()
                                         length:len
                                       encoding:NSUTF8StringEncoding] autorelease];
        if (*res == nil) {
          *res = [NSString string];
        }
        break;
      }

      case napi_number: {
        double val = 0;
        NAPI_GUARD(napi_get_value_double(env, value, &val)) {
          NAPI_THROW_LAST_ERROR
          return;
        }
        *res = [NSNumber numberWithDouble:val];
        break;
      }

      case napi_boolean: {
        bool val = false;
        NAPI_GUARD(napi_get_value_bool(env, value, &val)) {
          NAPI_THROW_LAST_ERROR
          return;
        }
        *res = [NSNumber numberWithBool:val];
        break;
      }

      case napi_bigint: {
        int64_t val = 0;
        bool lossless = false;
        NAPI_GUARD(napi_get_value_bigint_int64(env, value, &val, &lossless)) {
          NAPI_THROW_LAST_ERROR
          return;
        }
        *res = [NSNumber numberWithLongLong:val];
        break;
      }

      case napi_external:
        NAPI_GUARD(napi_get_value_external(env, value, (void**)res)) {
          NAPI_THROW_LAST_ERROR
          *res = nil;
          return;
        }
        break;

      case napi_object:
      case napi_function: {
        auto bridgeState = ObjCBridgeState::InstanceData(env);
        auto cacheRoundTrip = [&](id nativeObj) {
          if (nativeObj == nil || bridgeState == nullptr ||
              !bridgeState->hasRoundTripCacheFrame()) {
            return;
          }

          bridgeState->cacheRoundTripObject(env, nativeObj, value);
        };

        if (Pointer::isInstance(env, value)) {
          Pointer* ptr = Pointer::unwrap(env, value);
          void* pointerData = ptr != nullptr ? ptr->data : nullptr;
          if (id cachedObject = resolveCachedHandleObject(env, pointerData); cachedObject != nil) {
            *res = cachedObject;
            return;
          }
          *res = (id)pointerData;
          return;
        }

        if (Reference::isInstance(env, value)) {
          Reference* ref = Reference::unwrap(env, value);
          void* referenceData = ref != nullptr ? ref->data : nullptr;
          if (id cachedObject = resolveCachedHandleObject(env, referenceData);
              cachedObject != nil) {
            *res = cachedObject;
            return;
          }
          *res = (id)referenceData;
          return;
        }

        if (bridgeState != nullptr) {
          id bridgedType = nil;
          if (bridgeState->tryResolveBridgedTypeConstructor(env, value, &bridgedType) &&
              bridgedType != nil) {
            *res = bridgedType;
            return;
          }
        }

        void* wrapped = nullptr;
        bool knownNativeHandle = unwrapKnownNativeHandle(env, value, &wrapped);
        if (!knownNativeHandle) {
          bool isArrayBuffer = false;
          napi_is_arraybuffer(env, value, &isArrayBuffer);
          if (isArrayBuffer) {
            *res = createNSDataWrapper(env, value, bridgeState);
            if (*res != nil) {
              cacheRoundTrip(*res);
              return;
            }
          }

          bool isTypedArray = false;
          napi_is_typedarray(env, value, &isTypedArray);
          if (isTypedArray) {
            *res = createNSDataWrapper(env, value, bridgeState);
            if (*res != nil) {
              cacheRoundTrip(*res);
              return;
            }
          }

          bool isDataView = false;
          napi_is_dataview(env, value, &isDataView);
          if (isDataView) {
            *res = createNSDataWrapper(env, value, bridgeState);
            if (*res != nil) {
              cacheRoundTrip(*res);
              return;
            }
          }

          ScopedObjectConversion conversion(env, value);
          if (!conversion.entered()) {
            *res = nil;
            if (conversion.status() != napi_ok) {
              status = conversion.status();
              NAPI_THROW_LAST_ERROR
            }
            return;
          }

          bool isArray = false;
          napi_is_array(env, value, &isArray);
          if (isArray) {
            uint32_t len = 0;
            napi_get_array_length(env, value, &len);
            *res = [NSMutableArray arrayWithCapacity:len];

            for (uint32_t i = 0; i < len; i++) {
              napi_value elem;
              napi_get_element(env, value, i, &elem);
              id obj = nil;
              toNative(env, elem, (void*)&obj, shouldFree, shouldFreeAny);
              if (hasPendingException(env)) {
                *res = nil;
                return;
              }
              [(*res) addObject:obj != nil ? obj : [NSNull null]];
            }

            cacheRoundTrip(*res);
            return;
          } else {
            napi_value global, jsObject, valueConstructor, DateConstructor, MapConstructor;
            napi_value StringConstructor, NumberConstructor, BooleanConstructor;
            napi_get_global(env, &global);
            napi_get_named_property(env, global, "Object", &jsObject);
            napi_get_named_property(env, global, "Date", &DateConstructor);
            napi_get_named_property(env, global, "Map", &MapConstructor);
            napi_get_named_property(env, global, "String", &StringConstructor);
            napi_get_named_property(env, global, "Number", &NumberConstructor);
            napi_get_named_property(env, global, "Boolean", &BooleanConstructor);
            napi_get_named_property(env, value, "constructor", &valueConstructor);
            bool isEqual;
            napi_strict_equals(env, jsObject, valueConstructor, &isEqual);
            bool isDate;
            napi_strict_equals(env, DateConstructor, valueConstructor, &isDate);
            bool isMap;
            napi_strict_equals(env, MapConstructor, valueConstructor, &isMap);
            bool isStringObject = false;
            bool isNumberObject = false;
            bool isBooleanObject = false;
            napi_strict_equals(env, StringConstructor, valueConstructor, &isStringObject);
            napi_strict_equals(env, NumberConstructor, valueConstructor, &isNumberObject);
            napi_strict_equals(env, BooleanConstructor, valueConstructor, &isBooleanObject);

            if (isStringObject || isNumberObject || isBooleanObject) {
              napi_value valueOfMethod;
              napi_get_named_property(env, value, "valueOf", &valueOfMethod);
              napi_value primitiveValue;
              napi_call_function(env, value, valueOfMethod, 0, nullptr, &primitiveValue);
              if (hasPendingException(env)) {
                *res = nil;
                return;
              }
              toNative(env, primitiveValue, result, shouldFree, shouldFreeAny);
              return;
            }

            if (isDate) {
              // Get the timestamp from the JavaScript Date object
              napi_value getTimeMethod;
              napi_get_named_property(env, value, "getTime", &getTimeMethod);
              napi_value timestamp;
              napi_call_function(env, value, getTimeMethod, 0, nullptr, &timestamp);

              double timeInMilliseconds;
              napi_get_value_double(env, timestamp, &timeInMilliseconds);

              // Convert milliseconds to seconds for NSDate
              NSTimeInterval timeInSeconds = timeInMilliseconds / 1000.0;
              *res = [NSDate dateWithTimeIntervalSince1970:timeInSeconds];
              cacheRoundTrip(*res);
              return;
            }

            if (isMap) {
              *res = [NSMutableDictionary dictionary];

              napi_value entriesMethod;
              napi_get_named_property(env, value, "entries", &entriesMethod);
              napi_value iterator;
              napi_call_function(env, value, entriesMethod, 0, nullptr, &iterator);

              napi_value nextMethod;
              napi_get_named_property(env, iterator, "next", &nextMethod);

              while (true) {
                napi_value step;
                napi_call_function(env, iterator, nextMethod, 0, nullptr, &step);

                napi_value doneValue;
                napi_get_named_property(env, step, "done", &doneValue);
                bool done = false;
                napi_get_value_bool(env, doneValue, &done);
                if (done) {
                  break;
                }

                napi_value tuple;
                napi_get_named_property(env, step, "value", &tuple);
                napi_value keyValue;
                napi_value elementValue;
                napi_get_element(env, tuple, 0, &keyValue);
                napi_get_element(env, tuple, 1, &elementValue);

                id keyObject = nil;
                id valueObject = nil;
                toNative(env, keyValue, (void*)&keyObject, shouldFree, shouldFreeAny);
                if (hasPendingException(env)) {
                  *res = nil;
                  return;
                }
                toNative(env, elementValue, (void*)&valueObject, shouldFree, shouldFreeAny);
                if (hasPendingException(env)) {
                  *res = nil;
                  return;
                }

                if (keyObject != nil && valueObject != nil) {
                  [(*res) setObject:valueObject forKey:keyObject];
                }
              }

              cacheRoundTrip(*res);
              return;
            }

            if (!isEqual) {
              *res = jsObjectToId(env, value);
              return;
            }

            bool hasLength = false;
            napi_has_named_property(env, value, "length", &hasLength);
            if (hasLength) {
              napi_value lengthValue;
              napi_get_named_property(env, value, "length", &lengthValue);
              napi_valuetype lengthType = napi_undefined;
              napi_typeof(env, lengthValue, &lengthType);
              if (lengthType == napi_number) {
                uint32_t len = 0;
                napi_get_value_uint32(env, lengthValue, &len);
                *res = [NSMutableArray arrayWithCapacity:len];
                for (uint32_t i = 0; i < len; i++) {
                  bool hasElement = false;
                  napi_has_element(env, value, i, &hasElement);
                  if (!hasElement) {
                    [(*res) addObject:[NSNull null]];
                    continue;
                  }
                  napi_value elem;
                  napi_get_element(env, value, i, &elem);
                  id obj = nil;
                  toNative(env, elem, (void*)&obj, shouldFree, shouldFreeAny);
                  if (hasPendingException(env)) {
                    *res = nil;
                    return;
                  }
                  [(*res) addObject:obj != nil ? obj : [NSNull null]];
                }
                cacheRoundTrip(*res);
                return;
              }
            }

            *res = [NSMutableDictionary dictionary];
            napi_value objectKeysMethod = nullptr;
            napi_get_named_property(env, jsObject, "keys", &objectKeysMethod);
            napi_value keys = nullptr;
            napi_call_function(env, jsObject, objectKeysMethod, 1, &value, &keys);
            uint32_t len = 0;
            napi_get_array_length(env, keys, &len);

            for (uint32_t i = 0; i < len; i++) {
              napi_value key = nullptr;
              napi_get_element(env, keys, i, &key);

              if (key == nullptr) {
                continue;
              }

              napi_value keyString = key;
              napi_valuetype keyType = napi_undefined;
              if (napi_typeof(env, key, &keyType) != napi_ok) {
                continue;
              }

              if (keyType == napi_symbol) {
                continue;
              }

              if (keyType != napi_string) {
                if (napi_coerce_to_string(env, key, &keyString) != napi_ok ||
                    keyString == nullptr) {
                  continue;
                }
              }

              size_t keyLength = 0;
              if (napi_get_value_string_utf8(env, keyString, nullptr, 0, &keyLength) != napi_ok) {
                continue;
              }

              std::vector<char> keyBuffer(keyLength + 1, '\0');
              if (napi_get_value_string_utf8(env, keyString, keyBuffer.data(), keyBuffer.size(),
                                             &keyLength) != napi_ok) {
                continue;
              }

              NSString* nsKey = [NSString stringWithUTF8String:keyBuffer.data()];
              if (nsKey == nil) {
                continue;
              }

              id obj = nil;
              napi_value elem = nullptr;
              if (napi_get_property(env, value, key, &elem) != napi_ok) {
                continue;
              }
              toNative(env, elem, (void*)&obj, shouldFree, shouldFreeAny);
              if (hasPendingException(env)) {
                *res = nil;
                return;
              }
              if (obj != nil) {
                [(*res) setObject:obj forKey:nsKey];
              }
            }

            cacheRoundTrip(*res);
            return;
          }
        }

        if (bridgeState != nullptr && wrapped != nullptr) {
          for (const auto& entry : bridgeState->classes) {
            auto bridgedClass = entry.second;
            if (bridgedClass == wrapped) {
              *res = (id)bridgedClass->nativeClass;
              return;
            }
          }

          for (const auto& entry : bridgeState->protocols) {
            auto bridgedProtocol = entry.second;
            if (bridgedProtocol != wrapped) {
              continue;
            }

            Protocol* runtimeProtocol = objc_getProtocol(bridgedProtocol->name.c_str());
            if (runtimeProtocol == nil) {
              std::string baseName;
              if (stripProtocolSuffix(bridgedProtocol->name.c_str(), &baseName)) {
                runtimeProtocol = objc_getProtocol(baseName.c_str());
              }
            }

            if (runtimeProtocol != nil) {
              *res = (id)runtimeProtocol;
              return;
            }
          }
        }

        *res = (id)wrapped;
        cacheRoundTrip(*res);
        return;

        break;
      }

      default:
        napi_throw_error(env, nullptr, "Invalid object type");
        *res = nil;
        break;
    }
  }

  void free(napi_env env, void* value) override {
    id obj = *((id*)value);
    auto bridgeState = ObjCBridgeState::InstanceData(env);
    bridgeState->unregisterObject(obj);
  }

  void encode(std::string* encoding) override { *encoding += "@"; }
};

static const std::shared_ptr<ObjCObjectTypeConv> objcObjectTypeConv =
    std::make_shared<ObjCObjectTypeConv>();

class ObjCInstanceObjectTypeConv : public ObjCObjectTypeConv {
 public:
  ObjCInstanceObjectTypeConv() {
    type = &ffi_type_pointer;
    kind = mdTypeInstanceObject;
  }
};

static const auto objcInstanceObjectTypeConv = std::make_shared<ObjCInstanceObjectTypeConv>();

class ObjCNSStringObjectTypeConv : public TypeConv {
 public:
  ObjCNSStringObjectTypeConv() {
    type = &ffi_type_pointer;
    kind = mdTypeNSStringObject;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    NSString* str = *((NSString**)value);

    if (str == nullptr) {
      napi_value null;
      napi_get_null(env, &null);
      return null;
    }

    NSUInteger length = [str length];
    std::vector<char16_t> chars(length > 0 ? length : 1);
    if (length > 0) {
      [str getCharacters:(unichar*)chars.data() range:NSMakeRange(0, length)];
    }
    napi_value result;
    napi_create_string_utf16(env, chars.data(), length, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    ObjCObjectTypeConv typeConv;
    typeConv.toNative(env, value, result, shouldFree, shouldFreeAny);
  }

  void encode(std::string* encoding) override { *encoding += "@"; }
};

static const std::shared_ptr<ObjCNSStringObjectTypeConv> objcNSStringObjectTypeConv =
    std::make_shared<ObjCNSStringObjectTypeConv>();

class ObjCNSMutableStringObjectTypeConv : public TypeConv {
 public:
  ObjCNSMutableStringObjectTypeConv() {
    type = &ffi_type_pointer;
    kind = mdTypeNSMutableStringObject;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    NSMutableString* str = *((NSMutableString**)value);

    if (str == nullptr) {
      napi_value null;
      napi_get_null(env, &null);
      return null;
    }

    auto bridgeState = ObjCBridgeState::InstanceData(env);
    if (napi_value existing = bridgeState->findCachedObjectWrapper(env, str); existing != nullptr) {
      return existing;
    }

    ObjectOwnership ownership = (flags & kReturnOwned) != 0 ? kOwnedObject : kUnownedObject;
    return bridgeState->getObject(env, str, ownership);
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    napi_valuetype type;
    napi_typeof(env, value, &type);
    if (type == napi_string) {
      NSMutableString** res = (NSMutableString**)result;

      size_t len = 0;
      NAPI_GUARD(napi_get_value_string_utf16(env, value, nullptr, len, &len)) {
        NAPI_THROW_LAST_ERROR
        return;
      }

      std::vector<char16_t> chars(len + 1);

      NAPI_GUARD(napi_get_value_string_utf16(env, value, chars.data(), len + 1, &len)) {
        NAPI_THROW_LAST_ERROR
        return;
      }

      *res = [[NSMutableString alloc] initWithCharacters:(unichar*)chars.data() length:len];
      return;
    }

    ObjCObjectTypeConv typeConv;
    typeConv.toNative(env, value, result, shouldFree, shouldFreeAny);
  }

  void encode(std::string* encoding) override { *encoding += "@"; }
};

static const std::shared_ptr<ObjCNSMutableStringObjectTypeConv> objcNSMutableStringObjectTypeConv =
    std::make_shared<ObjCNSMutableStringObjectTypeConv>();

class ObjCClassTypeConv : public TypeConv {
 public:
  ObjCClassTypeConv() {
    type = &ffi_type_pointer;
    kind = mdTypeClass;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    Class cls = *((Class*)value);

    if (cls == nullptr) {
      napi_value null;
      napi_get_null(env, &null);
      return null;
    }

    if (napi_value constructor = findRegisteredClassConstructor(env, cls);
        constructor != nullptr) {
      return constructor;
    }

    auto bridgeState = ObjCBridgeState::InstanceData(env);
    return bridgeState != nullptr
               ? bridgeState->getObject(env, (id)cls, kUnownedObject, 0, nullptr)
               : nullptr;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    ObjCObjectTypeConv typeConv;
    typeConv.toNative(env, value, result, shouldFree, shouldFreeAny);
  }

  void encode(std::string* encoding) override { *encoding += "#"; }
};

static const std::shared_ptr<ObjCClassTypeConv> objcClassTypeConv =
    std::make_shared<ObjCClassTypeConv>();

class SelectorTypeConv : public TypeConv {
 public:
  SelectorTypeConv() {
    type = &ffi_type_pointer;
    kind = mdTypeSelector;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    SEL val = *((SEL*)value);
    napi_create_string_utf8(env, sel_getName(val), NAPI_AUTO_LENGTH, &result);
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    SEL* res = (SEL*)result;

    napi_valuetype type;
    napi_typeof(env, value, &type);

    switch (type) {
      case napi_string: {
        size_t length = 0;
        NAPI_GUARD(napi_get_value_string_utf8(env, value, nullptr, 0, &length)) {
          NAPI_THROW_LAST_ERROR
          *res = NULL;
          return;
        }

        std::vector<char> selectorName(length + 1, '\0');
        NAPI_GUARD(napi_get_value_string_utf8(env, value, selectorName.data(),
                                              selectorName.size(), &length)) {
          NAPI_THROW_LAST_ERROR
          *res = NULL;
          return;
        }
        *res = sel_registerName(selectorName.data());
        break;
      }

      case napi_undefined:
      case napi_null:
        *res = NULL;
        return;

      default:
        napi_throw_error(env, nullptr, "Invalid selector type");
        *res = NULL;
        return;
    }
  }

  void encode(std::string* encoding) override { *encoding += ":"; }
};

static const std::shared_ptr<SelectorTypeConv> selectorTypeConv =
    std::make_shared<SelectorTypeConv>();

class StructTypeConv : public TypeConv {
 public:
  MDSectionOffset structOffset;
  StructInfo* info = nullptr;
  bool structInfoSearched = false;

  StructTypeConv(MDSectionOffset structOffset, ffi_type* type, bool ownsType,
                 std::vector<std::shared_ptr<TypeConv>> retainedElementTypes)
      : structOffset(structOffset),
        ownsType(ownsType),
        retainedElementTypes(std::move(retainedElementTypes)) {
    this->type = type;
    kind = mdTypeStruct;
  }

  ~StructTypeConv() override {
    if (ownsType && type != nullptr) {
      std::free(type->elements);
      delete type;
      type = nullptr;
    }
  }

  inline StructInfo* getInfo(napi_env env) {
    if (!structInfoSearched) {
      auto bridgeState = ObjCBridgeState::InstanceData(env);
      info = bridgeState->getStructInfo(env, structOffset);
      structInfoSearched = true;
    }

    return info;
  }

  inline size_t getStructSize(napi_env env) {
    if (this->type != nullptr && this->type->size > 0) {
      return this->type->size;
    }

    auto info = getInfo(env);
    return info != nullptr ? info->size : 0;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    auto info = getInfo(env);

    if (info == nullptr) {
      napi_value result;
      void* data;
      napi_create_arraybuffer(env, type->size, &data, &result);
      memcpy(data, value, type->size);
      return result;
    } else {
      return StructObject::fromNative(env, info, value, (flags & kStructZeroCopy) == 0);
    }
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    const size_t structSize = getStructSize(env);
    if (structSize == 0) {
      napi_throw_type_error(env, "TypeError", "Invalid struct size");
      return;
    }

    bool isTypedArray = false;
    napi_is_typedarray(env, value, &isTypedArray);

    if (isTypedArray) {
      void* data;
      size_t length = 0;
      napi_typedarray_type type;
      NAPI_GUARD(napi_get_typedarray_info(env, value, &type, &length, &data, nullptr, nullptr)) {
        NAPI_THROW_LAST_ERROR
        return;
      }
      const size_t unitLength = getTypedArrayUnitLength(type);
      const size_t byteLength = length * unitLength;
      memset(result, 0, structSize);
      memcpy(result, data, std::min(byteLength, structSize));

      return;
    }

    napi_valuetype type;
    napi_typeof(env, value, &type);

    if (type == napi_null || type == napi_undefined) {
      auto info = getInfo(env);

      if (info == nullptr) {
        napi_throw_type_error(env, "TypeError",
                              "Invalid struct type, must be Struct Object, "
                              "Struct Object Descriptor or TypedArray");
        return;
      }

      memset(result, 0, info->size);
      return;
    } else if (type != napi_object) {
      napi_throw_type_error(env, "TypeError",
                            "Invalid struct type, must be Struct Object, "
                            "Struct Object Descriptor or TypedArray");
      return;
    }

    auto structObject = StructObject::isInstance(env, value)
                            ? StructObject::unwrap(env, value)
                            : nullptr;
    if (structObject != nullptr) {
      const size_t copySize = std::min(static_cast<size_t>(structObject->info->size), structSize);
      memset(result, 0, structSize);
      memcpy(result, structObject->data, copySize);
      return;
    }

    auto info = getInfo(env);

    if (info == nullptr) {
      napi_throw_type_error(env, "TypeError",
                            "Invalid struct type, must be Struct Object or TypedArray");
      return;
    }

    if (structSize < info->size) {
      std::vector<uint8_t> storage(info->size, 0);
      StructObject(env, info, value, storage.data());
      memcpy(result, storage.data(), structSize);
      return;
    }

    // Serialize directly to previously allocated memory.
    StructObject(env, info, value, result);
  }

 private:
  bool ownsType;
  std::vector<std::shared_ptr<TypeConv>> retainedElementTypes;
};

class ArrayTypeConv : public TypeConv {
 public:
  int arraySize;
  std::shared_ptr<TypeConv> elementType;
  bool decayToPointerForArguments = false;

  ArrayTypeConv(int arraySize, std::shared_ptr<TypeConv> elementType)
      : arraySize(arraySize), elementType(elementType) {
    auto arrayType = new ffi_type();
    arrayType->type = FFI_TYPE_STRUCT;
    arrayType->size = 0;
    arrayType->alignment = 0;
    arrayType->elements = (ffi_type**)malloc(sizeof(ffi_type*) * (arraySize + 1));
    for (int i = 0; i < arraySize; i++) {
      arrayType->elements[i] = elementType->type;
    }
    arrayType->elements[arraySize] = nullptr;
    type = arrayType;
    kind = mdTypeArray;
  }

  ~ArrayTypeConv() override {
    if (type != nullptr) {
      std::free(type->elements);
      delete type;
      type = nullptr;
    }
  }

  ffi_type* ffiTypeForArgument() override {
    decayToPointerForArguments = true;
    return &ffi_type_pointer;
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    if (decayToPointerForArguments) {
      void* raw = *((void**)value);
      if (raw == nullptr) {
        napi_value nullValue;
        napi_get_null(env, &nullValue);
        return nullValue;
      }
      return Pointer::create(env, raw);
    }

    napi_value result;
    napi_create_array_with_length(env, arraySize, &result);

    size_t elementSize = getElementSize();

    auto base = static_cast<uint8_t*>(value);
    for (int i = 0; i < arraySize; i++) {
      void* slot = base + (i * elementSize);
      napi_value elementValue = elementType->toJS(env, slot, flags);
      napi_valuetype elementValueType = napi_undefined;
      napi_typeof(env, elementValue, &elementValueType);
      if (elementValueType == napi_boolean) {
        bool boolValue = false;
        napi_get_value_bool(env, elementValue, &boolValue);
        napi_create_uint32(env, boolValue ? 1 : 0, &elementValue);
      }
      napi_set_element(env, result, i, elementValue);
      if (StructObject::isInstance(env, elementValue)) {
        napi_set_named_property(env, elementValue, "__ns_parent_struct_array", result);
      }
    }
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    if (decayToPointerForArguments) {
      void** pointerResult = static_cast<void**>(result);
      *pointerResult = nullptr;

      napi_valuetype valueType = napi_undefined;
      napi_typeof(env, value, &valueType);
      if (valueType == napi_null || valueType == napi_undefined) {
        return;
      }

      if (Pointer::isInstance(env, value)) {
        Pointer* ptr = Pointer::unwrap(env, value);
        *pointerResult = ptr != nullptr ? ptr->data : nullptr;
        return;
      }

      if (Reference::isInstance(env, value)) {
        Reference* ref = Reference::unwrap(env, value);
        *pointerResult = ref != nullptr ? ref->data : nullptr;
        return;
      }

      if (StructObject::isInstance(env, value)) {
        StructObject* structObject = StructObject::unwrap(env, value);
        *pointerResult = structObject != nullptr ? structObject->data : nullptr;
        return;
      }

      size_t arrayByteSize = getArrayByteSize();
      if (arrayByteSize == 0) {
        return;
      }
      void* copiedBuffer = malloc(arrayByteSize);
      if (copiedBuffer == nullptr) {
        napi_throw_error(env, nullptr, "Out of memory while converting C array argument");
        return;
      }

      copyToInlineArrayStorage(env, value, copiedBuffer, shouldFree, shouldFreeAny);

      bool hasPendingException = false;
      napi_is_exception_pending(env, &hasPendingException);
      if (hasPendingException) {
        ::free(copiedBuffer);
        return;
      }

      *pointerResult = copiedBuffer;
      if (shouldFree != nullptr) {
        *shouldFree = true;
      }
      if (shouldFreeAny != nullptr) {
        *shouldFreeAny = true;
      }
      return;
    }

    copyToInlineArrayStorage(env, value, result, shouldFree, shouldFreeAny);
  }

  void free(napi_env env, void* value) override {
    if (value != nullptr) {
      ::free(value);
    }
  }

  void encode(std::string* encoding) override {
    *encoding += "[";
    *encoding += std::to_string(arraySize);
    elementType->encode(encoding);
    *encoding += "]";
  }

 private:
  size_t getElementSize() const {
    size_t elementSize =
        elementType != nullptr && elementType->type != nullptr ? elementType->type->size : 0;
    if (elementSize == 0 && type != nullptr && arraySize > 0 && type->size >= (size_t)arraySize) {
      elementSize = type->size / static_cast<size_t>(arraySize);
    }
    if (elementSize == 0) {
      elementSize = sizeof(void*);
    }
    return elementSize;
  }

  size_t getArrayByteSize() const { return getElementSize() * static_cast<size_t>(arraySize); }

  void copyToInlineArrayStorage(napi_env env, napi_value value, void* result, bool* shouldFree,
                                bool* shouldFreeAny) {
    size_t elementSize = getElementSize();
    size_t arrayByteSize = getArrayByteSize();
    memset(result, 0, arrayByteSize);

    napi_valuetype valueType = napi_undefined;
    napi_typeof(env, value, &valueType);
    if (valueType == napi_null || valueType == napi_undefined) {
      return;
    }

    if (Pointer::isInstance(env, value)) {
      Pointer* ptr = Pointer::unwrap(env, value);
      if (ptr == nullptr || ptr->data == nullptr) {
        return;
      }
      memcpy(result, ptr->data, arrayByteSize);
      return;
    }

    bool isArray = false;
    napi_is_array(env, value, &isArray);
    if (isArray) {
      for (int i = 0; i < arraySize; i++) {
        bool hasElement = false;
        napi_has_element(env, value, i, &hasElement);
        if (!hasElement) {
          continue;
        }

        napi_value elementValue;
        napi_get_element(env, value, i, &elementValue);
        void* slot = static_cast<uint8_t*>(result) + (i * elementSize);
        elementType->toNative(env, elementValue, slot, shouldFree, shouldFreeAny);
      }
      return;
    }

    bool isArrayBuffer = false;
    napi_is_arraybuffer(env, value, &isArrayBuffer);
    if (isArrayBuffer) {
      void* data = nullptr;
      size_t byteLength = 0;
      napi_get_arraybuffer_info(env, value, &data, &byteLength);
      memcpy(result, data, std::min(byteLength, arrayByteSize));
      return;
    }

    void* data;
    size_t length = 0;
    napi_typedarray_type typedArrayType;
    napi_status typedArrayStatus =
        napi_get_typedarray_info(env, value, &typedArrayType, &length, &data, nullptr, nullptr);
    if (typedArrayStatus != napi_ok) {
      NAPI_THROW_LAST_ERROR
      return;
    }

    size_t copyLength = length * getTypedArrayUnitLength(typedArrayType);
    memcpy(result, data, std::min(copyLength, arrayByteSize));
  }
};

class VectorTypeConv : public TypeConv {
 public:
  uint16_t vectorSize;
  std::shared_ptr<TypeConv> elementType;
  MDTypeKind vectorKind;

  VectorTypeConv(MDTypeKind vectorKind, uint16_t vectorSize, std::shared_ptr<TypeConv> elementType)
      : vectorSize(vectorSize), elementType(elementType), vectorKind(vectorKind) {
    auto vectorType = new ffi_type();
#if defined(FFI_TYPE_EXT_VECTOR)
    vectorType->type = vectorKind == mdTypeComplex ? FFI_TYPE_COMPLEX : FFI_TYPE_EXT_VECTOR;
#else
    vectorType->type = vectorKind == mdTypeComplex ? FFI_TYPE_COMPLEX : FFI_TYPE_STRUCT;
#endif
    size_t lanes = std::max<size_t>(vectorSize, 1);
    // 3-lane vectors are ABI-lowered to 4-lane storage on Apple platforms.
    size_t abiLanes = lanes == 3 ? 4 : lanes;
    vectorType->elements = (ffi_type**)malloc(sizeof(ffi_type*) * (abiLanes + 1));

    ffi_type* elementFfiType = elementType != nullptr && elementType->type != nullptr
                                   ? elementType->type
                                   : &ffi_type_float;
    const size_t elementSize = std::max<size_t>(elementFfiType->size, sizeof(float));
    const size_t elementAlignment =
        std::max<size_t>(elementFfiType->alignment, static_cast<size_t>(1));

    for (size_t i = 0; i < abiLanes; i++) {
      vectorType->elements[i] = elementFfiType;
    }
    vectorType->elements[abiLanes] = nullptr;

    size_t vectorAlignment = elementAlignment;
    if (vectorKind != mdTypeComplex) {
      size_t packedSize = abiLanes * elementSize;
      size_t preferredAlignment = packedSize >= 16 ? 16 : packedSize;
      vectorAlignment = std::max(vectorAlignment, preferredAlignment);
    }
    vectorAlignment = std::min<size_t>(vectorAlignment, 16);
    vectorType->alignment = static_cast<unsigned short>(vectorAlignment);
    vectorType->size = alignUp(abiLanes * elementSize, vectorAlignment);

    type = vectorType;
    kind = vectorKind;
  }

  ~VectorTypeConv() override {
    if (type != nullptr) {
      std::free(type->elements);
      delete type;
      type = nullptr;
    }
  }

  napi_value toJS(napi_env env, void* value, uint32_t flags) override {
    napi_value result;
    napi_create_array_with_length(env, vectorSize, &result);

    size_t elementSize = getElementSize();
    auto base = static_cast<uint8_t*>(value);
    for (uint16_t i = 0; i < vectorSize; i++) {
      void* slot = base + (static_cast<size_t>(i) * elementSize);
      napi_value elementValue = elementType->toJS(env, slot, flags);
      napi_valuetype elementValueType = napi_undefined;
      napi_typeof(env, elementValue, &elementValueType);
      if (elementValueType == napi_boolean) {
        bool boolValue = false;
        napi_get_value_bool(env, elementValue, &boolValue);
        napi_create_uint32(env, boolValue ? 1 : 0, &elementValue);
      }
      napi_set_element(env, result, i, elementValue);
    }
    return result;
  }

  void toNative(napi_env env, napi_value value, void* result, bool* shouldFree,
                bool* shouldFreeAny) override {
    NS_OBJC_NAPI_PREAMBLE

    memset(result, 0, getVectorByteSize());

    napi_valuetype valueType = napi_undefined;
    napi_typeof(env, value, &valueType);
    if (valueType == napi_null || valueType == napi_undefined) {
      return;
    }

    if (Pointer::isInstance(env, value)) {
      Pointer* ptr = Pointer::unwrap(env, value);
      if (ptr != nullptr && ptr->data != nullptr) {
        copyFromContiguousBuffer(ptr->data, getVectorByteSize(), result);
      }
      return;
    }

    if (Reference::isInstance(env, value)) {
      Reference* ref = Reference::unwrap(env, value);
      if (ref != nullptr && ref->data != nullptr) {
        copyFromContiguousBuffer(ref->data, getVectorByteSize(), result);
      }
      return;
    }

    if (StructObject::isInstance(env, value)) {
      StructObject* structObject = StructObject::unwrap(env, value);
      if (structObject != nullptr && structObject->data != nullptr) {
        copyFromContiguousBuffer(structObject->data, structObject->info->size, result);
      }
      return;
    }

    bool isArray = false;
    napi_is_array(env, value, &isArray);
    if (isArray) {
      writeFromArrayElements(env, value, result, shouldFree, shouldFreeAny);
      return;
    }

    bool isArrayBuffer = false;
    napi_is_arraybuffer(env, value, &isArrayBuffer);
    if (isArrayBuffer) {
      void* data = nullptr;
      size_t byteLength = 0;
      napi_get_arraybuffer_info(env, value, &data, &byteLength);
      copyFromContiguousBuffer(data, byteLength, result);
      return;
    }

    void* data = nullptr;
    size_t length = 0;
    napi_typedarray_type typedArrayType = napi_int8_array;
    napi_status typedArrayStatus =
        napi_get_typedarray_info(env, value, &typedArrayType, &length, &data, nullptr, nullptr);
    if (typedArrayStatus == napi_ok) {
      size_t copyLength = length * getTypedArrayUnitLength(typedArrayType);
      copyFromContiguousBuffer(data, copyLength, result);
      return;
    }

    napi_throw_type_error(
        env, "TypeError",
        "Invalid vector type, expected array, typed array, array buffer, pointer or reference.");
  }

  void encode(std::string* encoding) override {
    *encoding += "V";
    *encoding += std::to_string(vectorSize);
    if (elementType != nullptr) {
      elementType->encode(encoding);
    }
  }

 private:
  inline size_t getElementSize() const {
    size_t elementSize =
        elementType != nullptr && elementType->type != nullptr ? elementType->type->size : 0;
    if (elementSize == 0) {
      elementSize = sizeof(float);
    }
    return elementSize;
  }

  inline size_t getVectorByteSize() const {
    size_t expectedSize = static_cast<size_t>(vectorSize) * getElementSize();
    if (type != nullptr && type->size > expectedSize) {
      expectedSize = type->size;
    }
    return expectedSize;
  }

  inline void copyFromContiguousBuffer(void* source, size_t sourceLength, void* destination) const {
    memcpy(destination, source, std::min(sourceLength, getVectorByteSize()));
  }

  void writeFromArrayElements(napi_env env, napi_value value, void* result, bool* shouldFree,
                              bool* shouldFreeAny) {
    size_t elementSize = getElementSize();
    auto base = static_cast<uint8_t*>(result);

    for (uint16_t i = 0; i < vectorSize; i++) {
      bool hasElement = false;
      napi_has_element(env, value, i, &hasElement);
      if (!hasElement) {
        continue;
      }

      napi_value elementValue;
      napi_get_element(env, value, i, &elementValue);
      void* slot = base + (static_cast<size_t>(i) * elementSize);
      elementType->toNative(env, elementValue, slot, shouldFree, shouldFreeAny);
    }
  }
};

std::shared_ptr<TypeConv> TypeConv::Make(napi_env env, const char** encoding) {
  char first = **encoding;
  bool readonly = false;
  if (first == 'r') {
    readonly = true;
    first = *(++(*encoding));
  }

  switch (first) {
    case 'c':
      (*encoding)++;
      return scharTypeConv;
    case 'i':
      (*encoding)++;
      return sint32TypeConv;
    case 's':
      (*encoding)++;
      return sint16TypeConv;
    case 'l':
    case 'q':
      (*encoding)++;
      return sint64TypeConv;
    case 'C':
      (*encoding)++;
      return uint8TypeConv;
    case 'I':
      (*encoding)++;
      return uint32TypeConv;
    case 'S':
      (*encoding)++;
      return uint16TypeConv;
    case 'L':
    case 'Q':
      (*encoding)++;
      return uint64TypeConv;
    case 'f':
      (*encoding)++;
      return float32TypeConv;
    case 'd':
      (*encoding)++;
      return float64TypeConv;
    case 'B':
      (*encoding)++;
      return boolTypeConv;
    case 'v':
      (*encoding)++;
      return voidTypeConv;
    case '*':
      (*encoding)++;
      return stringTypeConv;
    case '@':
      (*encoding)++;
      return objcObjectTypeConv;
    case '#':
      (*encoding)++;
      return objcClassTypeConv;
    case ':':
      (*encoding)++;
      return selectorTypeConv;
    case '[': {
      char c = **encoding;
      std::string num;
      while ((c = **encoding) >= '0' && c <= '9') {
        num += c;
        (*encoding)++;
      }
      auto arraySize = std::stoi(num);
      auto elementType = TypeConv::Make(env, encoding);
      while (**encoding != ']') {
        (*encoding)++;
      }  // skip array type
      (*encoding)++;  // skip ']'
      return std::make_shared<ArrayTypeConv>(arraySize, elementType);
    }
    case '{': {
      auto& encodingStructCache = structTypeCaches(env).encodingStructTypes;
      std::string structname;
      const char* c = *encoding + 1;
      while (*c != '\0' && *c != '=') {
        structname += *c;
        c++;
      }
      if (*c != '=') {
        while (**encoding != '\0' && **encoding != '}') {
          (*encoding)++;
        }
        if (**encoding == '}') {
          (*encoding)++;
        }
        return pointerTypeConv;
      }

      // Check if we already have a cached StructTypeConv for this encoding-based struct
      auto cacheIt = encodingStructCache.find(structname);
      if (cacheIt != encodingStructCache.end()) {
        return cacheIt->second;
      }

      auto bridgeState = ObjCBridgeState::InstanceData(env);
      MDSectionOffset structOffset = MD_SECTION_OFFSET_NULL;
      if (bridgeState != nullptr) {
        auto structOffsetIt = bridgeState->structOffsets.find(structname);
        if (structOffsetIt != bridgeState->structOffsets.end()) {
          structOffset = structOffsetIt->second;
        }
      }
      bool ownsType = false;
      std::vector<std::shared_ptr<TypeConv>> retainedElementTypes;
      auto type = typeFromStruct(env, encoding, &ownsType, &retainedElementTypes);
      auto structTypeConv = std::make_shared<StructTypeConv>(
          structOffset, type, ownsType, std::move(retainedElementTypes));

      // Cache the StructTypeConv
      encodingStructCache[structname] = structTypeConv;

      return structTypeConv;
    }
    case 'b': {
      (*encoding)++;
      char c = **encoding;
      while ((c = **encoding) >= '0' && c <= '9') {
        (*encoding)++;
      }  // skip bits
      return uint64TypeConv;
    }
    case '^':
      (*encoding)++;
      TypeConv::Make(env, encoding);
      return pointerTypeConv;
    case '?':
      // unknown type
      return pointerTypeConv;
    default:
      std::cout << "getTypeInfo unknown encoding: " << *encoding << std::endl;
      return pointerTypeConv;
  }
}

std::shared_ptr<TypeConv> TypeConv::Make(napi_env env, MDMetadataReader* reader,
                                         MDSectionOffset* offset, uint8_t opaquePointers) {
  auto kind = reader->getTypeKind(*offset);
  bool next = (MDTypeFlag)kind & mdTypeFlagNext;
  kind = (MDTypeKind)((kind & ~mdTypeFlagNext) & ~mdTypeFlagVariadic);
  *offset += sizeof(MDTypeKind);

  switch (kind) {
    case mdTypeChar: {
      return scharTypeConv;
    }

    case mdTypeSInt: {
      return sint32TypeConv;
    }

    case mdTypeSShort: {
      return sint16TypeConv;
    }

    case mdTypeSLong:
    case mdTypeSInt64: {
      return sint64TypeConv;
    }

    case mdTypeUInt8: {
      return uint8TypeConv;
    }

    case mdTypeUChar: {
      return ucharTypeConv;
    }

    case mdTypeUInt: {
      return uint32TypeConv;
    }

    case mdTypeUShort: {
      return uint16TypeConv;
    }

    case mdTypeUnichar: {
      return unicharTypeConv;
    }

    case mdTypeULong:
    case mdTypeUInt64: {
      return uint64TypeConv;
    }

    case mdTypeFloat: {
      return float32TypeConv;
    }

    case mdTypeF16: {
      return float16TypeConv;
    }

    case mdTypeDouble: {
      return float64TypeConv;
    }

    case mdTypeBool: {
      return boolTypeConv;
    }

    case mdTypeVoid: {
      return voidTypeConv;
    }

    case mdTypeString: {
      return stringTypeConv;
    }

    case mdTypeAnyObject: {
      return objcObjectTypeConv;
    }

    case mdTypeInstanceObject: {
      return objcInstanceObjectTypeConv;
    }

    case mdTypeClassObject: {
      auto classOffset = reader->getOffset(*offset);
      *offset += sizeof(MDSectionOffset);
      bool next = (classOffset & mdSectionOffsetNext) != 0;
      classOffset &= ~mdSectionOffsetNext;
      if (classOffset == MD_SECTION_OFFSET_NULL) {
        classOffset = 0;
      } else {
        classOffset += reader->classesOffset;
      }
      std::vector<MDSectionOffset> protocolOffsets;
      while (next) {
        auto protocolOffset = reader->getOffset(*offset);
        *offset += sizeof(MDSectionOffset);
        next = (protocolOffset & mdSectionOffsetNext) != 0;
        protocolOffset &= ~mdSectionOffsetNext;
        if (protocolOffset == MD_SECTION_OFFSET_NULL) {
          protocolOffset = 0;
        } else {
          protocolOffset += reader->protocolsOffset;
          protocolOffsets.push_back(protocolOffset);
        }
      }
      return std::make_shared<ObjCObjectTypeConv>(classOffset, protocolOffsets);
    }

    case mdTypeProtocolObject: {
      std::vector<MDSectionOffset> protocolOffsets;
      bool next = true;
      while (next) {
        auto protocolOffset = reader->getOffset(*offset);
        *offset += sizeof(MDSectionOffset);
        next = (protocolOffset & mdSectionOffsetNext) != 0;
        protocolOffset &= ~mdSectionOffsetNext;
        if (protocolOffset == MD_SECTION_OFFSET_NULL) {
          protocolOffset = 0;
        } else {
          protocolOffset += reader->protocolsOffset;
          protocolOffsets.push_back(protocolOffset);
        }
      }
      return std::make_shared<ObjCObjectTypeConv>(protocolOffsets);
    }

    case mdTypeNSStringObject: {
      return objcNSStringObjectTypeConv;
    }

    case mdTypeNSMutableStringObject: {
      return objcNSMutableStringObjectTypeConv;
    }

    case mdTypeClass: {
      return objcClassTypeConv;
    }

    case mdTypeSelector: {
      return selectorTypeConv;
    }

    case mdTypeArray: {
      auto arraySize = reader->getArraySize(*offset);
      *offset += sizeof(uint16_t);
      auto elementType = TypeConv::Make(env, reader, offset);
      return std::make_shared<ArrayTypeConv>(arraySize, elementType);
    }

    case mdTypeStruct: {
      auto& caches = structTypeCaches(env);
      auto& processingStructs = caches.processingStructs;
      auto& structTypeCache = caches.structTypes;
      auto structOffset = reader->getOffset(*offset);
      *offset += sizeof(MDSectionOffset);
      auto isUnion = (structOffset & mdSectionOffsetNext) != 0;
      structOffset &= ~mdSectionOffsetNext;
      if (structOffset == MD_SECTION_OFFSET_NULL) {
        return pointerTypeConv;
      }
      structOffset += isUnion ? reader->unionsOffset : reader->structsOffset;
      auto structName = reader->getString(structOffset);

      // Check if we already have a cached StructTypeConv for this struct
      auto cacheIt = structTypeCache.find(structOffset);
      if (cacheIt != structTypeCache.end()) {
        return cacheIt->second;
      }

      // Check if we're currently processing this struct (recursion detection)
      bool isRecursive = processingStructs.find(structOffset) != processingStructs.end();

      ffi_type* type = nullptr;
      bool ownsType = false;
      std::vector<std::shared_ptr<TypeConv>> retainedElementTypes;
      if (opaquePointers != 2 && !isRecursive) {
        type = typeFromStruct(env, reader, structOffset, isUnion, &ownsType,
                              &retainedElementTypes);
      }

      auto structTypeConv = std::make_shared<StructTypeConv>(
          structOffset, type, ownsType, std::move(retainedElementTypes));

      // Cache the StructTypeConv to handle recursion and avoid duplicates
      structTypeCache[structOffset] = structTypeConv;

      return structTypeConv;
    }

    case mdTypePointer: {
      auto pointeeType = TypeConv::Make(env, reader, offset, opaquePointers == 1 ? 2 : 0);
      return std::make_shared<PointerTypeConv>(pointeeType);
    }

    case mdTypeOpaquePointer: {
      return pointerTypeConv;
    }

    case mdTypeVector: {
      auto vectorSize = reader->getArraySize(*offset);
      *offset += sizeof(uint16_t);
      auto elementType = TypeConv::Make(env, reader, offset, opaquePointers);
      return std::make_shared<VectorTypeConv>(kind, vectorSize, elementType);
    }

    case mdTypeBlock: {
      auto blockSignature = reader->getOffset(*offset) + reader->signaturesOffset;
      *offset += sizeof(MDSectionOffset);
      return std::make_shared<BlockTypeConv>(blockSignature);
    }

    case mdTypeFunctionPointer: {
      auto blockSignature = reader->getOffset(*offset) + reader->signaturesOffset;
      *offset += sizeof(MDSectionOffset);
      return std::make_shared<FunctionPointerTypeConv>(blockSignature);
    }

    case mdTypeUInt128: {
      return uint128TypeConv;
    }

    case mdTypeExtVector: {
      auto vectorSize = reader->getArraySize(*offset);
      *offset += sizeof(uint16_t);
      auto elementType = TypeConv::Make(env, reader, offset, opaquePointers);
      return std::make_shared<VectorTypeConv>(kind, vectorSize, elementType);
    }

    case mdTypeComplex: {
      auto vectorSize = reader->getArraySize(*offset);
      *offset += sizeof(uint16_t);
      auto elementType = TypeConv::Make(env, reader, offset, opaquePointers);
      return std::make_shared<VectorTypeConv>(kind, vectorSize, elementType);
    }

    default:
      return pointerTypeConv;
  }
}

namespace {

bool tryFastConvertStringToNSString(napi_env env, napi_value value, id* out, bool mutableString) {
  if (out == nullptr) {
    return false;
  }

  if (mutableString) {
    constexpr size_t kStackUtf16Capacity = 128;
    char16_t utf16Stack[kStackUtf16Capacity];
    char16_t* utf16Buffer = utf16Stack;
    size_t utf16Capacity = kStackUtf16Capacity;
    size_t utf16Length = 0;
    if (napi_get_value_string_utf16(env, value, utf16Buffer, utf16Capacity, &utf16Length) !=
        napi_ok) {
      return false;
    }

    std::vector<char16_t> utf16Heap;
    if (utf16Length + 1 >= utf16Capacity) {
      if (napi_get_value_string_utf16(env, value, nullptr, 0, &utf16Length) != napi_ok) {
        return false;
      }
      utf16Heap.resize(utf16Length + 1, 0);
      utf16Buffer = utf16Heap.data();
      utf16Capacity = utf16Heap.size();
      if (napi_get_value_string_utf16(env, value, utf16Buffer, utf16Capacity, &utf16Length) !=
          napi_ok) {
        return false;
      }
    }

    *out = [[NSMutableString alloc] initWithCharacters:reinterpret_cast<const unichar*>(utf16Buffer)
                                                length:utf16Length];
    return true;
  }

  constexpr size_t kStackUtf8Capacity = 256;
  char utf8Stack[kStackUtf8Capacity];
  char* utf8Buffer = utf8Stack;
  size_t utf8Capacity = kStackUtf8Capacity;
  size_t utf8Length = 0;
  if (napi_get_value_string_utf8(env, value, utf8Buffer, utf8Capacity, &utf8Length) != napi_ok) {
    return false;
  }

  std::vector<char> utf8Heap;
  if (utf8Length + 1 >= utf8Capacity) {
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &utf8Length) != napi_ok) {
      return false;
    }
    utf8Heap.resize(utf8Length + 1, '\0');
    utf8Buffer = utf8Heap.data();
    utf8Capacity = utf8Heap.size();
    if (napi_get_value_string_utf8(env, value, utf8Buffer, utf8Capacity, &utf8Length) != napi_ok) {
      return false;
    }
  }

  id stringValue = [[[NSString alloc] initWithBytes:utf8Buffer
                                             length:utf8Length
                                           encoding:NSUTF8StringEncoding] autorelease];
  *out = stringValue != nil ? stringValue : [NSString string];
  return true;
}

bool tryFastConvertObjCObjectValue(napi_env env, napi_value value, napi_valuetype valueType,
                                   MDTypeKind kind, id* out) {
  if (out == nullptr) {
    return false;
  }

  switch (valueType) {
    case napi_null:
    case napi_undefined:
      *out = nil;
      return true;

    case napi_external: {
      void* external = nullptr;
      if (napi_get_value_external(env, value, &external) != napi_ok) {
        return false;
      }
      *out = static_cast<id>(external);
      return true;
    }

    case napi_number: {
      double numericValue = 0;
      if (napi_get_value_double(env, value, &numericValue) != napi_ok) {
        return false;
      }
      *out = [NSNumber numberWithDouble:numericValue];
      return true;
    }

    case napi_boolean: {
      bool boolValue = false;
      if (napi_get_value_bool(env, value, &boolValue) != napi_ok) {
        return false;
      }
      *out = [NSNumber numberWithBool:boolValue];
      return true;
    }

    case napi_bigint: {
      int64_t bigintValue = 0;
      bool lossless = false;
      if (napi_get_value_bigint_int64(env, value, &bigintValue, &lossless) != napi_ok) {
        return false;
      }
      *out = [NSNumber numberWithLongLong:bigintValue];
      return true;
    }

    case napi_string:
      return tryFastConvertStringToNSString(env, value, out, kind == mdTypeNSMutableStringObject);

    case napi_object:
    case napi_function: {
      auto bridgeState = ObjCBridgeState::InstanceData(env);
      auto cacheRoundTrip = [&](id nativeObj) {
        if (nativeObj == nil || bridgeState == nullptr || !bridgeState->hasRoundTripCacheFrame()) {
          return;
        }

        bridgeState->cacheRoundTripObject(env, nativeObj, value);
      };

      if (valueType == napi_object) {
        bool isArray = false;
        if (napi_is_array(env, value, &isArray) == napi_ok && isArray) {
          return false;
        }

        if (Pointer::isInstance(env, value)) {
          Pointer* ptr = Pointer::unwrap(env, value);
          void* pointerData = ptr != nullptr ? ptr->data : nullptr;
          if (id cachedObject = resolveCachedHandleObject(env, pointerData); cachedObject != nil) {
            *out = cachedObject;
            return true;
          }
          *out = (id)pointerData;
          return true;
        }
        if (Reference::isInstance(env, value)) {
          Reference* ref = Reference::unwrap(env, value);
          void* referenceData = ref != nullptr ? ref->data : nullptr;
          if (id cachedObject = resolveCachedHandleObject(env, referenceData);
              cachedObject != nil) {
            *out = cachedObject;
            return true;
          }
          *out = (id)referenceData;
          return true;
        }
      }

      void* nativeHandle = nullptr;
      if (unwrapKnownNativeHandle(env, value, &nativeHandle)) {
        *out = (id)nativeHandle;
        cacheRoundTrip(*out);
        return true;
      }

      bool isTypedArray = false;
      if (napi_is_typedarray(env, value, &isTypedArray) == napi_ok && isTypedArray) {
        *out = createNSDataWrapper(env, value, bridgeState);
        if (*out != nil) {
          cacheRoundTrip(*out);
          return true;
        }
        return false;
      }

      bool isArrayBuffer = false;
      if (napi_is_arraybuffer(env, value, &isArrayBuffer) == napi_ok && isArrayBuffer) {
        *out = createNSDataWrapper(env, value, bridgeState);
        if (*out != nil) {
          cacheRoundTrip(*out);
          return true;
        }
        return false;
      }

      bool isDataView = false;
      if (napi_is_dataview(env, value, &isDataView) == napi_ok && isDataView) {
        *out = createNSDataWrapper(env, value, bridgeState);
        if (*out != nil) {
          cacheRoundTrip(*out);
          return true;
        }
        return false;
      }

      return false;
    }

    default:
      return false;
  }
}

}  // namespace

bool TryFastConvertNapiArgument(napi_env env, MDTypeKind kind, napi_value value, void* result) {
  if (result == nullptr || value == nullptr) {
    return false;
  }

  napi_valuetype valueType = napi_undefined;
  if (napi_typeof(env, value, &valueType) != napi_ok) {
    return false;
  }

  switch (kind) {
    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
      return tryFastConvertObjCObjectValue(env, value, valueType, static_cast<MDTypeKind>(kind),
                                           reinterpret_cast<id*>(result));

    case mdTypeSelector: {
      SEL* selector = reinterpret_cast<SEL*>(result);
      switch (valueType) {
        case napi_null:
        case napi_undefined:
          *selector = nullptr;
          return true;

        case napi_string: {
          constexpr size_t kStackSelectorCapacity = 128;
          char selectorStack[kStackSelectorCapacity];
          size_t selectorLength = 0;
          if (napi_get_value_string_utf8(env, value, selectorStack, kStackSelectorCapacity,
                                         &selectorLength) != napi_ok) {
            return false;
          }
          const char* selectorName = selectorStack;
          std::vector<char> selectorHeap;
          if (selectorLength + 1 >= kStackSelectorCapacity) {
            if (napi_get_value_string_utf8(env, value, nullptr, 0, &selectorLength) != napi_ok) {
              return false;
            }
            selectorHeap.resize(selectorLength + 1, '\0');
            if (napi_get_value_string_utf8(env, value, selectorHeap.data(), selectorHeap.size(),
                                           &selectorLength) != napi_ok) {
              return false;
            }
            selectorName = selectorHeap.data();
          }
          *selector = sel_registerName(selectorName);
          return true;
        }

        default:
          return false;
      }
    }

    default:
      return false;
  }
}

bool ConsumeNapiArgumentConversionFailure(napi_env env) {
  if (failedObjectConversionEnv != env) {
    return false;
  }
  failedObjectConversionEnv = nullptr;
  return true;
}

bool TryFastConvertNapiUInt16Argument(napi_env env, napi_value value, uint16_t* result) {
  if (result == nullptr || value == nullptr) {
    return false;
  }

  napi_valuetype valueType = napi_undefined;
  if (napi_typeof(env, value, &valueType) != napi_ok) {
    return false;
  }

  if (valueType == napi_string) {
    size_t strLen = 0;
    if (napi_get_value_string_utf16(env, value, nullptr, 0, &strLen) != napi_ok) {
      return false;
    }
    if (strLen != 1) {
      napi_throw_type_error(env, nullptr, "Expected a single-character string.");
      *result = 0;
      return false;
    }

    char16_t chars[2] = {0, 0};
    if (napi_get_value_string_utf16(env, value, chars, 2, &strLen) != napi_ok) {
      return false;
    }

    *result = static_cast<uint16_t>(chars[0]);
    return true;
  }

  napi_value coerced = value;
  if (napi_coerce_to_number(env, value, &coerced) != napi_ok) {
    return false;
  }

  uint32_t converted = 0;
  if (napi_get_value_uint32(env, coerced, &converted) != napi_ok) {
    return false;
  }

  *result = static_cast<uint16_t>(converted);
  return true;
}

// Cleanup function to clear thread-local caches
void clearStructTypeCaches(napi_env env) {
  auto cacheIt = structTypeCachesByEnv.find(env);
  if (cacheIt == structTypeCachesByEnv.end()) {
    return;
  }

  auto& caches = cacheIt->second;
  caches.structTypes.clear();
  caches.encodingStructTypes.clear();

  for (const auto& entry : caches.forwardDeclaredStructs) {
    std::free(entry.second->elements);
    delete entry.second;
  }
  for (const auto& entry : caches.forwardDeclaredEncodingStructs) {
    std::free(entry.second->elements);
    delete entry.second;
  }

  structTypeCachesByEnv.erase(cacheIt);
}

}  // namespace nativescript
