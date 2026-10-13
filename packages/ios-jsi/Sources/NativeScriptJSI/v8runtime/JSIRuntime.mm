#include "JSIRuntime.h"

#include <mutex>
#include <unordered_map>

#include "V8Runtime.h"
#include "v8.h"

namespace {

std::mutex g_jsiMutex;
std::unordered_map<v8::Isolate*, std::shared_ptr<facebook::jsi::Runtime>>
    g_jsiRuntimes;

}  // namespace

namespace tns {

std::shared_ptr<facebook::jsi::Runtime> GetJSIRuntime() {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (isolate == nullptr) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(g_jsiMutex);
  auto it = g_jsiRuntimes.find(isolate);
  if (it != g_jsiRuntimes.end()) {
    return it->second;
  }

  auto runtime = std::make_shared<rnv8::V8Runtime>();
  g_jsiRuntimes.emplace(isolate, runtime);
  return runtime;
}

void DisposeJSIRuntimeForCurrentIsolate() {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (isolate == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_jsiMutex);
  g_jsiRuntimes.erase(isolate);
}

}  // namespace tns

extern "C" facebook::jsi::Runtime* NativeScriptJSIRuntime(void) {
  auto rt = tns::GetJSIRuntime();
  return rt ? rt.get() : nullptr;
}

#ifdef __OBJC__
@implementation JSIRuntime

+ (void*)runtime {
  return (void*)NativeScriptJSIRuntime();
}

+ (NSNumber*)runtimePointer {
  facebook::jsi::Runtime* rt = NativeScriptJSIRuntime();
  return @((uint64_t)(uintptr_t)rt);
}

+ (void)disposeRuntimeForCurrentIsolate {
  tns::DisposeJSIRuntimeForCurrentIsolate();
}

@end
#endif
