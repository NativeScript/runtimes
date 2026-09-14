#ifndef NATIVESCRIPT_FFI_JSC_NATIVE_API_JSC_H
#define NATIVESCRIPT_FFI_JSC_NATIVE_API_JSC_H

#include "ffi/objc/shared/NativeApiBackendConfig.h"
#include <JavaScriptCore/JavaScript.h>

namespace nativescript {

using NativeApiScheduler = NativeApiBackendScheduler;
using NativeApiConfig = NativeApiBackendConfig;

void InstallNativeApi(JSGlobalContextRef context,
                         const NativeApiConfig& config = NativeApiConfig{});
void CleanupNativeApi(JSGlobalContextRef context);

}  // namespace nativescript

extern "C" void NativeScriptInstallNativeApi(JSGlobalContextRef context,
                                                 const char* metadataPath);

#endif  // NATIVESCRIPT_FFI_JSC_NATIVE_API_JSC_H
