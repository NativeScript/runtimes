#ifndef nativescript_H
#define nativescript_H

#include <CoreFoundation/CFRunLoop.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <stdint.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AutoreleasePool.h"
#include "CFunction.h"
#include "Cif.h"
#include "Class.h"
#include "MetadataReader.h"
#include "NativeScript.h"
#include "Protocol.h"
#include "Struct.h"
#include "TypeConv.h"
#include "js_native_api.h"
#include "objc/runtime.h"

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports);

using namespace metagen;

namespace nativescript {

class ObjCBridgeState;

struct JSObjectFinalizerContext {
  ObjCBridgeState* bridgeState;
  uint64_t bridgeStateToken;
  id object;
  napi_ref ref;
};

struct HandleObjectRef {
  napi_ref ref = nullptr;
  bool ownsRef = false;
};

struct RecentObjectWrapperRef {
  uintptr_t objectKey = 0;
  uintptr_t objectClassKey = 0;
  napi_ref borrowedRef = nullptr;
};

void finalize_objc_object(napi_env /*env*/, void* data, void* hint);
bool IsBridgeStateLive(const ObjCBridgeState* bridgeState,
                       uint64_t token) noexcept;
void DeleteReferenceOnOwningThread(napi_env env, ObjCBridgeState* bridgeState,
                                   uint64_t bridgeStateToken, napi_ref ref);
void ReleaseAndDeleteReferenceOnOwningThread(napi_env env, ObjCBridgeState* bridgeState,
                                             uint64_t bridgeStateToken, napi_ref ref);
bool PostFinalizer(napi_env env, napi_finalize finalize_cb, void* finalize_data,
                   void* finalize_hint);

// Determines how retain/release should be called when an Objective-C
// object is exposed to JavaScript land.
typedef enum ObjectOwnership {
  // The object is already owned by JS land, and will be released
  // when the JS object is garbage collected.
  kOwnedObject,
  // The object is not owned by JS land, so to "take ownership"
  // we will call retain and release when the JS object is
  // garbage collected.
  kUnownedObject,
} ObjectOwnership;

class ObjCBridgeState {
 public:
  struct RoundTripCacheEntry {
    napi_ref ref = nullptr;
    napi_value rawValue = nullptr;
    bool persistBeyondFrame = true;
    uintptr_t objectKey = 0;
    uintptr_t objectClassKey = 0;
  };

  ObjCBridgeState(napi_env env, const char* metadata_path = nullptr,
                  const void* metadata_ptr = nullptr);
  ~ObjCBridgeState();

  static inline ObjCBridgeState* InstanceData(napi_env env) {
    if (env == nullptr) {
      return nullptr;
    }
    ObjCBridgeState* bridgeState;
    napi_status status = napi_get_instance_data(env, (void**)&bridgeState);
    if (status != napi_ok) {
      return nullptr;
    }
    return bridgeState;
  }

  bool enqueueFinalizer(napi_finalize callback, void* data, void* hint);
  void drainFinalizers(bool stop = false);

  static inline uintptr_t NormalizeHandleKey(void* handle) {
    if (handle == nullptr) {
      return 0;
    }
#if INTPTR_MAX == INT64_MAX
    return reinterpret_cast<uintptr_t>(handle) & 0x0000FFFFFFFFFFFFULL;
#else
    return reinterpret_cast<uintptr_t>(handle);
#endif
  }

  inline void registerRuntimeClass(ObjCClass* bridgedClass,
                                   Class runtimeClass) {
    if (bridgedClass == nullptr || runtimeClass == nil) {
      return;
    }

    bridgedClass->nativeClass = runtimeClass;
    classesByPointer[runtimeClass] = bridgedClass;
    nativeObjectsByBridgeWrapper[bridgedClass] = (id)runtimeClass;
    if (bridgedClass->metadataOffset != MD_SECTION_OFFSET_NULL) {
      mdClassesByPointer[runtimeClass] = bridgedClass->metadataOffset;
    }
  }

  inline void registerProtocolMetadata(Protocol* runtimeProtocol,
                                       MDSectionOffset metadataOffset) {
    if (runtimeProtocol == nil || metadataOffset == MD_SECTION_OFFSET_NULL) {
      return;
    }

    mdProtocolsByPointer[runtimeProtocol] = metadataOffset;
  }

