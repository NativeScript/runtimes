# Objective-C Metadata Generator

This project generates the metadata for the target iOS application. The build parameters are gathered by the `build-step-metadata-generator.py` and turned into command line parameters for the `objc-metadata-generator`.

## Building

```bash
cmake -B build
cmake --build build --target=clean
cmake --build build
# this can be sped up by using -jN where N is the number of cores.
# ex: cmake --build build -j10
```

### Additional cmake flags

* `-DMETADATA_BINARY_ARCH=arm64`: Generate the metadata for the arm64 architecture. Possible values: `arm64`, `x86_64`.
* `-DCMAKE_OSX_ARCHITECTURES=arm64`: Generate the metadata for the arm64 architecture. Possible values: `arm64`, `x86_64`.
* `-DCMAKE_BUILD_TYPE=Release`: Build the project in release mode.

Example:

```bash
METADATA_ARCH="arm64" # or "x86_64"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMETADATA_BINARY_ARCH=$METADATA_ARCH -DCMAKE_OSX_ARCHITECTURES=$METADATA_ARCH
cmake --build build
```

The repository packaging script builds both Intel and Apple Silicon host tools
by default. For local development on Xcode installations whose `libclang` only
contains the current host architecture, build just that architecture:

```bash
METADATA_GENERATOR_ARCHS="$(uname -m)" npm run build-metagen
```

## Debugging the metadata generator

To debug the metadata generator you first need to generate the xcode project for it:

```bash
cmake -B cmake-build -G Xcode
```

This will create the xcode project in the `cmake-build` directory, which you can open with `open cmake-build/MetadataGenerator.xcodeproj`.

To build and run the metadata generator you must first change the Scheme to `objc-metadata-generator`, then you must edit this scheme and add the command line parameters for the `Arguments Passed on Launch` section. These parameters can be found on the `build-step-metadata-generator.py` script or in the build logs for an app, in the metadata generator step. If getting this data from another app, ensure that the paths set on the command line are accurate (not relative to the app's directory).

Example command line arguments:
```bash
# replace NSV8RUNTIMEPATH with the path to the ns-v8ios-runtime path, ex: /Users/you/ns-v8ios-runtime
# replace YOU with your username
-verbose -output-typescript /tmp/tsdeclarations/ -output-bin NSV8RUNTIMEPATH/build/Debug-iphonesimulator/metadata-arm64.bin -output-umbrella NSV8RUNTIMEPATH/build/Debug-iphonesimulator/umbrella-arm64.h -docset-path /Users/YOU/Library/Developer/Shared/Documentation/DocSets/com.apple.adc.documentation.iOS.docset Xclang -isysroot /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator15.2.sdk -mios-simulator-version-min=9.0 -std=gnu99 -target arm64-apple-ios15.2-simulator -INSV8RUNTIMEPATH/build/Debug-iphonesimulator/include -INSV8RUNTIMEPATH/NativeScript -INSV8RUNTIMEPATH/platforms/apple/test/runtime/fixtures -FNSV8RUNTIMEPATH/build/Debug-iphonesimulator -DCOCOAPODS=1 -DDEBUG=1 -I. -fmodules
```

For a better way of generating these arguments, just run the TestRunner scheme on the v8ios-runtime project and get the arguments from the log.

## Opt-in bundle-based metadata filtering

`symbol-analyzer/` contains a Rust/Oxc analyzer that scans the emitted JavaScript
or TypeScript bundle for unresolved global symbols and writes them as a normal
NativeScript metadata whitelist. It processes multiple bundle/chunk files in
parallel and emits deterministically sorted rules. Parse or semantic-analysis
errors, dynamic global property access, and dynamic code execution fail open by
writing `*:*`, so an unsupported bundle cannot accidentally remove metadata.
Foundation and Runtime are retained by default as a conservative safety margin.

The existing `whitelist.mdg` and `blacklist.mdg` behavior remains available.
When automatic filtering is enabled, `whitelist.mdg` is merged into the generated
whitelist and `blacklist.mdg` is still applied afterwards.

Set these environment variables on the metadata-generator build phase:

```bash
NS_METADATA_AUTO_FILTER=1
# A shell-quoted list of emitted bundle files or directories.
NS_METADATA_BUNDLE_PATHS="$CONFIGURATION_BUILD_DIR/app/bundle.js"
```

The packaged analyzer next to `objc-metadata-generator` is used by default.
`NS_METADATA_SYMBOL_ANALYZER` can override its path for local development.

For repeatable full-SDK performance runs on macOS, use
`benchmarks/run-macos.sh` with a fresh output directory. Each iteration records
wall/CPU time, peak memory, and SHA-256 hashes for every generated artifact.

To validate the analyzer against the repository's real NativeScript macOS
examples as source, minified bundles, and split chunks, run:

```bash
metadata-generator/benchmarks/validate-bundles.sh \
  metadata-generator/symbol-analyzer/target/release/ns-metadata-symbols
```

The script pins esbuild 0.25.8, requires identical analyzer output for all three
forms, and verifies that the intentionally dynamic/malformed TestRunner corpus
fails open.
