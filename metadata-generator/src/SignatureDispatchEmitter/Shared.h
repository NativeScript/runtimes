#pragma once

#include <cstdint>
#include <cstddef>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "MetadataWriter.h"

namespace metagen::signature_dispatch {

enum class DispatchKind : uint8_t {
  ObjCMethod = 1,
  CFunction = 2,
  BlockInvoke = 3,
};

struct SignatureUse {
  DispatchKind kind;
  MDSectionOffset signatureOffset;
  uint8_t flags;
};

using SignatureMap = std::unordered_map<MDSectionOffset, MDSignature*>;

uint64_t composeDispatchId(uint64_t signatureHash, DispatchKind kind,
                           uint8_t flags);
uint64_t signatureHash(const MDSignature* signature,
                       MDSectionOffset signatureOffset,
                       const SignatureMap& signatures,
                       std::string* canonicalKeyOut);
bool mapTypeToCpp(const MDTypeInfo* type, std::string* out, bool allowVoid);
bool isSignatureSupported(const MDSignature* signature);
bool isFastPrimitiveDispatchKind(MDTypeKind kind);
bool isFastReferenceDispatchKind(MDTypeKind kind);
bool argKindMayNeedCleanup(MDTypeKind kind);
std::string toHexLiteral(uint64_t value);
std::string toBase36(size_t value);
std::string makeWrapperShapeKey(DispatchKind kind,
                                const MDSignature* signature);
void collectBlockUsesFromSignature(MDSectionOffset signatureOffset,
                                   const SignatureMap& signatures,
                                   std::unordered_set<MDSectionOffset>* active,
                                   std::vector<SignatureUse>* uses);
void collectMethodUses(const std::vector<MDMember*>& members,
                       std::vector<SignatureUse>* uses);

std::string makeNapiWrapperName(DispatchKind kind, size_t index);
std::string makePreparedWrapperName(DispatchKind kind, size_t index);
std::string makeGsdWrapperName(size_t index);
void writeNapiWrapper(std::ostringstream& out, DispatchKind kind,
                      const std::string& wrapperName,
                      const MDSignature* signature);
void writePreparedWrapper(std::ostringstream& out, DispatchKind kind,
                          const std::string& wrapperName,
                          const MDSignature* signature);
void writeGsdWrapper(std::ostringstream& out, const std::string& wrapperName,
                     const MDSignature* signature);
bool isGsdSignatureSupported(const MDSignature* signature);
std::string makeGsdWrapperShapeKey(const MDSignature* signature);

}  // namespace metagen::signature_dispatch