  inline void registerRuntimeProtocol(ObjCProtocol* bridgedProtocol,
                                      Protocol* runtimeProtocol) {
    if (bridgedProtocol == nullptr || runtimeProtocol == nil) {
      return;
    }

    nativeObjectsByBridgeWrapper[bridgedProtocol] = (id)runtimeProtocol;
    registerProtocolMetadata(runtimeProtocol, bridgedProtocol->metadataOffset);
  }

  inline id nativeObjectForBridgeWrapper(void* wrapped) const {
    if (wrapped == nullptr) {
      return nil;
    }

    auto cached = nativeObjectsByBridgeWrapper.find(wrapped);
    return cached != nativeObjectsByBridgeWrapper.end() ? cached->second : nil;
  }

  void registerVarGlobals(napi_env env, napi_value global);
  void registerEnumGlobals(napi_env env, napi_value global);
  void registerStructGlobals(napi_env env, napi_value global);
  void registerUnionGlobals(napi_env env, napi_value global);
  void registerFunctionGlobals(napi_env env, napi_value global);
  void registerClassGlobals(napi_env env, napi_value global);
  void registerProtocolGlobals(napi_env env, napi_value global);

  ObjCClass* getClass(napi_env env, MDSectionOffset offset);

  ObjCProtocol* getProtocol(napi_env env, MDSectionOffset offset);

  Cif* getMethodCif(napi_env env, Method method);
  Cif* getMethodCif(napi_env env, MDSectionOffset offset);
  Cif* getBlockCif(napi_env env, MDSectionOffset offset);
  Cif* getCFunctionCif(napi_env env, MDSectionOffset offset);

  napi_value proxyNativeObject(napi_env env, napi_value object,
                               id nativeObject);
#if defined(TARGET_ENGINE_HERMES)
  bool registerObjectFinalizer(napi_env env, napi_value object,
                               JSObjectFinalizerContext* context);
  bool takeObjectFinalizer(JSObjectFinalizerContext* context);
#endif

