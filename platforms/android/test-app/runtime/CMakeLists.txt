# documentation: https://d.android.com/studio/projects/add-native-code.html

# Command info: https://cmake.org/cmake/help/v3.4/command/cmake_minimum_required.html
cmake_minimum_required(VERSION 3.22.1)

project(NativeScriptAndroidRuntime)

# Command info: https://cmake.org/cmake/help/v3.4/command/message.html
# we pass the android_ndk_root from gradle because for some reason
# "-DANDROID_STL=c++_static" is just not enough for clang++ to find some libraries in the ndk
MESSAGE(STATUS "## ANDROID_NDK_ROOT: " ${ANDROID_NDK_ROOT})

# Add the ccache to the build system
find_program(CCACHE_FOUND ccache)
if (CCACHE_FOUND AND (USE_CCACHE))
    MESSAGE(STATUS "## Using CCache when building!")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif ()

set(COMMON_CMAKE_ARGUMENTS "-Wno-error -Wno-deprecated-declarations -Wno-unused-result -mstackrealign -fexceptions -fno-builtin-stpcpy")
set(MI_OVERRIDE OFF)
set(MI_BUILD_OBJECT OFF)
set(MI_BUILD_SHARED OFF)
set(MI_BUILD_TESTS OFF)

if (V8_13)
    set(COMMON_CMAKE_ARGUMENTS "${COMMON_CMAKE_ARGUMENTS} -std=c++20")
else ()
    set(COMMON_CMAKE_ARGUMENTS "${COMMON_CMAKE_ARGUMENTS} -std=c++17")
endif ()

if (HERMES OR SHERMES)
    set(COMMON_CMAKE_ARGUMENTS "${COMMON_CMAKE_ARGUMENTS} -frtti")
else ()
    set(COMMON_CMAKE_ARGUMENTS "${COMMON_CMAKE_ARGUMENTS} -fno-rtti")
endif ()

# AOSP has switched to using LLD by default and the NDK will use it by default in the next release.
# BFD and Gold will be removed once LLD has been through a release cycle with no major unresolved issues (estimated r21)
# Note: lld does not currently work on Windows: https://github.com/android-ndk/ndk/issues/888

# On MacOS using LLD seems problematic as it does not add the correct path for the libNativeScript.so dSYM.
# This issue affects debugging the C++ part of the runtime.
# Manually performing "add-dsym <lib-path>" in the LLDB console seems to fix that.
# We should try using LLD again once it's the default linker for the NDK.

#if (NOT CMAKE_HOST_SYSTEM_NAME MATCHES "Windows")
#  MESSAGE(STATUS "## Using LLD linker")
#  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=lld")
#else ()
#  MESSAGE(STATUS "## Using default linker")
#endif ()

#add_library( v8_shared STATIC IMPORTED )
#set_target_properties( v8_shared PROPERTIES IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/src/main/libs/${ANDROID_ABI}/libv8android.so )

