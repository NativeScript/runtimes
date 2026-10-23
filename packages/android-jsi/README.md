# @nativescript/android-jsi

Standalone `facebook::jsi::Runtime` (`rnv8::V8Runtime`) package for NativeScript Android.

This package keeps `facebook::jsi` completely decoupled from the core `@nativescript/android` runtime (`libNativeScript.so`) so apps that do not need JSI incur zero binary size overhead, while apps or plugins that use JSI (such as Nitro Modules or custom C++ `HostObject` bridges) can install it on demand.

## Installation

```bash
ns plugin add @nativescript/android-jsi
```

## Native C++ / CMake Usage

In your app or plugin `CMakeLists.txt`, include the `@nativescript/android-jsi` subdirectory or link against `nativescript-jsi`:

```cmake
add_subdirectory(
  "${NODE_MODULES_DIR}/@nativescript/android-jsi/platforms/android/cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/nativescript-jsi"
)

target_link_libraries(my-nitro-module nativescript-jsi)
```

Then in C++:

```cpp
#include "JSIRuntime.h"
#include "jsi/jsi.h"

void installBindings() {
  facebook::jsi::Runtime* rt = NativeScriptJSIRuntime();
  if (!rt) return;

  rt->global().setProperty(*rt, "myConstant", facebook::jsi::Value(42));
}
```

Each V8 isolate (the main UI thread and any NativeScript Worker threads) lazily owns its own `std::shared_ptr<facebook::jsi::Runtime>` keyed by `v8::Isolate::GetCurrent()`, resolving V8 symbols exported by `libNativeScript.so`.

## Scheduling work on the JS thread

`tns::GetJSICallInvoker()` returns a `tns::JSICallInvoker` (the equivalent of React Native's `CallInvoker`) for the calling thread's runtime, backed by that thread's `ALooper`. Grab it on the JS thread, then call `invokeAsync` from any thread to run work back on it, for example to settle a promise from a background queue:

```cpp
#include "JSIRuntime.h"

auto invoker = tns::GetJSICallInvoker();
std::thread([invoker, resolve] {
  double result = expensiveWork();
  invoker->invokeAsync([resolve, result](facebook::jsi::Runtime& rt) {
    resolve->call(rt, result);
  });
}).detach();
```

Each task runs as a top-level JS entry followed by a microtask checkpoint, so `await`ing code continues immediately. Errors thrown by a task are reported through NativeScript's uncaught error handling.

## JSI version

The bundled `facebook::jsi` headers match React Native 0.88.0, which is what Nitro Modules v1 (`nitro-core`) and other current JSI libraries compile against. Install Nitro with `margelo::nitro::install(*runtime, dispatcher)`, where the dispatcher forwards `runAsync` to `JSICallInvoker::invokeAsync` and `isOnDispatcher` to `isOnJSThread`.

## JavaScript / TypeScript Usage

```ts
import { getJSIRuntimePointer, isJSIRuntimeAvailable } from '@nativescript/android-jsi';

console.log('JSI available:', isJSIRuntimeAvailable());
console.log('JSI Runtime*:', '0x' + getJSIRuntimePointer().toString(16));
```

## License

Apache-2.0
