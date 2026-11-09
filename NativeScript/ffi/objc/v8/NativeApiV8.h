#ifndef NATIVESCRIPT_FFI_V8_NATIVE_API_V8_H
#define NATIVESCRIPT_FFI_V8_NATIVE_API_V8_H

#include "ffi/objc/shared/NativeApiBackendConfig.h"
#include "v8.h"

namespace nativescript {

using NativeApiScheduler = NativeApiBackendScheduler;
using NativeApiConfig = NativeApiBackendConfig;

void InstallNativeApi(v8::Isolate* isolate,
                        v8::Local<v8::Context> context,
                        const NativeApiConfig& config = NativeApiConfig{});
void CleanupNativeApi(v8::Isolate* isolate);

}  // namespace nativescript

extern "C" void NativeScriptInstallNativeApi(v8::Isolate* isolate,
                                                v8::Local<v8::Context> context,
                                                const char* metadataPath);

#endif  // NATIVESCRIPT_FFI_V8_NATIVE_API_V8_H
