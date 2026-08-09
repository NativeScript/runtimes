cmake_minimum_required(VERSION 3.22.1)
project(nativescript_rn)

# The guest build of the NativeScript Android runtime. See ../build.gradle for
# why this compiles from source rather than shipping a prebuilt .aar.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NS_RUNTIME_DIR "${NS_ROOT}/runtime")
set(NS_ANDROID_RUNTIME_DIR "${NS_RUNTIME_DIR}/android/jsi")
set(NS_JNI_DIR "${NS_ROOT}/ffi/jni/jsi")
set(NS_JSI_DIR "${NS_ROOT}/jsi")

# NS_JSI_HOST_RUNTIME: the jsi::Runtime belongs to React Native.
# TARGET_ENGINE_HERMES selects jsi/hermes, which despite the name is the
# adapter over plain facebook::jsi and holds nothing Hermes-specific -- under a
# host runtime the VM underneath may equally be JSC.
# NS_NO_INSPECTOR: the inspector is written against v8_inspector.
add_compile_definitions(
        NS_JSI_HOST_RUNTIME
        TARGET_ENGINE_HERMES
        NS_NO_INSPECTOR
        NATIVESCRIPT_REACT_NATIVE
        __HERMES__
)

include_directories(
        # Rooted includes ("jsi/hermes/HermesRuntime.h") resolve from here. Note
        # this shadows an angle-include of a jsi/ header from the engine itself,
        # so the shared layer's own includes stay quoted and rooted.
        ${NS_ROOT}
        ${NS_ANDROID_RUNTIME_DIR}
        ${NS_ANDROID_RUNTIME_DIR}/instrumentation
        ${NS_ANDROID_RUNTIME_DIR}/messageloop
        ${NS_ANDROID_RUNTIME_DIR}/modules
        ${NS_ANDROID_RUNTIME_DIR}/modules/console
        ${NS_ANDROID_RUNTIME_DIR}/modules/module
        ${NS_ANDROID_RUNTIME_DIR}/modules/performance
        ${NS_ANDROID_RUNTIME_DIR}/modules/timers
        ${NS_ANDROID_RUNTIME_DIR}/profiler
        ${NS_ANDROID_RUNTIME_DIR}/sighandler
        ${NS_ANDROID_RUNTIME_DIR}/util
        ${NS_ANDROID_RUNTIME_DIR}/version
        ${NS_ANDROID_RUNTIME_DIR}/workers
        ${NS_JNI_DIR}
        ${NS_JNI_DIR}/callbackhandlers
        ${NS_JNI_DIR}/constants
        ${NS_JNI_DIR}/conversion
        ${NS_JNI_DIR}/exceptions
        ${NS_JNI_DIR}/finalizer
        ${NS_JNI_DIR}/global
        ${NS_JNI_DIR}/jni
        ${NS_JNI_DIR}/jsonhelper
        ${NS_JNI_DIR}/metadata
        ${NS_JNI_DIR}/objectmanager
        ${NS_JNI_DIR}/weakref
        # ConcurrentMap.h, robin_hood.h, spinmutex.h. The directory is named for
        # Node-API because that is what else lives in it, but these three are
        # plain containers with no Node-API in them, and the runtime uses them on
        # both binding layers.
        ${NS_ROOT}/napi/common
        ${NS_METADATA_INCLUDE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp
)

file(GLOB_RECURSE RUNTIME_SOURCES "${NS_ANDROID_RUNTIME_DIR}/*.cpp")
file(GLOB_RECURSE JNI_SOURCES "${NS_JNI_DIR}/*.cpp")
list(FILTER JNI_SOURCES EXCLUDE REGEX "_test\\.cpp$")

# The asset extractor unpacks a standalone app's app/ directory out of the APK
# with libzip. Metro owns the JavaScript here and the only asset we need is the
# metadata, which the Java side stages -- so this is the one part of the runtime
# tree the guest drops, and dropping it takes libzip with it.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/assetextractor/")

add_library(nativescript_rn SHARED
        ${RUNTIME_SOURCES}
        ${JNI_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/NativeScriptRnInstall.cpp
)

find_package(ReactAndroid REQUIRED CONFIG)
find_package(fbjni REQUIRED CONFIG)

find_library(LOG_LIB log)
find_library(ANDROID_LIB android)

# jsi and nothing else. React Native 0.76+ merges most of its C++ into
# libreactnative.so while keeping jsi separate; both names are tried so the
# module spans the versions where that consolidation happened.
if (TARGET ReactAndroid::reactnative)
    set(RN_JSI_TARGETS ReactAndroid::jsi ReactAndroid::reactnative)
else ()
    set(RN_JSI_TARGETS ReactAndroid::jsi ReactAndroid::turbomodulejsijni ReactAndroid::react_nativemodule_core)
endif ()

target_link_libraries(nativescript_rn
        ${RN_JSI_TARGETS}
        fbjni::fbjni
        ${LOG_LIB}
        ${ANDROID_LIB}
)
