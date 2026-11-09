#ifndef STRUCT_H
#define STRUCT_H

#include <stdint.h>

#include "MetadataReader.h"
#include "TypeConv.h"
#include "js_native_api.h"

namespace nativescript {

class ObjCBridgeState;

napi_value JS_structGetter(napi_env env, napi_callback_info info);
napi_value JS_unionGetter(napi_env env, napi_callback_info info);

typedef struct StructFieldInfo {
  char* name;
  uint16_t offset;
  std::shared_ptr<TypeConv> type;
} StructFieldInfo;

typedef struct StructInfo {
  char* name;
  uint16_t size;
  std::vector<StructFieldInfo> fields;
  napi_ref jsClass;
} StructInfo;

StructInfo* getStructInfoFromMetadata(napi_env env, MDMetadataReader* metadata,
                                      MDSectionOffset offset);
StructInfo* getStructInfoFromUnionMetadata(napi_env env,
                                           MDMetadataReader* metadata,
                                           MDSectionOffset offset);

class StructObject {
 public:
  void* data;
  StructInfo* info;
  bool owned;
  napi_env env = nullptr;
  napi_ref backingRef = nullptr;
#if defined(TARGET_ENGINE_HERMES)
  napi_ref wrapperRef = nullptr;
#endif
  ObjCBridgeState* bridgeState = nullptr;
  uint64_t bridgeStateToken = 0;

  StructObject(StructInfo* info, void* data = nullptr, napi_env env = nullptr,
               napi_value backingValue = nullptr);
  StructObject(napi_env env, StructInfo* info, napi_value object,
               void* memory = nullptr);

  napi_value get(napi_env env, StructFieldInfo* field);
  void set(napi_env env, StructFieldInfo* field, napi_value value);

  static StructObject* unwrap(napi_env env, napi_value object);
  static napi_value defineJSClass(napi_env env, StructInfo* info);
  static napi_value getJSClass(napi_env env, StructInfo* info);
  static napi_value fromNative(napi_env env, StructInfo* info, void* data,
                               bool owned);

  static bool isInstance(napi_env env, napi_value object);

  ~StructObject();
};

}  // namespace nativescript

#endif /* STRUCT_H */
