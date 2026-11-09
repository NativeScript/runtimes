#include "SignatureDispatchEmitter/Shared.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <type_traits>
#include <unordered_set>

namespace metagen::signature_dispatch {

constexpr uint64_t kFNV64OffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFNV64Prime = 1099511628211ull;

uint64_t hashBytesFnv1a(const void* data, size_t size,
                        uint64_t seed = kFNV64OffsetBasis) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = seed;
  for (size_t i = 0; i < size; i++) {
    hash ^= static_cast<uint64_t>(bytes[i]);
    hash *= kFNV64Prime;
  }
  return hash;
}

uint64_t composeDispatchId(uint64_t signatureHash, DispatchKind kind,
                           uint8_t flags) {
  const uint8_t kindByte = static_cast<uint8_t>(kind);
  uint64_t hash = hashBytesFnv1a(&kindByte, sizeof(kindByte));
  hash = hashBytesFnv1a(&flags, sizeof(flags), hash);
  return hashBytesFnv1a(&signatureHash, sizeof(signatureHash), hash);
}

MDTypeKind canonicalizeSignatureTypeKind(MDTypeKind kind) {
  switch (kind) {
    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
      return mdTypeAnyObject;
    default:
      return kind;
  }
}

template <typename T>
void appendIntegral(uint64_t* hash, std::string* key, T value) {
  using Unsigned = typename std::make_unsigned<T>::type;
  Unsigned unsignedValue = static_cast<Unsigned>(value);
  for (size_t i = 0; i < sizeof(Unsigned); i++) {
    const uint8_t byte =
        static_cast<uint8_t>((unsignedValue >> (i * 8)) & 0xFF);
    *hash = hashBytesFnv1a(&byte, sizeof(byte), *hash);
    if (key != nullptr) {
      key->push_back(static_cast<char>(byte));
    }
  }
}

bool appendCanonicalSignature(
    const MDSignature* signature, MDSectionOffset signatureOffset,
    const SignatureMap& signatures,
    std::unordered_set<MDSectionOffset>* activeSignatures, uint64_t* hash,
    std::string* key);

bool appendCanonicalType(const MDTypeInfo* type, const SignatureMap& signatures,
                         std::unordered_set<MDSectionOffset>* activeSignatures,
                         uint64_t* hash, std::string* key) {
  if (type == nullptr || hash == nullptr) {
    return false;
  }

  appendIntegral<uint8_t>(hash, key, 0xB0);
  const MDTypeKind rawKind = type->kind;
  const MDTypeKind canonicalKind = canonicalizeSignatureTypeKind(rawKind);
  appendIntegral<uint8_t>(hash, key, static_cast<uint8_t>(canonicalKind));

  switch (rawKind) {
    case mdTypeArray:
    case mdTypeVector:
    case mdTypeExtVector:
    case mdTypeComplex:
      appendIntegral<uint16_t>(hash, key, type->arraySize);
      if (!appendCanonicalType(type->elementType, signatures, activeSignatures,
                               hash, key)) {
        return false;
      }
      break;

    case mdTypeStruct:
      appendIntegral<MDSectionOffset>(hash, key, type->structOffset);
      break;

    case mdTypePointer:
      if (!appendCanonicalType(type->pointeeType, signatures, activeSignatures,
                               hash, key)) {
        return false;
      }
      break;

    case mdTypeBlock:
    case mdTypeFunctionPointer: {
      const MDSectionOffset nestedSignatureOffset = type->signatureOffset;
      auto nestedIt = signatures.find(nestedSignatureOffset);
      if (nestedSignatureOffset == MD_SECTION_OFFSET_NULL ||
          nestedIt == signatures.end() || nestedIt->second == nullptr) {
        break;
      }

      if (!appendCanonicalSignature(nestedIt->second, nestedSignatureOffset,
                                    signatures, activeSignatures, hash, key)) {
        return false;
      }
      break;
    }

    default:
      break;
  }

  appendIntegral<uint8_t>(hash, key, 0xBF);
  return true;
}

bool appendCanonicalSignature(
    const MDSignature* signature, MDSectionOffset signatureOffset,
    const SignatureMap& signatures,
    std::unordered_set<MDSectionOffset>* activeSignatures, uint64_t* hash,
    std::string* key) {
  if (signature == nullptr || hash == nullptr || activeSignatures == nullptr) {
    return false;
  }

  const bool trackRecursion = signatureOffset != MD_SECTION_OFFSET_NULL;
  if (trackRecursion) {
    if (activeSignatures->find(signatureOffset) != activeSignatures->end()) {
      appendIntegral<uint8_t>(hash, key, 0xEE);
      return true;
    }
    activeSignatures->insert(signatureOffset);
  }

  appendIntegral<uint8_t>(hash, key, 0xA0);
  appendIntegral<uint8_t>(hash, key, signature->isVariadic ? 1 : 0);

  if (!appendCanonicalType(signature->returnType, signatures, activeSignatures,
                           hash, key)) {
    if (trackRecursion) {
      activeSignatures->erase(signatureOffset);
    }
    return false;
  }

  uint32_t argCount = 0;
  for (const auto* arg : signature->arguments) {
    if (!appendCanonicalType(arg, signatures, activeSignatures, hash, key)) {
      if (trackRecursion) {
        activeSignatures->erase(signatureOffset);
      }
      return false;
    }
    argCount++;
  }

  appendIntegral<uint32_t>(hash, key, argCount);
  appendIntegral<uint8_t>(hash, key, 0xAF);

  if (trackRecursion) {
    activeSignatures->erase(signatureOffset);
  }
  return true;
}

