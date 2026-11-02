#ifndef JSIRUNTIME_H_
#define JSIRUNTIME_H_

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

__attribute__((visibility("default"))) std::shared_ptr<facebook::jsi::Runtime>
GetJSIRuntime();

__attribute__((visibility("default"))) void
DisposeJSIRuntimeForCurrentIsolate();

}  // namespace tns
#endif

#endif /* JSIRUNTIME_H_ */
