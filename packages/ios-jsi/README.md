# @nativescript/ios-jsi

Standalone `facebook::jsi::Runtime` (`rnv8::V8Runtime`) package for NativeScript iOS.

This package keeps `facebook::jsi` completely decoupled from the core `@nativescript/ios` runtime binary so apps that do not need JSI incur zero binary size impact, while apps or plugins that use JSI (such as Nitro Modules or custom C++/Objective-C++ `HostObject` bridges) can install it on demand.

## Installation

```bash
ns plugin add @nativescript/ios-jsi
```

## Native C++ / Objective-C++ Usage

```objc
#include "JSIRuntime.h"
#include "jsi/jsi.h"

void installBindings() {
  facebook::jsi::Runtime* rt = NativeScriptJSIRuntime();
  if (!rt) return;

  rt->global().setProperty(*rt, "myConstant", facebook::jsi::Value(42));
}
```

Each V8 isolate (the main UI thread and any NativeScript Worker threads) lazily owns its own `std::shared_ptr<facebook::jsi::Runtime>` keyed by `v8::Isolate::GetCurrent()`.

## JavaScript / TypeScript Usage

```ts
import { getJSIRuntimePointer, isJSIRuntimeAvailable } from '@nativescript/ios-jsi';

console.log('JSI available:', isJSIRuntimeAvailable());
console.log('JSI Runtime*:', '0x' + getJSIRuntimePointer().toString(16));
```

## License

Apache-2.0
