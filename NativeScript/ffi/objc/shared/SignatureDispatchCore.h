#ifndef NS_FFI_SHARED_SIGNATURE_DISPATCH_CORE_H
#define NS_FFI_SHARED_SIGNATURE_DISPATCH_CORE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <unordered_set>

#include "Metadata.h"
#include "MetadataReader.h"

namespace nativescript {

enum class SignatureCallKind : uint8_t {
  ObjCMethod = 1,
  CFunction = 2,
  BlockInvoke = 3,
};

using ObjCPreparedInvoker = void (*)(void* fnptr, void** avalues,
                                     void* rvalue);
using CFunctionPreparedInvoker = void (*)(void* fnptr, void** avalues,
                                          void* rvalue);
using BlockPreparedInvoker = void (*)(void* fnptr, void** avalues,
                                      void* rvalue);

struct ObjCDispatchEntry {
  uint64_t dispatchId;
  ObjCPreparedInvoker invoker;
};

struct CFunctionDispatchEntry {
  uint64_t dispatchId;
  CFunctionPreparedInvoker invoker;
};

struct BlockDispatchEntry {
  uint64_t dispatchId;
  BlockPreparedInvoker invoker;
};

inline constexpr uint64_t kSignatureHashOffsetBasis = 14695981039346656037ull;
inline constexpr uint64_t kSignatureHashPrime = 1099511628211ull;
inline constexpr metagen::MDSectionOffset kNullMetadataSectionOffset =
    static_cast<metagen::MDSectionOffset>(0xFFFFFFFFu >> 1);

inline uint64_t hashBytesFnv1a(const void* data, size_t size,
                               uint64_t seed = kSignatureHashOffsetBasis) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = seed;
  for (size_t i = 0; i < size; i++) {
    hash ^= static_cast<uint64_t>(bytes[i]);
    hash *= kSignatureHashPrime;
  }
  return hash;
}

inline uint64_t composeSignatureDispatchId(uint64_t signatureHash,
                                           SignatureCallKind kind,
                                           uint8_t flags) {
  const uint8_t kindByte = static_cast<uint8_t>(kind);
  uint64_t hash = hashBytesFnv1a(&kindByte, sizeof(kindByte));
  hash = hashBytesFnv1a(&flags, sizeof(flags), hash);
  return hashBytesFnv1a(&signatureHash, sizeof(signatureHash), hash);
}