  napi_value getObject(napi_env env, id object, napi_value constructor,
                       ObjectOwnership ownership = kUnownedObject);
  napi_value getObject(napi_env env, id object,
                       ObjectOwnership ownership = kUnownedObject,
                       MDSectionOffset classOffset = 0,
                       std::vector<MDSectionOffset>* protocolOffsets = nullptr);
  napi_value findCachedObjectWrapper(napi_env env, id object);
  inline void deleteOwnedHandleObjectRef(napi_env env, HandleObjectRef& entry) {
    if (entry.ownsRef && env != nullptr && entry.ref != nullptr) {
      napi_delete_reference(env, entry.ref);
    }
    entry.ref = nullptr;
    entry.ownsRef = false;
  }
  inline void cacheHandleObject(napi_env env, void* handle, napi_value value) {
    if (handle == nullptr || value == nullptr) {
      return;
    }

    uintptr_t handleKey = NormalizeHandleKey(handle);
    auto it = handleObjectRefs.find(handleKey);
    if (it != handleObjectRefs.end()) {
      auto existing = get_ref_value(env, it->second.ref);
      if (existing != nullptr) {
        bool isSameValue = false;
        if (napi_strict_equals(env, existing, value, &isSameValue) == napi_ok &&
            isSameValue) {
          return;
        }
      }

      deleteOwnedHandleObjectRef(env, it->second);
      handleObjectRefs.erase(it);
      bumpHandleObjectRefsGeneration();
    }

    napi_ref ref = nullptr;
    napi_create_reference(env, value, 0, &ref);
    handleObjectRefs[handleKey] = HandleObjectRef{ref, true};
    bumpHandleObjectRefsGeneration();
  }
  inline void cacheHandleObjectRef(napi_env env, void* handle, napi_ref ref) {
    if (handle == nullptr || ref == nullptr) {
      return;
    }

    uintptr_t handleKey = NormalizeHandleKey(handle);
    auto it = handleObjectRefs.find(handleKey);
    if (it != handleObjectRefs.end()) {
      if (it->second.ref == ref) {
        return;
      }

      deleteOwnedHandleObjectRef(env, it->second);
      handleObjectRefs.erase(it);
      bumpHandleObjectRefsGeneration();
    }

    handleObjectRefs[handleKey] = HandleObjectRef{ref, false};
    bumpHandleObjectRefsGeneration();
  }
  inline napi_value getCachedHandleObject(napi_env env, void* handle) {
    if (handle == nullptr) {
      return nullptr;
    }

    uintptr_t handleKey = NormalizeHandleKey(handle);
    const uint64_t generation = currentHandleObjectRefsGeneration();

    struct LastHandleObjectRef {
      const ObjCBridgeState* bridgeState = nullptr;
      napi_env env = nullptr;
      uintptr_t handleKey = 0;
      napi_ref ref = nullptr;
      uint64_t generation = 0;
    };

    static thread_local LastHandleObjectRef lastHandleObjectRef;
    if (lastHandleObjectRef.bridgeState == this &&
        lastHandleObjectRef.env == env &&
        lastHandleObjectRef.handleKey == handleKey &&
        lastHandleObjectRef.ref != nullptr &&
        lastHandleObjectRef.generation == generation) {
      napi_value value = get_ref_value(env, lastHandleObjectRef.ref);
      if (value != nullptr) {
        return value;
      }
    }

    static thread_local LastHandleObjectRef recentHandleObjectRefs[8];
    static thread_local unsigned int nextRecentHandleObjectRefSlot = 0;
    for (const auto& entry : recentHandleObjectRefs) {
      if (entry.bridgeState != this ||
          entry.env != env ||
          entry.handleKey != handleKey ||
          entry.ref == nullptr ||
          entry.generation != generation) {
        continue;
      }

      napi_value value = get_ref_value(env, entry.ref);
      if (value != nullptr) {
        lastHandleObjectRef = entry;
        return value;
      }
      break;
    }

    auto it = handleObjectRefs.find(handleKey);
    if (it == handleObjectRefs.end()) {
      return nullptr;
    }

    auto value = get_ref_value(env, it->second.ref);
    if (value == nullptr) {
      deleteOwnedHandleObjectRef(env, it->second);
      handleObjectRefs.erase(it);
      bumpHandleObjectRefsGeneration();
      if (lastHandleObjectRef.bridgeState == this &&
          lastHandleObjectRef.handleKey == handleKey) {
        lastHandleObjectRef = {};
      }
      return nullptr;
    }

    lastHandleObjectRef = LastHandleObjectRef{
        .bridgeState = this,
        .env = env,
        .handleKey = handleKey,
        .ref = it->second.ref,
        .generation = generation,
    };
    recentHandleObjectRefs[nextRecentHandleObjectRefSlot++ & 7] =
        lastHandleObjectRef;
    return value;
  }
  inline bool ownsCachedHandleObjectRef(void* handle) const noexcept {
    if (handle == nullptr) {
      return false;
    }

    auto it = handleObjectRefs.find(NormalizeHandleKey(handle));
    return it != handleObjectRefs.end() && it->second.ownsRef;
  }
  inline void removeCachedHandleObject(napi_env env, void* handle) noexcept {
    if (handle == nullptr) {
      return;
    }

    uintptr_t handleKey = NormalizeHandleKey(handle);
    auto it = handleObjectRefs.find(handleKey);
    if (it == handleObjectRefs.end()) {
      return;
    }

    deleteOwnedHandleObjectRef(env, it->second);
    handleObjectRefs.erase(it);
    bumpHandleObjectRefsGeneration();
  }
  inline void cacheRecentObjectWrapper(napi_env env, id object,
                                       napi_value value, napi_ref ref) {
    if (env == nullptr || object == nil || value == nullptr || ref == nullptr) {
      return;
    }

    const uintptr_t objectKey = NormalizeHandleKey((void*)object);
    const uintptr_t objectClassKey = NormalizeHandleKey((void*)object_getClass(object));
    for (auto& entry : recentObjectWrappers) {
      if (entry.objectKey != objectKey || entry.objectClassKey != objectClassKey) {
        continue;
      }

      napi_value existing = get_ref_value(env, entry.borrowedRef);
      if (existing != nullptr) {
        bool isSameValue = false;
        if (napi_strict_equals(env, existing, value, &isSameValue) == napi_ok &&
            isSameValue) {
          return;
        }
      }

      entry.borrowedRef = ref;
      entry.objectKey = objectKey;
      entry.objectClassKey = objectClassKey;
      return;
    }

    RecentObjectWrapperRef entry{
        .objectKey = objectKey,
        .objectClassKey = objectClassKey,
        .borrowedRef = ref,
    };

    static constexpr size_t kRecentObjectWrapperLimit = 16;
    if (recentObjectWrappers.size() < kRecentObjectWrapperLimit) {
      recentObjectWrappers.push_back(entry);
      return;
    }

    RecentObjectWrapperRef& replaced =
        recentObjectWrappers[nextRecentObjectWrapperSlot++ % kRecentObjectWrapperLimit];
    replaced = entry;
  }
  inline napi_value getRecentObjectWrapper(napi_env env, id object) {
    if (env == nullptr || object == nil) {
      return nullptr;
    }

    const uintptr_t objectKey = NormalizeHandleKey((void*)object);
    const uintptr_t objectClassKey = NormalizeHandleKey((void*)object_getClass(object));
    for (auto it = recentObjectWrappers.begin(); it != recentObjectWrappers.end();) {
      if (it->objectKey != objectKey || it->objectClassKey != objectClassKey) {
        ++it;
        continue;
      }

      napi_value value = get_ref_value(env, it->borrowedRef);
      if (value != nullptr) {
        return value;
      }

      it = recentObjectWrappers.erase(it);
    }

    return nullptr;
  }
  inline void removeRecentObjectWrapper(napi_env env, id object) noexcept {
    if (env == nullptr || object == nil) {
      return;
    }

    const uintptr_t objectKey = NormalizeHandleKey((void*)object);
    const uintptr_t objectClassKey = NormalizeHandleKey((void*)object_getClass(object));
    for (auto it = recentObjectWrappers.begin(); it != recentObjectWrappers.end();) {
      if (it->objectKey == objectKey && it->objectClassKey == objectClassKey) {
        it = recentObjectWrappers.erase(it);
      } else {
        ++it;
      }
    }
  }

