#include "SignatureDispatchEmitter.h"
#include "SignatureDispatchEmitter/Shared.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace metagen {

using namespace signature_dispatch;

void writeSignatureDispatchBindings(const MDMetadataWriter& writer,
                                    const std::string& outputPath) {
  if (outputPath.empty()) {
    return;
  }

  std::vector<SignatureUse> signatureUses;

  for (const auto& pair : writer.functions.orderedEntries) {
    auto* function = pair.second;
    if (function == nullptr || function->signature == MD_SECTION_OFFSET_NULL) {
      continue;
    }
    const uint8_t flags =
        (function->flags & mdFunctionReturnOwned) != 0 ? 1 : 0;
    signatureUses.push_back(
        {DispatchKind::CFunction, function->signature, flags});
  }

  for (const auto& pair : writer.classes.orderedEntries) {
    auto* cls = pair.second;
    if (cls == nullptr) {
      continue;
    }
    collectMethodUses(cls->members, &signatureUses);
  }

  for (const auto& pair : writer.protocols.orderedEntries) {
    auto* protocol = pair.second;
    if (protocol == nullptr) {
      continue;
    }
    collectMethodUses(protocol->members, &signatureUses);
  }

  const auto rootSignatureUses = signatureUses;
  std::unordered_set<MDSectionOffset> activeBlockSignatures;
  for (const auto& use : rootSignatureUses) {
    collectBlockUsesFromSignature(use.signatureOffset, writer.signatures,
                                  &activeBlockSignatures, &signatureUses);
  }

  std::unordered_map<std::string, std::pair<DispatchKind, const MDSignature*>>
      wrappersByKey;
  std::unordered_map<std::string, std::pair<DispatchKind, const MDSignature*>>
      preparedWrappersByKey;
  // Engine-neutral GSD wrappers keyed by signature shape.
  std::unordered_map<std::string, const MDSignature*> gsdWrappersByKey;
  std::unordered_map<uint64_t, std::string> objcPreparedEntries;
  std::unordered_map<uint64_t, std::string> cFunctionPreparedEntries;
  std::unordered_map<uint64_t, std::string> blockPreparedEntries;
  std::unordered_map<uint64_t, std::string> objcNapiEntries;
  std::unordered_map<uint64_t, std::string> cFunctionNapiEntries;
  std::unordered_map<uint64_t, std::string> objcGsdEntries;
  std::unordered_map<uint64_t, std::string> dispatchEncoding;
  std::unordered_set<uint64_t> collidedDispatchIds;

  for (const auto& use : signatureUses) {
    auto signatureIt = writer.signatures.find(use.signatureOffset);
    if (signatureIt == writer.signatures.end()) {
      continue;
    }

    const MDSignature* signature = signatureIt->second;
    if (!isSignatureSupported(signature)) {
      continue;
    }

    std::string canonicalSignatureKey;
    const uint64_t sigHash =
        signatureHash(signature, use.signatureOffset, writer.signatures,
                      &canonicalSignatureKey);
    if (sigHash == 0) {
      continue;
    }
    const uint64_t dispatchId = composeDispatchId(sigHash, use.kind, use.flags);

    auto encodedIt = dispatchEncoding.find(dispatchId);
    if (encodedIt != dispatchEncoding.end() &&
        encodedIt->second != canonicalSignatureKey) {
      collidedDispatchIds.insert(dispatchId);
      objcPreparedEntries.erase(dispatchId);
      cFunctionPreparedEntries.erase(dispatchId);
      blockPreparedEntries.erase(dispatchId);
      objcNapiEntries.erase(dispatchId);
      cFunctionNapiEntries.erase(dispatchId);
      objcGsdEntries.erase(dispatchId);
      dispatchEncoding.erase(dispatchId);
      continue;
    }
    if (collidedDispatchIds.find(dispatchId) != collidedDispatchIds.end()) {
      continue;
    }
    dispatchEncoding.emplace(dispatchId, canonicalSignatureKey);

    const std::string wrapperKey = makeWrapperShapeKey(use.kind, signature);
    if (wrapperKey.empty()) {
      continue;
    }

    if (use.kind == DispatchKind::ObjCMethod) {
      wrappersByKey.emplace(wrapperKey, std::make_pair(use.kind, signature));
      preparedWrappersByKey.emplace(wrapperKey,
                                    std::make_pair(use.kind, signature));
      objcPreparedEntries.emplace(dispatchId, wrapperKey);
      objcNapiEntries.emplace(dispatchId, wrapperKey);
      // Engine-neutral GSD invokers for supported signatures.
      if (isGsdSignatureSupported(signature)) {
        const std::string gsdKey = makeGsdWrapperShapeKey(signature);
        if (!gsdKey.empty()) {
          gsdWrappersByKey.emplace(gsdKey, signature);
          objcGsdEntries.emplace(dispatchId, gsdKey);
        }
      }
    } else if (use.kind == DispatchKind::CFunction) {
      wrappersByKey.emplace(wrapperKey, std::make_pair(use.kind, signature));
      preparedWrappersByKey.emplace(wrapperKey,
                                    std::make_pair(use.kind, signature));
      cFunctionPreparedEntries.emplace(dispatchId, wrapperKey);
      cFunctionNapiEntries.emplace(dispatchId, wrapperKey);
    } else if (use.kind == DispatchKind::BlockInvoke) {
      preparedWrappersByKey.emplace(wrapperKey,
                                    std::make_pair(use.kind, signature));
      blockPreparedEntries.emplace(dispatchId, wrapperKey);
    }
  }

  std::vector<
      std::pair<std::string, std::pair<DispatchKind, const MDSignature*>>>
      wrappers(wrappersByKey.begin(), wrappersByKey.end());
  std::sort(
      wrappers.begin(), wrappers.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  std::vector<
      std::pair<std::string, std::pair<DispatchKind, const MDSignature*>>>
      preparedWrappers(preparedWrappersByKey.begin(),
                       preparedWrappersByKey.end());
  std::sort(
      preparedWrappers.begin(), preparedWrappers.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  std::unordered_map<std::string, std::string> wrapperNameByKey;
  wrapperNameByKey.reserve(wrappers.size());
  size_t wrapperIndex = 0;
  for (const auto& wrapper : wrappers) {
    wrapperNameByKey.emplace(
        wrapper.first,
        makeNapiWrapperName(wrapper.second.first, wrapperIndex++));
  }

  std::unordered_map<std::string, std::string> preparedWrapperNameByKey;
  preparedWrapperNameByKey.reserve(preparedWrappers.size());
  size_t preparedWrapperIndex = 0;
  for (const auto& wrapper : preparedWrappers) {
    preparedWrapperNameByKey.emplace(
        wrapper.first,
        makePreparedWrapperName(wrapper.second.first, preparedWrapperIndex++));
  }

  // Engine-neutral GSD wrappers
  std::vector<std::pair<std::string, const MDSignature*>> gsdWrappers(
      gsdWrappersByKey.begin(), gsdWrappersByKey.end());
  std::sort(gsdWrappers.begin(), gsdWrappers.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.first < rhs.first;
            });

  std::unordered_map<std::string, std::string> gsdWrapperNameByKey;
  gsdWrapperNameByKey.reserve(gsdWrappers.size());
  size_t gsdWrapperIndex = 0;
  for (const auto& wrapper : gsdWrappers) {
    gsdWrapperNameByKey.emplace(wrapper.first,
                                makeGsdWrapperName(gsdWrapperIndex++));
  }

  std::vector<std::pair<uint64_t, std::string>> sortedObjCNapiEntries(
      objcNapiEntries.begin(), objcNapiEntries.end());
  std::sort(
      sortedObjCNapiEntries.begin(), sortedObjCNapiEntries.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  std::vector<std::pair<uint64_t, std::string>> sortedCFunctionNapiEntries(
      cFunctionNapiEntries.begin(), cFunctionNapiEntries.end());
  std::sort(
      sortedCFunctionNapiEntries.begin(), sortedCFunctionNapiEntries.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  std::vector<std::pair<uint64_t, std::string>> sortedObjCPreparedEntries(
      objcPreparedEntries.begin(), objcPreparedEntries.end());
  std::sort(
      sortedObjCPreparedEntries.begin(), sortedObjCPreparedEntries.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  std::vector<std::pair<uint64_t, std::string>> sortedCFunctionPreparedEntries(
      cFunctionPreparedEntries.begin(), cFunctionPreparedEntries.end());
  std::sort(sortedCFunctionPreparedEntries.begin(),
            sortedCFunctionPreparedEntries.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.first < rhs.first;
            });

  std::vector<std::pair<uint64_t, std::string>> sortedBlockPreparedEntries(
      blockPreparedEntries.begin(), blockPreparedEntries.end());
  std::sort(
      sortedBlockPreparedEntries.begin(), sortedBlockPreparedEntries.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  std::vector<std::pair<uint64_t, std::string>> sortedObjCGsdEntries(
      objcGsdEntries.begin(), objcGsdEntries.end());
  std::sort(
      sortedObjCGsdEntries.begin(), sortedObjCGsdEntries.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  std::ostringstream generated;
  generated << "#ifndef NS_GENERATED_SIGNATURE_DISPATCH_INC\n";
  generated << "#define NS_GENERATED_SIGNATURE_DISPATCH_INC\n\n";
  generated << "#if NS_GSD_BACKEND_NAPI || "
               "NS_GSD_BACKEND_HERMES || NS_GSD_BACKEND_PREPARED\n";
  generated << "#undef NS_HAS_GENERATED_SIGNATURE_DISPATCH\n";
  generated << "#define NS_HAS_GENERATED_SIGNATURE_DISPATCH 1\n";
  generated << "#endif\n";
  generated << "#if NS_GSD_BACKEND_NAPI\n";
  generated << "#undef NS_HAS_GENERATED_SIGNATURE_NAPI_DISPATCH\n";
  generated << "#define NS_HAS_GENERATED_SIGNATURE_NAPI_DISPATCH 1\n";
  generated << "#endif\n";
  generated << "namespace nativescript {\n\n";

  generated << "#if NS_GSD_BACKEND_NAPI || "
               "NS_GSD_BACKEND_HERMES || NS_GSD_BACKEND_PREPARED\n";
  for (const auto& wrapper : preparedWrappers) {
    writePreparedWrapper(generated, wrapper.second.first,
                         preparedWrapperNameByKey.at(wrapper.first),
                         wrapper.second.second);
  }
  generated << "#endif\n\n";

  generated << "#if NS_GSD_BACKEND_NAPI\n";
  for (const auto& wrapper : wrappers) {
    writeNapiWrapper(generated, wrapper.second.first,
                     wrapperNameByKey.at(wrapper.first), wrapper.second.second);
  }
  generated << "#endif\n\n";

  generated << "#if NS_GSD_BACKEND_NAPI || "
               "NS_GSD_BACKEND_HERMES || NS_GSD_BACKEND_PREPARED\n";
  generated << "inline constexpr ObjCDispatchEntry "
               "kGeneratedObjCDispatchEntries[] = {\n";
  generated << "    {0, nullptr},\n";
  for (const auto& entry : sortedObjCPreparedEntries) {
    generated << "    {" << toHexLiteral(entry.first) << ", &"
              << preparedWrapperNameByKey.at(entry.second) << "},\n";
  }
  generated << "};\n\n";

  generated << "inline constexpr CFunctionDispatchEntry "
               "kGeneratedCFunctionDispatchEntries[] = {\n";
  generated << "    {0, nullptr},\n";
  for (const auto& entry : sortedCFunctionPreparedEntries) {
    generated << "    {" << toHexLiteral(entry.first) << ", &"
              << preparedWrapperNameByKey.at(entry.second) << "},\n";
  }
  generated << "};\n\n";

  generated << "inline constexpr BlockDispatchEntry "
               "kGeneratedBlockDispatchEntries[] = {\n";
  generated << "    {0, nullptr},\n";
  for (const auto& entry : sortedBlockPreparedEntries) {
    generated << "    {" << toHexLiteral(entry.first) << ", &"
              << preparedWrapperNameByKey.at(entry.second) << "},\n";
  }
  generated << "};\n\n";
  generated << "#endif\n\n";

  generated << "#if NS_GSD_BACKEND_NAPI\n";
  generated << "inline constexpr ObjCNapiDispatchEntry "
               "kGeneratedObjCNapiDispatchEntries[] = {\n";
  generated << "    {0, nullptr},\n";
  for (const auto& entry : sortedObjCNapiEntries) {
    generated << "    {" << toHexLiteral(entry.first) << ", &"
              << wrapperNameByKey.at(entry.second) << "},\n";
  }
  generated << "};\n\n";

  generated << "inline constexpr CFunctionNapiDispatchEntry "
               "kGeneratedCFunctionNapiDispatchEntries[] = {\n";
  generated << "    {0, nullptr},\n";
  for (const auto& entry : sortedCFunctionNapiEntries) {
    generated << "    {" << toHexLiteral(entry.first) << ", &"
              << wrapperNameByKey.at(entry.second) << "},\n";
  }
  generated << "};\n\n";
  generated << "#endif\n\n";

  generated << "}  // namespace nativescript\n\n";
  generated << "#endif  // NS_GENERATED_SIGNATURE_DISPATCH_INC\n";

  std::ofstream outFile(outputPath, std::ios::trunc | std::ios::binary);
  outFile << generated.str();

  // Write engine-neutral GSD invokers + dispatch table to a separate file.
  // It is included from each engine backend after that engine's
  // GsdObjCContext struct is defined (avoiding namespace ordering issues).
  // The generated code only references the engine-neutral GsdObjCContext
  // interface, so the same file compiles unchanged in every backend.
  if (!sortedObjCGsdEntries.empty()) {
    std::string gsdOutputPath = outputPath;
    auto lastSlash = gsdOutputPath.rfind('/');
    if (lastSlash != std::string::npos) {
      gsdOutputPath = gsdOutputPath.substr(0, lastSlash + 1) +
                      "GeneratedGsdSignatureDispatch.inc";
    } else {
      gsdOutputPath = "GeneratedGsdSignatureDispatch.inc";
    }

    std::ostringstream gsdGenerated;
    gsdGenerated << "#ifndef NS_GENERATED_GSD_SIGNATURE_DISPATCH_INC\n";
    gsdGenerated << "#define NS_GENERATED_GSD_SIGNATURE_DISPATCH_INC\n\n";
    gsdGenerated << "#undef NS_HAS_GENERATED_SIGNATURE_GSD_DISPATCH\n";
    gsdGenerated << "#define NS_HAS_GENERATED_SIGNATURE_GSD_DISPATCH 1\n\n";
    gsdGenerated << "#include <objc/message.h>\n\n";
    // No namespace wrapper — included from within namespace nativescript in
    // each engine backend, after GsdObjCContext is defined.

    for (const auto& wrapper : gsdWrappers) {
      writeGsdWrapper(gsdGenerated, gsdWrapperNameByKey.at(wrapper.first),
                      wrapper.second);
    }

    gsdGenerated << "inline constexpr ObjCGsdDispatchEntry "
                    "kGeneratedObjCGsdDispatchEntries[] = {\n";
    gsdGenerated << "    {0, nullptr},\n";
    for (const auto& entry : sortedObjCGsdEntries) {
      gsdGenerated << "    {" << toHexLiteral(entry.first) << ", &"
                   << gsdWrapperNameByKey.at(entry.second) << "},\n";
    }
    gsdGenerated << "};\n\n";

    gsdGenerated << "#endif  // NS_GENERATED_GSD_SIGNATURE_DISPATCH_INC\n";

    std::ofstream gsdOutFile(gsdOutputPath, std::ios::trunc | std::ios::binary);
    gsdOutFile << gsdGenerated.str();
  }
}

}  // namespace metagen
