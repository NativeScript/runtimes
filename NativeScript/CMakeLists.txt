cmake_minimum_required(VERSION 3.15)

# Metadata
# C is required for the quickjs engine sources (quickjs.c, libregexp.c, …) and
# quickjs-api.c. It used to be enabled as a side effect of add_subdirectory()ing
# mimalloc; without it CMake silently drops every .c file from the target and the
# build fails at link with undefined JS_* symbols.
project(NativeScript C CXX OBJCXX)

set(NAME NativeScript)
set(VERSION 0.1.0)
set(BUNDLE_IDENTIFIER "org.nativescript.runtime")

set(CMAKE_CXX_STANDARD 20)

set(BUILD_FRAMEWORK TRUE)

set(HERMES_XCFRAMEWORK "${CMAKE_SOURCE_DIR}/../Frameworks/hermes.xcframework")

set(COMMON_FLAGS "-O3 -Wno-shorten-64-to-32")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")

# Arguments
set(TARGET_PLATFORM "macos" CACHE STRING "Target platform for the Objective-C bridge")
set(TARGET_ENGINE "v8" CACHE STRING "Target JS engine for the NativeScript runtime")
set(NS_FFI_BACKEND "auto" CACHE STRING "FFI backend: auto, napi, v8, jsc, quickjs, or hermes")
set(NS_GSD_BACKEND "auto" CACHE STRING "Generated signature dispatch backend: auto, v8, jsc, quickjs, hermes, napi, or none")
set(METADATA_SIZE 0 CACHE STRING "Size of embedded metadata in bytes")
set(BUILD_CLI_BINARY OFF CACHE BOOL "Build the NativeScript CLI binary")
set(BUILD_MACOS_NODE_API OFF CACHE BOOL "Build the NativeScript macOS Node API dylib")
set_property(CACHE NS_FFI_BACKEND PROPERTY STRINGS auto napi v8 jsc quickjs hermes)
set_property(CACHE NS_GSD_BACKEND PROPERTY STRINGS auto v8 jsc quickjs hermes napi none)

if (BUILD_MACOS_NODE_API)
  set(BUILD_FRAMEWORK OFF)
endif()

if(TARGET_PLATFORM STREQUAL "ios")
  set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "13.0")
  set(CMAKE_XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
  set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
  set(LIBFFI_BUILD "iphoneos-arm64")
  set(TARGET_PLATFORM_IOS TRUE)
  set(SDK_NAME "iphoneos")
  set(CMAKE_OSX_ARCHITECTURES "arm64")
  set(TARGET_PLATFORM_SPEC "ios-arm64")

elseif(TARGET_PLATFORM STREQUAL "ios-sim")
  set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "13.0")
  set(CMAKE_XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
  set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
  set(LIBFFI_BUILD "iphonesimulator-universal")
  set(TARGET_PLATFORM_IOS TRUE)
  set(TARGET_PLATFORM_SIM TRUE)
  set(SDK_NAME "iphonesimulator")
  set(CMAKE_OSX_ARCHITECTURES "arm64")
  set(TARGET_PLATFORM_SPEC "ios-arm64-simulator")
  if(EXISTS "${HERMES_XCFRAMEWORK}/ios-arm64_x86_64-simulator")
    set(TARGET_PLATFORM_SPEC "ios-arm64_x86_64-simulator")
  endif()

elseif(TARGET_PLATFORM STREQUAL "visionos")
  set(CMAKE_XCODE_ATTRIBUTE_XROS_DEPLOYMENT_TARGET "2.0")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "2.0")
  set(LIBFFI_BUILD "xros-arm64")
  set(TARGET_PLATFORM_IOS TRUE)
  set(TARGET_PLATFORM_VISIONOS TRUE)
  set(SDK_NAME "xros")
  set(CMAKE_OSX_ARCHITECTURES "arm64")
  set(TARGET_PLATFORM_SPEC "xros-arm64")

elseif(TARGET_PLATFORM STREQUAL "visionos-sim")
  set(CMAKE_XCODE_ATTRIBUTE_XROS_DEPLOYMENT_TARGET "2.0")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "2.0")
  set(LIBFFI_BUILD "xrsimulator-arm64")
  set(TARGET_PLATFORM_IOS TRUE)
  set(TARGET_PLATFORM_VISIONOS TRUE)
  set(TARGET_PLATFORM_SIM TRUE)
  set(SDK_NAME "xrsimulator")
  set(CMAKE_OSX_ARCHITECTURES "arm64")
  set(TARGET_PLATFORM_SPEC "xrsimulator-arm64")

