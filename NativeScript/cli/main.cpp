#ifdef ENABLE_JS_RUNTIME

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "runtime/apple/NativeScriptException.h"
#include "runtime/apple/Bundle.h"
#include "runtime/apple/Runtime.h"
#include "runtime/apple/RuntimeConfig.h"
#include "segappend.h"
#include "ffi/objc/shared/Tasks.h"
#include "BundleLoader.h"

using namespace nativescript;

void bootFromBytecode(std::string baseDir, const void* data, size_t size) {
  RuntimeConfig.BaseDir = baseDir;

  auto runtime = Runtime();

  runtime.Init();

  // TODO
  // runtime.ExecuteBytecode(data, size);

  runtime.RunLoop();

  Tasks::Drain();
}

void bootFromModuleSpec(std::string baseDir, std::string spec) {
  RuntimeConfig.BaseDir = baseDir;
  RuntimeConfig.ApplicationPath = baseDir;

  // In CLI mode, map "~" imports to the directory of the entry file when
  // possible so app bundles that use "~/package.json" resolve consistently.
  std::error_code ec;
  std::filesystem::path specPath(spec);
  auto absoluteSpecPath = std::filesystem::absolute(specPath, ec);
  if (!ec && !absoluteSpecPath.empty()) {
    RuntimeConfig.ApplicationPath = absoluteSpecPath.parent_path().string();
  }

  auto runtime = Runtime();

  runtime.Init();

  try {
    runtime.RunModule(spec);
  } catch (const nativescript::NativeScriptException& e) {
    std::cerr << "Uncaught Exception: " << e.Description() << std::endl;
    std::exit(1);
  }

  runtime.RunLoop();

  Tasks::Drain();
}

int main(int argc, char** argv) {
  RuntimeConfig.LogToSystemConsole = true;
  RuntimeConfig.Arguments.clear();
  if (argv != nullptr && argc > 0) {
    RuntimeConfig.Arguments.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; i++) {
      RuntimeConfig.Arguments.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
  }

#ifdef __APPLE__
  std::string bytecodePath = getBytecodePathFromBundle();
  if (!bytecodePath.empty()) {
    std::string bundlePath = getBundlePath();

    std::ifstream file(bytecodePath, std::ios::binary);
    if (!file.is_open()) {
      std::cout << "Failed to open bytecode file" << std::endl;
      return 1;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(size);
    file.read((char*)data.data(), size);

    file.close();

    bootFromBytecode(bundlePath, data.data(), size);

    return 0;
  }
#endif  // __APPLE__

  const uint8_t* segmentData;
  size_t segmentSize;
  auto status = segappend_load_segment("__nativescript_start",
                                       (void**)&segmentData, &segmentSize);

  std::string cwd = std::filesystem::current_path().string();

  if (status == segappend_ok) {
    size_t bytecode_size = *(size_t*)segmentData;
    segmentData += sizeof(size_t);

    bootFromBytecode(cwd, segmentData, bytecode_size);
  } else {
    std::string mainPath = resolveMainPath();

    if (!mainPath.empty()) {
      bootFromModuleSpec(cwd, mainPath);
      return 0;
    }

    if (argc < 3) {
      std::cout << "Usage: " << argv[0] << " run <js file>" << std::endl;
      return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "run") {
      bootFromModuleSpec(cwd, argv[2]);
    } else {
      std::cout << "Unknown command: " << cmd << std::endl;
      return 1;
    }
  }

  return 0;
}

#endif  // ENABLE_JS_RUNTIME
