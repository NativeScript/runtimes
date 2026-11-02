# @nativescript/ios-jsi

Standalone `facebook::jsi::Runtime` (`rnv8::V8Runtime`) Swift Package for NativeScript iOS.

This package keeps `facebook::jsi` completely decoupled from the core `@nativescript/ios` runtime binary so apps that do not need JSI incur zero binary size impact, while apps or plugins that use JSI (such as Nitro Modules or custom C++/Objective-C++ `HostObject` bridges) can install it on demand via **Swift Package Manager (SPM)** with **zero CocoaPods required**.

## Installation

```bash
ns plugin add @nativescript/ios-jsi
```

When installed, the NativeScript CLI automatically registers the bundled `NativeScriptJSI` Swift Package (`Package.swift`) via `nativescript.config.ts`:

```ts
import { NativeScriptConfig } from '@nativescript/core';

export default {
  ios: {
    SPMPackages: [
      {
        name: 'NativeScriptJSI',
        libs: ['NativeScriptJSI'],
        path: '.',
      },
    ],
  },
} as NativeScriptConfig;
```

## Native C++ / Objective-C++ / Swift Usage

In Objective-C++ (`.mm`) or C++ (`.cpp`):

```objc
#include <JSIRuntime.h>
#include <jsi/jsi.h>

void installBindings() {
  facebook::jsi::Runtime* rt = NativeScriptJSIRuntime();
  if (!rt) return;

  rt->global().setProperty(*rt, "myConstant", facebook::jsi::Value(42));
}
```

In Swift:

```swift
import NativeScriptJSI

let ptr = JSIRuntime.runtimePointer()
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
