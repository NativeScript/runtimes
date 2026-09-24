#include "NativeApiJsi.h"

#ifdef TARGET_ENGINE_HERMES

#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Metadata.h"
#include "MetadataReader.h"
#include "ffi.h"
#include "NativeApiJsiSignatureDispatch.h"

#ifndef NATIVESCRIPT_REACT_NATIVE
extern "C" void* js_lock_unsafe_jsi_runtime(facebook::jsi::Runtime* runtime);
extern "C" void js_unlock_unsafe_jsi_runtime(void* lockToken);
#endif

@protocol NativeApiClassBuilderProtocol
@end

#ifdef EMBED_METADATA_SIZE
extern const unsigned char embedded_metadata[EMBED_METADATA_SIZE];
#endif

namespace nativescript {
namespace {

using facebook::jsi::Array;
using facebook::jsi::ArrayBuffer;
using facebook::jsi::BigInt;
using facebook::jsi::Function;
using facebook::jsi::HostObject;
using facebook::jsi::MutableBuffer;
using NativeApiNativeState = facebook::jsi::NativeState;
using facebook::jsi::Object;
using facebook::jsi::PropNameID;
using facebook::jsi::Runtime;
using facebook::jsi::String;
using facebook::jsi::StringBuffer;
using facebook::jsi::Value;
using facebook::jsi::JSError;
using facebook::jsi::WeakObject;

using NativeApiConfig = NativeApiJsiConfig;
using NativeApiScheduler = NativeApiJsiScheduler;
using metagen::MDMemberFlag;
using metagen::MDMetadataReader;
using metagen::MDSectionOffset;
using metagen::MDTypeKind;

void SetNativeApiObjectPrototype(Runtime& runtime, Object& object,
                                       const Object& prototype) {
  Object objectConstructor =
      runtime.global().getPropertyAsObject(runtime, "Object");
  Function setPrototypeOf =
      objectConstructor.getPropertyAsFunction(runtime, "setPrototypeOf");
  setPrototypeOf.call(runtime, Value(runtime, object), Value(runtime, prototype));
}

// Native callbacks can arrive on arbitrary platform threads. Entering Hermes
// through the unsafe JSI runtime without the ThreadSafeRuntime lock permits two
// threads to mutate the VM concurrently. This recursive scope is also safe for
// host operations that are already running on the JS thread.
//
// Not under @nativescript/react-native: there the bridge is installed into a
// React Native-owned jsi::Runtime, so no standalone runtime lock exists and
// serialization remains React Native's responsibility.
#ifndef NATIVESCRIPT_REACT_NATIVE
#define NATIVESCRIPT_NATIVE_API_RUNTIME_SCOPE 1

class NativeApiRuntimeScope final {
 public:
  explicit NativeApiRuntimeScope(Runtime& runtime)
      : lockToken_(js_lock_unsafe_jsi_runtime(&runtime)) {}

  ~NativeApiRuntimeScope() {
    if (lockToken_ != nullptr) {
      js_unlock_unsafe_jsi_runtime(lockToken_);
    }
  }

  NativeApiRuntimeScope(const NativeApiRuntimeScope&) = delete;
  NativeApiRuntimeScope& operator=(const NativeApiRuntimeScope&) = delete;

