require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))
fabric_enabled = ENV["RCT_NEW_ARCH_ENABLED"] == "1"

folly_compiler_flags = "-DFOLLY_NO_CONFIG -DFOLLY_MOBILE=1 -DFOLLY_USE_LIBCPP=1 -DFOLLY_CFG_NO_COROUTINES=1 -Wno-comma -Wno-shorten-64-to-32"

Pod::Spec.new do |s|
  s.name = "NativeScriptNativeApi"
  s.version = package["version"]
  s.summary = package["description"]
  s.homepage = "https://github.com/NativeScript/runtimes"
  s.license = "Apache-2.0"
  s.author = package["author"]
  s.platforms = { :ios => "15.1" }
  s.source = { :git => "https://github.com/NativeScript/runtimes.git", :tag => "react-native-v#{s.version}" }
  s.requires_arc = false

  s.source_files = [
    "ios/**/*.{h,mm}",
    "native-api/ffi/objc/hermes/**/*.h",
    "native-api/ffi/objc/shared/**/*.h",
    "native-api/ffi/objc/hermes/NativeApiJsi.mm"
  ]
  s.exclude_files = "ios/Fabric/**/*" unless fabric_enabled
  s.public_header_files = "ios/**/*.h"
  s.resource_bundles = {
    "NativeScriptNativeApi" => ["metadata/*.nsmd"]
  }
  s.vendored_frameworks = "ios/vendor/Libffi.xcframework"

  s.compiler_flags = folly_compiler_flags
  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
    "CLANG_CXX_LIBRARY" => "libc++",
    "GCC_PREPROCESSOR_DEFINITIONS" => "$(inherited) TARGET_ENGINE_HERMES=1 NS_GSD_BACKEND_HERMES=1",
    "HEADER_SEARCH_PATHS" => [
      "\"$(PODS_TARGET_SRCROOT)/ios\"",
      "\"$(PODS_TARGET_SRCROOT)/native-api\"",
      "\"$(PODS_TARGET_SRCROOT)/native-api/metadata/include\"",
      "\"$(PODS_TARGET_SRCROOT)/ios/vendor/libffi/include\"",
      "\"$(PODS_ROOT)/Headers/Public/React-Codegen\"",
      "\"$(PODS_ROOT)/Headers/Private/React-Codegen\"",
      "\"$(PODS_ROOT)/Headers/Public/ReactCommon\"",
      "\"$(PODS_ROOT)/Headers/Private/ReactCommon\"",
      "\"$(PODS_ROOT)/Headers/Public/RNWorklets\""
    ].join(" ")
  }

  if respond_to?(:install_modules_dependencies, true)
    install_modules_dependencies(s)
  else
    s.dependency "React-Core"
    s.dependency "React-jsi"
    s.dependency "ReactCommon/turbomodule/core"
  end
  s.dependency "RNWorklets"
end