template <typename Entry, typename Invoker, size_t N>
inline Invoker lookupDispatchInvoker(const Entry (&entries)[N],
                                     uint64_t dispatchId) {
  if (dispatchId == 0 || N <= 1) {
    return nullptr;
  }

  size_t low = 1;
  size_t high = N;
  while (low < high) {
    const size_t mid = low + ((high - low) >> 1);
    const uint64_t midId = entries[mid].dispatchId;
    if (midId < dispatchId) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  if (low < N && entries[low].dispatchId == dispatchId) {
    return entries[low].invoker;
  }
  return nullptr;
}

inline bool isGeneratedDispatchEnabled() {
  static const bool enabled = []() {
    const char* disableFlag = std::getenv("NS_DISABLE_GSD");
    return disableFlag == nullptr || disableFlag[0] == '\0' ||
           (disableFlag[0] == '0' && disableFlag[1] == '\0');
  }();
  return enabled;
}

namespace signature_dispatch_detail {

inline metagen::MDTypeKind canonicalizeSignatureTypeKind(
    metagen::MDTypeKind kind) {
  switch (kind) {
    case metagen::mdTypeAnyObject:
    case metagen::mdTypeProtocolObject:
    case metagen::mdTypeClassObject:
    case metagen::mdTypeInstanceObject:
    case metagen::mdTypeNSStringObject:
    case metagen::mdTypeNSMutableStringObject:
      return metagen::mdTypeAnyObject;
    default:
      return kind;
  }
}

template <typename T>
inline void appendIntegralToHash(uint64_t* hash, T value) {
  using Unsigned = typename std::make_unsigned<T>::type;
  Unsigned unsignedValue = static_cast<Unsigned>(value);
  for (size_t i = 0; i < sizeof(Unsigned); i++) {
    const uint8_t byte =
        static_cast<uint8_t>((unsignedValue >> (i * 8)) & 0xFF);
    *hash = hashBytesFnv1a(&byte, sizeof(byte), *hash);
  }
}

inline metagen::MDTypeKind stripMetadataTypeFlags(metagen::MDTypeKind kind) {
  uint8_t raw = static_cast<uint8_t>(kind);
  raw &= ~(metagen::mdTypeFlagNext | metagen::mdTypeFlagVariadic);
  return static_cast<metagen::MDTypeKind>(raw);
}

inline bool appendMetadataSignatureHash(
    metagen::MDMetadataReader* reader, metagen::MDSectionOffset signatureOffset,
    std::unordered_set<metagen::MDSectionOffset>* activeSignatures,
    uint64_t* hash);

inline bool appendMetadataTypeHash(
    metagen::MDMetadataReader* reader, metagen::MDSectionOffset* offset,
    std::unordered_set<metagen::MDSectionOffset>* activeSignatures,
    uint64_t* hash) {
  if (reader == nullptr || offset == nullptr || hash == nullptr ||
      activeSignatures == nullptr) {
    return false;
  }

  const metagen::MDTypeKind kindWithFlags = reader->getTypeKind(*offset);
  *offset += sizeof(metagen::MDTypeKind);
  const metagen::MDTypeKind rawKind = stripMetadataTypeFlags(kindWithFlags);

  appendIntegralToHash<uint8_t>(hash, 0xB0);
  appendIntegralToHash<uint8_t>(
      hash, static_cast<uint8_t>(canonicalizeSignatureTypeKind(rawKind)));

  switch (rawKind) {
    case metagen::mdTypeArray:
    case metagen::mdTypeVector:
    case metagen::mdTypeExtVector:
    case metagen::mdTypeComplex: {
      const auto arraySize = reader->getArraySize(*offset);
      *offset += sizeof(uint16_t);
      appendIntegralToHash<uint16_t>(hash, arraySize);
      if (!appendMetadataTypeHash(reader, offset, activeSignatures, hash)) {
        return false;
      }
      break;
    }

    case metagen::mdTypeStruct: {
      const auto structOffset = reader->getOffset(*offset);
      *offset += sizeof(metagen::MDSectionOffset);
      appendIntegralToHash<metagen::MDSectionOffset>(hash, structOffset);
      break;
    }

    case metagen::mdTypeClassObject: {
      auto classOffset = reader->getOffset(*offset);
      *offset += sizeof(metagen::MDSectionOffset);
      bool hasNext = (classOffset & metagen::mdSectionOffsetNext) != 0;
      while (hasNext) {
        auto protocolOffset = reader->getOffset(*offset);
        *offset += sizeof(metagen::MDSectionOffset);
        hasNext = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
      }
      break;
    }

    case metagen::mdTypeProtocolObject: {
      bool hasNext = true;
      while (hasNext) {
        auto protocolOffset = reader->getOffset(*offset);
        *offset += sizeof(metagen::MDSectionOffset);
        hasNext = (protocolOffset & metagen::mdSectionOffsetNext) != 0;
      }
      break;
    }

    case metagen::mdTypePointer:
      if (!appendMetadataTypeHash(reader, offset, activeSignatures, hash)) {
        return false;
      }
      break;

    case metagen::mdTypeBlock:
    case metagen::mdTypeFunctionPointer: {
      const auto nestedSignatureOffset = reader->getOffset(*offset);
      *offset += sizeof(metagen::MDSectionOffset);
      if (nestedSignatureOffset != kNullMetadataSectionOffset) {
        const auto nestedAbsoluteOffset =
            reader->signaturesOffset + nestedSignatureOffset;
        if (!appendMetadataSignatureHash(reader, nestedAbsoluteOffset,
                                         activeSignatures, hash)) {
          return false;
        }
      }
      break;
    }

    default:
      break;
  }

  appendIntegralToHash<uint8_t>(hash, 0xBF);
  return true;
}

inline bool appendMetadataSignatureHash(
    metagen::MDMetadataReader* reader, metagen::MDSectionOffset signatureOffset,
    std::unordered_set<metagen::MDSectionOffset>* activeSignatures,
    uint64_t* hash) {
  if (reader == nullptr || hash == nullptr || activeSignatures == nullptr) {
    return false;
  }

  if (activeSignatures->find(signatureOffset) != activeSignatures->end()) {
    appendIntegralToHash<uint8_t>(hash, 0xEE);
    return true;
  }
  activeSignatures->insert(signatureOffset);

  metagen::MDSectionOffset offset = signatureOffset;
  const metagen::MDTypeKind returnTypeKind = reader->getTypeKind(offset);
  bool next =
      (static_cast<uint8_t>(returnTypeKind) & metagen::mdTypeFlagNext) != 0;
  const bool isVariadic =
      (static_cast<uint8_t>(returnTypeKind) & metagen::mdTypeFlagVariadic) != 0;

  appendIntegralToHash<uint8_t>(hash, 0xA0);
  appendIntegralToHash<uint8_t>(hash, isVariadic ? 1 : 0);

  if (!appendMetadataTypeHash(reader, &offset, activeSignatures, hash)) {
    activeSignatures->erase(signatureOffset);
    return false;
  }

  uint32_t argCount = 0;
  while (next) {
    const metagen::MDTypeKind argTypeKind = reader->getTypeKind(offset);
    next =
        (static_cast<uint8_t>(argTypeKind) & metagen::mdTypeFlagNext) != 0;
    if (!appendMetadataTypeHash(reader, &offset, activeSignatures, hash)) {
      activeSignatures->erase(signatureOffset);
      return false;
    }
    argCount++;
  }

  appendIntegralToHash<uint32_t>(hash, argCount);
  appendIntegralToHash<uint8_t>(hash, 0xAF);

  activeSignatures->erase(signatureOffset);
  return true;
}

}  // namespace signature_dispatch_detail

inline uint64_t metadataSignatureHash(
    metagen::MDMetadataReader* reader,
    metagen::MDSectionOffset signatureOffset) {
  if (reader == nullptr || signatureOffset == kNullMetadataSectionOffset) {
    return 0;
  }

  uint64_t hash = kSignatureHashOffsetBasis;
  std::unordered_set<metagen::MDSectionOffset> activeSignatures;
  if (!signature_dispatch_detail::appendMetadataSignatureHash(
          reader, signatureOffset, &activeSignatures, &hash)) {
    return 0;
  }
  return hash;
}

}  // namespace nativescript

#endif  // NS_FFI_SHARED_SIGNATURE_DISPATCH_CORE_H
