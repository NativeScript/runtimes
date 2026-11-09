#include <clang-c/Index.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "IR.h"
#include "JSONEmitter.h"
#include "Metadata.h"
#include "MetadataWriter.h"
#include "SignatureDispatchEmitter.h"
#include "TSEmitter.h"
#include "Umbrella.h"

using namespace metagen;

int main(int argc, char** argv) {
  std::string sdk;
  std::string frameworksDir;

  std::string code;

  std::vector<std::string> args = {"-v",
                                   "-x",
                                   "objective-c",
                                   "-fno-objc-arc",
                                   "-fmodule-maps",
                                   "-ferror-limit=0",
                                   "-Wno-unknown-pragmas",
                                   "-Wno-ignored-attributes",
                                   "-Wno-nullability-completeness",
                                   "-Wno-expansion-to-defined",
                                   "-Wno-deprecated-declarations",
                                   "-Wno-objc-property-no-attribute",
                                   "-std=gnu11",
                                   "-D__NATIVESCRIPT_METADATA_GENERATOR=1"};
  args.reserve(args.size() + static_cast<size_t>(argc) * 2);

  std::unordered_set<std::string> includePaths;
  includePaths.reserve(static_cast<size_t>(argc));

  bool Xclang = false;
  // bool verbose = false;
  // bool strictIncludes = false;
  std::string umbrellaHeaderName = "umbrella.h";
  std::string inputUmbrellaHeaderName = "";
  std::string outputYamlFolder;
  std::string outputModuleMapsFolder;
  std::string outputBinFile;
  std::string outputSignatureBindingsCppFile;
  std::string outputDtsFolder;
  std::string outputJsonFolder;
  std::string docSetFile;
  std::string blacklistModulesFile;
  std::string whitelistModulesFile;
  std::string tsIndexMode = "all";
  std::vector<std::string> tsIndexFrameworks;
  // bool applyManualDtsChanges = true;

  std::cerr << "MetadataGenerator called with args: ";

  for (int i = 1; i < argc; i++) {
    std::cerr << argv[i] << " ";
  }

  std::cerr << std::endl;

  auto addFramework = [&](const std::string& framework) {
    if (frameworksDir.empty()) {
      std::cerr << "framework= argument must be specified after sdk="
                << std::endl;
      std::exit(1);
    }

    std::string includePath = frameworksDir + "/" + framework + ".framework";
    includePaths.emplace(includePath);
    args.emplace_back("-I" + includePath + "/Headers");
    code += "#import <" + framework + "/" + framework + ".h>\n";
  };

  for (int i = 1; i < argc; i++) {
    std::string arg(argv[i]);

    // clang arguments following Xclang delim
    if (arg == "Xclang") {
      Xclang = true;
      continue;
    } else if (Xclang) {
      args.emplace_back(std::move(arg));
      continue;
    }

    if (arg.find("-verbose") == 0) {
      // verbose = true;
    } else if (arg.find("-apply-manual-dts-changes") == 0) {
      // TODO: we don't really need it
      // applyManualDtsChanges = true;
    } else if (arg == "-strict-includes") {
      // strictIncludes = true;
    } else if (arg == "-output-umbrella") {
      umbrellaHeaderName = argv[++i];
    } else if (arg == "-input-umbrella") {
      inputUmbrellaHeaderName = argv[++i];
    } else if (arg == "-output-yaml") {
      outputYamlFolder = argv[++i];
    } else if (arg == "-output-module-maps") {
      outputModuleMapsFolder = argv[++i];
    } else if (arg == "-output-bin") {
      outputBinFile = argv[++i];
    } else if (arg == "-output-signature-bindings-cpp") {
      outputSignatureBindingsCppFile = argv[++i];
    } else if (arg == "-output-dts" || arg == "-output-typescript") {
      outputDtsFolder = argv[++i];
    } else if (arg == "-output-json") {
      outputJsonFolder = argv[++i];
    } else if (arg == "-docset-path") {
      docSetFile = argv[++i];
    } else if (arg == "-blacklist-modules" ||
               arg == "--blacklist-modules-file") {
      blacklistModulesFile = argv[++i];
    } else if (arg == "-whitelist-modules" ||
               arg == "--whitelist-modules-file") {
      whitelistModulesFile = argv[++i];
    } else if (arg.find("framework=") == 0) {
      addFramework(arg.substr(10));
    } else if (arg.find("include=") == 0) {
      includePaths.emplace(arg.substr(8));
    } else if (arg.find("headers=") == 0) {
      args.emplace_back("-I" + arg.substr(8));
    } else if (arg.find("import=") == 0) {
      code += "#import " + arg.substr(7) + "\n";
    } else if (arg.find("sdk=") == 0) {
      sdk = arg.substr(4);
      args[2] = sdk;
      args.emplace_back("-I" + sdk + "/usr/include");
      frameworksDir = sdk + "/System/Library/Frameworks";
      args.emplace_back("-F" + frameworksDir);
    } else if (arg.find("target=") == 0) {
      args.emplace_back("-target");
      args.emplace_back(arg.substr(7));
    } else if (arg.find("arg=") == 0) {
      args.emplace_back(arg.substr(4));
    } else if (arg.find("output=") == 0) {
      outputBinFile = arg.substr(7);
    } else if (arg.find("types=") == 0) {
      outputDtsFolder = arg.substr(6);
    } else if (arg.find("json=") == 0) {
      outputJsonFolder = arg.substr(5);
    } else if (arg.find("ts-index-mode=") == 0) {
      tsIndexMode = arg.substr(14);
    } else if (arg.find("ts-index-frameworks=") == 0) {
      std::string list = arg.substr(20);
      std::stringstream ss(list);
      std::string item;
      while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
          tsIndexFrameworks.push_back(item);
        }
      }
    } else if (arg.find("arch=") == 0) {
      std::string arch = arg.substr(5);
      args.emplace_back("-arch");
      args.emplace_back(arch);
    } else {
      std::cerr << "Unknown argument: " << arg << std::endl;
      std::exit(1);
    }
  }

  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "-target") {
      setAvailabilityTargetTriple(args[i + 1]);
      std::cerr << "Availability target triple: " << args[i + 1] << std::endl;
      break;
    }
  }

  // Use automatic umbrella header generation if manual one is empty
  if (code.empty()) {
    std::vector<std::string> includePathsInner, frameworksInner;
    code = CreateUmbrellaHeader(args, includePathsInner, frameworksInner);
    for (const auto& includePath : includePathsInner) {
      std::cerr << "Adding include path: " << includePath << std::endl;
      args.emplace_back("-idirafter" + includePath);
      includePaths.emplace(includePath);
    }
    for (const auto& framework : frameworksInner) {
      std::cerr << "Adding framework: " << framework << std::endl;
      args.emplace_back("-framework");
      args.emplace_back(framework);
    }
  }

  std::error_code umbrellaDirError;
  std::filesystem::path umbrellaPath(umbrellaHeaderName);
  if (!umbrellaPath.parent_path().empty()) {
    std::filesystem::create_directories(umbrellaPath.parent_path(),
                                        umbrellaDirError);
    if (umbrellaDirError) {
      std::cerr << "Failed to create umbrella output directory: "
                << umbrellaPath.parent_path() << " ("
                << umbrellaDirError.message() << ")" << std::endl;
      std::exit(1);
    }
  }

  std::ofstream umbrellaHeader(
      umbrellaHeaderName, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!umbrellaHeader.is_open()) {
    std::cerr << "Failed to open umbrella header for writing: "
              << umbrellaHeaderName << " (" << std::strerror(errno) << ")"
              << std::endl;
    std::exit(1);
  }

  umbrellaHeader.write(code.data(), static_cast<std::streamsize>(code.size()));
  if (!umbrellaHeader) {
    std::cerr << "Failed to write umbrella header: " << umbrellaHeaderName
              << std::endl;
    std::exit(1);
  }
  umbrellaHeader.close();

  std::vector<const char*> argsC(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    argsC[i] = args[i].c_str();
  }

  CXIndex index = clang_createIndex(0, 0);
  CXTranslationUnit unit = clang_parseTranslationUnit(
      index, umbrellaHeaderName.c_str(), argsC.data(),
      (MDSectionOffset)argsC.size(), nullptr, 0,
      CXTranslationUnit_SkipFunctionBodies);

  // std::remove(umbrellaHeaderName.c_str());

  if (unit == nullptr) {
    std::cerr << "Failed to parse translation unit" << std::endl;
    std::exit(1);
  }

  // print diagnostics
  unsigned numDiagnostics = clang_getNumDiagnostics(unit);

  for (unsigned i = 0; i < numDiagnostics; ++i) {
    CXDiagnostic diagnostic = clang_getDiagnostic(unit, i);
    CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
    if (severity != CXDiagnostic_Error && severity != CXDiagnostic_Fatal) {
      continue;
    }
    CXString string = clang_formatDiagnostic(
        diagnostic,
        CXDiagnostic_DisplayCategoryId | CXDiagnostic_DisplaySourceLocation |
            CXDiagnostic_DisplayColumn | CXDiagnostic_DisplaySourceRanges |
            CXDiagnostic_DisplayOption);
    std::cerr << clang_getCString(string) << std::endl;
    clang_disposeString(string);
  }

  CXCursor cursor = clang_getTranslationUnitCursor(unit);

  // Must be set before IR construction — availability is captured while
  // cursors are alive, not at emit time.
  gCaptureAvailability = !outputJsonFolder.empty();

  MetadataFactory factory;
  factory.includePaths = includePaths;
  factory.metadataFilter.configure(whitelistModulesFile, blacklistModulesFile);
  factory.process(cursor);
  factory.postProcess();

  clang_disposeTranslationUnit(unit);
  clang_disposeIndex(index);

  if (!outputDtsFolder.empty()) {
    if (tsIndexMode == "frameworks-list" && tsIndexFrameworks.empty()) {
      std::cerr << "ts-index-mode=frameworks-list requires ts-index-frameworks"
                << std::endl;
      std::exit(1);
    }
    TSEmitter::Options tsOptions;
    tsOptions.indexMode = tsIndexMode;
    tsOptions.indexFrameworks = tsIndexFrameworks;
    TSEmitter ts(factory, outputDtsFolder, tsOptions);
    ts.write();
  }

  if (!outputJsonFolder.empty()) {
    JSONEmitter json(factory, outputJsonFolder);
    json.write();
  }

  if (!outputBinFile.empty()) {
    MDMetadataWriter writer(factory);
    writer.write();

    if (!outputSignatureBindingsCppFile.empty()) {
      writeSignatureDispatchBindings(writer, outputSignatureBindingsCppFile);
    }

    auto result = writer.serialize();

    std::cerr << " --- serialization stats ---" << std::endl;
    std::cerr << "           total size: " << result.second / 1024. / 1024.
              << " MB" << std::endl;
    std::cerr << "       total sections: " << MD_NUM_SECTIONS << std::endl;
    std::cerr << "    strings (n, size): " << writer.strings.size() << ", "
              << writer.strings.section_size / 1024. << " KB" << std::endl;
    std::cerr << "     consts (n, size): " << writer.constants.size() << ","
              << writer.constants.section_size / 1024. << " KB" << std::endl;
    std::cerr << "      enums (n, size): " << writer.enums.size() << ", "
              << writer.enums.section_size / 1024. << " KB" << std::endl;
    std::cerr << " signatures (n, size): " << writer.signatures.size() << ", "
              << writer.signatures.section_size / 1024. << " KB" << std::endl;
    std::cerr << "  functions (n, size): " << writer.functions.size() << ", "
              << writer.functions.section_size / 1024. << " KB" << std::endl;
    std::cerr << "  protocols (n, size): " << writer.protocols.size() << ", "
              << writer.protocols.section_size / 1024. << " KB" << std::endl;
    std::cerr << "    structs (n, size): " << writer.structs.size() << ", "
              << writer.structs.section_size / 1024. << " KB" << std::endl;
    std::cerr << "    classes (n, size): " << writer.classes.size() << ", "
              << writer.classes.section_size / 1024. << " KB" << std::endl;
    std::cerr << "     unions (n, size): " << writer.unions.size() << ", "
              << writer.unions.section_size / 1024. << " KB" << std::endl;

    auto file = std::fopen(outputBinFile.c_str(), "w");
    std::fwrite(result.first, 1, result.second, file);
    std::fclose(file);
  }

  return 0;
}
