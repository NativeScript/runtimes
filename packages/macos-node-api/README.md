# @nativescript/macos-node-api

An embeddable, engine-agnostic NativeScript runtime for macOS based on Node-API.

## Installation

```sh
npm install @nativescript/macos-node-api
```

## Usage

See [examples](https://github.com/NativeScript/runtime-node-api/tree/main/platforms/apple/examples) in the repo. Best run on Node.js for now.

## Contributing

To build `@nativescript/macos-node-api` for yourself:

```sh
git clone git@github.com:NativeScript/napi-ios.git
cd napi-ios
npm install

# This script does the following:
# 1. Builds the metadata generator, which consists of the following steps:
#    - Download LLVM
#    - Inside ./metadata-generator, run cmake to produce:
#      - ./metadata-generator/dist/arm64/bin/objc-metadata-generator
#      - ./metadata-generator/dist/x86_64/bin/objc-metadata-generator
# 2. Generates metadata for iOS and macOS:
#    - ./metadata-generator/metadata/metadata.ios.arm64.{h,nsmd}
#    - ./metadata-generator/metadata/metadata.ios-sim.{arm64,x86_64}.{h,nsmd}
#    - ./metadata-generator/metadata/metadata.macos.{arm64,x86_64}.{h,nsmd}
# 3. Builds the NativeScript iOS XCFramework:
#    - ./packages/ios-node-api/build/RelWithDebInfo/NativeScript.apple.node
# 4. Builds the NativeScript macOS XCFramework:
#    - ./packages/macos-node-api/build/RelWithDebInfo/NativeScript.apple.node
./scripts/build_all_react_native.sh
```

Currently, we are following a convention of committing most, if not all, of these build files to source, so you may find that it's ready set up to begin with.
