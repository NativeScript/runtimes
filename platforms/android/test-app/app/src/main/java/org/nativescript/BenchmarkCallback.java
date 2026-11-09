package org.nativescript;

// Interface implemented from JS to measure the cost of invoking a JS callback
// through a Java interface (see Benchmarks.invokeCallback).
public interface BenchmarkCallback {
    void onCallback();

    int onInts(int a, int b);

    void onString(String value);
}
