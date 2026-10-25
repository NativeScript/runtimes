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
// js_jsr_for_runtime: maps the bridge's unsafe jsi::Runtime back to the
// ThreadSafeRuntime that guards it (see NativeApiRuntimeScope below). Only the
// standalone runtime owns a JSR; @nativescript/react-native ships this
// translation unit without napi/, so the include must not be reached there.
#include "napi/hermes/jsr.h"
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
using facebook::jsi::Object;
using facebook::jsi::PropNameID;
using facebook::jsi::Runtime;
using facebook::jsi::String;
using facebook::jsi::StringBuffer;
using facebook::jsi::Value;
using facebook::jsi::JSError;

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

// Entering the VM has to take the ThreadSafeRuntime lock, because native
// callbacks and host-object accessors run on whatever thread the platform
// hands them to -- an NSOperationQueue worker, a URLSession delegate queue --
// and the bridge only holds getUnsafeRuntime(). Without this, Callbacks.mm
// falls back to its empty default scope and lets two threads into the
// interpreter at once, which corrupts the handle stack and GC roots. V8
// supplies the same thing as a v8::Locker (see NativeApiV8RuntimeSupport.mm).
// HermesMutex is a recursive_mutex, so nesting these is safe.
//
// Not under @nativescript/react-native: there the bridge is installed into a
// jsi::Runtime React Native created and owns, so no JSR exists to look up and
// serialization is React Native's job (its JS thread and CallInvoker). That
// build falls through to the empty default scope in Callbacks.mm, which is what
// it has always used. The guard is negative on purpose -- a build that forgets
// to define the macro keeps the lock rather than silently losing it.
#ifndef NATIVESCRIPT_REACT_NATIVE
#define NATIVESCRIPT_NATIVE_API_RUNTIME_SCOPE 1

class NativeApiRuntimeScope final {
 public:
  explicit NativeApiRuntimeScope(Runtime& runtime)
      : jsr_(js_jsr_for_runtime(&runtime)) {
    if (jsr_ != nullptr) {
      jsr_->lock();
    }
  }

  ~NativeApiRuntimeScope() {
    if (jsr_ != nullptr) {
      jsr_->unlock();
    }
  }

  NativeApiRuntimeScope(const NativeApiRuntimeScope&) = delete;
  NativeApiRuntimeScope& operator=(const NativeApiRuntimeScope&) = delete;

 private:
  JSR* jsr_;
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
          }
          // Fall through rather than else-if: the bound receiver is weak, so a
          // collected wrapper leaves it empty, and the call still has a perfectly
          // good receiver in thisValue. Binding must not turn a live call into
          // "requires a native receiver". SelectorGroupCall.h already resolves it
          // this way; this lambda was the one place that did not.
          if (!receiverHostObject && thisValue.isObject()) {
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
        // typed cast, produce the jsi return value — bypassing all generic
        // marshalling. Only engages for plain calls (no super dispatch, init
        // disown handling, or implicit NSError-out argument).
        if (call.prepared->gsdEngineCallable && call.dispatchClass == Nil &&
            count == call.prepared->gsdEngineArgumentCount &&
            !(!state.receiverIsClass && call.prepared->isInitMethod)) {
          auto invoker =
              reinterpret_cast<ObjCGsdInvoker>(call.prepared->engineInvoker);
          GsdObjCContext ctx{runtime, state.bridge, call.receiver,
                             call.prepared->selector, args,
                             call.prepared->signature.returnType};
          if (invoker(ctx)) {
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
        return receiverHostObject->callPreparedObjectSelector(
            runtime, *call.prepared, args, count, call.dispatchClass);
      });
}

#define NATIVESCRIPT_NATIVE_API_HAS_ENGINE_RUNTIME_TEARDOWN 1

// Runtime data is destroyed with the runtime while its Values can still be
// released: Hermes frees it before tearing down its heap, and jsi's default
// implementation frees it from a finalizer of that runtime's own global.
class NativeApiRuntimeBridges final {
 public:
  ~NativeApiRuntimeBridges() {
    for (auto& bridge : bridges_) {
      bridge->releaseRuntimeValues();
    }
  }

  void add(std::shared_ptr<NativeApiBridge> bridge) {
    bridges_.push_back(std::move(bridge));
  }

 private:
  std::vector<std::shared_ptr<NativeApiBridge>> bridges_;
};

constexpr facebook::jsi::UUID kNativeApiRuntimeBridgesUUID{
    0x5f0e2c71, 0x8b3d, 0x4a6e, 0x9c41, 0x2d7a6b13e8f4};

void BindNativeApiBridgeToRuntime(Runtime& runtime,
                                  std::shared_ptr<NativeApiBridge> bridge) {
  auto bridges = std::static_pointer_cast<NativeApiRuntimeBridges>(
      runtime.getRuntimeData(kNativeApiRuntimeBridgesUUID));
  if (bridges == nullptr) {
    bridges = std::make_shared<NativeApiRuntimeBridges>();
    runtime.setRuntimeData(kNativeApiRuntimeBridgesUUID, bridges);
  }
  bridges->add(std::move(bridge));
}

}  // namespace

#include "../shared/bridge/Install.mm"

Object CreateNativeApiJSI(Runtime& runtime, const NativeApiJsiConfig& config) {
  return CreateNativeApi(runtime, config);
}

void InstallNativeApiJSI(Runtime& runtime, const NativeApiJsiConfig& config) {
  InstallNativeApi(runtime, config);
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
