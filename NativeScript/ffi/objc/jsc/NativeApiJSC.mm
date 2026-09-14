#include "NativeApiJSC.h"

#ifdef TARGET_ENGINE_JSC

#include "NativeApiJSCRuntime.h"
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
#define NATIVESCRIPT_NATIVE_API_BACKEND_NAME "jsc"
#include "../shared/bridge/ObjCBridge.mm"
// clang-format on
#define NATIVESCRIPT_NATIVE_API_RETAIN_RUNTIME 1
#define NATIVESCRIPT_NATIVE_API_HAS_ENGINE_SELECTOR_GROUP_FUNCTION 1

#include "NativeApiJSCRuntimeSupport.mm"

// clang-format off
#include "../shared/bridge/HostObjects.mm"
#include "../shared/bridge/Callbacks.mm"
#include "../shared/bridge/TypeConv.mm"
#include "../shared/bridge/Invocation.mm"
#include "../shared/bridge/ClassBuilder.mm"
#include "../shared/bridge/HostObject.mm"
// clang-format on

#include "NativeApiJSCSelectorGroups.mm"

}  // namespace

#include "../shared/bridge/Install.mm"

void InstallNativeApi(JSGlobalContextRef context, const NativeApiConfig& config) {
  if (context == nullptr) {
    return;
  }
  Runtime runtime(context);
  InstallNativeApi(runtime, config);
}

void CleanupNativeApi(JSGlobalContextRef context) {
  if (context != nullptr) {
    engine::jscengine::releaseStateForContext(context);
  }
}

}  // namespace nativescript

extern "C" void NativeScriptInstallNativeApi(JSGlobalContextRef context,
                                                const char* metadataPath) {
  nativescript::NativeApiConfig config;
  config.metadataPath = metadataPath;
  nativescript::InstallNativeApi(context, config);
}

#endif  // TARGET_ENGINE_JSC