  void unregisterObject(id object) noexcept;
  bool unregisterObjectIfRefMatches(id object, napi_ref ref) noexcept;
  void detachObject(id object) noexcept;
  void trackObject(id object) noexcept;
  bool isTrackedObjectAlive(id object) const noexcept;
  inline bool hasObjectRef(id object) const noexcept {
    std::lock_guard<std::mutex> lock(objectRefsMutex);

    if (objectRefs.find(object) != objectRefs.end()) {
      return true;
    }

    uintptr_t objectKey = NormalizeHandleKey((void*)object);
    uintptr_t objectClassKey = NormalizeHandleKey((void*)object_getClass(object));
    for (const auto& entry : objectRefs) {
      if (NormalizeHandleKey((void*)entry.first) == objectKey &&
          NormalizeHandleKey((void*)object_getClass(entry.first)) == objectClassKey) {
        return true;
      }
    }

    return false;
  }

  inline uint64_t currentObjectRefsGeneration() const noexcept {
    return objectRefsGeneration.load(std::memory_order_relaxed);
  }

  inline uint64_t currentHandleObjectRefsGeneration() const noexcept {
    return handleObjectRefsGeneration.load(std::memory_order_relaxed);
  }

  inline void beginRoundTripCacheFrame(napi_env /*env*/) {
    roundTripCacheFrames.emplace_back();
  }

  inline bool hasRoundTripCacheFrame() const {
    return !roundTripCacheFrames.empty();
  }

  static inline void releaseRoundTripEntry(napi_env env,
                                           RoundTripCacheEntry& entry) {
    if (env != nullptr && entry.ref != nullptr) {
      napi_delete_reference(env, entry.ref);
      entry.ref = nullptr;
    }
    entry.rawValue = nullptr;
  }

  static inline napi_value getRoundTripEntryValue(napi_env env,
                                                  const RoundTripCacheEntry& entry) {
    if (entry.rawValue != nullptr) {
      return entry.rawValue;
    }

    return entry.ref != nullptr ? get_ref_value(env, entry.ref) : nullptr;
  }

  inline void cacheRoundTripObject(napi_env env, id object, napi_value value) {
    if (object == nil || roundTripCacheFrames.empty()) {
      return;
    }

    bool isArrayBuffer = false;
    bool isTypedArray = false;
    bool isDataView = false;
    bool isBufferLike =
        (napi_is_arraybuffer(env, value, &isArrayBuffer) == napi_ok && isArrayBuffer) ||
        (napi_is_typedarray(env, value, &isTypedArray) == napi_ok && isTypedArray) ||
        (napi_is_dataview(env, value, &isDataView) == napi_ok && isDataView);

    auto& frame = roundTripCacheFrames.back();
    if (frame.find(object) != frame.end()) {
      return;
    }

    uintptr_t objectKey = NormalizeHandleKey((void*)object);
    uintptr_t objectClassKey = NormalizeHandleKey((void*)object_getClass(object));
    frame[object] = RoundTripCacheEntry{
        .ref = make_ref(env, value),
        .rawValue = nullptr,
        .persistBeyondFrame = !isBufferLike,
        .objectKey = objectKey,
        .objectClassKey = objectClassKey,
    };
  }

