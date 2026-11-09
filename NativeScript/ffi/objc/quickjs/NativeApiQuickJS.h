#ifndef NATIVESCRIPT_FFI_QUICKJS_NATIVE_API_QUICKJS_H
#define NATIVESCRIPT_FFI_QUICKJS_NATIVE_API_QUICKJS_H

#include "ffi/objc/shared/NativeApiBackendConfig.h"
#include "quickjs.h"

namespace nativescript {

using NativeApiScheduler = NativeApiBackendScheduler;
using NativeApiConfig = NativeApiBackendConfig;

void InstallNativeApi(JSContext* context,
                             const NativeApiConfig& config =
                                 NativeApiConfig{});
void CleanupNativeApi(JSContext* context);

}  // namespace nativescript

extern "C" void NativeScriptInstallNativeApi(JSContext* context,
                                                     const char* metadataPath);

#endif  // NATIVESCRIPT_FFI_QUICKJS_NATIVE_API_QUICKJS_H
