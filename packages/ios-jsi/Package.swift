// swift-tools-version: 5.9
import PackageDescription

let package = Package(
  name: "NativeScriptJSI",
  platforms: [
    .iOS(.v13),
    .macCatalyst(.v13),
    .macOS(.v13),
    .visionOS(.v1),
  ],
  products: [
    .library(
      name: "NativeScriptJSI",
      targets: ["NativeScriptJSI"]
    ),
  ],
  targets: [
    .target(
      name: "NativeScriptJSI",
      path: "Sources/NativeScriptJSI",
      exclude: [
        "v8_headers/libplatform/DEPS",
      ],
      publicHeadersPath: "include",
      cxxSettings: [
        .headerSearchPath("include"),
        .headerSearchPath("include/jsi"),
        .headerSearchPath("v8runtime"),
        .headerSearchPath("v8_headers"),
        .unsafeFlags(["-O3", "-DNDEBUG"]),
      ],
      linkerSettings: [
        .linkedLibrary("c++"),
        .linkedFramework("Foundation"),
      ]
    ),
  ],
  cxxLanguageStandard: .cxx20
)