  inline napi_value getRoundTripObject(napi_env env, id object) const {
    if (object == nil) {
      return nullptr;
    }

    uintptr_t objectKey = NormalizeHandleKey((void*)object);
    uintptr_t objectClassKey = NormalizeHandleKey((void*)object_getClass(object));
    auto matchesObject = [&](const RoundTripCacheEntry& entry) {
      return entry.objectKey == objectKey && entry.objectClassKey == objectClassKey;
    };

    if (roundTripCacheFrames.empty()) {
      auto recent = recentRoundTripCache.find(object);
      if (recent != recentRoundTripCache.end() && matchesObject(recent->second)) {
        return getRoundTripEntryValue(env, recent->second);
      }

      for (const auto& entry : recentRoundTripCache) {
        if (matchesObject(entry.second)) {
          return getRoundTripEntryValue(env, entry.second);
        }
      }

      return nullptr;
    }

    for (auto frame = roundTripCacheFrames.rbegin();
         frame != roundTripCacheFrames.rend(); ++frame) {
      auto find = frame->find(object);
      if (find != frame->end() && matchesObject(find->second)) {
        return getRoundTripEntryValue(env, find->second);
      }

      for (const auto& entry : *frame) {
        if (matchesObject(entry.second)) {
          return getRoundTripEntryValue(env, entry.second);
        }
      }
    }

    auto recent = recentRoundTripCache.find(object);
    if (recent != recentRoundTripCache.end() && matchesObject(recent->second)) {
      return getRoundTripEntryValue(env, recent->second);
    }

    for (const auto& entry : recentRoundTripCache) {
      if (matchesObject(entry.second)) {
        return getRoundTripEntryValue(env, entry.second);
      }
    }

    return nullptr;
  }

  inline void removeRoundTripObject(id object) {
    if (object == nil) {
      return;
    }

    uintptr_t objectKey = NormalizeHandleKey((void*)object);
    uintptr_t objectClassKey = NormalizeHandleKey((void*)object_getClass(object));
    auto matchesObject = [&](const RoundTripCacheEntry& entry) {
      return entry.objectKey == objectKey && entry.objectClassKey == objectClassKey;
    };
    auto removeFromMap = [&](auto& map) {
      for (auto it = map.begin(); it != map.end();) {
        if (matchesObject(it->second)) {
          releaseRoundTripEntry(env, it->second);
          it = map.erase(it);
        } else {
          ++it;
        }
      }
    };

    for (auto& frame : roundTripCacheFrames) {
      removeFromMap(frame);
    }
    removeFromMap(recentRoundTripCache);
  }

  inline void endRoundTripCacheFrame(napi_env env) {
    if (roundTripCacheFrames.empty()) {
      return;
    }

    auto frame = std::move(roundTripCacheFrames.back());
    roundTripCacheFrames.pop_back();

    if (!roundTripCacheFrames.empty()) {
      auto& parent = roundTripCacheFrames.back();
      for (auto& entry : frame) {
        if (parent.find(entry.first) == parent.end()) {
          parent[entry.first] = entry.second;
        } else {
          releaseRoundTripEntry(env, entry.second);
        }
      }
      return;
    }

    for (auto& entry : recentRoundTripCache) {
      releaseRoundTripEntry(env, entry.second);
    }
    recentRoundTripCache.clear();
    for (auto& entry : frame) {
      if (entry.second.persistBeyondFrame) {
        recentRoundTripCache.emplace(entry.first, entry.second);
      } else {
        releaseRoundTripEntry(env, entry.second);
      }
    }
  }

