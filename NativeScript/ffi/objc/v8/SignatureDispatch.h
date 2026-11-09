#ifndef NATIVESCRIPT_FFI_V8_SIGNATURE_DISPATCH_H
#define NATIVESCRIPT_FFI_V8_SIGNATURE_DISPATCH_H

#include <cstdint>

#include "ffi/objc/shared/SignatureDispatchCore.h"

// Engine-neutral GSD (Generated Signature Dispatch). The GsdObjCContext struct,
// the ObjCGsdInvoker/ObjCGsdDispatchEntry types, the generated dispatch table,
// and lookupObjCGsdInvoker are all defined in NativeApiV8SelectorGroups.mm,
// which NativeApiV8.mm includes after the host object helpers the context
// relies on. Nothing GSD-related is declared here to avoid creating an
// ambiguous second GsdObjCContext.

#ifndef NS_GSD_BACKEND_PREPARED
#define NS_GSD_BACKEND_PREPARED 1
#endif

#ifndef NS_GSD_BACKEND_NAPI
#define NS_GSD_BACKEND_NAPI 0
#endif

#ifndef NS_GSD_BACKEND_HERMES
#define NS_GSD_BACKEND_HERMES 0
#endif

#ifndef NS_HAS_GENERATED_SIGNATURE_DISPATCH
#define NS_HAS_GENERATED_SIGNATURE_DISPATCH 0
#endif

#ifndef NS_HAS_GENERATED_SIGNATURE_GSD_DISPATCH
#define NS_HAS_GENERATED_SIGNATURE_GSD_DISPATCH 0
#endif

// NOTE: GeneratedGsdSignatureDispatch.inc is included from
// NativeApiV8SelectorGroups.mm after GsdObjCContext is defined (avoids
// namespace ordering issues).

// The main .inc (prepared invokers + tables) is included here.
#if defined(__has_include)
#if __has_include("../shared/GeneratedSignatureDispatch.inc")
#include "../shared/GeneratedSignatureDispatch.inc"
#endif
#endif

#include "ffi/objc/shared/PreparedSignatureDispatch.h"

#endif  // NATIVESCRIPT_FFI_V8_SIGNATURE_DISPATCH_H
