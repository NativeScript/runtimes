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

## JavaScript / TypeScript Usage

```ts
import { getJSIRuntimePointer, isJSIRuntimeAvailable } from '@nativescript/android-jsi';

console.log('JSI available:', isJSIRuntimeAvailable());
console.log('JSI Runtime*:', '0x' + getJSIRuntimePointer().toString(16));
```

## License

Apache-2.0
