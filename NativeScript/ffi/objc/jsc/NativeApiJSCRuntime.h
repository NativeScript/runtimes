#ifndef NATIVESCRIPT_FFI_JSC_NATIVE_API_JSC_RUNTIME_H
#define NATIVESCRIPT_FFI_JSC_NATIVE_API_JSC_RUNTIME_H

// Apple-side entry point for the JavaScriptCore engine layer.
//
// The engine layer itself -- the `nativescript::engine` classes wrapping JSC
// -- moved to jsi/jsc/JSCRuntime.h so the Android runtime can compile it too.
// What stays here is exactly what the Apple ObjC bridge needs on top of it and
// Android has no use for: Foundation, the Objective-C runtime, the Mach-O
// metadata section lookup, and the metadata reader.
//
// Apple sources include this path, unchanged, and get both halves.

#ifdef TARGET_ENGINE_JSC

#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include "Metadata.h"
#include "MetadataReader.h"
#include "ffi.h"

#include "jsi/jsc/JSCRuntime.h"

@protocol NativeApiClassBuilderProtocol
@end

#ifdef EMBED_METADATA_SIZE
extern const unsigned char embedded_metadata[EMBED_METADATA_SIZE];
#endif

#endif  // TARGET_ENGINE_JSC

#endif  // NATIVESCRIPT_FFI_JSC_NATIVE_API_JSC_RUNTIME_H
