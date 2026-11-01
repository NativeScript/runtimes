#include "NativeScriptNativeApiModule.h"

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif

#include <memory>
#include <mutex>
#include <utility>
#include <dispatch/dispatch.h>

#include "NativeApiJsiReactNative.h"
#include "NativeScriptUIKitHost.h"

#import <React/RCTBridge+Private.h>
#import <React/RCTConvert.h>
#import <React/RCTImageLoader.h>
#import <React/RCTImageSource.h>
#import <React/RCTResizeMode.h>
#import <ReactCommon/RCTTurboModule.h>
#include <worklets/Compat/Holders.h>
#include <worklets/WorkletRuntime/WorkletRuntime.h>

namespace {

std::string pathForResource(NSBundle* bundle, NSString* name, NSString* type) {
  if (bundle == nil) {
    return "";
  }
  NSString* path = [bundle pathForResource:name ofType:type];
  return path != nil ? path.UTF8String : "";
}

std::string bundledMetadataPath() {
#if TARGET_OS_OSX
#if defined(__x86_64__)
  NSString* metadataName = @"metadata.macos.x86_64";
#else
  NSString* metadataName = @"metadata.macos.arm64";
#endif
#elif TARGET_OS_SIMULATOR
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

#if TARGET_OS_OSX
using NativeScriptPlatformImage = NSImage;
#else
using NativeScriptPlatformImage = UIImage;
#endif

NativeScriptPlatformImage* imageWithRenderingMode(NativeScriptPlatformImage* image,
                                                  bool isTemplate) {
  if (image == nil) {
    return nil;
  }

#if TARGET_OS_OSX
  NSImage* copy = [[image copy] autorelease];
  [copy setTemplate:isTemplate];
  return copy;
#else
  return [image imageWithRenderingMode:isTemplate ? UIImageRenderingModeAlwaysTemplate
                                                  : UIImageRenderingModeAlwaysOriginal];
#endif
}

std::mutex& nativeScriptWorkletRuntimeMutex() {
  static std::mutex mutex;
  return mutex;
}

std::weak_ptr<worklets::WorkletRuntime>& nativeScriptWorkletRuntime() {
  static std::weak_ptr<worklets::WorkletRuntime> runtime;
  return runtime;
}

void setNativeScriptWorkletRuntime(std::shared_ptr<worklets::WorkletRuntime> runtime) {
  std::lock_guard<std::mutex> lock(nativeScriptWorkletRuntimeMutex());
  nativeScriptWorkletRuntime() = std::move(runtime);
}

std::shared_ptr<worklets::WorkletRuntime> getNativeScriptWorkletRuntime() {
  std::lock_guard<std::mutex> lock(nativeScriptWorkletRuntimeMutex());
  return nativeScriptWorkletRuntime().lock();
}

NSString* stringProperty(facebook::jsi::Runtime& runtime, facebook::jsi::Object& object,
                         const char* name) {
  auto value = object.getProperty(runtime, name);
  if (!value.isString()) {
    return nil;
  }
  std::string text = value.getString(runtime).utf8(runtime);
  return [NSString stringWithUTF8String:text.c_str()];
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
    NativeScriptPlatformImage* image,
    NSString* errorMessage) {
  NativeScriptPlatformImage* retainedImage = image != nil ? [image retain] : nil;
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

NSDictionary<NSString*, NSString*>* handlesFromJSIValue(facebook::jsi::Runtime& runtime,
                                                        facebook::jsi::Value&& result) {
  if (!result.isObject()) {
    return nil;
  }

  auto resultObject = result.asObject(runtime);
  NSMutableDictionary<NSString*, NSString*>* handles =
      [NSMutableDictionary dictionaryWithCapacity:3];
  NSString* nativeViewHandle = stringProperty(runtime, resultObject, "nativeViewHandle");
  NSString* childrenViewHandle = stringProperty(runtime, resultObject, "childrenViewHandle");
  NSString* controllerHandle = stringProperty(runtime, resultObject, "controllerHandle");

  if (nativeViewHandle.length > 0) {
    handles[@"nativeViewHandle"] = nativeViewHandle;
  }
  if (childrenViewHandle.length > 0) {
    handles[@"childrenViewHandle"] = childrenViewHandle;
  }
  if (controllerHandle.length > 0) {
    handles[@"controllerHandle"] = controllerHandle;
  }
  return handles;
}

NSDictionary<NSString*, NSString*>* runUIKitHostFunction(NSString* hostId, NSString* phase,
                                                         const char* globalName,
                                                         const char* logAction) {
  if (hostId.length == 0 || ![NSThread isMainThread]) {
    return nil;
  }

  auto workletRuntime = getNativeScriptWorkletRuntime();
  if (workletRuntime == nullptr) {
    return nil;
  }

  std::string hostIdString = hostId.UTF8String != nullptr ? hostId.UTF8String : "";
  if (hostIdString.empty()) {
    return nil;
  }

  std::string phaseString = phase.UTF8String != nullptr ? phase.UTF8String : "";

  try {
    return workletRuntime->runSync(
        [hostIdString = std::move(hostIdString), phaseString = std::move(phaseString),
         globalName](facebook::jsi::Runtime& runtime) -> NSDictionary<NSString*, NSString*>* {
          auto global = runtime.global();
          auto functionValue = global.getProperty(runtime, globalName);
          if (!functionValue.isObject()) {
            return nil;
          }

          auto functionObject = functionValue.asObject(runtime);
          if (!functionObject.isFunction(runtime)) {
            return nil;
          }

          auto function = functionObject.asFunction(runtime);
          auto hostIdValue = facebook::jsi::String::createFromUtf8(runtime, hostIdString);
          if (phaseString.empty()) {
            return handlesFromJSIValue(runtime, function.call(runtime, hostIdValue));
          }

          return handlesFromJSIValue(
              runtime, function.call(runtime, hostIdValue,
                                     facebook::jsi::String::createFromUtf8(runtime, phaseString)));
        });
  } catch (const std::exception& error) {
    NSLog(@"NativeScript failed to %s UIKit host %@: %s", logAction, hostId, error.what());
  } catch (...) {
    NSLog(@"NativeScript failed to %s UIKit host %@", logAction, hostId);
  }
  return nil;
}

}  // namespace

NSDictionary<NSString*, NSString*>* NativeScriptCreateUIKitHost(NSString* hostId) {
  return runUIKitHostFunction(hostId, nil, "__nativeScriptCreateUIKitHostFromNative", "create");
}

NSDictionary<NSString*, NSString*>* NativeScriptRunUIKitHostLifecycle(NSString* hostId,
                                                                      NSString* phase) {
  return runUIKitHostFunction(hostId, phase, "__nativeScriptRunUIKitHostLifecycleFromNative",
                              "run");
}

#if TARGET_OS_OSX
// NativeScriptUIView is UIKit-only, so macOS has no host views to refresh.
BOOL NativeScriptRefreshUIKitHostView(NSString*) {
  return NO;
}
#endif

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

bool NativeScriptNativeApiModule::installWorkletRuntime(jsi::Runtime& runtime,
                                                        jsi::Object runtimeHolder,
                                                        std::string metadataPath) {
  writeSmokeMarkerIfRequested("installWorkletRuntime:headers");
  if (!runtimeHolder.hasNativeState<worklets::WorkletRuntimeHolder>(runtime)) {
    writeSmokeMarkerIfRequested("installWorkletRuntime:no-holder");
    return false;
  }

  auto holder = runtimeHolder.getNativeState<worklets::WorkletRuntimeHolder>(runtime);
  if (holder == nullptr || holder->runtime_ == nullptr) {
    writeSmokeMarkerIfRequested("installWorkletRuntime:null-runtime");
    return false;
  }

  setNativeScriptWorkletRuntime(holder->runtime_);

  std::string resolvedMetadataPath = metadataPath.empty() ? bundledMetadataPath() : metadataPath;
  auto jsInvoker = jsInvoker_;
  auto workletRuntimeRef = holder->runtime_;
  return holder->runtime_->runSync(
      [jsInvoker = std::move(jsInvoker), resolvedMetadataPath = std::move(resolvedMetadataPath),
       workletRuntimeRef = std::move(workletRuntimeRef)](
          jsi::Runtime& workletRuntime) -> bool {
        if (!nativeApiInstalled(workletRuntime)) {
          std::weak_ptr<worklets::WorkletRuntime> workletRuntimeWeak(workletRuntimeRef);
          const char* metadataPathArg =
              resolvedMetadataPath.empty() ? nullptr : resolvedMetadataPath.c_str();
          auto config =
              nativescript::MakeReactNativeNativeApiJsiConfig(
                  jsInvoker, nullptr, metadataPathArg, nullptr, "__nativeScriptNativeApi");
          config.installGlobalSymbols = true;
          config.invokeCallbacksOnNativeCallerThread = true;
          config.runtimeCallbackInvoker =
              [workletRuntimeWeak](std::function<void()> task) mutable {
                auto runtimeStrong = workletRuntimeWeak.lock();
                if (runtimeStrong == nullptr) {
                  return;
                }

                auto taskBox =
                    std::make_shared<std::function<void()>>(std::move(task));
                dispatch_semaphore_t done = dispatch_semaphore_create(0);
                runtimeStrong->schedule(
                    [taskBox = std::move(taskBox), done](jsi::Runtime&) mutable {
                      (*taskBox)();
                      dispatch_semaphore_signal(done);
                    });
                dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
              };
          nativescript::InstallNativeApiJSI(workletRuntime, config);
        }

	        auto refreshUIKitHostView = jsi::Function::createFromHostFunction(
	            workletRuntime,
	            jsi::PropNameID::forAscii(workletRuntime, "__nativeScriptRefreshUIKitHostView"),
            1,
            [](jsi::Runtime& runtime, const jsi::Value&, const jsi::Value* args,
               size_t count) -> jsi::Value {
              if (count < 1 || !args[0].isString()) {
                return false;
              }

              std::string handle = args[0].asString(runtime).utf8(runtime);
              NSString* nativeHandle = [NSString stringWithUTF8String:handle.c_str()];
              return NativeScriptRefreshUIKitHostView(nativeHandle) == YES;
            });
	        workletRuntime.global().setProperty(
	            workletRuntime, "__nativeScriptRefreshUIKitHostView", std::move(refreshUIKitHostView));

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
	                                  partialLoadBlock:^(NativeScriptPlatformImage*) {
	                                  }
	                                   completionBlock:^(NSError* error, NativeScriptPlatformImage* image) {
	                                     dispatch_async(dispatch_get_main_queue(), ^{
	                                       NativeScriptPlatformImage* renderedImage =
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

}  // namespace facebook::react
