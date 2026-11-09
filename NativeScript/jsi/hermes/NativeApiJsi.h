#ifndef NATIVE_API_JSI_H
#define NATIVE_API_JSI_H

#include <jsi/jsi.h>

#include "jsi/shared/NativeApiBackendConfig.h"

namespace nativescript {

using NativeApiJsiScheduler = NativeApiBackendScheduler;
using NativeApiJsiConfig = NativeApiBackendConfig;

facebook::jsi::Object CreateNativeApiJSI(
    facebook::jsi::Runtime& runtime,
    const NativeApiJsiConfig& config = NativeApiJsiConfig{});

void InstallNativeApiJSI(
    facebook::jsi::Runtime& runtime,
    const NativeApiJsiConfig& config = NativeApiJsiConfig{});

// Wraps an Objective-C object in the same bridge host object used by ordinary
// native crossings. `void*` keeps this contract usable from plain C++ callers.
// When `ownsObject` is true, the wrapper consumes the caller's +1 retain.
facebook::jsi::Value NativeScriptWrapNativeObject(
    facebook::jsi::Runtime& runtime, void* object, bool ownsObject = false);

// Returns the native pointer behind a live bridge wrapper, or nullptr when the
// value is not a supported native-object wrapper.
void* NativeScriptUnwrapNativeObject(facebook::jsi::Runtime& runtime,
                                     const facebook::jsi::Value& value);

}  // namespace nativescript

extern "C" void NativeScriptInstallNativeApiJSI(
    facebook::jsi::Runtime* runtime, const char* metadataPath);

#endif  // NATIVE_API_JSI_H
