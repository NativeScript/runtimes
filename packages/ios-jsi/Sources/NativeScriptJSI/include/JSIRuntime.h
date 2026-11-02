#ifndef JSIRUNTIME_H_
#define JSIRUNTIME_H_

#ifdef __cplusplus
#include <memory>

#include "jsi/jsi.h"
#endif

#ifdef __OBJC__
#import <Foundation/Foundation.h>

@interface NSJSIRuntime : NSObject
#ifdef __cplusplus
/**
 * Returns the shared `facebook::jsi::Runtime` instance for the active V8
 * isolate on the calling thread, or nullptr if no V8 isolate is active.
 */
+ (std::shared_ptr<facebook::jsi::Runtime>)runtime;
#endif

/**
 * Returns the raw `facebook::jsi::Runtime*` pointer for the active V8 isolate
 * on the calling thread, or NULL if no V8 isolate is active on this thread.
 */
+ (void* _Nullable)rawRuntime;

/**
 * Returns the pointer address of the active `facebook::jsi::Runtime*` as an
 * unsigned 64-bit integer NSNumber (0 if no isolate is active).
 */
+ (NSNumber* _Nonnull)runtimePointer;

/**
 * Releases the JSI runtime associated with the calling thread's V8 isolate.
 */
+ (void)disposeRuntimeForCurrentIsolate;
@end
#endif /* __OBJC__ */

#ifdef __cplusplus
extern "C" {

// The JSI Runtime of the NativeScript V8 isolate on the calling thread, or
// nullptr when this thread has no active V8 isolate. Each isolate (main thread
// and each Worker) owns a separate JSI Runtime bound to that V8 isolate.
#ifdef __clang__
__attribute__((visibility("default"))) facebook::jsi::Runtime* _Nullable
NativeScriptJSIRuntime(void);
#else
__attribute__((visibility("default"))) facebook::jsi::Runtime*
NativeScriptJSIRuntime(void);
#endif

}  // extern "C"

namespace tns {

__attribute__((visibility("default"))) std::shared_ptr<facebook::jsi::Runtime>
GetJSIRuntime();

__attribute__((visibility("default"))) void
DisposeJSIRuntimeForCurrentIsolate();

}  // namespace tns
#endif /* __cplusplus */

#endif /* JSIRUNTIME_H_ */