  inline bool tryResolveBridgedClassConstructor(napi_env env, napi_value value,
                                                Class* out) {
    if (out == nullptr || value == nullptr) {
      return false;
    }

    auto readFunctionName = [&](napi_value candidate) -> std::string {
      napi_valuetype candidateType = napi_undefined;
      if (napi_typeof(env, candidate, &candidateType) != napi_ok ||
          candidateType != napi_function) {
        return "";
      }

      bool hasName = false;
      if (napi_has_named_property(env, candidate, "name", &hasName) !=
              napi_ok ||
          !hasName) {
        return "";
      }

      napi_value nameValue = nullptr;
      if (napi_get_named_property(env, candidate, "name", &nameValue) !=
              napi_ok ||
          nameValue == nullptr) {
        return "";
      }

      napi_valuetype nameType = napi_undefined;
      if (napi_typeof(env, nameValue, &nameType) != napi_ok ||
          nameType != napi_string) {
        return "";
      }

      size_t nameLength = 0;
      if (napi_get_value_string_utf8(env, nameValue, nullptr, 0, &nameLength) !=
              napi_ok ||
          nameLength == 0) {
        return "";
      }

      std::string name(nameLength, '\0');
      if (napi_get_value_string_utf8(env, nameValue, name.data(),
                                     name.size() + 1, &nameLength) != napi_ok) {
        return "";
      }

      name.resize(nameLength);
      return name;
    };

    auto matchesConstructor = [&](ObjCClass* bridgedClass,
                                  ObjCClass** unresolvedMatch) -> bool {
      if (bridgedClass == nullptr || bridgedClass->constructor == nullptr) {
        return false;
      }

      napi_value constructor = get_ref_value(env, bridgedClass->constructor);
      if (constructor == nullptr) {
        return false;
      }

      bool isSameValue = false;
      if (napi_strict_equals(env, value, constructor, &isSameValue) ==
              napi_ok &&
          isSameValue) {
        if (bridgedClass->nativeClass == nil) {
          if (unresolvedMatch != nullptr) {
            *unresolvedMatch = bridgedClass;
          }
          return false;
        }

        *out = bridgedClass->nativeClass;
        return true;
      }

      return false;
    };

    ObjCClass* unresolvedConstructorMatch = nullptr;

    for (const auto& entry : classesByPointer) {
      if (matchesConstructor(entry.second, nullptr)) {
        return true;
      }
    }

    for (const auto& entry : classes) {
      if (matchesConstructor(entry.second, &unresolvedConstructorMatch)) {
        return true;
      }
    }

    std::string candidateName = readFunctionName(value);
    if (!candidateName.empty()) {
      for (const auto& entry : classesByPointer) {
        ObjCClass* bridgedClass = entry.second;
        if (bridgedClass == nullptr || bridgedClass->nativeClass == nil) {
          continue;
        }

        if (bridgedClass->name == candidateName) {
          *out = bridgedClass->nativeClass;
          return true;
        }
      }

      Class runtimeClass = objc_lookUpClass(candidateName.c_str());
      if (runtimeClass != nil) {
        if (unresolvedConstructorMatch != nullptr) {
          registerRuntimeClass(unresolvedConstructorMatch, runtimeClass);
        }
        *out = runtimeClass;
        return true;
      }
    }

    return false;
  }