 private:
  void* lockToken_;
};
#endif  // NATIVESCRIPT_REACT_NATIVE

// clang-format off
#define NATIVESCRIPT_NATIVE_API_RUNTIME_NAME "jsi"
#define NATIVESCRIPT_NATIVE_API_BACKEND_NAME "hermes"
#define NATIVESCRIPT_NATIVE_API_HOST_SET_VOID 1
#define NATIVESCRIPT_NATIVE_API_HOST_EXPLICIT_OVERRIDE 1
#define NATIVESCRIPT_NATIVE_API_HAS_ENGINE_SELECTOR_GROUP_FUNCTION 1
#include "../shared/bridge/ObjCBridge.mm"
#include "../shared/bridge/HostObjects.mm"
#include "../shared/bridge/Callbacks.mm"
#include "../shared/bridge/TypeConv.mm"
#include "../shared/bridge/Invocation.mm"
#include "../shared/bridge/ClassBuilder.mm"
#include "../shared/bridge/HostObject.mm"
// clang-format on

#include "NativeApiJsiGsd.mm"

#include "../shared/bridge/SelectorGroupCall.h"


void* lookupGeneratedEngineObjCGsdInvoker(uint64_t dispatchId) {
  return reinterpret_cast<void*>(lookupObjCGsdInvoker(dispatchId));
}

bool tryCallGeneratedEngineObjCSelector(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id receiver, const NativeApiPreparedObjCInvocation& prepared,
    const Value* args, size_t count, Class dispatchSuperClass, Value* result) {
  if (result == nullptr || receiver == nil ||
      !prepared.gsdEngineCallable || dispatchSuperClass != Nil ||
      count != prepared.gsdEngineArgumentCount) {
    return false;
  }

  auto invoker = reinterpret_cast<ObjCGsdInvoker>(prepared.engineInvoker);
  GsdObjCContext ctx{runtime, bridge, receiver, prepared.selector, args,
                     prepared.signature.returnType};
  if (!invoker(ctx)) {
    return false;
  }
  *result = std::move(ctx.result);
  return true;
}

Function CreateNativeApiSelectorGroupFunctionImpl(
    Runtime& runtime, std::shared_ptr<NativeApiBridge> bridge,
    Class lookupClass, bool receiverIsClass,
    std::shared_ptr<std::vector<NativeApiSelectorGroupEntry>> selectors,
    std::shared_ptr<
        std::vector<std::shared_ptr<NativeApiPreparedObjCInvocation>>>
        preparedInvocations,
    std::weak_ptr<NativeApiObjectHostObject> boundReceiver,
    std::shared_ptr<NativeApiObjectLifetimeState> boundReceiverState) {
  NativeApiSelectorGroupState state(
      std::move(bridge), lookupClass, receiverIsClass, std::move(selectors),
      std::move(preparedInvocations), std::move(boundReceiver),
      std::move(boundReceiverState));
  return Function::createFromHostFunction(
      runtime, PropNameID::forAscii(runtime, "__nativeSelectorGroup"), 0,
      [state = std::move(state)](
          Runtime& runtime, const Value& thisValue, const Value* args,
          size_t count) mutable -> Value {
        NativeApiRoundTripCacheFrameGuard roundTripFrame(state.bridge);
        std::shared_ptr<NativeApiObjectHostObject> receiverHostObject;
        auto resolveReceiverHost = [&]() {
          if (receiverHostObject) {
            return receiverHostObject;
          }
          if (state.boundReceiverState != nullptr) {
            receiverHostObject = state.boundReceiver.lock();
            if (receiverHostObject && receiverHostObject->object() != nil) {
              return receiverHostObject;
            }
            receiverHostObject.reset();
            // The bound receiver's wrapper has already been torn down (its
            // owning JS proxy was collected) since this selector-group
            // function was minted and cached as a native-object expando
            // (Object.mm's `bridge_->setObjectExpando(..., methodFunction)`).
            // The expando itself is keyed by the native pointer and survives
            // wrapper churn, so a LATER crossing that re-wraps the SAME
            // native object in a fresh `NativeApiObjectHostObject` (this
            // runtime mints a new wrapper per crossing) finds the stale
            // cached function still bound to the dead original -- every call
            // through it then resolves a nil receiver and throws "Objective-C
            // selector requires a native receiver" even though the method is
            // being invoked on a perfectly live object right now. Fall
            // through to `thisValue` exactly like the unbound path below:
            // this IS a method call (`receiver.method(...)`), so `thisValue`
            // is always the correct, live receiver for this invocation.
          }
          if (thisValue.isObject()) {
            Object receiverObject = thisValue.asObject(runtime);
            if (receiverObject.isHostObject<NativeApiObjectHostObject>(
                    runtime)) {
              receiverHostObject =
                  receiverObject.getHostObject<NativeApiObjectHostObject>(
                      runtime);
            }
          }
          return receiverHostObject;
        };
        auto call = resolveNativeApiSelectorGroupCall<false>(
            runtime, state, count,
            [&]() -> id {
              auto host = resolveReceiverHost();
              return host != nullptr ? host->object() : nil;
            },
            resolveReceiverHost,
            [](uint64_t dispatchId) {
              return lookupObjCGsdInvoker(dispatchId);
            });
        if (call.hasImmediateResult) {
          return std::move(call.immediateResult);
        }

        // GSD fast path: read jsi args directly, call objc_msgSend with a
        // typed cast, produce the jsi return value , bypassing all generic
        // marshalling. Only engages for plain calls (no super dispatch, init
        // disown handling, implicit NSError-out argument, or appearance
        // static selector — those need the generic path's proxy tagging).
        if (call.prepared->gsdEngineCallable && call.dispatchClass == Nil &&
            count == call.prepared->gsdEngineArgumentCount &&
            !(!state.receiverIsClass && call.prepared->isInitMethod) &&
            call.gsdAllowed) {
          auto invoker =
              reinterpret_cast<ObjCGsdInvoker>(call.prepared->engineInvoker);
          GsdObjCContext ctx{runtime, state.bridge, call.receiver,
                             call.prepared->selector, args,
                             call.prepared->signature.returnType};
          if (invoker(ctx)) {
            cachePreparedAppearanceProxySetterValue(
                runtime, state.bridge, call.receiver, *call.prepared, args,
                count);
            return std::move(ctx.result);
          }
        }

        if (state.receiverIsClass) {
          return callPreparedObjCSelector(runtime, state.bridge, call.receiver,
                                          true, *call.prepared, args, count,
                                          Nil);
        }
        if (!receiverHostObject) {
          receiverHostObject = resolveReceiverHost();
        }
        if (!receiverHostObject) {
          throw JSError(runtime,
                        "Objective-C selector requires a native receiver.");
        }
        Value result = receiverHostObject->callPreparedObjectSelector(
            runtime, *call.prepared, args, count, call.dispatchClass);
        if (!state.receiverIsClass && call.prepared->isInitMethod) {
          if (auto preserved = preservedNativeApiInitializerSelfReturn(
                  runtime, state.bridge, call.receiver, result, thisValue)) {
            return std::move(*preserved);
          }
        }
        return result;
      });
}

}  // namespace

#include "../shared/bridge/Install.mm"

Object CreateNativeApiJSI(Runtime& runtime, const NativeApiJsiConfig& config) {
  return CreateNativeApi(runtime, config);
}

void InstallNativeApiJSI(Runtime& runtime, const NativeApiJsiConfig& config) {
  InstallNativeApi(runtime, config);
}

namespace {
// The bridge for a given runtime is reached the same way every other
// caller finds it: the `NativeApiHostObject` stashed under the well-known
// global name every InstallNativeApi call uses by default
// (NativeApiBackendConfig::globalName, "__nativeScriptNativeApi") --
// identical to how `nativeApiInstalled()` in NativeScriptNativeApiModule.mm
// already probes for this same global.
std::shared_ptr<NativeApiBridge> NativeScriptBridgeForRuntime(Runtime& runtime) {
  Value apiValue = runtime.global().getProperty(runtime, "__nativeScriptNativeApi");
  if (!apiValue.isObject()) {
    return nullptr;
  }
  Object apiObject = apiValue.asObject(runtime);
  if (!apiObject.isHostObject<NativeApiHostObject>(runtime)) {
    return nullptr;
  }
  return apiObject.getHostObject<NativeApiHostObject>(runtime)->bridge();
}
}  // namespace

Value NativeScriptWrapNativeObject(Runtime& runtime, void* object, bool ownsObject) {
  if (object == nullptr) {
    return Value::null();
  }
  auto bridge = NativeScriptBridgeForRuntime(runtime);
  if (bridge == nullptr) {
    return Value::null();
  }
  // __bridge: a plain ownership-neutral cast, valid identically whether this
  // translation unit is compiled ARC or MRC (unlike a raw C-style cast,
  // which ARC rejects for void* <-> id without an explicit bridge
  // annotation).
  return makeNativeObjectValue(runtime, bridge, (__bridge id)object, ownsObject);
}

void* NativeScriptUnwrapNativeObject(Runtime& runtime, const Value& value) {
  void* pointer = nullptr;
  if (readPointerLikeValue(runtime, value, &pointer)) {
    return pointer;
  }
  return nullptr;
}

}  // namespace nativescript

extern "C" void NativeScriptInstallNativeApiJSI(facebook::jsi::Runtime* runtime,
                                                const char* metadataPath) {
  if (runtime == nullptr) {
    return;
  }
  nativescript::NativeApiJsiConfig config;
  config.metadataPath = metadataPath;
  nativescript::InstallNativeApiJSI(*runtime, config);
}

#endif  // TARGET_ENGINE_HERMES
