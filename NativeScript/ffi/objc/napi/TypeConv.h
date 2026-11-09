#ifndef TYPE_CONV_H
#define TYPE_CONV_H

#include <iostream>

#include "MetadataReader.h"
#include "ffi.h"
#include "js_native_api.h"
#include "objc/runtime.h"

using namespace metagen;

namespace nativescript {

class Cif;

typedef enum ConvertToJSFlags : uint32_t {
  kReturnOwned = 1 << 0,
  kBlockParam = 1 << 1,
  kStructZeroCopy = 1 << 2,
  kCStringAsReference = 1 << 3,
  kCFunctionObjectReturn = 1 << 4,
} ConvertToJSFlags;

class TypeConv {
 public:
  static std::shared_ptr<TypeConv> Make(napi_env env, const char** encoding);
  static std::shared_ptr<TypeConv> Make(napi_env env, MDMetadataReader* reader,
                                        MDSectionOffset* offset,
                                        uint8_t opaquePointers = 0);

  ffi_type* type = nullptr;
  MDTypeKind kind = mdTypeChar;

  virtual ~TypeConv() = default;

  virtual napi_value toJS(napi_env env, void* value, uint32_t flags = 0) {
    return nullptr;
  }

  virtual void toNative(napi_env env, napi_value value, void* result,
                        bool* shouldFree, bool* shouldFreeAny) {}

  virtual void free(napi_env env, void* value) {}

  virtual ffi_type* ffiTypeForArgument() { return type; }

  virtual void encode(std::string* encoding) {}
};

// Fast-path conversion for known metadata kinds used by generated dispatch
// wrappers. Returns true only when conversion is fully handled and written to
// `result`. Returns false when caller should fall back to TypeConv::toNative.
bool TryFastConvertNapiArgument(napi_env env, MDTypeKind kind, napi_value value,
                                void* result);

// Returns and clears conversion failures that cannot be observed through
// napi_is_exception_pending on every backend.
bool ConsumeNapiArgumentConversionFailure(napi_env env);

// Fast direct conversion for uint16_t / unichar arguments used by generated
// dispatch wrappers. Supports both numeric values and single-character JS
// strings.
bool TryFastConvertNapiUInt16Argument(napi_env env, napi_value value,
                                      uint16_t* result);

// Cleanup function to clear thread-local struct type caches
void clearStructTypeCaches(napi_env env);

}  // namespace nativescript

#endif /* TYPE_CONV_H */