  inline bool tryResolveBridgedProtocolConstructor(napi_env env,
                                                   napi_value value,
                                                   Protocol** out) const {
    if (out == nullptr || value == nullptr) {
      return false;
    }

    auto readFunctionName = [&](napi_value candidate) -> std::string {
      napi_valuetype candidateType = napi_undefined;
      if (napi_typeof(env, candidate, &candidateType) != napi_ok ||
          candidateType != napi_function) {
        return "";
      }

      bool hasName = false;
      if (napi_has_named_property(env, candidate, "name", &hasName) !=
              napi_ok ||
          !hasName) {
        return "";
      }

      napi_value nameValue = nullptr;
      if (napi_get_named_property(env, candidate, "name", &nameValue) !=
              napi_ok ||
          nameValue == nullptr) {
        return "";
      }

      napi_valuetype nameType = napi_undefined;
      if (napi_typeof(env, nameValue, &nameType) != napi_ok ||
          nameType != napi_string) {
        return "";
      }

      size_t nameLength = 0;
      if (napi_get_value_string_utf8(env, nameValue, nullptr, 0, &nameLength) !=
              napi_ok ||
          nameLength == 0) {
        return "";
      }

      std::string name(nameLength, '\0');
      if (napi_get_value_string_utf8(env, nameValue, name.data(),
                                     name.size() + 1, &nameLength) != napi_ok) {
        return "";
      }

      name.resize(nameLength);
      return name;
    };

    for (const auto& entry : protocols) {
      ObjCProtocol* bridgedProtocol = entry.second;
      if (bridgedProtocol == nullptr ||
          bridgedProtocol->constructor == nullptr) {
        continue;
      }

      napi_value constructor = get_ref_value(env, bridgedProtocol->constructor);
      if (constructor == nullptr) {
        continue;
      }

      bool isSameValue = false;
      if (napi_strict_equals(env, value, constructor, &isSameValue) !=
              napi_ok ||
          !isSameValue) {
        continue;
      }

      Protocol* runtimeProtocol =
          objc_getProtocol(bridgedProtocol->name.c_str());
      if (runtimeProtocol == nullptr) {
        static const std::string suffix = "Protocol";
        if (bridgedProtocol->name.size() > suffix.size() &&
            bridgedProtocol->name.compare(
                bridgedProtocol->name.size() - suffix.size(), suffix.size(),
                suffix) == 0) {
          std::string baseName = bridgedProtocol->name.substr(
              0, bridgedProtocol->name.size() - suffix.size());
          runtimeProtocol = objc_getProtocol(baseName.c_str());
        }
      }

      if (runtimeProtocol != nullptr) {
        *out = runtimeProtocol;
        return true;
      }
    }

    auto resolveProtocolByName =
        [](const std::string& protocolName) -> Protocol* {
      if (protocolName.empty()) {
        return nullptr;
      }

      Protocol* runtimeProtocol = objc_getProtocol(protocolName.c_str());
      if (runtimeProtocol != nullptr) {
        return runtimeProtocol;
      }

      static const std::string suffix = "Protocol";
      if (protocolName.size() > suffix.size() &&
          protocolName.compare(protocolName.size() - suffix.size(),
                               suffix.size(), suffix) == 0) {
        std::string baseName =
            protocolName.substr(0, protocolName.size() - suffix.size());
        return objc_getProtocol(baseName.c_str());
      }

      return nullptr;
    };

    std::string candidateName = readFunctionName(value);
    if (!candidateName.empty()) {
      for (const auto& entry : protocols) {
        ObjCProtocol* bridgedProtocol = entry.second;
        if (bridgedProtocol == nullptr) {
          continue;
        }

        if (bridgedProtocol->name == candidateName) {
          Protocol* runtimeProtocol =
              resolveProtocolByName(bridgedProtocol->name);
          if (runtimeProtocol != nullptr) {
            *out = runtimeProtocol;
            return true;
          }
        }
      }

      Protocol* runtimeProtocol = resolveProtocolByName(candidateName);
      if (runtimeProtocol != nullptr) {
        *out = runtimeProtocol;
        return true;
      }
    }

    return false;
  }

  inline bool tryResolveBridgedTypeConstructor(napi_env env, napi_value value,
                                               id* out) {
    if (out == nullptr || value == nullptr) {
      return false;
    }

    Class bridgedClass = nil;
    if (tryResolveBridgedClassConstructor(env, value, &bridgedClass)) {
      *out = (id)bridgedClass;
      return true;
    }

    Protocol* bridgedProtocol = nullptr;
    if (tryResolveBridgedProtocolConstructor(env, value, &bridgedProtocol)) {
      *out = (id)bridgedProtocol;
      return true;
    }

    return false;
  }

  CFunction* getCFunction(napi_env env, MDSectionOffset offset);

  inline StructInfo* getStructInfo(napi_env env, MDSectionOffset offset) {
    auto cached = structInfoCache.find(offset);
    if (cached != structInfoCache.end()) {
      return cached->second;
    }

    auto structInfo = getStructInfoFromMetadata(env, metadata, offset);
    structInfoCache[offset] = structInfo;

    return structInfo;
  }

  inline StructInfo* getUnionInfo(napi_env env, MDSectionOffset offset) {
    auto cached = structInfoCache.find(offset);
    if (cached != structInfoCache.end()) {
      return cached->second;
    }

    auto structInfo = getStructInfoFromUnionMetadata(env, metadata, offset);
    structInfoCache[offset] = structInfo;

    return structInfo;
  }

 public:
  napi_env env = nullptr;
  uint64_t lifetimeToken = 0;
  std::thread::id jsThreadId = std::this_thread::get_id();
  CFRunLoopRef jsRunLoop = CFRunLoopGetCurrent();
  struct PendingFinalizer {
    napi_finalize callback;
    void* data;
    void* hint;
  };
  std::mutex pendingFinalizersMutex;
  std::vector<PendingFinalizer> pendingFinalizers;
  bool finalizerDrainScheduled = false;
  bool acceptingFinalizers = true;
  std::unordered_map<id, napi_ref> objectRefs;
  struct PointerCacheEntry {
    void* owner = nullptr;
    napi_ref ref = nullptr;
  };
  std::unordered_map<uintptr_t, PointerCacheEntry> pointerCache;
  std::unordered_map<uintptr_t, HandleObjectRef> handleObjectRefs;
  std::vector<RecentObjectWrapperRef> recentObjectWrappers;
  size_t nextRecentObjectWrapperSlot = 0;

