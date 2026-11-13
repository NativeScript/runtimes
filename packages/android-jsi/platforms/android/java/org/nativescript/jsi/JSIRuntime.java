package org.nativescript.jsi;

public final class JSIRuntime {
  static {
    System.loadLibrary("nativescript-jsi");
  }

  private JSIRuntime() {}

  /**
   * Returns the raw `facebook::jsi::Runtime*` pointer for the active V8 isolate
   * on the calling thread, or 0L if no V8 isolate is active on this thread.
   */
  public static long getRuntimePointer() {
    return nativeRuntimePointer();
  }

  /**
   * Releases the JSI runtime associated with the calling thread's V8 isolate.
   */
  public static void disposeRuntimeForCurrentIsolate() {
    nativeDisposeRuntimeForCurrentIsolate();
  }

  private static native long nativeRuntimePointer();
  private static native void nativeDisposeRuntimeForCurrentIsolate();
}
