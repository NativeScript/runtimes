#ifndef NS_FFI_NAPI_SIGNATURE_DISPATCH_H
#define NS_FFI_NAPI_SIGNATURE_DISPATCH_H

#include <objc/runtime.h>

#include "Cif.h"
#include "ffi/objc/shared/SignatureDispatchCore.h"
#include "js_native_api.h"

namespace nativescript {

using ObjCNapiInvoker = bool (*)(napi_env env, Cif* cif, void* fnptr, id self,
                                 SEL selector, const napi_value* argv,
                                 void* rvalue);
using CFunctionNapiInvoker = bool (*)(napi_env env, Cif* cif, void* fnptr,
                                      const napi_value* argv, void* rvalue);

struct ObjCNapiDispatchEntry {
  uint64_t dispatchId;
  ObjCNapiInvoker invoker;
};

struct CFunctionNapiDispatchEntry {
  uint64_t dispatchId;
  CFunctionNapiInvoker invoker;
};

}  // namespace nativescript

#ifndef NS_GSD_BACKEND_NAPI
#define NS_GSD_BACKEND_NAPI 1
#endif

#ifndef NS_HAS_GENERATED_SIGNATURE_DISPATCH
#define NS_HAS_GENERATED_SIGNATURE_DISPATCH 0
#endif

#ifndef NS_HAS_GENERATED_SIGNATURE_NAPI_DISPATCH
#define NS_HAS_GENERATED_SIGNATURE_NAPI_DISPATCH 0
#endif

#ifndef NS_GSD_BACKEND_HERMES
#define NS_GSD_BACKEND_HERMES 0
#endif

#ifndef NS_GSD_BACKEND_PREPARED
#define NS_GSD_BACKEND_PREPARED 0
#endif

#define NS_REQUIRES_GENERATED_SIGNATURE_DISPATCH \
  (NS_GSD_BACKEND_HERMES || NS_GSD_BACKEND_NAPI || NS_GSD_BACKEND_PREPARED)

#if defined(__has_include)
#if __has_include("../shared/GeneratedSignatureDispatch.inc")
#include "../shared/GeneratedSignatureDispatch.inc"
#elif NS_REQUIRES_GENERATED_SIGNATURE_DISPATCH
#error GeneratedSignatureDispatch.inc is required when generated signature dispatch is enabled.
#endif
#elif NS_REQUIRES_GENERATED_SIGNATURE_DISPATCH
#error __has_include is required to validate GeneratedSignatureDispatch.inc.
#endif

#if NS_REQUIRES_GENERATED_SIGNATURE_DISPATCH && !NS_HAS_GENERATED_SIGNATURE_DISPATCH
#error GeneratedSignatureDispatch.inc did not enable this generated signature dispatch backend.
#endif

#if NS_GSD_BACKEND_NAPI && !NS_HAS_GENERATED_SIGNATURE_NAPI_DISPATCH
#error GeneratedSignatureDispatch.inc did not enable Node-API generated signature dispatch.
#endif

#if !NS_HAS_GENERATED_SIGNATURE_DISPATCH
namespace nativescript {
inline constexpr ObjCDispatchEntry kGeneratedObjCDispatchEntries[] = {
    {0, nullptr}};
inline constexpr CFunctionDispatchEntry kGeneratedCFunctionDispatchEntries[] = {
    {0, nullptr}};
inline constexpr BlockDispatchEntry kGeneratedBlockDispatchEntries[] = {
    {0, nullptr}};
}  // namespace nativescript
#endif

#if !NS_HAS_GENERATED_SIGNATURE_NAPI_DISPATCH
namespace nativescript {
inline constexpr ObjCNapiDispatchEntry kGeneratedObjCNapiDispatchEntries[] = {
    {0, nullptr}};
inline constexpr CFunctionNapiDispatchEntry
    kGeneratedCFunctionNapiDispatchEntries[] = {{0, nullptr}};
}  // namespace nativescript
#endif

namespace nativescript {

inline ObjCPreparedInvoker lookupObjCPreparedInvoker(uint64_t dispatchId) {
  if (!isGeneratedDispatchEnabled()) {
    return nullptr;
  }
  return lookupDispatchInvoker<ObjCDispatchEntry, ObjCPreparedInvoker>(
      kGeneratedObjCDispatchEntries, dispatchId);
}

inline CFunctionPreparedInvoker lookupCFunctionPreparedInvoker(
    uint64_t dispatchId) {
  if (!isGeneratedDispatchEnabled()) {
    return nullptr;
  }
  return lookupDispatchInvoker<CFunctionDispatchEntry,
                               CFunctionPreparedInvoker>(
      kGeneratedCFunctionDispatchEntries, dispatchId);
}

inline BlockPreparedInvoker lookupBlockPreparedInvoker(uint64_t dispatchId) {
  if (!isGeneratedDispatchEnabled()) {
    return nullptr;
  }
  return lookupDispatchInvoker<BlockDispatchEntry, BlockPreparedInvoker>(
      kGeneratedBlockDispatchEntries, dispatchId);
}

inline ObjCNapiInvoker lookupObjCNapiInvoker(uint64_t dispatchId) {
  if (!isGeneratedDispatchEnabled()) {
    return nullptr;
  }
  return lookupDispatchInvoker<ObjCNapiDispatchEntry, ObjCNapiInvoker>(
      kGeneratedObjCNapiDispatchEntries, dispatchId);
}

inline CFunctionNapiInvoker lookupCFunctionNapiInvoker(uint64_t dispatchId) {
  if (!isGeneratedDispatchEnabled()) {
    return nullptr;
  }
  return lookupDispatchInvoker<CFunctionNapiDispatchEntry,
                               CFunctionNapiInvoker>(
      kGeneratedCFunctionNapiDispatchEntries, dispatchId);
}

}  // namespace nativescript

#endif  // NS_FFI_NAPI_SIGNATURE_DISPATCH_H
