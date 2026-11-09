#include "NativeScriptNativeApiModule.h"

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <memory>
#include <mutex>
#include <utility>
#include <dispatch/dispatch.h>

#include "NativeApiJsiReactNative.h"
#include "NativeScriptFabricGateway.h"
#include "Fabric/NativeScriptComponentRegistration.h"
#include "Fabric/NativeScriptComponentView.h"

#import <React/RCTBridge+Private.h>
#import <React/RCTConvert.h>
#import <React/RCTImageLoader.h>
#import <React/RCTImageSource.h>
#import <React/RCTResizeMode.h>
#import <ReactCommon/RCTTurboModule.h>
#include <worklets/Compat/Holders.h>
#include <worklets/WorkletRuntime/WorkletRuntime.h>

#import <pthread.h>

#include <sstream>

namespace {

std::string pathForResource(NSBundle* bundle, NSString* name, NSString* type) {
  if (bundle == nil) {
    return "";
  }
  NSString* path = [bundle pathForResource:name ofType:type];
  return path != nil ? path.UTF8String : "";
}

std::string bundledMetadataPath() {
#if TARGET_OS_SIMULATOR
#if defined(__x86_64__)
  NSString* metadataName = @"metadata.ios-sim.x86_64";
#else
  NSString* metadataName = @"metadata.ios-sim.arm64";
#endif
#else
  NSString* metadataName = @"metadata.ios.arm64";
#endif

  std::string path = pathForResource([NSBundle mainBundle], metadataName, @"nsmd");
  if (!path.empty()) {
    return path;
  }

  Class providerClass = NSClassFromString(@"NativeScriptNativeApiModuleProvider");
  NSBundle* providerBundle = providerClass != Nil ? [NSBundle bundleForClass:providerClass] : nil;
  NSString* resourceBundlePath = [providerBundle pathForResource:@"NativeScriptNativeApi"
                                                          ofType:@"bundle"];
  NSBundle* resourceBundle =
      resourceBundlePath != nil ? [NSBundle bundleWithPath:resourceBundlePath] : nil;

  path = pathForResource(resourceBundle, metadataName, @"nsmd");
  if (!path.empty()) {
    return path;
  }

  return pathForResource(resourceBundle, @"metadata", @"nsmd");
}

void writeSmokeMarkerIfRequested(const char* stage) {
  const char* enabled = getenv("NATIVESCRIPT_RN_TURBO_SMOKE_MARKER");
  if (enabled == nullptr || enabled[0] == '\0') {
    return;
  }

  NSString* path =
      [NSTemporaryDirectory() stringByAppendingPathComponent:@"NativeScriptNativeApiSmoke.marker"];
  NSString* content = [NSString stringWithFormat:@"stage=%s\n", stage != nullptr ? stage : ""];
  [content writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

bool writeSmokeMarkerContentIfRequested(const std::string& content) {
  const char* enabled = getenv("NATIVESCRIPT_RN_TURBO_SMOKE_MARKER");
  if (enabled == nullptr || enabled[0] == '\0') {
    return false;
  }

  NSString* path =
      [NSTemporaryDirectory() stringByAppendingPathComponent:@"NativeScriptNativeApiSmoke.marker"];
  NSString* nativeContent = [[NSString alloc] initWithBytes:content.data()
                                                     length:content.size()
                                                   encoding:NSUTF8StringEncoding];
  if (nativeContent == nil) {
    nativeContent = @"";
  }

  BOOL ok = [nativeContent writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
#if !__has_feature(objc_arc)
  [nativeContent release];
#endif
  return ok == YES;
}

// Dev-reload (JOB2) phase tracking; deliberately a SEPARATE file from the
// smoke marker above. First cut of this reused the smoke-marker file/path
// for both; that broke (infinite reload loop, observed on-sim) because
// writeSmokeMarkerIfRequested's OWN install-milestone writes
// ("stage=engine:installed" etc, fired by the native re-install sequence a
// DevSettings.reload() triggers) clobber it before the reloaded JS ever
// gets to read back what it wrote as "phase 1 done". A dedicated file next
// to it is immune to that.
NSString* reloadPhaseMarkerPath() {
  return [NSTemporaryDirectory() stringByAppendingPathComponent:@"NativeScriptM1ReloadPhase.marker"];
}

bool writeReloadPhaseMarkerIfRequested(const std::string& content) {
  const char* enabled = getenv("NATIVESCRIPT_RN_TURBO_SMOKE_MARKER");
  if (enabled == nullptr || enabled[0] == '\0') {
    return false;
  }
  NSString* nativeContent = [[NSString alloc] initWithBytes:content.data()
                                                     length:content.size()
                                                   encoding:NSUTF8StringEncoding];
  if (nativeContent == nil) {
    nativeContent = @"";
  }
  BOOL ok = [nativeContent writeToFile:reloadPhaseMarkerPath() atomically:YES encoding:NSUTF8StringEncoding error:nil];
#if !__has_feature(objc_arc)
  [nativeContent release];
#endif
  return ok == YES;
}

std::string readReloadPhaseMarkerIfRequested() {
  const char* enabled = getenv("NATIVESCRIPT_RN_TURBO_SMOKE_MARKER");
  if (enabled == nullptr || enabled[0] == '\0') {
    return "";
  }
  NSString* content = [NSString stringWithContentsOfFile:reloadPhaseMarkerPath()
                                                  encoding:NSUTF8StringEncoding
                                                     error:nil];
  if (content == nil) {
    return "";
  }
  return std::string(content.UTF8String != nullptr ? content.UTF8String : "");
}

// Symmetric to writeSmokeMarkerContentIfRequested above; test-only (same
// NATIVESCRIPT_RN_TURBO_SMOKE_MARKER gate), used by the M1 dev-reload test
// (JOB2) so JS can detect "did a previous phase already run" by reading
// back its own marker file across a DevSettings.reload() cycle, which tears
// down the JS VM (and any JS-side globals) but not the on-disk file or this
// TurboModule's process. Returns "" if disabled, unreadable, or absent --
// never throws, so a pre-first-write read is a normal, expected case.
std::string readSmokeMarkerContentIfRequested() {
  const char* enabled = getenv("NATIVESCRIPT_RN_TURBO_SMOKE_MARKER");
  if (enabled == nullptr || enabled[0] == '\0') {
    return "";
  }

  NSString* path =
      [NSTemporaryDirectory() stringByAppendingPathComponent:@"NativeScriptNativeApiSmoke.marker"];
  NSString* content = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
  if (content == nil) {
    return "";
  }
  return std::string(content.UTF8String != nullptr ? content.UTF8String : "");
}

bool nativeApiInstalled(facebook::jsi::Runtime& runtime) {
  return runtime.global().hasProperty(runtime, "__nativeScriptNativeApi");
}

NSString* nativeScriptHandleFromNSObject(id object) {
  if (object == nil) {
    return @"";
  }

  return [NSString stringWithFormat:@"%p", object];
}

RCTImageLoader* currentReactImageLoader() {
  RCTBridge* bridge = [RCTBridge currentBridge];
  if (bridge == nil) {
    return nil;
  }

  RCTImageLoader* imageLoader = nil;
  if ([bridge respondsToSelector:@selector(imageLoader)]) {
    imageLoader = bridge.imageLoader;
  }
  if (imageLoader == nil &&
      [bridge respondsToSelector:@selector(moduleForName:lazilyLoadIfNecessary:)]) {
    id module = [bridge moduleForName:@"RCTImageLoader" lazilyLoadIfNecessary:YES];
    if ([module isKindOfClass:RCTImageLoader.class]) {
      imageLoader = (RCTImageLoader*)module;
    }
  }
  return imageLoader;
}

UIImage* imageWithRenderingMode(UIImage* image, bool isTemplate) {
  if (image == nil) {
    return nil;
  }

  return [image imageWithRenderingMode:isTemplate ? UIImageRenderingModeAlwaysTemplate
                                                  : UIImageRenderingModeAlwaysOriginal];
}

id imageSourceFromJSIValue(facebook::jsi::Runtime& runtime,
                           const facebook::jsi::Value& value,
                           const std::shared_ptr<facebook::react::CallInvoker>& jsInvoker) {
  if (value.isString()) {
    std::string uri = value.asString(runtime).utf8(runtime);
    return @{@"uri" : [NSString stringWithUTF8String:uri.c_str()]};
  }

  id object =
      facebook::react::TurboModuleConvertUtils::convertJSIValueToObjCObject(
          runtime, value, jsInvoker, YES);
  return object == nil || object == [NSNull null] ? nil : object;
}

void callImageLoadCallback(
    std::weak_ptr<worklets::WorkletRuntime> workletRuntimeWeak,
    std::shared_ptr<facebook::jsi::Function> callback,
    UIImage* image,
    NSString* errorMessage) {
  UIImage* retainedImage = image != nil ? [image retain] : nil;
  std::string imageHandle =
      retainedImage != nil ? nativeScriptHandleFromNSObject(retainedImage).UTF8String : "";
  std::string errorText = errorMessage.UTF8String != nullptr ? errorMessage.UTF8String : "";

  auto runtimeStrong = workletRuntimeWeak.lock();
  if (runtimeStrong == nullptr) {
    [retainedImage release];
    return;
  }

  runtimeStrong->schedule(
      [callback = std::move(callback), imageHandle = std::move(imageHandle),
       errorText = std::move(errorText), retainedImage](facebook::jsi::Runtime& runtime) mutable {
        facebook::jsi::Value imageValue =
            imageHandle.empty()
                ? facebook::jsi::Value::null()
                : facebook::jsi::Value(
                      runtime,
                      facebook::jsi::String::createFromUtf8(runtime, imageHandle));
        facebook::jsi::Value errorValue =
            errorText.empty()
                ? facebook::jsi::Value::null()
                : facebook::jsi::Value(
                      runtime,
                      facebook::jsi::String::createFromUtf8(runtime, errorText));
        callback->call(runtime, imageValue, errorValue);
        dispatch_async(dispatch_get_main_queue(), ^{
          [retainedImage release];
        });
      });
}

}  // namespace

namespace facebook::react {

NativeScriptNativeApiModule::NativeScriptNativeApiModule(std::shared_ptr<CallInvoker> jsInvoker)
    : NativeScriptNativeApiCxxSpec(jsInvoker), jsInvoker_(std::move(jsInvoker)) {}

bool NativeScriptNativeApiModule::install(jsi::Runtime& runtime, std::string metadataPath) {
  writeSmokeMarkerIfRequested("install:resolve-metadata");
  std::string resolvedMetadataPath = metadataPath.empty() ? bundledMetadataPath() : metadataPath;
  const char* metadataPathArg =
      resolvedMetadataPath.empty() ? nullptr : resolvedMetadataPath.c_str();

  writeSmokeMarkerIfRequested("install:before-jsi");
  auto config =
      nativescript::MakeReactNativeNativeApiJsiConfig(
          jsInvoker_, nullptr, metadataPathArg, nullptr, "__nativeScriptNativeApi");
  config.installGlobalSymbols = false;
  config.invokeCallbacksOnNativeCallerThread = false;
  nativescript::InstallNativeApiJSI(runtime, config);
  writeSmokeMarkerIfRequested("install:after-jsi");
  return isInstalled(runtime);
}

bool NativeScriptNativeApiModule::installUIRuntime(jsi::Runtime& runtime,
                                                   jsi::Object runtimeHolder,
                                                   jsi::Object schedulerHolder,
                                                   std::string metadataPath) {
  writeSmokeMarkerIfRequested("installUIRuntime:headers");
  if (!runtimeHolder.hasNativeState<worklets::WorkletRuntimeHolder>(runtime)) {
    writeSmokeMarkerIfRequested("installUIRuntime:no-holder");
    return false;
  }

  auto holder = runtimeHolder.getNativeState<worklets::WorkletRuntimeHolder>(runtime);
  if (holder == nullptr || holder->runtime_ == nullptr) {
    writeSmokeMarkerIfRequested("installUIRuntime:null-runtime");
    return false;
  }

  // The gateway is the single source of truth for the installed UI runtime
  // (M1: the old dual-write; a second, separately-maintained weak_ptr here
  //; is gone).
  nativescript::NativeScriptFabricGatewaySetUIRuntime(holder->runtime_);

  // UIScheduler holder handshake (ARCHITECTURE.md §3.3/§7.1), same unwrap
  // pattern as the WorkletRuntime holder just above (StableApi.h's
  // getUISchedulerFromHolder, which; unlike the WorkletRuntimeHolder path
  // above; throws rather than returning null if the object carries no
  // native state, so the hasNativeState check here is load-bearing, not
  // defensive noise). Missing/invalid is non-fatal: the gateway's
  // ScheduleOnUI falls back to a plain dispatch_async(main) when no
  // scheduler is installed, so this never blocks bootstrap.
  auto uiScheduler = schedulerHolder.hasNativeState<worklets::UISchedulerHolder>(runtime)
                          ? worklets::getUISchedulerFromHolder(runtime, schedulerHolder)
                          : nullptr;
  if (uiScheduler != nullptr) {
    nativescript::NativeScriptFabricGatewaySetUIScheduler(std::move(uiScheduler));
  }

  std::string resolvedMetadataPath = metadataPath.empty() ? bundledMetadataPath() : metadataPath;
  auto jsInvoker = jsInvoker_;
  auto workletRuntimeRef = holder->runtime_;

  // This call itself is the ONE sanctioned exception to "only enter the UI
  // runtime from main" (ARCHITECTURE.md §3.3/§9.2): it runs once, at
  // bootstrap, before any TS hook exists to race with. But everything it
  // installs (host functions, the ObjC bridge's own notion of its "home"
  // thread) must behave as if it always runs on main from here on; so we
  // hop to main, rather than calling runSync directly from the RN JS thread
  // as the refactor baseline did. Otherwise NativeApiBridge captures the JS
  // thread as its "home" thread and later, genuinely-main-thread nested
  // re-entry (spike 1) takes the wrong (off-home-thread) callback-dispatch
  // path.
  //
  // M1 review §3/#3 (a real contract breach): this used to be
  // `dispatch_sync(main)`, called FROM the JS thread; exactly the
  // blocking cross-thread wait §3.4 says must never exist, and a live
  // AB-BA edge if main is ever itself blocked waiting on the JS thread
  // during some other RN synchronous-surface startup path. Fixed per the
  // review's own suggested option: `dispatch_async` instead, with the
  // gateway's existing "not yet installed" graceful no-op (every Fabric
  // hook dispatch already tolerates a runtime with no dispatcher installed
  // yet; NativeScriptFabricGatewayDispatchComponentHook returns
  // Value::undefined() rather than crashing) covering the now-nonzero
  // window between this call returning and the async block actually
  // running. That window cannot be observed by a REAL Fabric hook in
  // practice: Fabric cannot call anything before React's first commit,
  // which cannot happen before this synchronous JS-thread call already
  // returned. `installed` can therefore no longer report the async work's
  // actual outcome; it now means "accepted for install", matching how
  // `runOnUIAsync`-style bootstrap calls already work elsewhere in this file.
  bool installed = true;
  dispatch_async(dispatch_get_main_queue(), ^{
    workletRuntimeRef->runSync(
      [jsInvoker = std::move(jsInvoker), resolvedMetadataPath = std::move(resolvedMetadataPath),
       workletRuntimeRef](
          jsi::Runtime& workletRuntime) -> bool {
        if (!nativeApiInstalled(workletRuntime)) {
          const char* metadataPathArg =
              resolvedMetadataPath.empty() ? nullptr : resolvedMetadataPath.c_str();
          auto config =
              nativescript::MakeReactNativeNativeApiJsiConfig(
                  jsInvoker, nullptr, metadataPathArg, nullptr, "__nativeScriptNativeApi");
          config.installGlobalSymbols = true;
          config.invokeCallbacksOnNativeCallerThread = true;
          // ARCHITECTURE.md §3.3/§3.4: no blocking cross-thread waits. A
          // callback arriving off the UI runtime's home thread is routed
          // through the gateway's ScheduleOnUI; the sanctioned
          // `worklets::scheduleOnUI` (M1; M0 used a raw dispatch_async(main)
          // here and flagged it as the one deviation from the design's
          // letter). The DISPATCH_TIME_FOREVER semaphore the refactor
          // baseline used here remains deleted, not just widened; both
          // paths are fire-and-forget async, never a blocking wait.
          config.runtimeCallbackInvoker = [](std::function<void()> task) {
            nativescript::NativeScriptFabricGatewayScheduleOnUI(std::move(task));
          };
          nativescript::InstallNativeApiJSI(workletRuntime, config);
        }

	        // ctx.emit / ctx.setContentSize / ctx.scheduleOnMainQueue targets
	        // (src/ui/dispatcher.ts); idempotent, safe to call on every
	        // install (including reload re-installs onto a fresh UI VM).
	        NativeScriptInstallComponentHostFunctions(workletRuntime);

	        std::weak_ptr<worklets::WorkletRuntime> imageWorkletRuntimeWeak(workletRuntimeRef);
	        auto loadImage = jsi::Function::createFromHostFunction(
	            workletRuntime,
	            jsi::PropNameID::forAscii(workletRuntime, "__nativeScriptLoadReactImage"),
	            3,
	            [jsInvoker, imageWorkletRuntimeWeak](jsi::Runtime& runtime, const jsi::Value&,
	                                                 const jsi::Value* args,
	                                                 size_t count) -> jsi::Value {
	              if (count < 3 || !args[2].isObject() ||
	                  !args[2].asObject(runtime).isFunction(runtime)) {
	                return false;
	              }

	              id jsonSource = imageSourceFromJSIValue(runtime, args[0], jsInvoker);
	              if (jsonSource == nil) {
	                return false;
	              }

	              RCTImageSource* imageSource = [RCTConvert RCTImageSource:jsonSource];
	              RCTImageLoader* imageLoader = currentReactImageLoader();
	              if (imageSource == nil || imageLoader == nil) {
	                return false;
	              }

	              bool isTemplate = count > 1 && args[1].isBool() && args[1].getBool();
	              auto callback = std::make_shared<jsi::Function>(
	                  args[2].asObject(runtime).asFunction(runtime));

	              [imageLoader loadImageWithURLRequest:imageSource.request
	                                              size:imageSource.size
	                                             scale:imageSource.scale
	                                           clipped:YES
	                                        resizeMode:RCTResizeModeCenter
	                                     progressBlock:^(int64_t, int64_t) {
	                                     }
	                                  partialLoadBlock:^(UIImage*) {
	                                  }
	                                   completionBlock:^(NSError* error, UIImage* image) {
	                                     dispatch_async(dispatch_get_main_queue(), ^{
	                                       UIImage* renderedImage =
	                                           imageWithRenderingMode(image, isTemplate);
	                                       callImageLoadCallback(
	                                           imageWorkletRuntimeWeak, callback, renderedImage,
	                                           error.localizedDescription);
	                                     });
	                                   }];
	              return true;
	            });
	        workletRuntime.global().setProperty(
	            workletRuntime, "__nativeScriptLoadReactImage", std::move(loadImage));
	        return nativeApiInstalled(workletRuntime);
	      });
  });
  return installed;
}

bool NativeScriptNativeApiModule::isInstalled(jsi::Runtime& runtime) {
  return nativeApiInstalled(runtime);
}

std::string NativeScriptNativeApiModule::defaultMetadataPath(jsi::Runtime&) {
  return bundledMetadataPath();
}

std::string NativeScriptNativeApiModule::getRuntimeBackend(jsi::Runtime&) {
  writeSmokeMarkerIfRequested("getRuntimeBackend");
  return "hermes-jsi";
}

bool NativeScriptNativeApiModule::__writeTestMarker(jsi::Runtime&, std::string content) {
  return writeSmokeMarkerContentIfRequested(content);
}

std::string NativeScriptNativeApiModule::__readTestMarker(jsi::Runtime&) {
  return readSmokeMarkerContentIfRequested();
}

bool NativeScriptNativeApiModule::__writeReloadPhaseMarker(jsi::Runtime&, std::string content) {
  return writeReloadPhaseMarkerIfRequested(content);
}

std::string NativeScriptNativeApiModule::__readReloadPhaseMarker(jsi::Runtime&) {
  return readReloadPhaseMarkerIfRequested();
}

// ---------------------------------------------------------------------------
// registerComponent (ARCHITECTURE.md §5.2 steps 1-2). M0's three spike*
// entry points (registerFlavoredComponent/spikeRunSyncFromMain/
// spikeFlavorMountSnapshot) are gone: real Fabric hooks now exercise the
// same gateway path they existed only to prove in isolation.
// ---------------------------------------------------------------------------

bool NativeScriptNativeApiModule::registerComponent(jsi::Runtime& runtime, std::string name, jsi::Object spec,
                                                     double hookMask, double shouldBeRecycled) {
  if (name.empty()) {
    return false;
  }

  // Extraction happens HERE, on the JS thread, synchronously; it never
  // enters the UI runtime (extractSerializable just walks the JS value
  // graph). By the time this call returns, `name`'s spec is fully stored;
  // Fabric's first mount of a component with this name can only happen
  // after React renders it, which can only happen after this call already
  // returned; so there is no ordering race between "definition shipped"
  // and "first mount" (§5.2 step 1).
  std::shared_ptr<worklets::Serializable> serializable;
  try {
    serializable = worklets::extractSerializable(
        runtime, jsi::Value(runtime, spec),
        "[NativeScript] defineNativeComponent's spec must be serializable by react-native-worklets "
        "(plain data plus 'worklet' functions).");
  } catch (const std::exception& error) {
    NSLog(@"NativeScript: failed to register component \"%s\": %s", name.c_str(), error.what());
    return false;
  }
  if (serializable == nullptr) {
    return false;
  }

  uint32_t hookMaskValue = hookMask > 0 ? static_cast<uint32_t>(hookMask) : 0;
  nativescript::NativeScriptFabricGatewayRegisterComponentSpec(name, std::move(serializable), hookMaskValue);

  NSString* nsName = [NSString stringWithUTF8String:name.c_str()];
  if (nsName.length == 0) {
    return false;
  }
  BOOL hasShouldBeRecycled = shouldBeRecycled >= 0;
  BOOL shouldBeRecycledValue = shouldBeRecycled > 0;
  NativeScriptRegisterFlavoredComponent(nsName, hookMaskValue, hasShouldBeRecycled, shouldBeRecycledValue);
  return true;
}

}  // namespace facebook::react
