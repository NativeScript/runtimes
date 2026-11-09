---
name: write-nativescript-native-modules
description: Port React Native iOS native modules, UIKit containers, navigation stacks, tabs, gestures, delegates, or TurboModule-backed libraries to drop-in pure TypeScript implementations using @nativescript/react-native. Use when proving NativeScript can replace Objective-C/Swift module code without app-native code, when auditing runtime API gaps, or when writing reusable guidance for native modules authored entirely in TypeScript.
---

# Write NativeScript Native Modules

## Overview

Port the library mechanically first, then improve the generic NativeScript runtime only when the port hits an Objective-C/Swift capability that TypeScript cannot express yet. The app using the fork must not add native code or demo-only fallbacks.

## Workflow

1. Compare against the upstream iOS implementation before editing.
   Use `rg` on Objective-C/Swift sources for `UIViewController`, `UINavigationController`, delegates, target/action, gestures, presentation, animation, and event emission. Keep upstream ordering and state guards unless NativeScript cannot express them yet.

2. Keep the public JS API drop-in.
   Preserve exports, component names, props, events, route behavior, accessibility labels, and timing semantics. Do not require app config, Podfile changes, native files, or userland shims beyond depending on `@nativescript/react-native`.

3. Put native behavior in TypeScript worklets.
   Create UIKit objects with `defineUIViewController`, `defineUIKitView`, `nativeValue`, and worklet callbacks. Native UI creation, mutation, delegates, and target/action must run on the UI worklet runtime, not on React Native's JS thread.

4. Extend NativeScript only with generic primitives.
   Accept APIs like `runtimeInvoker(callback)`, host refresh, generic delegate creation, function callback thread policy, selector metadata, or value serialization. Reject APIs like `afterReactNavigationTransition`, `presentNativeStackModal`, or anything named after the library being ported.

5. Verify against both implementations.
   Maintain an original-library demo and a NativeScript-fork demo. Test on the requested simulator/device, use SimDeck for taps, gestures, screenshots, recordings when available, and logs. Compare first paint, safe areas, headers, tab bars, animations, gestures, modal presentation, repeated taps, event order, and scroll insets.

## Porting Rules

- Use UIKit APIs directly from TypeScript whenever upstream does.
- Keep worklets enabled; NativeScript native UI is the reason the UI runtime exists.
- Retain delegates/targets through NativeScript context helpers so UIKit callbacks survive.
- Let UIKit own native transitions. During active `transitionCoordinator`, defer reconciliation until completion.
- Do not dispatch every granular native call through `dispatch_sync`; batch behavior in UI worklets.
- Do not run React Native's JS runtime on the UI thread.
- Do not add native source files to the consuming app.
- Add tests for every generic runtime primitive added to unblock a port.

## References

- Read `references/porting-patterns.md` when implementing or reviewing a port.
- Read `references/runtime-gap-log.md` before adding or rejecting a NativeScript TurboModule/runtime API.