uint64_t signatureHash(const MDSignature* signature,
                       MDSectionOffset signatureOffset,
                       const SignatureMap& signatures,
                       std::string* canonicalKeyOut) {
  if (signature == nullptr) {
    return 0;
  }

  uint64_t hash = kFNV64OffsetBasis;
  std::unordered_set<MDSectionOffset> activeSignatures;
  if (!appendCanonicalSignature(signature, signatureOffset, signatures,
                                &activeSignatures, &hash, canonicalKeyOut)) {
    return 0;
  }
  return hash;
}

bool mapTypeToCpp(const MDTypeInfo* type, std::string* out,
                  bool allowVoid = false);

bool mapPointerPointeeToCpp(const MDTypeInfo* type, std::string* out) {
  if (type == nullptr || out == nullptr) {
    return false;
  }

  switch (type->kind) {
    case mdTypeVoid:
      *out = "void";
      return true;
    case mdTypePointer: {
      std::string nested;
      if (!mapPointerPointeeToCpp(type->pointeeType, &nested)) {
        return false;
      }
      *out = nested + "*";
      return true;
    }
    default:
      return mapTypeToCpp(type, out, false);
  }
}

bool mapTypeToCpp(const MDTypeInfo* type, std::string* out, bool allowVoid) {
  if (type == nullptr || out == nullptr) {
    return false;
  }

  switch (type->kind) {
    case mdTypeVoid:
      if (!allowVoid) {
        return false;
      }
      *out = "void";
      return true;

    case mdTypeBool:
      *out = "uint8_t";
      return true;

    case mdTypeChar:
      *out = "int8_t";
      return true;

    case mdTypeUChar:
    case mdTypeUInt8:
      *out = "uint8_t";
      return true;

    case mdTypeSShort:
      *out = "int16_t";
      return true;

    case mdTypeUShort:
      *out = "uint16_t";
      return true;

    case mdTypeSInt:
      *out = "int32_t";
      return true;

    case mdTypeUInt:
      *out = "uint32_t";
      return true;

    case mdTypeSLong:
    case mdTypeSInt64:
      *out = "int64_t";
      return true;

    case mdTypeULong:
    case mdTypeUInt64:
      *out = "uint64_t";
      return true;

    case mdTypeFloat:
      *out = "float";
      return true;

    case mdTypeDouble:
      *out = "double";
      return true;

    case mdTypeString:
      *out = "char*";
      return true;

    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
      *out = "id";
      return true;

    case mdTypeClass:
      *out = "Class";
      return true;

    case mdTypeSelector:
      *out = "SEL";
      return true;

    case mdTypePointer: {
      std::string pointee;
      if (!mapPointerPointeeToCpp(type->pointeeType, &pointee)) {
        return false;
      }
      *out = pointee + "*";
      return true;
    }

    case mdTypeOpaquePointer:
    case mdTypeBlock:
    case mdTypeFunctionPointer:
      *out = "void*";
      return true;

    default:
      return false;
  }
}

bool isSignatureSupported(const MDSignature* signature) {
  if (signature == nullptr || signature->isVariadic) {
    return false;
  }
  // Keep generated dispatch focused on hot paths and avoid huge wrappers.
  if (signature->arguments.size() > 8) {
    return false;
  }

  std::string unused;
  if (!mapTypeToCpp(signature->returnType, &unused, true)) {
    return false;
  }

  for (const auto* arg : signature->arguments) {
    if (!mapTypeToCpp(arg, &unused, false)) {
      return false;
    }
  }

  return true;
}

std::string toHexLiteral(uint64_t value) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << value
         << "ULL";
  return stream.str();
}

std::string toBase36(size_t value) {
  std::ostringstream stream;
  if (value == 0) {
    stream << '0';
    return stream.str();
  }

  std::string digits;
  while (value > 0) {
    const size_t digit = value % 36;
    digits.push_back(
        static_cast<char>(digit < 10 ? ('0' + digit) : ('a' + digit - 10)));
    value /= 36;
  }
  std::reverse(digits.begin(), digits.end());
  stream << digits;
  return stream.str();
}

