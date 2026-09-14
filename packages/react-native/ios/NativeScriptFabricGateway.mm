#include "NativeScriptFabricGateway.h"

#import <Foundation/Foundation.h>
#import <pthread.h>

#include <atomic>
#include <mutex>

using facebook::jsi::Function;
using facebook::jsi::Object;
using facebook::jsi::Runtime;
using facebook::jsi::Value;

namespace nativescript {

namespace {

std::mutex& UIRuntimeMutex() {
  static std::mutex mutex;
  return mutex;
}

std::weak_ptr<worklets::WorkletRuntime>& UIRuntimeWeak() {
  static std::weak_ptr<worklets::WorkletRuntime> runtime;
  return runtime;
}

std::atomic<uint64_t>& UIRuntimeGenerationCounter() {
  static std::atomic<uint64_t> generation{0};
  return generation;
}

std::shared_ptr<worklets::UIScheduler>& UISchedulerStorage() {
  static std::shared_ptr<worklets::UIScheduler> scheduler;
  return scheduler;
}

std::mutex& ComponentSpecMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, NativeScriptComponentSpecEntry>& ComponentSpecs() {
  static std::unordered_map<std::string, NativeScriptComponentSpecEntry> specs;
  return specs;
}

// name -> the UI-runtime generation TS last confirmed it has a materialized
// copy of that name's spec for. Reset implicitly by generation mismatch
// (never explicitly cleared; stale entries for old generations are simply
// never matched again).
std::unordered_map<std::string, uint64_t>& MaterializedGenerationByName() {
  static std::unordered_map<std::string, uint64_t> materialized;
  return materialized;
}

}  // namespace

void NativeScriptFabricGatewaySetUIRuntime(std::shared_ptr<worklets::WorkletRuntime> runtime) {
  std::lock_guard<std::mutex> lock(UIRuntimeMutex());
  UIRuntimeWeak() = runtime;
  if (runtime != nullptr) {
    UIRuntimeGenerationCounter().fetch_add(1, std::memory_order_relaxed);
  }
}

std::shared_ptr<worklets::WorkletRuntime> NativeScriptFabricGatewayGetUIRuntime() {
  std::lock_guard<std::mutex> lock(UIRuntimeMutex());
  return UIRuntimeWeak().lock();
}

uint64_t NativeScriptFabricGatewayGeneration() {
  return UIRuntimeGenerationCounter().load(std::memory_order_relaxed);
}

void NativeScriptFabricGatewaySetUIScheduler(std::shared_ptr<worklets::UIScheduler> scheduler) {
  std::lock_guard<std::mutex> lock(UIRuntimeMutex());
  UISchedulerStorage() = std::move(scheduler);
}

void NativeScriptFabricGatewayScheduleOnUI(std::function<void()> job) {
  std::shared_ptr<worklets::UIScheduler> scheduler;
  {
    std::lock_guard<std::mutex> lock(UIRuntimeMutex());
    scheduler = UISchedulerStorage();
  }
  if (scheduler != nullptr) {
    worklets::scheduleOnUI(scheduler, job);
    return;
  }
  // No UIScheduler installed yet (e.g. called before bootstrap); fall back
  // to a plain main-queue hop rather than dropping the job. Still "no
  // blocking cross-thread waits" (§3.4): dispatch_async, never _sync.
  auto jobBox = std::make_shared<std::function<void()>>(std::move(job));
  dispatch_async(dispatch_get_main_queue(), ^{
    (*jobBox)();
  });
}

bool NativeScriptFabricGatewayIsOnEntryThread() {
  return pthread_main_np() != 0;
}

void NativeScriptFabricGatewayRegisterComponentSpec(const std::string& name,
                                                    std::shared_ptr<worklets::Serializable> serializable,
                                                    uint32_t hookMask) {
  std::lock_guard<std::mutex> lock(ComponentSpecMutex());
  ComponentSpecs()[name] = NativeScriptComponentSpecEntry{std::move(serializable), hookMask};
  // M1 review §3/#4 + §5/#4: a fast-refresh re-invocation of
  // `defineNativeComponent("name", ...)` lands a NEW serializable here
  // WITHOUT the UI runtime's generation having changed; if
  // `MaterializedGenerationByName()[name]` still says "already materialized
  // for the current generation", DispatchComponentHook's own generation
  // check (below) would never re-materialize, and the UI runtime keeps
  // executing the STALE hooks from the previous edit until a full reload.
  // Erasing here forces the next dispatch to re-materialize unconditionally,
  // regardless of whether the generation itself moved.
  MaterializedGenerationByName().erase(name);
}

uint32_t NativeScriptFabricGatewayHookMaskForComponent(const std::string& name) {
  std::lock_guard<std::mutex> lock(ComponentSpecMutex());
  auto it = ComponentSpecs().find(name);
  return it != ComponentSpecs().end() ? it->second.hookMask : 0;
}

Value NativeScriptFabricGatewayDispatchComponentHook(Runtime& rt, const std::string& name, double tag,
                                                      const std::string& hookName, const Value& view,
                                                      const Value& a, const Value& b, const Value& c) {
  uint64_t currentGeneration = NativeScriptFabricGatewayGeneration();

  // 1. Ensure TS has this name's materialized spec for the current
  // generation (at most once per name per generation).
  bool needsMaterialize = false;
  {
    std::lock_guard<std::mutex> lock(ComponentSpecMutex());
    auto materializedIt = MaterializedGenerationByName().find(name);
    needsMaterialize =
        materializedIt == MaterializedGenerationByName().end() || materializedIt->second != currentGeneration;
  }
  if (needsMaterialize) {
    std::shared_ptr<worklets::Serializable> serializable;
    {
      std::lock_guard<std::mutex> lock(ComponentSpecMutex());
      auto specIt = ComponentSpecs().find(name);
      if (specIt != ComponentSpecs().end()) {
        serializable = specIt->second.serializable;
      }
    }
    if (serializable == nullptr) {
      return Value::undefined();
    }
    Value registerFnValue = rt.global().getProperty(rt, "__nativeScriptRegisterMaterializedSpec");
    if (!registerFnValue.isObject() || !registerFnValue.asObject(rt).isFunction(rt)) {
      return Value::undefined();
    }
    Value specValue = serializable->toJSValue(rt);
    registerFnValue.asObject(rt).asFunction(rt).call(
        rt, Value(rt, facebook::jsi::String::createFromUtf8(rt, name)), std::move(specValue));
    std::lock_guard<std::mutex> lock(ComponentSpecMutex());
    MaterializedGenerationByName()[name] = currentGeneration;
  }

  // 2. Fetch the one TS dispatcher function; a fresh global-object property
  // lookup every call (same pattern as step 1's
  // __nativeScriptRegisterMaterializedSpec lookup above), NOT cached across
  // calls. A previous version of this function cached the resolved
  // `jsi::Function` in a process-lifetime `static std::shared_ptr<Function>`
  // keyed by generation; that crashed (SIGSEGV in jsi::Function::~Function,
  // observed via a real on-sim debug-configuration run) even on a FIRST-EVER
  // dispatch, before any reload/generation change could be involved --
  // holding a jsi::Function handle in a static that outlives the call stack
  // it was resolved in is exactly the kind of "per-crossing memo machinery"
  // DECISIONS.md's "Never reintroduce" list warns about, and a HashMap
  // lookup on the global object per hook dispatch is not worth reintroducing
  // that risk for. See the M1 verification report for the crash detail.
  Value dispatchFnValue = rt.global().getProperty(rt, "__nativeScriptDispatchComponentHook");
  if (!dispatchFnValue.isObject() || !dispatchFnValue.asObject(rt).isFunction(rt)) {
    return Value::undefined();
  }
  Function dispatchFn = dispatchFnValue.asObject(rt).asFunction(rt);

  return dispatchFn.call(
      rt, Value(rt, facebook::jsi::String::createFromUtf8(rt, name)), tag,
      Value(rt, facebook::jsi::String::createFromUtf8(rt, hookName)), Value(rt, view), Value(rt, a),
      Value(rt, b), Value(rt, c));
}

}  // namespace nativescript
