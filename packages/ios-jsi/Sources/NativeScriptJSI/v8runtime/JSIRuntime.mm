#include "JSIRuntime.h"

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>

#include <mutex>
#include <unordered_map>

#include "V8Runtime.h"
#include "v8.h"

namespace {

class RunLoopCallInvoker final : public tns::JSICallInvoker {
 public:
  explicit RunLoopCallInvoker(std::weak_ptr<rnv8::V8Runtime> runtime)
      : runtime_(std::move(runtime)),
        runLoop_((CFRunLoopRef)CFRetain(CFRunLoopGetCurrent())),
        thread_(pthread_self()) {}

  ~RunLoopCallInvoker() override { CFRelease(runLoop_); }

  void invokeAsync(
      std::function<void(facebook::jsi::Runtime&)>&& task) override {
    std::weak_ptr<rnv8::V8Runtime> weakRuntime = runtime_;
    std::function<void(facebook::jsi::Runtime&)> blockTask = std::move(task);
    CFRunLoopPerformBlock(runLoop_, kCFRunLoopCommonModes, ^{
      if (auto runtime = weakRuntime.lock()) {
        runtime->ExecuteTask(blockTask);
      }
    });
    CFRunLoopWakeUp(runLoop_);
  }

  bool isOnJSThread() const override {
    return pthread_equal(pthread_self(), thread_) != 0;
  }

 private:
  std::weak_ptr<rnv8::V8Runtime> runtime_;
  CFRunLoopRef runLoop_;
  pthread_t thread_;
};

struct IsolateEntry {
  std::shared_ptr<rnv8::V8Runtime> runtime;
  std::shared_ptr<tns::JSICallInvoker> callInvoker;
};

std::mutex g_jsiMutex;

// Leaked so no runtime is torn down by static destructors during exit, when
// its isolate may already be gone.
std::unordered_map<v8::Isolate*, IsolateEntry>& isolateEntries() {
  static auto* entries = new std::unordered_map<v8::Isolate*, IsolateEntry>();
  return *entries;
}

// Requires g_jsiMutex.
IsolateEntry* entryForCurrentIsolate() {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (isolate == nullptr) {
    return nullptr;
  }
  IsolateEntry& entry = isolateEntries()[isolate];
  if (!entry.runtime) {
    entry.runtime = std::make_shared<rnv8::V8Runtime>();
  }
  return &entry;
}

}  // namespace

namespace tns {

std::shared_ptr<facebook::jsi::Runtime> GetJSIRuntime() {
  std::lock_guard<std::mutex> lock(g_jsiMutex);
  IsolateEntry* entry = entryForCurrentIsolate();
  return entry ? entry->runtime : nullptr;
}

std::shared_ptr<JSICallInvoker> GetJSICallInvoker() {
  std::lock_guard<std::mutex> lock(g_jsiMutex);
  IsolateEntry* entry = entryForCurrentIsolate();
  if (entry == nullptr) {
    return nullptr;
  }
  if (!entry->callInvoker) {
    entry->callInvoker = std::make_shared<RunLoopCallInvoker>(entry->runtime);
  }
  return entry->callInvoker;
}

void DisposeJSIRuntimeForCurrentIsolate() {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (isolate == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_jsiMutex);
  isolateEntries().erase(isolate);
}

}  // namespace tns

extern "C" facebook::jsi::Runtime* NativeScriptJSIRuntime(void) {
  auto rt = tns::GetJSIRuntime();
  return rt ? rt.get() : nullptr;
}

#ifdef __OBJC__
@implementation NSJSIRuntime

+ (std::shared_ptr<facebook::jsi::Runtime>)runtime {
  return tns::GetJSIRuntime();
}

+ (void*)rawRuntime {
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
