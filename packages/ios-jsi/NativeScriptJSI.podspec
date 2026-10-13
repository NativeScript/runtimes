Pod::Spec.new do |s|
  s.name         = "NativeScriptJSI"
  s.version      = "0.1.0"
  s.summary      = "Standalone Facebook JSI + V8Runtime bridge for NativeScript iOS"
  s.homepage     = "https://github.com/NativeScript/runtimes"
  s.license      = { :type => "Apache-2.0", :file => "LICENSE" }
  s.author       = { "NativeScript Team" => "oss@nativescript.org" }
  s.platforms    = { :ios => "13.0" }
  s.source       = { :git => "https://github.com/NativeScript/runtimes.git", :tag => "v#{s.version}" }

  s.source_files = [
    "platforms/ios/src/jsi/**/*.{h,cpp}",
    "platforms/ios/src/v8runtime/**/*.{h,cpp,mm}"
  ]
  s.public_header_files = [
    "platforms/ios/src/jsi/*.h",
    "platforms/ios/src/v8runtime/JSIRuntime.h",
    "platforms/ios/src/v8runtime/V8Runtime.h",
    "platforms/ios/src/v8runtime/V8RuntimeConfig.h",
    "platforms/ios/src/v8runtime/V8RuntimeFactory.h"
  ]
  s.header_Mappings_dir = "platforms/ios/src"
  s.libraries = "c++"
  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
    "HEADER_SEARCH_PATHS" => "$(inherited) \"$(PODS_TARGET_SRCROOT)/platforms/ios/src\" \"$(PODS_TARGET_SRCROOT)/platforms/ios/src/v8runtime\" \"$(CONFIGURATION_BUILD_DIR)/NativeScript.framework/Headers/include\" \"$(PODS_TARGET_SRCROOT)/platforms/ios/src/include\""
  }
end