get_filename_component(REPO_ROOT "${PROJECT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(NS_ROOT "${REPO_ROOT}/NativeScript")
set(NS_RUNTIME_DIR "${NS_ROOT}/runtime")
set(NS_ANDROID_RUNTIME_DIR "${NS_RUNTIME_DIR}/android")
set(NS_RUNTIME_MODULES_DIR "${NS_RUNTIME_DIR}/modules")
set(NS_NAPI_DIR "${NS_ROOT}/napi")
set(NS_ANDROID_NAPI_DIR "${NS_NAPI_DIR}/android")
set(NS_JNI_NAPI_DIR "${NS_ROOT}/ffi/jni/napi")

# Command info: https://cmake.org/cmake/help/v3.4/command/include_directories.html
include_directories(
        # zip
        src/main/cpp/zip/include
        # runtime
        ${NS_ANDROID_RUNTIME_DIR}
        ${NS_ANDROID_RUNTIME_DIR}/assetextractor
        ${NS_ANDROID_RUNTIME_DIR}/inspector
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
        # JNI Node-API interop
        ${NS_JNI_NAPI_DIR}
        ${NS_JNI_NAPI_DIR}/callbackhandlers
        ${NS_JNI_NAPI_DIR}/constants
        ${NS_JNI_NAPI_DIR}/conversion
        ${NS_JNI_NAPI_DIR}/exceptions
        ${NS_JNI_NAPI_DIR}/global
        ${NS_JNI_NAPI_DIR}/jni
        ${NS_JNI_NAPI_DIR}/jsonhelper
        ${NS_JNI_NAPI_DIR}/metadata
        ${NS_JNI_NAPI_DIR}/objectmanager
        ${NS_JNI_NAPI_DIR}/weakref
        # shared runtime modules
        ${NS_RUNTIME_MODULES_DIR}
        ${NS_RUNTIME_MODULES_DIR}/url
        # shared Node-API headers
        ${NS_NAPI_DIR}/common
)

# Search for all runtime and JNI Node-API interop sources outside platforms/android.
file(GLOB_RECURSE ANDROID_RUNTIME_FILES
        "${NS_ANDROID_RUNTIME_DIR}/*.cpp"
)

file(GLOB_RECURSE JNI_NAPI_FILES
        "${NS_JNI_NAPI_DIR}/*.cpp"
)

set(MODULE_FILES
        ${NS_RUNTIME_MODULES_DIR}/url/URL.cpp
        ${NS_RUNTIME_MODULES_DIR}/url/URLSearchParams.cpp
        ${NS_RUNTIME_MODULES_DIR}/url/ada/ada.cpp
)

set(SOURCES ${ANDROID_RUNTIME_FILES} ${JNI_NAPI_FILES} ${MODULE_FILES})

if (QUICKJS OR QUICKJS_NG)
    add_subdirectory(${NS_NAPI_DIR}/quickjs/mimalloc-dev mimalloc)

    if (QUICKJS_NG)
        set(QJS_SOURCE_DIR ${NS_ANDROID_NAPI_DIR}/quickjs/source_ng)
    else()
        set(QJS_SOURCE_DIR ${NS_ANDROID_NAPI_DIR}/quickjs/source)
    endif()

    set(SOURCES ${SOURCES}
            # quickjs
            ${QJS_SOURCE_DIR}/cutils.c
            ${QJS_SOURCE_DIR}/libregexp.c
            ${QJS_SOURCE_DIR}/libunicode.c
            ${QJS_SOURCE_DIR}/quickjs.c
            ${QJS_SOURCE_DIR}/dtoa.c
            # napi
            ${NS_ANDROID_NAPI_DIR}/quickjs/quickjs-api.c
            ${NS_ANDROID_NAPI_DIR}/quickjs/jsr.cpp


    )

    include_directories(
            ${NS_ANDROID_NAPI_DIR}/quickjs
            ${NS_NAPI_DIR}/quickjs
            ${QJS_SOURCE_DIR}
            ${NS_NAPI_DIR}/common
            # mimalloc
            ${NS_NAPI_DIR}/quickjs/mimalloc-dev/include
    )

endif ()


if (PRIMJS)
    set(SOURCES ${SOURCES}
            ${NS_ANDROID_NAPI_DIR}/primjs/jsr.cpp
            ${NS_ANDROID_NAPI_DIR}/primjs/code_cache.cc
            ${NS_ANDROID_NAPI_DIR}/primjs/primjs-api.cc
            ${NS_ANDROID_NAPI_DIR}/primjs/napi_env.cc
    )
    include_directories(
            ${NS_ANDROID_NAPI_DIR}/primjs
            ${NS_ANDROID_NAPI_DIR}/primjs/include
            ${NS_NAPI_DIR}/common
    )

endif ()

if (HERMES OR SHERMES)
    include_directories(
            ${NS_NAPI_DIR}/hermes
            ${NS_NAPI_DIR}/hermes/include
            ${NS_NAPI_DIR}/common
    )
    set(SOURCES ${SOURCES}
            ${NS_NAPI_DIR}/hermes/jsr.cpp
    )
endif ()

if (JSC)
    include_directories(
            ${NS_ANDROID_NAPI_DIR}/jsc
            ${NS_NAPI_DIR}/jsc
            ${NS_ANDROID_NAPI_DIR}/jsc/include
            ${NS_NAPI_DIR}/common
    )
    set(SOURCES ${SOURCES}
            ${NS_ANDROID_NAPI_DIR}/jsc/jsc-api.cpp
            ${NS_ANDROID_NAPI_DIR}/jsc/jsr.cpp
    )
endif ()

if (V8)
    if (V8_10)
        include_directories(
                ${NS_ANDROID_NAPI_DIR}/v8
                ${NS_ANDROID_NAPI_DIR}/v8-10
                ${NS_ANDROID_NAPI_DIR}/v8-10/include
                ${NS_NAPI_DIR}/common
                ${NS_ANDROID_NAPI_DIR}/v8/v8_inspector
                ${NS_ANDROID_NAPI_DIR}/v8-10/v8_inspector
        )
    elseif (V8_11)
        include_directories(
                ${NS_ANDROID_NAPI_DIR}/v8
                ${NS_ANDROID_NAPI_DIR}/v8-11
                ${NS_ANDROID_NAPI_DIR}/v8-11/include
                ${NS_NAPI_DIR}/common
                ${NS_ANDROID_NAPI_DIR}/v8/v8_inspector
                ${NS_ANDROID_NAPI_DIR}/v8-11/v8_inspector
        )
    else ()
        include_directories(
                ${NS_ANDROID_NAPI_DIR}/v8
                ${NS_ANDROID_NAPI_DIR}/v8-13
                ${NS_ANDROID_NAPI_DIR}/v8-13/include
                ${NS_NAPI_DIR}/common
                ${NS_ANDROID_NAPI_DIR}/v8/v8_inspector
                ${NS_ANDROID_NAPI_DIR}/v8-13/v8_inspector
        )
    endif ()

    if (NOT OPTIMIZED_BUILD OR OPTIMIZED_WITH_INSPECTOR_BUILD)
        add_definitions(-DAPPLICATION_IN_DEBUG)
        set(
                SOURCES
                ${SOURCES}
                ${NS_ANDROID_NAPI_DIR}/v8/v8_inspector/Utils.cpp
                ${NS_ANDROID_NAPI_DIR}/v8/v8_inspector/ns-v8-tracing-agent-impl.cpp
        )
    endif ()

    set(SOURCES ${SOURCES}
            ${NS_ANDROID_NAPI_DIR}/v8/v8-api.cpp
            ${NS_ANDROID_NAPI_DIR}/v8/jsr.cpp
            ${NS_NAPI_DIR}/v8/SimpleAllocator.cpp
    )


    if (V8_10)
        set(COMMON_CMAKE_ARGUMENTS "${COMMON_CMAKE_ARGUMENTS} -DV8_31BIT_SMIS_ON_64BIT_ARCH -DV8_31BIT_SMIS_ON_64BIT_ARCH -DV8_ENABLE_REGEXP_INTERPRETER_THREADED_DISPATCH -DV8_EMBEDDED_BUILTINS")
        # Enable pointer compression on 64 bit platforms
        if ("${ANDROID_ABI}" MATCHES "arm64-v8a$" OR "${ANDROID_ABI}" MATCHES "x86_64$")
            set(COMMON_CMAKE_ARGUMENTS "${COMMON_CMAKE_ARGUMENTS} -DV8_COMPRESS_POINTERS")
        endif ()
    else ()
        set(COMMON_CMAKE_ARGUMENTS "${COMMON_CMAKE_ARGUMENTS} -DV8_ENABLE_REGEXP_INTERPRETER_THREADED_DISPATCH -DV8_EMBEDDED_BUILTINS")
    endif ()


endif ()


if (OPTIMIZED_BUILD OR OPTIMIZED_WITH_INSPECTOR_BUILD)
    set(CMAKE_CXX_FLAGS "${COMMON_CMAKE_ARGUMENTS} -O3 -flto -fvisibility=hidden -ffunction-sections -fno-data-sections")
else ()
    set(CMAKE_CXX_FLAGS "${COMMON_CMAKE_ARGUMENTS} -g")
endif ()


# Command info: https://cmake.org/cmake/help/v3.4/command/add_library.html
# Creates(shared static) and names a library given relative sources
# Gradle automatically packages shared libraries with your APK.
add_library(
        # Sets the name of the library. When it's built you can find it with lib prefix libNativeScript.so
        NativeScript

        # Sets the library as a shared library.
        SHARED

        ${SOURCES}
)

if (OPTIMIZED_BUILD OR OPTIMIZED_WITH_INSPECTOR_BUILD)
    set_target_properties(
            NativeScript
            PROPERTIES LINK_FLAGS -Wl,--allow-multiple-definition -Wl,--exclude-libs=ALL -Wl,--gc-sections
    )
else ()
    set_target_properties(
            NativeScript
            PROPERTIES LINK_FLAGS -Wl,--allow-multiple-definition
    )
    #    target_compile_options(NativeScript PUBLIC -fsanitize=hwaddress -fno-omit-frame-pointer)
    #    target_link_options(NativeScript PUBLIC -fsanitize=hwaddress)
endif ()

MESSAGE(STATUS "# General cmake Info")
MESSAGE(STATUS "# PROJECT_SOURCE_DIR: " ${PROJECT_SOURCE_DIR})
MESSAGE(STATUS "# CMAKE_VERSION: " ${CMAKE_VERSION})
MESSAGE(STATUS "# CMAKE_C_COMPILER_ID: " ${CMAKE_C_COMPILER_ID})
MESSAGE(STATUS "# CMAKE_CXX_COMPILER_ID: " ${CMAKE_CXX_COMPILER_ID})
MESSAGE(STATUS "# CMAKE_C_FLAGS: " ${CMAKE_C_FLAGS})
MESSAGE(STATUS "# CMAKE_CXX_FLAGS: " ${CMAKE_CXX_FLAGS})

# Command info: https://cmake.org/cmake/help/v3.4/command/target_link_libraries.html
# linking custom STL libraries to the runtime (NativeScript library)
target_link_libraries(NativeScript ${PROJECT_SOURCE_DIR}/src/main/libs/common/${ANDROID_ABI}/libzip.a)
#target_link_libraries(NativeScript ${PROJECT_SOURCE_DIR}/src/main/libs/common/${ANDROID_ABI}/libclang_rt.asan-aarch64-android.so)

if (HERMES OR SHERMES)
    target_link_libraries(NativeScript ${PROJECT_SOURCE_DIR}/src/main/libs/hermes/${ANDROID_ABI}/libhermesvm.so)
    target_compile_definitions(NativeScript PRIVATE __HERMES__ NS_HERMES_SKIP_ENV_CLEANUP_HOOKS)
endif ()

if (JSC)
    target_link_libraries(NativeScript ${PROJECT_SOURCE_DIR}/src/main/libs/jsc/${ANDROID_ABI}/libjsc.so)
    target_compile_definitions(NativeScript PRIVATE __JSC__)
endif ()

if (V8)
    target_compile_definitions(NativeScript PRIVATE __V8__)
endif ()

if (V8_10)
    target_link_libraries(NativeScript ${PROJECT_SOURCE_DIR}/src/main/libs/v8-10/${ANDROID_ABI}/libv8_monolith.a)
    target_compile_definitions(NativeScript PRIVATE __V8_10__)
endif ()

if (V8_11)
    target_link_libraries(NativeScript ${PROJECT_SOURCE_DIR}/src/main/libs/v8-11/${ANDROID_ABI}/libv8_monolith.a)
    target_compile_definitions(NativeScript PRIVATE __V8_11__)
endif ()

if (V8_13)
    target_link_libraries(NativeScript ${PROJECT_SOURCE_DIR}/src/main/libs/v8-13/${ANDROID_ABI}/libv8_monolith.a)
    target_compile_definitions(NativeScript PRIVATE __V8_13__)
endif ()

if (PRIMJS)
    # target_link_libraries(NativeScript ${PROJECT_SOURCE_DIR}/src/main/libs/primjs/${ANDROID_ABI}/libnapi.so)
    target_link_libraries(NativeScript ${PROJECT_SOURCE_DIR}/src/main/libs/primjs/${ANDROID_ABI}/libquick.so)
    target_compile_definitions(NativeScript PRIVATE __PRIMJS__)
endif ()

if (QUICKJS OR QUICKJS_NG)
    target_compile_definitions(NativeScript PRIVATE __QJS__)
    if (QUICKJS_NG)
        target_compile_definitions(NativeScript PRIVATE __QJS_NG__)
    endif ()
    if (USE_MIMALLOC)
        target_compile_definitions(NativeScript PRIVATE USE_MIMALLOC)
    endif ()
endif ()

if (USE_HOST_OBJECTS)
    target_compile_definitions(NativeScript PRIVATE USE_HOST_OBJECT)
endif ()

if (IS_NAPI_MODULE)
    target_compile_definitions(NativeScript PRIVATE IS_NAPI_MODULE)
endif ()

# if("${ANDROID_ABI}" MATCHES "armeabi-v7a$" OR "${ANDROID_ABI}" MATCHES "x86$")
#     # On API Level 19 and lower we need to link with android_support
#     # because it contains some implementation of functions such as "strtoll" and "strtoul"
#     MESSAGE(STATUS "# Linking with libandroid_support.a")
#     target_link_libraries(NativeScript ${ANDROID_NDK_ROOT}/sources/cxx-stl/llvm-libc++/libs/${ANDROID_ABI}/libandroid_support.a)
# endif()

# Command info: https://cmake.org/cmake/help/v3.4/command/find_library.html
# Searches for a specified prebuilt library and stores the path as a
# variable. Because CMake includes system libraries in the search path by
# default, you only need to specify the name of the public NDK library
# you want to add. CMake verifies that the library exists before
# completing its build.
find_library(system-log log)
find_library(system-z z)
find_library(system-android android)

# Command info: https://cmake.org/cmake/help/v3.4/command/target_link_libraries.html
# Specifies libraries CMake should link to your target library.
if (QUICKJS)
    if (USE_MIMALLOC)
        target_link_libraries(NativeScript ${system-log} ${system-z} ${system-android} mimalloc-static)
    else ()
        target_link_libraries(NativeScript ${system-log} ${system-z} ${system-android})
    endif ()
elseif (HERMES OR SHERMES)
    target_link_libraries(NativeScript ${system-log} ${system-z} ${system-android})
elseif (JSC OR V8 OR PRIMJS)
    target_link_libraries(NativeScript ${system-log} ${system-z} ${system-android})
endif ()
