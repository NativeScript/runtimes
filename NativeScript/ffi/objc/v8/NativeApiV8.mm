#include "NativeApiV8.h"

#ifdef TARGET_ENGINE_V8

#include "NativeApiV8Runtime.h"
#include "SignatureDispatch.h"

namespace nativescript {

namespace {

using nativescript::engine::Array;
using nativescript::engine::ArrayBuffer;
using nativescript::engine::BigInt;
using nativescript::engine::Function;
using nativescript::engine::HostObject;
using nativescript::engine::MutableBuffer;
using nativescript::engine::Object;
using nativescript::engine::PropNameID;
using nativescript::engine::Runtime;
using nativescript::engine::String;
using nativescript::engine::StringBuffer;
using nativescript::engine::Value;
using nativescript::engine::JSError;
using metagen::MDMemberFlag;
using metagen::MDMetadataReader;
using metagen::MDSectionOffset;
using metagen::MDTypeKind;

// clang-format off
#define NATIVESCRIPT_NATIVE_API_BACKEND_NAME "v8"
#include "../shared/bridge/ObjCBridge.mm"
// clang-format on

#define NATIVESCRIPT_NATIVE_API_HAS_ENGINE_LAZY_GLOBALS 1
#define NATIVESCRIPT_NATIVE_API_HAS_ENGINE_SELECTOR_GROUP_FUNCTION 1
#define NATIVESCRIPT_NATIVE_API_RETAIN_RUNTIME 1
#define NATIVESCRIPT_NATIVE_API_RUNTIME_SCOPE 1

#include "NativeApiV8RuntimeSupport.mm"

// clang-format off
#include "../shared/bridge/HostObjects.mm"
#include "../shared/bridge/Callbacks.mm"
#include "../shared/bridge/TypeConv.mm"
#include "../shared/bridge/Invocation.mm"
#include "../shared/bridge/ClassBuilder.mm"
#include "../shared/bridge/HostObject.mm"
// clang-format on


#include "NativeApiV8SelectorGroups.mm"

}  // namespace

#include "../shared/bridge/Install.mm"

void InstallNativeApi(v8::Isolate* isolate, v8::Local<v8::Context> context,
                        const NativeApiConfig& config) {
  if (isolate == nullptr || context.IsEmpty()) {
    return;
  }
  v8::Locker locker(isolate);
  v8::Isolate::Scope isolateScope(isolate);
  v8::HandleScope handleScope(isolate);
  v8::Context::Scope contextScope(context);
  Runtime runtime(isolate, context);
  InstallNativeApi(runtime, config);
}

void CleanupNativeApi(v8::Isolate* isolate) {
  if (isolate != nullptr) {
    engine::v8engine::cleanupRuntimeAllocations(isolate);
  }
}

}  // namespace nativescript

extern "C" void NativeScriptInstallNativeApi(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                               const char* metadataPath) {
  nativescript::NativeApiConfig config;
  config.metadataPath = metadataPath;
  nativescript::InstallNativeApi(isolate, context, config);
}

#endif  // TARGET_ENGINE_V8
