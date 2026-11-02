/**
 * Returns the native `facebook::jsi::Runtime*` pointer address for the active
 * V8 isolate on the calling thread, or `0` if unavailable.
 */
export declare function getJSIRuntimePointer(): number;

/**
 * Returns `true` if a `facebook::jsi::Runtime` is available on the calling thread.
 */
export declare function isJSIRuntimeAvailable(): boolean;

/**
 * Disposes the `facebook::jsi::Runtime` instance bound to the calling thread's V8 isolate.
 */
export declare function disposeJSIRuntimeForCurrentIsolate(): void;
