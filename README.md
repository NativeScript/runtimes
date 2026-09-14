# NativeScript Runtime

The core backbone that powers [NativeScript](https://nativescript.org/) apps and provides powerful, zero-boilerplate interop with native APIs directly from JavaScript.

## Key Features

- **Direct Native Interop**: Access 100% of the underlying platform APIs (Objective-C/C/C++) directly from JS/TS without writing native plugin wrappers
- **Multiple JS Engines**: Engine-agnostic architecture natively supporting **V8, Hermes, QuickJS, and JavaScriptCore**
- **Node-API Support**: Runs on Node.js & Deno as well
- **React Native Ecosystem**: Includes JSI-based TurboModule support and seamless React Native compatibility layers

## Repository Structure

- `NativeScript/`: The core C++ runtime source code handling FFI (Foreign Function Interface) operations and JS engine bindings.
- `metadata-generator/`: Clang-based utility that parses C/Obj-C to generate the metadata consumed by the FFI
- `packages/`: The NPM packages published from this repo (e.g., `@nativescript/ios`, `@nativescript/macos`, `@nativescript/visionos`, and specific engine combinations).
- `platforms/apple/`: Apple platform project files, templates, tests, examples, and TKLiveSync sources.
- `scripts/`: Shell and Node utilities for building frameworks, packaging runtimes, and driving CI/CD.

## Development & Building

1. **Prerequisites**: Ensure you have modern versions of **Xcode**, **CMake**, and **Node.js** installed.
2. **Install Dependencies**:
   ```bash
   npm install
   ```
3. **Main Build Commands**:
   - `npm run build-ios`: Builds the iOS runtime frameworks.
   - `npm run build-macos`: Builds the macOS runtime platforms.
   - `npm run build-vision`: Builds the visionOS runtime platforms.
   - `npm run build-node-api`: Compiles the Node-API (N-API) addon.
   - `npm run build-rn-turbomodule`: Compiles the React Native TurboModules compatibility layer.
4. **Core Dependencies**:
   - `npm run build-metagen`: Builds the Clang metadata generator binary.
   - `npm run metagen`: Runs the metadata generator against Xcode SDKs to produce type information.
   - `npm run build-libffi`: Compiles the multi-architecture LibFFI statically linked into the runtime.

## Testing

Execute engine-specific and FFI boundary tests via the included test runners:
- `node scripts/run-tests-ios.js` — iOS tests across supported runtimes
- `node scripts/run-tests-macos.js` — macOS test runner
- `npm run test-rn-turbomodule` — React Native TurboModules unit tests
- `npm run check:ffi-boundaries` — Validates C/Objective-C boundaries via FFI

## Contribute

We welcome community contributions! For major architecture changes or engine integrations, please open an issue first to discuss what you'd like to alter.

## License

This project is licensed under the [MIT License](https://opensource.org/licenses/MIT).