elseif(TARGET_PLATFORM STREQUAL "macos")
  set(CMAKE_XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "13.3")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")
  set(LIBFFI_BUILD "macosx-universal")
  set(TARGET_PLATFORM_MACOS TRUE)
  set(SDK_NAME "macosx")
  set(CMAKE_OSX_ARCHITECTURES "arm64")
  set(TARGET_PLATFORM_SPEC "macos-arm64_x86_64")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DTARGET_PLATFORM_MACOS")

  # Unlike "ios", "macos" doesn't seem to produce a NativeScript.framework.dSYM
  # by default. We can produce it by setting these variables.
  set(CMAKE_XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
  set(CMAKE_XCODE_ATTRIBUTE_DWARF_DSYM_FOLDER_PATH "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
else()
  message(FATAL_ERROR "Unknown target platform: ${TARGET_PLATFORM}")
  return()
endif()

set(ENABLE_JS_RUNTIME TRUE)

if(TARGET_ENGINE STREQUAL "none")
  unset(ENABLE_JS_RUNTIME)
  set(TARGET_ENGINE_NONE TRUE)
elseif(TARGET_ENGINE STREQUAL "hermes")
  set(TARGET_ENGINE_HERMES TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -std=c++20 -DTARGET_ENGINE_HERMES")
elseif(TARGET_ENGINE STREQUAL "v8")
  set(TARGET_ENGINE_V8 TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -stdlib=libc++ -std=c++20 -DTARGET_ENGINE_V8")
elseif(TARGET_ENGINE STREQUAL "quickjs")
  set(TARGET_ENGINE_QUICKJS TRUE)
  # The shared quickjs backend selects between the quickjs-ng and bellard APIs
  # with __QJS_NG__ / __QUICKJS_NG__. Only quickjs-ng is vendored now, so both
  # platforms must define them or the bellard branches get compiled against an
  # ng header (JS_IsArray, JS_FreeValue, … then mismatch on arity).
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__QJS_NG__ -D__QUICKJS_NG__")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -std=c++20 -DTARGET_ENGINE_QUICKJS -D__QJS_NG__ -D__QUICKJS_NG__")
elseif(TARGET_ENGINE STREQUAL "jsc")
  set(TARGET_ENGINE_JSC TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -std=c++20 -DTARGET_ENGINE_JSC")
else()
  message(FATAL_ERROR "Unknown target engine: ${TARGET_ENGINE}")
  return()
endif()

if(ENABLE_JS_RUNTIME)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DENABLE_JS_RUNTIME")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DENABLE_JS_RUNTIME")
else()
  set(GENERIC_NAPI TRUE)
endif()

if(BUILD_MACOS_NODE_API)
  set(GENERIC_NAPI TRUE)
endif()

message(STATUS "TARGET_PLATFORM = ${TARGET_PLATFORM}")
message(STATUS "TARGET_ENGINE = ${TARGET_ENGINE}")
message(STATUS "ENABLE_JS_RUNTIME = ${ENABLE_JS_RUNTIME}")
message(STATUS "GENERIC_NAPI = ${GENERIC_NAPI}")

if(NS_FFI_BACKEND STREQUAL "auto")
  if(GENERIC_NAPI OR TARGET_ENGINE_NONE)
    set(NS_EFFECTIVE_FFI_BACKEND "napi")
  elseif(TARGET_ENGINE_HERMES)
    set(NS_EFFECTIVE_FFI_BACKEND "hermes")
  elseif(TARGET_ENGINE_V8)
    set(NS_EFFECTIVE_FFI_BACKEND "v8")
  elseif(TARGET_ENGINE_JSC)
    set(NS_EFFECTIVE_FFI_BACKEND "jsc")
  elseif(TARGET_ENGINE_QUICKJS)
    set(NS_EFFECTIVE_FFI_BACKEND "quickjs")
  else()
    set(NS_EFFECTIVE_FFI_BACKEND "napi")
  endif()
elseif(NS_FFI_BACKEND STREQUAL "napi" OR
       NS_FFI_BACKEND STREQUAL "v8" OR
       NS_FFI_BACKEND STREQUAL "jsc" OR
       NS_FFI_BACKEND STREQUAL "quickjs" OR
       NS_FFI_BACKEND STREQUAL "hermes")
  set(NS_EFFECTIVE_FFI_BACKEND "${NS_FFI_BACKEND}")
else()
  message(FATAL_ERROR "Unknown NS_FFI_BACKEND: ${NS_FFI_BACKEND}")
endif()

if(NOT NS_EFFECTIVE_FFI_BACKEND STREQUAL "napi" AND
   (GENERIC_NAPI OR TARGET_ENGINE_NONE OR BUILD_MACOS_NODE_API))
  message(FATAL_ERROR
    "NS_FFI_BACKEND=${NS_EFFECTIVE_FFI_BACKEND} requires an embedded JS runtime build")
endif()

if(NOT NS_EFFECTIVE_FFI_BACKEND STREQUAL "napi" AND
   NOT NS_EFFECTIVE_FFI_BACKEND STREQUAL "${TARGET_ENGINE}")
  message(FATAL_ERROR
    "NS_FFI_BACKEND=${NS_EFFECTIVE_FFI_BACKEND} requires TARGET_ENGINE=${NS_EFFECTIVE_FFI_BACKEND}")
endif()

message(STATUS "NS_FFI_BACKEND = ${NS_FFI_BACKEND} (${NS_EFFECTIVE_FFI_BACKEND})")

if(NS_GSD_BACKEND STREQUAL "auto")
  if(NS_EFFECTIVE_FFI_BACKEND STREQUAL "napi")
    set(NS_EFFECTIVE_GSD_BACKEND "napi")
  else()
    set(NS_EFFECTIVE_GSD_BACKEND "${NS_EFFECTIVE_FFI_BACKEND}")
  endif()
elseif(NS_GSD_BACKEND STREQUAL "v8" OR
       NS_GSD_BACKEND STREQUAL "jsc" OR
       NS_GSD_BACKEND STREQUAL "quickjs" OR
       NS_GSD_BACKEND STREQUAL "hermes" OR
       NS_GSD_BACKEND STREQUAL "napi" OR
       NS_GSD_BACKEND STREQUAL "none")
  set(NS_EFFECTIVE_GSD_BACKEND "${NS_GSD_BACKEND}")
else()
  message(FATAL_ERROR "Unknown NS_GSD_BACKEND: ${NS_GSD_BACKEND}")
endif()

if(NS_EFFECTIVE_GSD_BACKEND STREQUAL "v8" AND NOT TARGET_ENGINE_V8)
  message(FATAL_ERROR "NS_GSD_BACKEND=v8 requires TARGET_ENGINE=v8")
endif()
if(NS_EFFECTIVE_GSD_BACKEND STREQUAL "jsc" AND NOT TARGET_ENGINE_JSC)
  message(FATAL_ERROR "NS_GSD_BACKEND=jsc requires TARGET_ENGINE=jsc")
endif()
if(NS_EFFECTIVE_GSD_BACKEND STREQUAL "quickjs" AND NOT TARGET_ENGINE_QUICKJS)
  message(FATAL_ERROR "NS_GSD_BACKEND=quickjs requires TARGET_ENGINE=quickjs")
endif()
if(NS_EFFECTIVE_GSD_BACKEND STREQUAL "hermes" AND NOT TARGET_ENGINE_HERMES)
  message(FATAL_ERROR "NS_GSD_BACKEND=hermes requires TARGET_ENGINE=hermes")
endif()
if(NS_EFFECTIVE_FFI_BACKEND STREQUAL "napi" AND
   NOT (NS_EFFECTIVE_GSD_BACKEND STREQUAL "napi" OR
        NS_EFFECTIVE_GSD_BACKEND STREQUAL "none"))
  message(FATAL_ERROR
    "NS_FFI_BACKEND=napi is the pure Node-API FFI backend and only supports "
    "NS_GSD_BACKEND=napi or none.")
endif()
if(NOT NS_EFFECTIVE_FFI_BACKEND STREQUAL "napi" AND
   NS_EFFECTIVE_GSD_BACKEND STREQUAL "napi")
  message(FATAL_ERROR
    "NS_FFI_BACKEND=${NS_EFFECTIVE_FFI_BACKEND} cannot use NS_GSD_BACKEND=napi. "
    "Use the matching engine backend or none.")
endif()

message(STATUS "NS_GSD_BACKEND = ${NS_GSD_BACKEND} (${NS_EFFECTIVE_GSD_BACKEND})")

# Set up sources
include_directories(
  ./
  ../metadata-generator/include
  napi/common
  libffi/${LIBFFI_BUILD}/include
)

set(FFI_SHARED_SOURCE_FILES
  jsi/shared/Tasks.cpp
)

set(FFI_NAPI_SOURCE_FILES
  ffi/objc/napi/AutoreleasePool.mm
  ffi/objc/napi/Protocol.mm
  ffi/objc/napi/ObjCBridge.mm
  ffi/objc/napi/Block.mm
  ffi/objc/napi/Class.mm
  ffi/objc/napi/Closure.mm
  ffi/objc/napi/ClassMember.mm
  ffi/objc/napi/Cif.mm
  ffi/objc/napi/TypeConv.mm
  ffi/objc/napi/Util.mm
  ffi/objc/napi/Struct.mm
  ffi/objc/napi/ObjectRef.mm
  ffi/objc/napi/JSObject.mm
  ffi/objc/napi/Enum.mm
  ffi/objc/napi/Variable.mm
  ffi/objc/napi/Object.mm
  ffi/objc/napi/CFunction.mm
  ffi/objc/napi/Interop.mm
  ffi/objc/napi/InlineFunctions.mm
  ffi/objc/napi/ClassBuilder.mm
)

set(FFI_ENGINE_SHARED_SOURCE_FILES
  ffi/objc/shared/MetadataState.mm
)

set(FFI_HERMES_ENGINE_SOURCE_FILES
  ${FFI_ENGINE_SHARED_SOURCE_FILES}
  ffi/objc/hermes/NativeApiJsi.mm
)

set(FFI_V8_ENGINE_SOURCE_FILES
  ${FFI_ENGINE_SHARED_SOURCE_FILES}
  # Platform-neutral engine layer, shared with the Android runtime.
  jsi/v8/V8HostObjects.cpp
  jsi/v8/V8Runtime.cpp
  jsi/v8/V8Value.cpp
  # Apple-only ObjC bridge on top of it.
  ffi/objc/v8/NativeApiV8.mm
)

set(FFI_JSC_ENGINE_SOURCE_FILES
  ${FFI_ENGINE_SHARED_SOURCE_FILES}
  # Platform-neutral engine layer, shared with the Android runtime.
  jsi/jsc/JSCHostObjects.cpp
  jsi/jsc/JSCRuntime.cpp
  jsi/jsc/JSCValue.cpp
  # Apple-only ObjC bridge on top of it.
  ffi/objc/jsc/NativeApiJSC.mm
)

set(FFI_QUICKJS_ENGINE_SOURCE_FILES
  ${FFI_ENGINE_SHARED_SOURCE_FILES}
  # Platform-neutral engine layer, shared with the Android runtime.
  jsi/quickjs/QuickJSHostObjects.cpp
  jsi/quickjs/QuickJSRuntime.cpp
  jsi/quickjs/QuickJSValue.cpp
  # Apple-only ObjC bridge on top of it.
  ffi/objc/quickjs/NativeApiQuickJS.mm
)

set(SOURCE_FILES
  ${FFI_SHARED_SOURCE_FILES}
  runtime/apple/NativeScriptException.mm
)

if(NS_EFFECTIVE_FFI_BACKEND STREQUAL "napi")
  set(SOURCE_FILES
    ${SOURCE_FILES}
    ${FFI_NAPI_SOURCE_FILES}
  )
endif()

if(ENABLE_JS_RUNTIME)
  set(SOURCE_FILES
    ${SOURCE_FILES}
    runtime/apple/modules/console/Console.cpp
    runtime/apple/Runtime.cpp
    runtime/apple/modules/worker/Worker.mm
    runtime/apple/modules/worker/MessageJSON.cpp
    runtime/apple/modules/worker/MessageV8.cpp
    runtime/apple/modules/worker/ConcurrentQueue.cpp
    runtime/apple/modules/worker/WorkerImpl.mm
    runtime/apple/modules/module/ModuleInternal.cpp
    runtime/apple/modules/node/Node.cpp
    runtime/apple/modules/node/FS.cpp
    runtime/apple/modules/node/Path.cpp
    runtime/apple/modules/node/Process.cpp
    runtime/apple/modules/node/VM.cpp
    runtime/apple/modules/performance/Performance.cpp
    runtime/apple/ThreadSafeFunction.mm
    runtime/apple/Bundle.mm
    runtime/apple/modules/timers/Timers.mm
    runtime/apple/modules/app/App.mm
    runtime/apple/modules/web/Web.mm
    runtime/apple/NativeScript.mm
    runtime/apple/RuntimeConfig.cpp
    # resolveMainPath() (cli/BundleLoader.h) is called directly from
    # NativeScript.mm's runMainApplication, so it must be compiled into every
    # framework/app target that includes NativeScript.mm -- not only the
    # BUILD_CLI_BINARY executable. cli/main.cpp and cli/segappend.cpp stay
    # CLI-binary-only below (main.cpp defines main(), which cannot also be
    # linked into the NativeScript shared library).
    cli/BundleLoader.mm
    runtime/modules/url/ada/ada.cpp
    runtime/modules/url/URL.cpp
    runtime/modules/url/URLSearchParams.cpp
  )

  if(TARGET_ENGINE_V8)
    # V8's headers come from the pinned release via scripts/download_v8.sh, the
    # same set Android uses. Both spellings are needed: <v8.h> from the napi
    # backend, and "src/inspector/..." from the console/inspector code, which
    # spells paths relative to the V8 checkout root.
    set(V8_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/v8")
    if(NOT EXISTS "${V8_VENDOR_DIR}/src/inspector/v8-console-message.h")
      message(FATAL_ERROR
        "V8 headers missing at ${V8_VENDOR_DIR}. Run scripts/download_v8.sh.")
    endif()

    include_directories(
      napi/v8
      ${V8_VENDOR_DIR}
      ${V8_VENDOR_DIR}/v8_inspector
      ${V8_VENDOR_DIR}/include
      # V8 14.9's internals include Abseil as <absl/...>.
      ${V8_VENDOR_DIR}/third_party/abseil-cpp
    )

    set(SOURCE_FILES
      ${SOURCE_FILES}
      ${V8_VENDOR_DIR}/v8-api.cpp
      ${V8_VENDOR_DIR}/v8-module-loader.cpp
      napi/v8/jsr.cpp
      ${V8_VENDOR_DIR}/SimpleAllocator.cpp
    )

    if(NS_EFFECTIVE_FFI_BACKEND STREQUAL "v8")
      set(SOURCE_FILES
        ${SOURCE_FILES}
        ${FFI_V8_ENGINE_SOURCE_FILES}
      )
    endif()

  elseif(TARGET_ENGINE_HERMES)
    set(HERMES_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/hermes/include")

    include_directories(
      napi/hermes
    )

    # Fetched by scripts/download_hermes.sh alongside hermes.xcframework, not
    # committed -- these headers must match the prebuilt binary, and a stale
    # in-tree copy silently compiling against the wrong ABI is worse than a
    # missing one.
    if(NOT EXISTS "${HERMES_HEADERS_DIR}/jsi/jsi.h")
      message(FATAL_ERROR
        "Hermes headers missing at ${HERMES_HEADERS_DIR}. Run scripts/download_hermes.sh.")
    endif()

    include_directories(
      ${HERMES_HEADERS_DIR}
    )

    set(SOURCE_FILES
      ${SOURCE_FILES}
      napi/hermes/jsr.cpp
    )

    if(NS_EFFECTIVE_FFI_BACKEND STREQUAL "hermes")
      set(SOURCE_FILES
        ${SOURCE_FILES}
        ${FFI_HERMES_ENGINE_SOURCE_FILES}
      )
    endif()

  elseif(TARGET_ENGINE_QUICKJS)
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")

    include_directories(
      napi/quickjs
      ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/quickjs/source_ng
      napi/common
    )

    set(SOURCE_FILES ${SOURCE_FILES}

      # quickjs-ng, NativeScript-patched, vendored at the repo root and shared
      # with the Android build. 0.11.0 replaced libbf with dtoa.
      ../vendor/quickjs/source_ng/cutils.c
      ../vendor/quickjs/source_ng/libregexp.c
      ../vendor/quickjs/source_ng/libunicode.c
      ../vendor/quickjs/source_ng/quickjs.c
      ../vendor/quickjs/source_ng/dtoa.c

      # napi
      ../vendor/quickjs/quickjs-api.c
      napi/quickjs/jsr.cpp
    )

    if(NS_EFFECTIVE_FFI_BACKEND STREQUAL "quickjs")
      set(SOURCE_FILES
        ${SOURCE_FILES}
        ${FFI_QUICKJS_ENGINE_SOURCE_FILES}
      )
    endif()

  elseif(TARGET_ENGINE_JSC)
    include_directories(
      napi/jsc
      ../vendor/jsc
      napi/common
    )

    set(SOURCE_FILES ${SOURCE_FILES}
      ../vendor/jsc/jsc-api.cpp
      napi/jsc/jsr.cpp
    )

    if(NS_EFFECTIVE_FFI_BACKEND STREQUAL "jsc")
      set(SOURCE_FILES
        ${SOURCE_FILES}
        ${FFI_JSC_ENGINE_SOURCE_FILES}
      )
    endif()

  endif()
else()
  include_directories(
    napi/generic
  )
endif()

if(BUILD_CLI_BINARY)
  set(SOURCE_FILES ${SOURCE_FILES}
    cli/main.cpp
    cli/segappend.cpp
  )
endif()

# Find SDK
find_program(XCODEBUILD_EXECUTABLE xcodebuild)

if(METADATA_SIZE EQUAL 0)
  unset(METADATA_SIZE)
else()
  message(STATUS "EMBED_METADATA_SIZE = ${METADATA_SIZE}"
    " (metadata will be embedded in the library)")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DEMBED_METADATA_SIZE=${METADATA_SIZE}")
endif()

message(STATUS "TARGET_PLATFORM = ${TARGET_PLATFORM}")

execute_process(
  COMMAND ${XCODEBUILD_EXECUTABLE} -version -sdk ${SDK_NAME} Path
  OUTPUT_VARIABLE CMAKE_OSX_SYSROOT_INT
  ERROR_QUIET
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(CMAKE_OSX_SYSROOT_INT "${CMAKE_OSX_SYSROOT_INT}" CACHE INTERNAL "")
set(CMAKE_OSX_SYSROOT "${CMAKE_OSX_SYSROOT_INT}" CACHE INTERNAL "")

message(STATUS "SDK = ${CMAKE_OSX_SYSROOT}")

# Build targets
if(BUILD_CLI_BINARY)
  add_executable(${NAME} ${SOURCE_FILES})
  set(BUILD_FRAMEWORK FALSE)
else()
  add_library(
    ${NAME}
    SHARED
    ${SOURCE_FILES}
  )
endif()

target_sources(
  ${NAME}
  PRIVATE
  "NativeScript.h"
)

# NOTE: the Apple V8 slices are built with v8_enable_pointer_compression=false
# (see scripts/matrix/build-ios.sh in v8-buildscripts), so the embedder must NOT
# declare V8_COMPRESS_POINTERS -- V8::Initialize() aborts on a mismatch in
# either direction. This previously defined it for iOS, which was correct for
# the old 14.3 xcframework but would abort at startup against 14.9. The Android
# slices differ: they leave the gn arg at its default, which is ON for 64-bit.


if(ENABLE_JS_RUNTIME)
  target_compile_definitions(${NAME} PRIVATE ENABLE_JS_RUNTIME)
endif()

if(TARGET_PLATFORM_MACOS)
  target_compile_definitions(${NAME} PRIVATE TARGET_PLATFORM_MACOS)
endif()

if(TARGET_ENGINE_HERMES)
  target_compile_definitions(${NAME} PRIVATE TARGET_ENGINE_HERMES)
elseif(TARGET_ENGINE_V8)
  target_compile_definitions(${NAME} PRIVATE TARGET_ENGINE_V8)
elseif(TARGET_ENGINE_QUICKJS)
  target_compile_definitions(${NAME} PRIVATE TARGET_ENGINE_QUICKJS)
elseif(TARGET_ENGINE_JSC)
  target_compile_definitions(${NAME} PRIVATE TARGET_ENGINE_JSC)
endif()

set(NS_GSD_BACKEND_HERMES_VALUE 0)
set(NS_GSD_BACKEND_NAPI_VALUE 0)
set(NS_GSD_BACKEND_PREPARED_VALUE 0)
set(NS_FFI_BACKEND_NAPI_VALUE 0)
set(NS_FFI_BACKEND_V8_VALUE 0)
set(NS_FFI_BACKEND_JSC_VALUE 0)
set(NS_FFI_BACKEND_QUICKJS_VALUE 0)
set(NS_FFI_BACKEND_HERMES_VALUE 0)

if(NS_EFFECTIVE_GSD_BACKEND STREQUAL "v8")
  set(NS_GSD_BACKEND_PREPARED_VALUE 1)
elseif(NS_EFFECTIVE_GSD_BACKEND STREQUAL "jsc")
  set(NS_GSD_BACKEND_PREPARED_VALUE 1)
elseif(NS_EFFECTIVE_GSD_BACKEND STREQUAL "quickjs")
  set(NS_GSD_BACKEND_PREPARED_VALUE 1)
elseif(NS_EFFECTIVE_GSD_BACKEND STREQUAL "hermes")
  set(NS_GSD_BACKEND_HERMES_VALUE 1)
elseif(NS_EFFECTIVE_GSD_BACKEND STREQUAL "napi")
  set(NS_GSD_BACKEND_NAPI_VALUE 1)
endif()

if(NS_EFFECTIVE_FFI_BACKEND STREQUAL "napi")
  set(NS_FFI_BACKEND_NAPI_VALUE 1)
elseif(NS_EFFECTIVE_FFI_BACKEND STREQUAL "v8")
  set(NS_FFI_BACKEND_V8_VALUE 1)
elseif(NS_EFFECTIVE_FFI_BACKEND STREQUAL "jsc")
  set(NS_FFI_BACKEND_JSC_VALUE 1)
elseif(NS_EFFECTIVE_FFI_BACKEND STREQUAL "quickjs")
  set(NS_FFI_BACKEND_QUICKJS_VALUE 1)
elseif(NS_EFFECTIVE_FFI_BACKEND STREQUAL "hermes")
  set(NS_FFI_BACKEND_HERMES_VALUE 1)
endif()

target_compile_definitions(${NAME} PRIVATE
  NS_GSD_BACKEND_HERMES=${NS_GSD_BACKEND_HERMES_VALUE}
  NS_GSD_BACKEND_NAPI=${NS_GSD_BACKEND_NAPI_VALUE}
  NS_GSD_BACKEND_PREPARED=${NS_GSD_BACKEND_PREPARED_VALUE}
  NS_FFI_BACKEND_NAPI=${NS_FFI_BACKEND_NAPI_VALUE}
  NS_FFI_BACKEND_V8=${NS_FFI_BACKEND_V8_VALUE}
  NS_FFI_BACKEND_JSC=${NS_FFI_BACKEND_JSC_VALUE}
  NS_FFI_BACKEND_QUICKJS=${NS_FFI_BACKEND_QUICKJS_VALUE}
  NS_FFI_BACKEND_HERMES=${NS_FFI_BACKEND_HERMES_VALUE}
)

set(FRAMEWORK_VERSION_VALUE "${VERSION}")
if(TARGET_PLATFORM_MACOS)
  # macOS framework consumers (including Xcode's copy/sign phases) expect
  # the canonical Versions/A layout instead of semantic versioned directories.
  set(FRAMEWORK_VERSION_VALUE "A")
endif()

set_target_properties(${NAME} PROPERTIES
  VERSION ${VERSION}
  SOVERSION ${VERSION}
  FRAMEWORK_VERSION ${FRAMEWORK_VERSION_VALUE}
  MACOSX_FRAMEWORK_SHORT_VERSION_STRING ${VERSION}
  MACOSX_FRAMEWORK_BUNDLE_VERSION ${VERSION}
  MACOSX_FRAMEWORK_IDENTIFIER ${BUNDLE_IDENTIFIER}
  PUBLIC_HEADER "NativeScript.h"
)

if(BUILD_FRAMEWORK)
  message(STATUS "FRAMEWORK = TRUE")
  set_target_properties(${NAME} PROPERTIES
    FRAMEWORK TRUE
  )
endif()

if(TARGET_PLATFORM_MACOS)
  # add_custom_command(TARGET ${NAME} POST_BUILD
  # COMMAND /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string ${CMAKE_OSX_DEPLOYMENT_TARGET}" $<TARGET_FILE_DIR:${NAME}>/Resources/Info.plist
  # )

  # Convert the Info.plist from binary format to XML format.
  # This seemed to unblock a build error when using in React Native.
  if (BUILD_FRAMEWORK)
    add_custom_command(TARGET ${NAME} POST_BUILD
      COMMAND plutil -convert xml1 $<TARGET_FILE_DIR:${NAME}>/Resources/Info.plist
    )
  endif()
  set(METADATA_FILE "metadata.macos.nsmd")
elseif(TARGET_PLATFORM_IOS)
  # Convert the Info.plist from binary format to XML format.
  # This seemed to unblock a build error when using in React Native.
  add_custom_command(TARGET ${NAME} POST_BUILD
    COMMAND plutil -convert xml1 $<TARGET_FILE_DIR:${NAME}>/Info.plist
  )

  if(TARGET_PLATFORM_SIM)
    set(METADATA_FILE "metadata.ios-sim.nsmd")
  else()
    set(METADATA_FILE "metadata.ios.nsmd")
  endif()
endif()

if(NOT METADATA_SIZE EQUAL 0)
  message(STATUS "Running script to embed metadata in the library")

  add_custom_command(TARGET ${NAME} POST_BUILD
    COMMAND codesign --remove-signature $<TARGET_FILE:${NAME}>
  )

  add_custom_command(TARGET ${NAME} POST_BUILD
    COMMAND node ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/write_metadata.js $<TARGET_FILE:${NAME}> ${METADATA_FILE}
  )

  add_custom_command(TARGET ${NAME} POST_BUILD
    COMMAND codesign -fs - $<TARGET_FILE:${NAME}>
  )
endif()

message(STATUS "LIBFFI_BUILD = ${LIBFFI_BUILD}")

target_link_directories(
  ${NAME}
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/libffi/${LIBFFI_BUILD}
)

if(TARGET_ENGINE_HERMES)
  target_link_options(
    ${NAME}
    PRIVATE
    -Wl,-rpath,@executable_path/../Frameworks
    -Wl,-rpath,${CMAKE_CURRENT_SOURCE_DIR}/../Frameworks/hermes.xcframework/${TARGET_PLATFORM_SPEC}
    -Wl,-F${CMAKE_CURRENT_SOURCE_DIR}/../Frameworks/hermes.xcframework/${TARGET_PLATFORM_SPEC}
  )

  target_link_libraries(
    ${NAME}
    PRIVATE
    "-framework hermes"
  )
endif()

if(TARGET_ENGINE_JSC)
  target_link_libraries(
    ${NAME}
    PRIVATE
    "-framework JavaScriptCore"
  )
endif()

if(TARGET_ENGINE_V8)
  add_library(v8::monolith STATIC IMPORTED GLOBAL)

  set(V8_XCFRAMEWORK "${CMAKE_SOURCE_DIR}/../Frameworks/libv8_monolith.xcframework")

  # Decide platform + slice
  if(APPLE)
    if(TARGET_PLATFORM STREQUAL "ios-sim")
        # Prefer universal sim slice if present
        set(V8_SLICE_DIR "${V8_XCFRAMEWORK}/ios-arm64-simulator/libv8_monolith.framework")
        if(NOT EXISTS "${V8_SLICE_DIR}")
          set(V8_SLICE_DIR "${V8_XCFRAMEWORK}/ios-arm64-simulator/libv8_monolith.framework")
        endif()
    elseif(TARGET_PLATFORM STREQUAL "ios")
        # Prefer universal sim slice if present
        set(V8_SLICE_DIR "${V8_XCFRAMEWORK}/ios-arm64/libv8_monolith.framework")
        if(NOT EXISTS "${V8_SLICE_DIR}")
          set(V8_SLICE_DIR "${V8_XCFRAMEWORK}/ios-arm64/libv8_monolith.framework")
        endif()
    elseif(TARGET_PLATFORM STREQUAL "visionos-sim")
      set(V8_SLICE_DIR "${V8_XCFRAMEWORK}/xrsimulator-arm64/libv8_monolith.framework")
    elseif(TARGET_PLATFORM STREQUAL "visionos")
      set(V8_SLICE_DIR "${V8_XCFRAMEWORK}/xros-arm64/libv8_monolith.framework")
    else()
      # macOS
      set(V8_SLICE_DIR "${V8_XCFRAMEWORK}/macos-arm64/libv8_monolith.framework")
      if(NOT EXISTS "${V8_SLICE_DIR}")
        set(V8_SLICE_DIR "${V8_XCFRAMEWORK}/macos-arm64/libv8_monolith.framework") # keep same, just explicit
      endif()
    endif()
  else()
    message(FATAL_ERROR "This example is for Apple platforms only.")
  endif()

  if(NOT EXISTS "${V8_SLICE_DIR}/libv8_monolith")
    message(FATAL_ERROR "Missing V8 slice for target platform ${TARGET_PLATFORM}: ${V8_SLICE_DIR}")
  endif()

  # Point imported lib at the slice
  set_target_properties(v8::monolith PROPERTIES
    IMPORTED_LOCATION "${V8_SLICE_DIR}/libv8_monolith"
    INTERFACE_INCLUDE_DIRECTORIES "${V8_SLICE_DIR}/Headers"
  )

  target_link_libraries(${NAME} PRIVATE v8::monolith)
endif()

target_link_libraries(
  ${NAME}
  PRIVATE
  ffi
  "-framework Foundation"
)

if(TARGET_PLATFORM_MACOS)
  target_link_libraries(
    ${NAME}
    PRIVATE
    "-framework AppKit"
  )
elseif(TARGET_PLATFORM_IOS)
  target_link_libraries(
    ${NAME}
    PRIVATE
    "-framework UIKit"
  )
endif()

if(GENERIC_NAPI)
  target_link_options(
    ${NAME}
    PRIVATE
    "-Wl"
    "-undefined"
    "dynamic_lookup"
  )
endif()
