#ifndef NS_JSI_SHARED_SIGNATURE_HASHING_H
#define NS_JSI_SHARED_SIGNATURE_HASHING_H

// Platform-neutral half of generated signature dispatch (GSD).
//
// This header must stay free of Objective-C and of any platform metadata
// format: it is compiled by the Apple runtime and, in future, by the Android
// runtime. The metadata-driven signature hashing (which walks the Objective-C
// metadata via metagen::MDMetadataReader) deliberately stays behind in
// ffi/objc/shared/SignatureDispatchCore.h, because the Android metadata is a
// different binary format entirely.
//
// What is genuinely common is the mechanism: hash a signature with FNV-1a,
// compose a dispatch id from (hash, call kind, flags), and binary-search a
// sorted, ahead-of-time generated table for a typed trampoline.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

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

}  // namespace nativescript

#endif  // NS_JSI_SHARED_SIGNATURE_HASHING_H
