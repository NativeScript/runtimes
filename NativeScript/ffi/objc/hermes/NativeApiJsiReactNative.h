#ifndef NATIVE_API_JSI_REACT_NATIVE_H
#define NATIVE_API_JSI_REACT_NATIVE_H

#include "NativeApiJsi.h"

#if __has_include(<ReactCommon/CallInvoker.h>)
#include <ReactCommon/CallInvoker.h>
#define NATIVESCRIPT_HAS_REACT_NATIVE_CALL_INVOKER 1
#elif __has_include(<CallInvoker.h>)
#include <CallInvoker.h>
#define NATIVESCRIPT_HAS_REACT_NATIVE_CALL_INVOKER 1
#else
#define NATIVESCRIPT_HAS_REACT_NATIVE_CALL_INVOKER 0
#endif

#if NATIVESCRIPT_HAS_REACT_NATIVE_CALL_INVOKER

#include <dispatch/dispatch.h>
#include <stdexcept>

namespace nativescript {

class ReactNativeCallInvokerScheduler final : public NativeApiJsiScheduler {
 public:
  ReactNativeCallInvokerScheduler(
      std::shared_ptr<facebook::react::CallInvoker> jsInvoker,
      std::shared_ptr<facebook::react::CallInvoker> uiInvoker)
      : jsInvoker_(std::move(jsInvoker)), uiInvoker_(std::move(uiInvoker)) {
    if (!jsInvoker_) {
      throw std::invalid_argument(
          "NativeScript React Native JSI scheduler requires a JS CallInvoker");
    }
  }

  void invokeOnJS(std::function<void()> task) override {
    jsInvoker_->invokeAsync(std::move(task));
  }

  void invokeOnUI(std::function<void()> task) override {
    if (uiInvoker_) {
      uiInvoker_->invokeAsync(std::move(task));
      return;
    }
    auto heapTask = std::make_shared<std::function<void()>>(std::move(task));
    dispatch_async(dispatch_get_main_queue(), ^{
      (*heapTask)();
    });
  }

 private:
  std::shared_ptr<facebook::react::CallInvoker> jsInvoker_;
  std::shared_ptr<facebook::react::CallInvoker> uiInvoker_;
};

inline NativeApiJsiConfig MakeReactNativeNativeApiJsiConfig(
    std::shared_ptr<facebook::react::CallInvoker> jsInvoker,
    std::shared_ptr<facebook::react::CallInvoker> uiInvoker,
    const char* metadataPath = nullptr,
    const void* metadataPtr = nullptr,
    const char* globalName = "__nativeScriptNativeApi") {
  NativeApiJsiConfig config;
  config.metadataPath = metadataPath;
  config.metadataPtr = metadataPtr;
  config.globalName = globalName;
  // RN launch cost: don't eagerly install the aggregate global surface or
  // realize every class/protocol runtime pointer at symbol-index time.
  config.installGlobalSymbols = false;
  config.indexRuntimePointers = false;
  config.invokeCallbacksOnNativeCallerThread = true;
  config.scheduler = std::make_shared<ReactNativeCallInvokerScheduler>(
      std::move(jsInvoker), std::move(uiInvoker));
  return config;
}

inline void InstallReactNativeNativeApiJSI(
    facebook::jsi::Runtime& runtime,
    std::shared_ptr<facebook::react::CallInvoker> jsInvoker,
    std::shared_ptr<facebook::react::CallInvoker> uiInvoker,
    const char* metadataPath = nullptr,
    const void* metadataPtr = nullptr,
    const char* globalName = "__nativeScriptNativeApi") {
  InstallNativeApiJSI(
      runtime,
      MakeReactNativeNativeApiJsiConfig(std::move(jsInvoker), std::move(uiInvoker),
                                        metadataPath, metadataPtr, globalName));
}

}  // namespace nativescript

#endif  // NATIVESCRIPT_HAS_REACT_NATIVE_CALL_INVOKER

#endif  // NATIVE_API_JSI_REACT_NATIVE_H
