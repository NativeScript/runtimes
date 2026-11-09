cmake_minimum_required(VERSION 3.15)

set(NAME objc-metadata-generator)
set(VERSION 0.1.0)

project(${NAME} VERSION ${VERSION} LANGUAGES OBJC CXX)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -target ${METADATA_BINARY_ARCH}-apple-darwin -fno-rtti")

set(CMAKE_CXX_STANDARD 20)

option(METADATA_GENERATOR_BUILD_TESTS "Build metadata generator unit tests" OFF)

if(NOT DEFINED METADATA_BINARY_ARCH)
  set(METADATA_BINARY_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif(NOT DEFINED METADATA_BINARY_ARCH)

# set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(EXEC_SOURCE_FILES
  src/main.cpp
  src/SignatureDispatchEmitter.cpp
  src/SignatureDispatchEmitter/Napi.cpp
  src/SignatureDispatchEmitter/Prepared.cpp
  src/SignatureDispatchEmitter/Shared.cpp
  src/SignatureDispatchEmitter/Gsd.cpp
  src/Umbrella.cpp
  src/IR/Category.cpp
  src/IR/Class.cpp
  src/IR/Enum.cpp
  src/IR/Factory.cpp
  src/IR/Function.cpp
  src/IR/Member.cpp
  src/IR/Protocol.cpp
  src/IR/Record.cpp
  src/IR/TypeSpec.cpp
  src/IR/Variable.cpp
  src/JSONEmitter/Emitter.cpp
  src/TSEmitter/Class.cpp
  src/TSEmitter/Emitter.cpp
  src/TSEmitter/Enum.cpp
  src/TSEmitter/Function.cpp
  src/TSEmitter/Member.cpp
  src/TSEmitter/Protocol.cpp
  src/TSEmitter/Record.cpp
  src/TSEmitter/TypeSpec.cpp
  src/TSEmitter/Variable.cpp
  src/MetadataWriter/Class.cpp
  src/MetadataWriter/Enum.cpp
  src/MetadataWriter/Function.cpp
  src/MetadataWriter/Member.cpp
  src/MetadataWriter/Protocol.cpp
  src/MetadataWriter/Record.cpp
  src/MetadataWriter/TypeSpec.cpp
  src/MetadataWriter/Variable.cpp
  src/MetadataWriter/Writer.cpp
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})


add_executable(
  ${NAME}
  ${EXEC_SOURCE_FILES}
)

set(LIBCLANG_DIR "/Library/Developer/CommandLineTools/usr/lib")
execute_process(
  COMMAND xcrun --find clang
  OUTPUT_VARIABLE XCRUN_CLANG
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(XCRUN_CLANG)
  get_filename_component(XCRUN_CLANG_BIN_DIR "${XCRUN_CLANG}" DIRECTORY)
  get_filename_component(XCRUN_TOOLCHAIN_USR_DIR "${XCRUN_CLANG_BIN_DIR}" DIRECTORY)
  if(EXISTS "${XCRUN_TOOLCHAIN_USR_DIR}/lib/libclang.dylib")
    set(LIBCLANG_DIR "${XCRUN_TOOLCHAIN_USR_DIR}/lib")
  endif()
endif()
message(STATUS "LIBCLANG_DIR = ${LIBCLANG_DIR}")

target_link_directories(
  ${NAME}
  PRIVATE
  "${LIBCLANG_DIR}"
)

target_link_libraries(
  ${NAME}
  PRIVATE
  clang
)

# Set runtime path for libclang
set_target_properties(${NAME} PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "${LIBCLANG_DIR}"
)

install(TARGETS ${NAME}
  RUNTIME DESTINATION bin)

configure_file(build-step-metadata-generator.py ${CMAKE_CURRENT_BINARY_DIR}/bin/build-step-metadata-generator.py COPYONLY)

if(METADATA_GENERATOR_BUILD_TESTS)
  enable_testing()
  add_executable(metadata-filter-tests tests/MetadataFilterTests.cpp)
  target_include_directories(metadata-filter-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(NAME metadata-filter-tests COMMAND metadata-filter-tests)
endif()