  napi_ref pointerClass = nullptr;
  napi_ref referenceClass = nullptr;
  napi_ref functionReferenceClass = nullptr;
  napi_ref createNativeProxy = nullptr;
  napi_ref createFastEnumeratorIterator = nullptr;
  napi_ref transferOwnershipToNative = nullptr;
#if defined(TARGET_ENGINE_HERMES)
  napi_ref objectFinalizationRegistry = nullptr;
  std::unordered_set<JSObjectFinalizerContext*> objectFinalizers;
#endif

  std::unordered_map<MDSectionOffset, ObjCClass*> classes;
  std::unordered_map<MDSectionOffset, ObjCProtocol*> protocols;
  std::unordered_map<Class, ObjCClass*> classesByPointer;
  std::unordered_map<Class, MDSectionOffset> mdClassesByPointer;
  std::unordered_map<Protocol*, MDSectionOffset> mdProtocolsByPointer;
  std::unordered_map<void*, id> nativeObjectsByBridgeWrapper;
  std::unordered_map<Class, napi_ref> constructorsByPointer;
  StructInfo* syntheticCGPointInfo = nullptr;

  std::unordered_map<std::string, Cif*> cifs;
  std::unordered_map<MDSectionOffset, napi_ref> mdValueCache;
  std::unordered_map<MDSectionOffset, CFunction*> cFunctionCache;
  std::unordered_map<MDSectionOffset, Cif*> mdFunctionSignatureCache;
  std::unordered_map<MDSectionOffset, Cif*> mdMethodSignatureCache;
  std::unordered_map<MDSectionOffset, Cif*> mdBlockSignatureCache;
  std::unordered_map<std::string, MDSectionOffset> structOffsets;
  std::unordered_map<std::string, MDSectionOffset> unionOffsets;
  // std::unordered_map<std::string, MDSectionOffset> protocolOffsets;

  void* self_dl;

  MDMetadataReader* metadata;

 private:
  inline void bumpObjectRefsGeneration() noexcept {
    objectRefsGeneration.fetch_add(1, std::memory_order_relaxed);
  }

  inline void bumpHandleObjectRefsGeneration() noexcept {
    handleObjectRefsGeneration.fetch_add(1, std::memory_order_relaxed);
  }

  inline void storeObjectRef(id object, napi_ref ref) noexcept {
    std::lock_guard<std::mutex> lock(objectRefsMutex);
    objectRefs[object] = ref;
    bumpObjectRefsGeneration();
  }

  inline napi_value getNormalizedObjectRef(napi_env env, id object) const {
    std::lock_guard<std::mutex> lock(objectRefsMutex);

    auto exact = objectRefs.find(object);
    if (exact != objectRefs.end()) {
      return get_ref_value(env, exact->second);
    }

    uintptr_t objectKey = NormalizeHandleKey((void*)object);
    uintptr_t objectClassKey = NormalizeHandleKey((void*)object_getClass(object));
    for (const auto& entry : objectRefs) {
      if (NormalizeHandleKey((void*)entry.first) != objectKey ||
          NormalizeHandleKey((void*)object_getClass(entry.first)) != objectClassKey) {
        continue;
      }

      return get_ref_value(env, entry.second);
    }

    return nullptr;
  }

  inline napi_ref takeObjectRef(id object,
                                napi_ref expectedRef = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(objectRefsMutex);

    auto exact = objectRefs.find(object);
    if (exact == objectRefs.end()) {
      return nullptr;
    }

    if (expectedRef != nullptr && exact->second != expectedRef) {
      return nullptr;
    }

    napi_ref ref = exact->second;
    objectRefs.erase(exact);
    bumpObjectRefsGeneration();
    return ref;
  }

  std::unordered_map<MDSectionOffset, StructInfo*> structInfoCache;
  std::vector<std::unordered_map<id, RoundTripCacheEntry>> roundTripCacheFrames;
  std::unordered_map<id, RoundTripCacheEntry> recentRoundTripCache;
  std::atomic<uint64_t> objectRefsGeneration{1};
  std::atomic<uint64_t> handleObjectRefsGeneration{1};
  mutable std::mutex objectRefsMutex;
  void* trackedObjectLiveness = nullptr;
  void* objc_autoreleasePool;
};

}  // namespace nativescript

#endif /* nativescript_H */
