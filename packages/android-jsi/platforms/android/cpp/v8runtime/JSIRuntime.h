#ifndef JSIRUNTIME_H_
#define JSIRUNTIME_H_

#include <functional>
#include <memory>

#include "jsi/jsi.h"

#ifdef __cplusplus
extern "C" {
#endif

// The JSI Runtime of the NativeScript V8 isolate on the calling thread, or
// nullptr when this thread has no active V8 isolate. Each isolate (main thread
// and each Worker) owns a separate JSI Runtime bound to that V8 isolate.
__attribute__((visibility("default"))) facebook::jsi::Runtime*
NativeScriptJSIRuntime(void);

#ifdef __cplusplus
}

namespace tns {

// Schedules work on the thread that owns a JSI Runtime, like React Native's
// CallInvoker. Each task is a top-level entry into JS: it runs in the
// isolate's context and is followed by a microtask checkpoint, so promises it
// settles resolve right away. Exceptions escaping a task are reported as
// uncaught errors.
class __attribute__((visibility("default"))) JSICallInvoker {
 public:
  virtual ~JSICallInvoker() = default;

  // Safe to call from any thread. Tasks run in FIFO order; tasks still queued
  // once the runtime is disposed are dropped.
  virtual void invokeAsync(
      std::function<void(facebook::jsi::Runtime&)>&& task) = 0;

  // Whether the calling thread is the runtime's JS thread.
  virtual bool isOnJSThread() const = 0;
};

__attribute__((visibility("default"))) std::shared_ptr<facebook::jsi::Runtime>
GetJSIRuntime();

// The call invoker for the calling thread's JSI Runtime, or nullptr when this
// thread has no active V8 isolate or no ALooper. Call it on the JS thread; the
// invoker can then be used from any thread.
__attribute__((visibility("default"))) std::shared_ptr<JSICallInvoker>
GetJSICallInvoker();

__attribute__((visibility("default"))) void
DisposeJSIRuntimeForCurrentIsolate();

}  // namespace tns
#endif

#endif /* JSIRUNTIME_H_ */
