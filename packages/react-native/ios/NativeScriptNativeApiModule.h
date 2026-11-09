#pragma once

#include <memory>
#include <string>

#include <NativeScriptNativeApiSpecJSI.h>
#include <ReactCommon/CallInvoker.h>
#include <jsi/jsi.h>

namespace facebook::react {

class NativeScriptNativeApiModule
    : public NativeScriptNativeApiCxxSpec<NativeScriptNativeApiModule> {
 public:
  explicit NativeScriptNativeApiModule(std::shared_ptr<CallInvoker> jsInvoker);

  bool install(jsi::Runtime& runtime, std::string metadataPath);
  // `schedulerHolder` is the UIScheduler holder handshake (ARCHITECTURE.md
  // §3.3/§7.1) alongside M0's WorkletRuntime holder; lets the gateway
  // route off-main async entries through the sanctioned
  // `worklets::scheduleOnUI` instead of a raw `dispatch_async(main)`.
  bool installUIRuntime(jsi::Runtime& runtime, jsi::Object runtimeHolder,
                        jsi::Object schedulerHolder, std::string metadataPath);
  bool isInstalled(jsi::Runtime& runtime);
  std::string defaultMetadataPath(jsi::Runtime& runtime);
  std::string getRuntimeBackend(jsi::Runtime& runtime);
  bool __writeTestMarker(jsi::Runtime& runtime, std::string content);
  // Test-only companion to __writeTestMarker, symmetric read-back of the
  // SAME smoke-marker file (used by callers that just want to see the
  // latest progress/result marker; NOT used for JOB2 phase-tracking --
  // see __writeReloadPhaseMarker below for why that needs its own file).
  std::string __readTestMarker(jsi::Runtime& runtime);
  // JOB2 dev-reload test: a SEPARATE marker file from the smoke marker
  // above. A DevSettings.reload() cycle re-runs the native install
  // sequence, which writes its own "stage=..." progress markers to the
  // smoke-marker file via writeSmokeMarkerIfRequested; reusing that same
  // file for "did my previous JS-side phase already run" round-tripping
  // caused an infinite reload loop (native's own install-stage write
  // clobbered the phase marker before the reloaded JS ever read it back;
  // confirmed on-sim). This pair is immune to that because nothing else
  // writes to NativeScriptM1ReloadPhase.marker.
  bool __writeReloadPhaseMarker(jsi::Runtime& runtime, std::string content);
  std::string __readReloadPhaseMarker(jsi::Runtime& runtime);

  // `defineNativeComponent`'s native registration step (ARCHITECTURE.md
  // §5.2 step 1-2): extracts a worklets Serializable from `spec` --
  // synchronously, on the JS thread, no UI-runtime entry needed; and
  // stores it in the gateway's spec store keyed by `name`, alongside
  // `hookMask` (bitwise-OR of NativeScriptComponentHook). Also performs the
  // Fabric flavored-class registration (NativeScriptRegisterFlavoredComponent).
  // Synchronous/blocking by design: by the time this call returns, the
  // component is fully registered, so there is no ordering race between
  // "definition shipped" and Fabric's first mount of it (§5.2).
  // `shouldBeRecycled`: tri-state number (-1 unspecified, 0/1 false/true --
  // see NativeScriptNativeApi.ts's Spec comment).
  bool registerComponent(jsi::Runtime& runtime, std::string name, jsi::Object spec, double hookMask,
                         double shouldBeRecycled);

 private:
  std::shared_ptr<CallInvoker> jsInvoker_;
};

}  // namespace facebook::react
