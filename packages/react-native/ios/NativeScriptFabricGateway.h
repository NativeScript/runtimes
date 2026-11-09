#pragma once

// Connects native Fabric callbacks to the Worklets UI runtime. TypeScript owns
// component instances and hook dispatch. This gateway stores the runtime,
// scheduler, generation number, and serialized component definitions.

#include <jsi/jsi.h>
#include <worklets/Compat/StableApi.h>
#include <worklets/WorkletRuntime/WorkletRuntime.h>

#import <Foundation/Foundation.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace nativescript {

// Stores a weak runtime reference and increments the generation when Worklets
// installs a new UI runtime. Generation checks invalidate materialized specs
// after a reload.
void NativeScriptFabricGatewaySetUIRuntime(std::shared_ptr<worklets::WorkletRuntime> runtime);
std::shared_ptr<worklets::WorkletRuntime> NativeScriptFabricGatewayGetUIRuntime();
uint64_t NativeScriptFabricGatewayGeneration();

// Stores the Worklets scheduler used for asynchronous UI-runtime entry.
void NativeScriptFabricGatewaySetUIScheduler(std::shared_ptr<worklets::UIScheduler> scheduler);

// Schedules `job` through Worklets. The scheduler may run it inline on the
// main thread. `ctx.scheduleOnMainQueue` uses dispatch_async separately when
// it must wait for the next run-loop turn.
void NativeScriptFabricGatewayScheduleOnUI(std::function<void()> job);

// Returns true on the thread allowed to enter the UI runtime synchronously.
bool NativeScriptFabricGatewayIsOnEntryThread();

/*
 * Enters the UI runtime synchronously and returns `job(rt)`. Call this only
 * from the main thread. Debug builds assert when that contract is broken.
 *
 * Returns a default-constructed Result (via the bool out-param) if no UI
 * runtime is currently installed (e.g. called before bootstrap, or after the
 * UI VM was torn down by a Worklets reload and not yet reinstalled).
 */
template <typename Callable>
auto NativeScriptFabricGatewayRunSyncOnMain(Callable&& job, bool* ranOut = nullptr)
    -> decltype(job(std::declval<facebook::jsi::Runtime&>())) {
  using Result = decltype(job(std::declval<facebook::jsi::Runtime&>()));

#ifndef NDEBUG
  if (!NativeScriptFabricGatewayIsOnEntryThread()) {
    // Synchronous entry from another thread can deadlock with the main queue.
    NSLog(@"NativeScriptFabricGateway: runSyncOnMain called off the main "
          @"thread. Enter the UI runtime from the main thread.");
    assert(false && "NativeScriptFabricGatewayRunSyncOnMain called off-main");
  }
#endif

  auto runtime = NativeScriptFabricGatewayGetUIRuntime();
  if (runtime == nullptr) {
    if (ranOut != nullptr) {
      *ranOut = false;
    }
    return Result{};
  }

  if (ranOut != nullptr) {
    *ranOut = true;
  }
  return runtime->runSync(std::forward<Callable>(job));
}

// Stores the serialized definition and hook mask for each component name.
struct NativeScriptComponentSpecEntry {
  std::shared_ptr<worklets::Serializable> serializable;
  uint32_t hookMask = 0;
};

void NativeScriptFabricGatewayRegisterComponentSpec(const std::string& name,
                                                    std::shared_ptr<worklets::Serializable> serializable,
                                                    uint32_t hookMask);
uint32_t NativeScriptFabricGatewayHookMaskForComponent(const std::string& name);

// Hook bits let native code skip dispatch for hooks a component did not define.
// Creation is unconditional and therefore needs no bit.
enum NativeScriptComponentHook : uint32_t {
  NativeScriptComponentHookUpdateProps = 1 << 0,
  NativeScriptComponentHookMountChild = 1 << 1,
  NativeScriptComponentHookUnmountChild = 1 << 2,
  NativeScriptComponentHookWillMount = 1 << 3,
  NativeScriptComponentHookDidMount = 1 << 4,
  NativeScriptComponentHookUpdateLayoutMetrics = 1 << 5,
  NativeScriptComponentHookFinalizeUpdates = 1 << 6,
  NativeScriptComponentHookPrepareForRecycle = 1 << 7,
  NativeScriptComponentHookCommands = 1 << 8,
  NativeScriptComponentHookSafeAreaInsetsDidChange = 1 << 9,
};

// Materializes a component definition once per runtime generation, then calls
// the TypeScript hook dispatcher. The caller must already hold the UI runtime.
// Returns undefined until the component and dispatcher are registered.
facebook::jsi::Value NativeScriptFabricGatewayDispatchComponentHook(
    facebook::jsi::Runtime& rt, const std::string& name, double tag, const std::string& hookName,
    const facebook::jsi::Value& view, const facebook::jsi::Value& a, const facebook::jsi::Value& b,
    const facebook::jsi::Value& c);

}  // namespace nativescript
