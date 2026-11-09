#ifndef NATIVESCRIPT_FFI_SHARED_PREPARED_SIGNATURE_DISPATCH_H
#define NATIVESCRIPT_FFI_SHARED_PREPARED_SIGNATURE_DISPATCH_H

// Moved to jsi/shared/PreparedSignatureDispatch.h so the Android runtime can
// use it. This forwarding header keeps the Apple include paths unchanged.
//
// SignatureDispatchCore.h is included first because callers of this header
// have always got it transitively, and it is what computes the dispatch ids
// from the Objective-C metadata.

#include "ffi/objc/shared/SignatureDispatchCore.h"
#include "jsi/shared/PreparedSignatureDispatch.h"

#endif  // NATIVESCRIPT_FFI_SHARED_PREPARED_SIGNATURE_DISPATCH_H
