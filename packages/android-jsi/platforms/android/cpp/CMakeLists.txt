cmake_minimum_required(VERSION 3.22.1)
project(nativescript_jsi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Allow overriding V8_INCLUDE_DIR for custom V8 headers
if(NOT DEFINED V8_INCLUDE_DIR)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/v8.h")
    set(V8_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
  endif()
endif()

add_library(
  nativescript-jsi
  SHARED
  jsi/jsi.cpp
  jsi/jsilib-posix.cpp
  v8runtime/V8Runtime.cpp
  v8runtime/V8RuntimeFactory.cpp
  v8runtime/V8PointerValue.cpp
  v8runtime/JSIV8ValueConverter.cpp
  v8runtime/HostProxy.cpp
  v8runtime/JSIRuntime.cpp
  v8runtime/V8ExportShims.cpp
)

# Inline V8 header code reads object layouts, so these must match the V8 build
# inside libNativeScript.so (NativeScript/android test-app/runtime/CMakeLists.txt).
target_compile_definitions(nativescript-jsi PUBLIC V8_31BIT_SMIS_ON_64BIT_ARCH)
if(ANDROID_ABI MATCHES "arm64-v8a$" OR ANDROID_ABI MATCHES "x86_64$")
  target_compile_definitions(nativescript-jsi PUBLIC V8_COMPRESS_POINTERS)
endif()

target_include_directories(
  nativescript-jsi
  PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/v8runtime"
)

if(DEFINED V8_INCLUDE_DIR AND EXISTS "${V8_INCLUDE_DIR}")
  target_include_directories(
    nativescript-jsi
    PRIVATE
    "${V8_INCLUDE_DIR}"
  )
endif()

target_compile_options(
  nativescript-jsi
  PRIVATE
  -O2
  -fexceptions
  -frtti
  -fvisibility=default
)

find_library(android-log-lib log)
find_library(android-lib android)

# V8 symbols are exported by libNativeScript.so which is already loaded in the
# Android process by the NativeScript runtime. Allow unresolved shared-library
# symbols at link time when NATIVESCRIPT_SO_PATH is not explicitly provided.
if(DEFINED NATIVESCRIPT_SO_PATH AND EXISTS "${NATIVESCRIPT_SO_PATH}")
  add_library(NativeScriptRuntime SHARED IMPORTED)
  set_target_properties(
    NativeScriptRuntime
    PROPERTIES IMPORTED_LOCATION "${NATIVESCRIPT_SO_PATH}"
  )
  target_link_libraries(
    nativescript-jsi
    ${android-log-lib}
    ${android-lib}
    NativeScriptRuntime
  )
else()
  # Android's dynamic linker binds a library's undefined symbols only against
  # its own DT_NEEDED tree, never against libraries the process loaded earlier.
  # Linking against an empty stub whose soname is libNativeScript.so records
  # that dependency, so the V8 symbols bind to the runtime already in memory.
  add_library(nativescript-runtime-stub SHARED v8runtime/NativeScriptRuntimeStub.cpp)
  set_target_properties(nativescript-runtime-stub PROPERTIES NO_SONAME ON)
  target_link_options(nativescript-runtime-stub PRIVATE "-Wl,-soname,libNativeScript.so")

  string(REGEX REPLACE "(^| )-Wl,--no-undefined( |$)" " " CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  target_link_options(
    nativescript-jsi
    PRIVATE
    "-Wl,--unresolved-symbols=ignore-all"
    "-Wl,--allow-shlib-undefined"
  )
  target_link_libraries(
    nativescript-jsi
    ${android-log-lib}
    ${android-lib}
    nativescript-runtime-stub
  )
endif()
