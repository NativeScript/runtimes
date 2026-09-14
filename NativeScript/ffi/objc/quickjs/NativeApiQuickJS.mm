#include "NativeApiQuickJS.h"

#ifdef TARGET_ENGINE_QUICKJS

#include "NativeApiQuickJSRuntime.h"
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
#define NATIVESCRIPT_NATIVE_API_HOST_EXPLICIT_OVERRIDE 1
#define NATIVESCRIPT_NATIVE_API_BACKEND_NAME "quickjs"
#include "../shared/bridge/ObjCBridge.mm"
// clang-format on

#define NATIVESCRIPT_NATIVE_API_HAS_ENGINE_LAZY_GLOBALS 1
#define NATIVESCRIPT_NATIVE_API_RETAIN_RUNTIME 1
#define NATIVESCRIPT_NATIVE_API_HAS_ENGINE_SELECTOR_GROUP_FUNCTION 1

#include "NativeApiQuickJSRuntimeSupport.mm"

// clang-format off
#include "../shared/bridge/HostObjects.mm"
#include "../shared/bridge/Callbacks.mm"
#include "../shared/bridge/TypeConv.mm"
#include "../shared/bridge/Invocation.mm"
#include "../shared/bridge/ClassBuilder.mm"
#include "../shared/bridge/HostObject.mm"
// clang-format on

#include "NativeApiQuickJSSelectorGroups.mm"

}  // namespace

#include "../shared/bridge/Install.mm"

void InstallNativeApi(JSContext* context, const NativeApiConfig& config) {
  if (context == nullptr) {
    return;
  }
  auto state = engine::quickjsengine::stateForContext(context);
  nativescript::engine::Runtime runtime(state);
  engine::quickjsengine::ensureClasses(runtime);
  InstallNativeApi(runtime, config);
}

void CleanupNativeApi(JSContext* context) {
  if (context != nullptr) {
    engine::quickjsengine::releaseStateForContext(context);
  }
}

}  // namespace nativescript

extern "C" void NativeScriptInstallNativeApi(JSContext* context, const char* metadataPath) {
  nativescript::NativeApiConfig config;
  config.metadataPath = metadataPath;
  nativescript::InstallNativeApi(context, config);
}

#endif  // TARGET_ENGINE_QUICKJS