bool isFastPrimitiveDispatchKind(MDTypeKind kind) {
  switch (kind) {
    case mdTypeBool:
    case mdTypeChar:
    case mdTypeUChar:
    case mdTypeUInt8:
    case mdTypeSShort:
    case mdTypeUShort:
    case mdTypeSInt:
    case mdTypeUInt:
    case mdTypeSLong:
    case mdTypeULong:
    case mdTypeSInt64:
    case mdTypeUInt64:
    case mdTypeFloat:
    case mdTypeDouble:
      return true;
    default:
      return false;
  }
}

bool isFastReferenceDispatchKind(MDTypeKind kind) {
  switch (kind) {
    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
    case mdTypeSelector:
      return true;
    default:
      return false;
  }
}

bool argKindMayNeedCleanup(MDTypeKind kind) {
  switch (kind) {
    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
    case mdTypeClass:
    case mdTypeSelector:
      return false;
    default:
      return !isFastPrimitiveDispatchKind(kind);
  }
}

std::string makeWrapperShapeKey(DispatchKind kind,
                                const MDSignature* signature) {
  if (signature == nullptr) {
    return {};
  }

  std::string returnType;
  if (!mapTypeToCpp(signature->returnType, &returnType, true)) {
    return {};
  }

  std::ostringstream key;
  key << static_cast<int>(kind) << "|" << returnType << "|";
  for (const auto* arg : signature->arguments) {
    std::string argType;
    if (!mapTypeToCpp(arg, &argType, false)) {
      return {};
    }

    if (isFastPrimitiveDispatchKind(arg->kind)) {
      key << "F" << static_cast<int>(arg->kind);
    } else if (isFastReferenceDispatchKind(arg->kind)) {
      key << "H" << static_cast<int>(arg->kind);
    } else {
      key << "M" << argType;
    }
    key << "|";
  }

  return key.str();
}
void collectBlockUsesFromSignature(MDSectionOffset signatureOffset,
                                   const SignatureMap& signatures,
                                   std::unordered_set<MDSectionOffset>* active,
                                   std::vector<SignatureUse>* uses);

void collectBlockUsesFromType(const MDTypeInfo* type,
                              const SignatureMap& signatures,
                              std::unordered_set<MDSectionOffset>* active,
                              std::vector<SignatureUse>* uses) {
  if (type == nullptr || active == nullptr || uses == nullptr) {
    return;
  }

  switch (type->kind) {
    case mdTypeArray:
    case mdTypeVector:
    case mdTypeExtVector:
    case mdTypeComplex:
      collectBlockUsesFromType(type->elementType, signatures, active, uses);
      break;

    case mdTypePointer:
      collectBlockUsesFromType(type->pointeeType, signatures, active, uses);
      break;

    case mdTypeBlock:
      if (type->signatureOffset != MD_SECTION_OFFSET_NULL) {
        uses->push_back({DispatchKind::BlockInvoke, type->signatureOffset, 0});
        collectBlockUsesFromSignature(type->signatureOffset, signatures, active,
                                      uses);
      }
      break;

    case mdTypeFunctionPointer:
      if (type->signatureOffset != MD_SECTION_OFFSET_NULL) {
        collectBlockUsesFromSignature(type->signatureOffset, signatures, active,
                                      uses);
      }
      break;

    default:
      break;
  }
}

void collectBlockUsesFromSignature(MDSectionOffset signatureOffset,
                                   const SignatureMap& signatures,
                                   std::unordered_set<MDSectionOffset>* active,
                                   std::vector<SignatureUse>* uses) {
  if (active == nullptr || uses == nullptr ||
      signatureOffset == MD_SECTION_OFFSET_NULL ||
      active->find(signatureOffset) != active->end()) {
    return;
  }

  auto it = signatures.find(signatureOffset);
  if (it == signatures.end() || it->second == nullptr) {
    return;
  }

  active->insert(signatureOffset);
  const MDSignature* signature = it->second;
  collectBlockUsesFromType(signature->returnType, signatures, active, uses);
  for (const auto* arg : signature->arguments) {
    collectBlockUsesFromType(arg, signatures, active, uses);
  }
  active->erase(signatureOffset);
}

void collectMethodUses(const std::vector<MDMember*>& members,
                       std::vector<SignatureUse>* uses) {
  if (uses == nullptr) {
    return;
  }

  for (auto* member : members) {
    if (member == nullptr) {
      continue;
    }

    const uint8_t methodFlags =
        (member->flags & mdMemberReturnOwned) != 0 ? 1 : 0;

    if ((member->flags & mdMemberProperty) != 0) {
      if (member->getterSignature != MD_SECTION_OFFSET_NULL) {
        uses->push_back(
            {DispatchKind::ObjCMethod, member->getterSignature, methodFlags});
      }
      if (((member->flags & mdMemberReadonly) == 0) &&
          member->setterSignature != MD_SECTION_OFFSET_NULL) {
        uses->push_back({DispatchKind::ObjCMethod, member->setterSignature, 0});
      }
    } else {
      if (member->signature != MD_SECTION_OFFSET_NULL) {
        uses->push_back(
            {DispatchKind::ObjCMethod, member->signature, methodFlags});
      }
    }
  }
}

}  // namespace metagen::signature_dispatch
