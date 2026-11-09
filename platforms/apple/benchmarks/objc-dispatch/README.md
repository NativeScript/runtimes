# Objective-C Dispatch Benchmarks

This benchmark compares hot Objective-C dispatch shapes across the current
engine package runtime and the legacy iOS AOT direct-call runtime.

The benchmark body is plain NativeScript JavaScript:

- `objc-dispatch-benchmarks.js`

The runner can execute it in three modes:

- `napi-node`: fastest smoke run using the packaged macOS Node-API runtime.
- `ios-package`: builds a temporary iOS app from a packaged `@nativescript/ios*`
  template and runs it in Simulator.
- `legacy-ios`: temporarily injects the benchmark into the PR branch
  `TestRunner` app, builds it, runs it in Simulator, then restores the app
  entry point.

For V8 builds, `gsd-off` still uses the V8-native callback/marshalling path,
but generated signature dispatch lookup is disabled so Objective-C calls fall
back to the dynamic prepared/`ffi_call` path. This keeps the comparison focused
on the generated dispatch win instead of accidentally measuring a hand-written
direct `objc_msgSend` fast path.

For JSC, QuickJS, and Hermes builds, `gsd-off` follows the same rule: the
engine-native callback and marshalling layer remains active, while only the
generated typed invoker lookup is disabled.

Examples:

```sh
npm run benchmark:objc-dispatch -- --runtime napi-node --iterations 100000
npm run benchmark:objc-dispatch -- --runtime ios-package,legacy-ios --iterations 250000
npm run benchmark:objc-dispatch -- --runtime all --include-gsd-off
npm run benchmark:objc-dispatch -- --runtime ios-package --package-tgz packages/ios-v8/dist/nativescript-ios-v8-0.0.2.tgz --variant-label ios-v8 --include-gsd-off
```

Useful options:

```sh
--legacy-repo /path/to/NativeScript/ios
--destination "platform=iOS Simulator,id=<UDID>"
--package-tgz /path/to/nativescript-ios-v8.tgz
--variant-label ios-v8
--iterations 250000
--include-gsd-off
```
