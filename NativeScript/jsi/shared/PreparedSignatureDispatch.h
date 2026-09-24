#ifndef NATIVESCRIPT_JSI_SHARED_PREPARED_SIGNATURE_DISPATCH_H
#define NATIVESCRIPT_JSI_SHARED_PREPARED_SIGNATURE_DISPATCH_H

// Lookup side of generated signature dispatch: given a dispatch id, find the
// ahead-of-time generated trampoline for it.
//
// Platform-neutral. It needs only the hashing/lookup core, not the
// metadata-format-specific half that computes the ids -- which is why this can
// live here while ffi/objc/shared/SignatureDispatchCore.h cannot.

#include "jsi/shared/SignatureHashing.h"

#ifndef NS_GSD_BACKEND_PREPARED
#define NS_GSD_BACKEND_PREPARED 0
#endif

#ifndef NS_GSD_BACKEND_HERMES
#define NS_GSD_BACKEND_HERMES 0
#endif

#ifndef NS_GSD_BACKEND_NAPI
#define NS_GSD_BACKEND_NAPI 0
#endif

#ifndef NS_HAS_GENERATED_SIGNATURE_DISPATCH
#define NS_HAS_GENERATED_SIGNATURE_DISPATCH 0
#endif

#define NS_REQUIRES_GENERATED_SIGNATURE_DISPATCH \
  (NS_GSD_BACKEND_HERMES || NS_GSD_BACKEND_NAPI || NS_GSD_BACKEND_PREPARED)

#if NS_REQUIRES_GENERATED_SIGNATURE_DISPATCH && \
    !NS_HAS_GENERATED_SIGNATURE_DISPATCH
#error GeneratedSignatureDispatch.inc did not enable this generated signature dispatch backend.
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

inline bool isPreparedGeneratedDispatchRequired() {
#if NS_HAS_GENERATED_SIGNATURE_DISPATCH && \
    (NS_GSD_BACKEND_PREPARED || NS_GSD_BACKEND_HERMES)
  return isGeneratedDispatchEnabled();
#else
  return false;
#endif
}

}  // namespace nativescript

#endif  // NATIVESCRIPT_JSI_SHARED_PREPARED_SIGNATURE_DISPATCH_H
