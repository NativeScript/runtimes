#ifndef NATIVE_API_JSI_H
#define NATIVE_API_JSI_H

#include <jsi/jsi.h>

#include "ffi/objc/shared/NativeApiBackendConfig.h"

namespace nativescript {

using NativeApiJsiScheduler = NativeApiBackendScheduler;
using NativeApiJsiConfig = NativeApiBackendConfig;

facebook::jsi::Object CreateNativeApiJSI(
    facebook::jsi::Runtime& runtime,
    const NativeApiJsiConfig& config = NativeApiJsiConfig{});

void InstallNativeApiJSI(
    facebook::jsi::Runtime& runtime,
    const NativeApiJsiConfig& config = NativeApiJsiConfig{});

// M1 (ARCHITECTURE.md §4.3): the two ObjC<->JSI helpers that make the
// by-reference Fabric handoff possible; "the Fabric boundary must hand JS
// a real bridge-wrapped object, not a string handle"
// (CLEANUP_AND_REARCHITECTURE_PLAN.md §2.0). Both wrap the SAME
// NativeApiObjectHostObject mechanism every other native object crossing in
// this bridge already uses (Object.mm/Class.mm); so a wrapped value
// round-trips through the identical `nativeValue(...)`-style method dispatch
// as any other bridged object, not a bespoke RPC.
//
// `object`/the return value are `void*`-typed ObjC `id`s, kept untyped here
// (not `id`) so this header stays includable from a plain C++ translation
// unit that never imports Objective-C (e.g. runtime/apple/Runtime.cpp,
// which includes this header under `#ifdef TARGET_ENGINE_HERMES` without
// itself being compiled as Objective-C++); the same convention
// NativeApiBackendConfig.h already follows.
//
// Only implemented for the Hermes backend (this header/its .mm are
// Hermes-only, ffi/objc/hermes/); RN only ever uses Hermes, so this does not
// touch the V8/JSC/QuickJS engine backends or their standalone builds.
//
// If `ownsObject` is true, the wrapper takes over a +1 retain already held
// by the caller (matching `makeNativeObjectValue`'s `ownsObject` semantics
// used everywhere else in the bridge); if false, the wrapper retains its own
// reference and the caller's reference is untouched.
facebook::jsi::Value NativeScriptWrapNativeObject(facebook::jsi::Runtime& runtime,
                                                  void* object,
                                                  bool ownsObject = false);

// Reverse direction: given a JSI value produced by NativeScriptWrapNativeObject
// (or any other native-object-wrapping mechanism the bridge already uses --
// NativeApiObjectHostObject, NativeApiPointerHostObject,
// NativeApiReferenceHostObject), returns the underlying native pointer, or
// nullptr if `value` does not wrap a live native object.
void* NativeScriptUnwrapNativeObject(facebook::jsi::Runtime& runtime,
                                     const facebook::jsi::Value& value);

}  // namespace nativescript

extern "C" void NativeScriptInstallNativeApiJSI(
    facebook::jsi::Runtime* runtime, const char* metadataPath);

#endif  // NATIVE_API_JSI_H
