#include "ObjCBridge.h"
#include "AutoreleasePool.h"
#include "Block.h"
#include "Class.h"
#include "ClassMember.h"
#include "Enum.h"
#include "InlineFunctions.h"
#include "Interop.h"
#include "Metadata.h"
#include "MetadataReader.h"
#include "NativeScript.h"
#include "Object.h"
#include "ObjectRef.h"
#include "Struct.h"
#include "TypeConv.h"
#include "Util.h"
#include "Variable.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "node_api_util.h"

#import <Foundation/Foundation.h>

#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <objc/runtime.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>

#ifdef EMBED_METADATA_SIZE
const unsigned char __attribute__((section("__objc_metadata,__objc_metadata")))
#if defined(__aarch64__)
embedded_metadata[EMBED_METADATA_SIZE] = "NSMDSectionHeaderARM";
#else
embedded_metadata[EMBED_METADATA_SIZE] = "NSMDSectionHeaderX86";
#endif
#endif

namespace nativescript {
namespace {
std::mutex gLiveBridgeStatesMutex;
std::unordered_map<const ObjCBridgeState*, uint64_t> gLiveBridgeStates;
std::atomic<uint64_t> gNextBridgeStateToken{1};
constexpr const char* kNativePointerProperty = "__ns_native_ptr";

bool envFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return false;
  }

  return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
         std::strcmp(value, "FALSE") != 0 && std::strcmp(value, "False") != 0 &&
         std::strcmp(value, "off") != 0 && std::strcmp(value, "OFF") != 0 &&
         std::strcmp(value, "no") != 0 && std::strcmp(value, "NO") != 0;
}

inline void deleteReferenceNow(napi_env env, napi_ref ref, bool unrefFirst) {
  if (env == nullptr || ref == nullptr) {
    return;
  }

  if (unrefFirst) {
    uint32_t remaining = 0;
    napi_reference_unref(env, ref, &remaining);
  }

  napi_delete_reference(env, ref);
}

inline void deleteReferenceOnOwningThread(napi_env env, ObjCBridgeState* bridgeState,
                                          uint64_t bridgeStateToken, napi_ref ref,
                                          bool unrefFirst) {
  if (env == nullptr || ref == nullptr) {
    return;
  }

  if (bridgeState == nullptr) {
    deleteReferenceNow(env, ref, unrefFirst);
    return;
  }

  if (!IsBridgeStateLive(bridgeState, bridgeStateToken)) {
    return;
  }

  if (bridgeState->jsThreadId == std::this_thread::get_id()) {
#if !defined(TARGET_ENGINE_QUICKJS)
    deleteReferenceNow(env, ref, unrefFirst);
    return;
#endif
  }

  CFRunLoopRef runLoop = bridgeState->jsRunLoop;
  if (runLoop == nullptr) {
    runLoop = CFRunLoopGetMain();
  }

  if (runLoop == nullptr) {
    if (bridgeState->jsThreadId == std::this_thread::get_id()) {
      deleteReferenceNow(env, ref, unrefFirst);
    }
    return;
  }

  CFRetain(runLoop);
  CFRunLoopPerformBlock(runLoop, kCFRunLoopCommonModes, ^{
    if (IsBridgeStateLive(bridgeState, bridgeStateToken)) {
      deleteReferenceNow(env, ref, unrefFirst);
    }
    CFRelease(runLoop);
  });
  CFRunLoopWakeUp(runLoop);
}

uint64_t RegisterBridgeState(const ObjCBridgeState* bridgeState) {
  if (bridgeState == nullptr) {
    return 0;
  }

  uint64_t token = gNextBridgeStateToken.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(gLiveBridgeStatesMutex);
  gLiveBridgeStates[bridgeState] = token;
  return token;
}

void UnregisterBridgeState(const ObjCBridgeState* bridgeState) {
  if (bridgeState == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(gLiveBridgeStatesMutex);
  gLiveBridgeStates.erase(bridgeState);
}
}  // namespace

bool IsBridgeStateLive(const ObjCBridgeState* bridgeState, uint64_t token) noexcept {
  if (bridgeState == nullptr || token == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(gLiveBridgeStatesMutex);
  auto find = gLiveBridgeStates.find(bridgeState);
  return find != gLiveBridgeStates.end() && find->second == token;
}

void DeleteReferenceOnOwningThread(napi_env env, ObjCBridgeState* bridgeState,
                                   uint64_t bridgeStateToken, napi_ref ref) {
  deleteReferenceOnOwningThread(env, bridgeState, bridgeStateToken, ref, false);
}

void ReleaseAndDeleteReferenceOnOwningThread(napi_env env, ObjCBridgeState* bridgeState,
                                             uint64_t bridgeStateToken, napi_ref ref) {
  deleteReferenceOnOwningThread(env, bridgeState, bridgeStateToken, ref, true);
}

bool PostFinalizer(napi_env env, napi_finalize finalize_cb, void* finalize_data,
                   void* finalize_hint) {
  if (env == nullptr || finalize_cb == nullptr) {
    return false;
  }

  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState != nullptr && bridgeState->jsThreadId == std::this_thread::get_id()) {
#if !defined(TARGET_ENGINE_QUICKJS) && !defined(TARGET_ENGINE_V8)
    finalize_cb(env, finalize_data, finalize_hint);
    return true;
#endif
  }

  if (bridgeState != nullptr) {
    return bridgeState->enqueueFinalizer(finalize_cb, finalize_data, finalize_hint);
  }

  CFRunLoopRef runLoop = CFRunLoopGetMain();
  if (runLoop == nullptr) {
    return false;
  }

  if (bridgeState == nullptr && [NSThread isMainThread]) {
#if !defined(TARGET_ENGINE_QUICKJS) && !defined(TARGET_ENGINE_V8)
    finalize_cb(env, finalize_data, finalize_hint);
    return true;
#endif
  }

  CFRetain(runLoop);
  CFRunLoopPerformBlock(runLoop, kCFRunLoopCommonModes, ^{
    finalize_cb(env, finalize_data, finalize_hint);
    CFRelease(runLoop);
  });
  CFRunLoopWakeUp(runLoop);
  return true;
}

bool ObjCBridgeState::enqueueFinalizer(napi_finalize callback, void* data, void* hint) {
  if (callback == nullptr || jsRunLoop == nullptr) {
    return false;
  }

  bool scheduleDrain = false;
  {
    std::lock_guard<std::mutex> lock(pendingFinalizersMutex);
    if (!acceptingFinalizers) {
      return false;
    }
    pendingFinalizers.push_back({callback, data, hint});
    if (!finalizerDrainScheduled) {
      finalizerDrainScheduled = true;
      scheduleDrain = true;
    }
  }

  if (!scheduleDrain) {
    return true;
  }

  CFRunLoopRef runLoop = jsRunLoop;
  const uint64_t token = lifetimeToken;
  CFRunLoopPerformBlock(runLoop, kCFRunLoopCommonModes, ^{
    if (IsBridgeStateLive(this, token)) {
      drainFinalizers();
    }
  });
  CFRunLoopWakeUp(runLoop);
  return true;
}

void ObjCBridgeState::drainFinalizers(bool stop) {
  std::vector<PendingFinalizer> pending;
  {
    std::lock_guard<std::mutex> lock(pendingFinalizersMutex);
    if (stop) {
      acceptingFinalizers = false;
    }
    pending.swap(pendingFinalizers);
    finalizerDrainScheduled = false;
  }

  for (const auto& finalizer : pending) {
    finalizer.callback(env, finalizer.data, finalizer.hint);
  }
}

void finalize_bridge_data(napi_env env, void* data, void* hint) {
  auto bridgeState = (ObjCBridgeState*)data;
  delete bridgeState;
}

#if defined(TARGET_ENGINE_HERMES)
napi_value runObjectFinalizer(napi_env env, napi_callback_info info) {
  napi_value token = nullptr;
  size_t argc = 1;
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, &token, nullptr, &data);

  uint64_t rawContext = 0;
  bool lossless = false;
  if (argc == 1 && napi_get_value_bigint_uint64(env, token, &rawContext, &lossless) == napi_ok &&
      lossless) {
    auto* bridgeState = static_cast<ObjCBridgeState*>(data);
    auto* context = reinterpret_cast<JSObjectFinalizerContext*>(
        static_cast<uintptr_t>(rawContext));
    if (IsBridgeStateLive(bridgeState, bridgeState != nullptr ? bridgeState->lifetimeToken : 0) &&
        bridgeState->takeObjectFinalizer(context)) {
      finalize_objc_object(env, context, nullptr);
    }
  }

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}
#endif

MDMetadataReader* loadMetadataFromFile(const char* metadata_path) {
  const char* path = metadata_path != nullptr ? metadata_path : "metadata.nsmd";
  auto f = fopen(path, "rb");
  if (f == nullptr) {
    fprintf(stderr, "metadata.nsmd not found: %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    fprintf(stderr, "metadata.nsmd is empty: %s\n", path);
    exit(1);
  }

  auto buffer = static_cast<uint8_t*>(malloc(static_cast<size_t>(size)));
  if (buffer == nullptr) {
    fclose(f);
    fprintf(stderr, "failed to allocate metadata buffer: %s\n", path);
    exit(1);
  }

  const size_t bytesRead = fread(buffer, 1, static_cast<size_t>(size), f);
  fclose(f);
  if (bytesRead != static_cast<size_t>(size)) {
    free(buffer);
    fprintf(stderr, "failed to read metadata: %s\n", path);
    exit(1);
  }

  return new MDMetadataReader(buffer, true);
}

inline bool hasNamedProperty(napi_env env, napi_value object, const char* name) {
  bool hasProperty = false;
  napi_has_named_property(env, object, name, &hasProperty);
  return hasProperty;
}

inline bool isFunctionValue(napi_env env, napi_value value) {
  if (value == nullptr) {
    return false;
  }
  napi_valuetype valueType = napi_undefined;
  if (napi_typeof(env, value, &valueType) != napi_ok) {
    return false;
  }
  if (valueType == napi_function) {
    return true;
  }

  if (valueType != napi_object) {
    return false;
  }

  napi_value instance = nullptr;
  napi_status status = napi_new_instance(env, value, 0, nullptr, &instance);
  if (status == napi_ok) {
    return true;
  }

  bool hasPendingException = false;
  if (napi_is_exception_pending(env, &hasPendingException) == napi_ok && hasPendingException) {
    napi_value exception = nullptr;
    napi_get_and_clear_last_exception(env, &exception);
  }
  return false;
}

inline void clearPendingException(napi_env env) {
  bool hasPendingException = false;
  if (napi_is_exception_pending(env, &hasPendingException) == napi_ok && hasPendingException) {
    napi_value exception = nullptr;
    napi_get_and_clear_last_exception(env, &exception);
  }
}

inline bool isConstructableValue(napi_env env, napi_value value) {
  napi_value instance = nullptr;
  napi_status status = napi_new_instance(env, value, 0, nullptr, &instance);
  if (status == napi_ok) {
    return true;
  }

  clearPendingException(env);
  return false;
}

inline bool hasConstructableNamedProperty(napi_env env, napi_value global, const char* name) {
  if (!hasNamedProperty(env, global, name)) {
    return false;
  }

  napi_value value = nullptr;
  if (napi_get_named_property(env, global, name, &value) != napi_ok || value == nullptr) {
    clearPendingException(env);
    return false;
  }

  return isConstructableValue(env, value);
}

inline void defineGlobalValue(napi_env env, napi_value global, const char* name, napi_value value) {
  if (name == nullptr || value == nullptr) {
    return;
  }

  if (napi_set_named_property(env, global, name, value) == napi_ok) {
    return;
  }
  clearPendingException(env);

  napi_property_descriptor prop = {
      .utf8name = name,
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = value,
      .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
      .data = nullptr,
  };
  if (napi_define_properties(env, global, 1, &prop) != napi_ok) {
    clearPendingException(env);
  }
}

inline bool defineConstructableGlobalValue(napi_env env, napi_value global, const char* name,
                                           napi_value value) {
  if (!isConstructableValue(env, value)) {
    return false;
  }

  if (napi_set_named_property(env, global, name, value) == napi_ok &&
      hasConstructableNamedProperty(env, global, name)) {
    return true;
  }
  clearPendingException(env);

  napi_property_descriptor prop = {
      .utf8name = name,
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = value,
      .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
      .data = nullptr,
  };
  if (napi_define_properties(env, global, 1, &prop) == napi_ok &&
      hasConstructableNamedProperty(env, global, name)) {
    return true;
  }
  clearPendingException(env);

  napi_value key = nullptr;
  napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &key);
  if (key != nullptr) {
    bool deleted = false;
    if (napi_delete_property(env, global, key, &deleted) == napi_ok && deleted) {
      if (napi_define_properties(env, global, 1, &prop) == napi_ok &&
          hasConstructableNamedProperty(env, global, name)) {
        return true;
      }
      clearPendingException(env);
      if (napi_set_named_property(env, global, name, value) == napi_ok &&
          hasConstructableNamedProperty(env, global, name)) {
        return true;
      }
      clearPendingException(env);
    } else {
      clearPendingException(env);
    }
  }

  return hasConstructableNamedProperty(env, global, name);
}

inline std::string buildStructEncoding(StructInfo* info) {
  if (info == nullptr || info->name == nullptr) {
    return "";
  }

  std::string encoding = "{";
  encoding += info->name;
  encoding += "=";
  for (const auto& field : info->fields) {
    if (field.type == nullptr) {
      return "";
    }
    field.type->encode(&encoding);
  }
  encoding += "}";
  return encoding;
}

inline void setTypeEncodingSymbol(napi_env env, napi_value value, const std::string& encoding) {
  if (value == nullptr || encoding.empty()) {
    return;
  }

  napi_value typeSymbol = jsSymbolFor(env, "type");
  napi_value encodedValue = nullptr;
  napi_create_string_utf8(env, encoding.c_str(), NAPI_AUTO_LENGTH, &encodedValue);
  if (typeSymbol != nullptr && encodedValue != nullptr) {
    napi_set_property(env, value, typeSymbol, encodedValue);
  }
}

inline void registerStructAlias(napi_env env, napi_value global, ObjCBridgeState* bridgeState,
                                const char* aliasName,
                                std::initializer_list<const char*> candidates) {
  if (bridgeState == nullptr || aliasName == nullptr) {
    return;
  }

  if (hasNamedProperty(env, global, aliasName)) {
    napi_value existing = nullptr;
    if (napi_get_named_property(env, global, aliasName, &existing) == napi_ok &&
        isFunctionValue(env, existing)) {
      return;
    }
  }

  for (const char* candidate : candidates) {
    if (candidate == nullptr || candidate[0] == '\0') {
      continue;
    }

    auto structIt = bridgeState->structOffsets.find(candidate);
    if (structIt != bridgeState->structOffsets.end()) {
      StructInfo* info = bridgeState->getStructInfo(env, structIt->second);
      if (info != nullptr) {
        napi_value cls = StructObject::getJSClass(env, info);
        if (isFunctionValue(env, cls)) {
          setTypeEncodingSymbol(env, cls, buildStructEncoding(info));
          defineGlobalValue(env, global, aliasName, cls);
          return;
        }
      }
    }

    if (hasNamedProperty(env, global, candidate)) {
      napi_value source = nullptr;
      if (napi_get_named_property(env, global, candidate, &source) == napi_ok &&
          isFunctionValue(env, source)) {
        defineGlobalValue(env, global, aliasName, source);
        return;
      }
    }
  }
}

inline void ensureSyntheticCGPoint(napi_env env, napi_value global,
                                   ObjCBridgeState* bridgeState) {
  if (hasConstructableNamedProperty(env, global, "CGPoint")) {
    return;
  }

  StructInfo*& syntheticInfo = bridgeState->syntheticCGPointInfo;
  if (syntheticInfo == nullptr) {
    syntheticInfo = new StructInfo();
    syntheticInfo->name = strdup("CGPoint");
    syntheticInfo->size = sizeof(double) * 2;
    syntheticInfo->jsClass = nullptr;

    const char* doubleEncodingX = "d";
    const char* doubleEncodingY = "d";

    StructFieldInfo fieldX;
    fieldX.name = strdup("x");
    fieldX.offset = 0;
    fieldX.type = TypeConv::Make(env, &doubleEncodingX);
    syntheticInfo->fields.push_back(fieldX);

    StructFieldInfo fieldY;
    fieldY.name = strdup("y");
    fieldY.offset = sizeof(double);
    fieldY.type = TypeConv::Make(env, &doubleEncodingY);
    syntheticInfo->fields.push_back(fieldY);
  }

  napi_value cls = StructObject::getJSClass(env, syntheticInfo);
  if (!isFunctionValue(env, cls)) {
    return;
  }

  setTypeEncodingSymbol(env, cls, "{CGPoint=dd}");
  defineConstructableGlobalValue(env, global, "CGPoint", cls);
}

inline void ensureConstructableStructAlias(napi_env env, napi_value global,
                                           ObjCBridgeState* bridgeState, const char* aliasName,
                                           std::initializer_list<const char*> candidates) {
  if (bridgeState == nullptr || aliasName == nullptr) {
    return;
  }

  if (hasConstructableNamedProperty(env, global, aliasName)) {
    return;
  }

  for (const char* candidate : candidates) {
    if (candidate == nullptr || candidate[0] == '\0') {
      continue;
    }

    if (hasNamedProperty(env, global, candidate)) {
      napi_value value = nullptr;
      if (napi_get_named_property(env, global, candidate, &value) == napi_ok &&
          defineConstructableGlobalValue(env, global, aliasName, value)) {
        return;
      }
      clearPendingException(env);
    }

    auto structIt = bridgeState->structOffsets.find(candidate);
    if (structIt != bridgeState->structOffsets.end()) {
      StructInfo* info = bridgeState->getStructInfo(env, structIt->second);
      if (info != nullptr) {
        napi_value cls = StructObject::getJSClass(env, info);
        if (defineConstructableGlobalValue(env, global, aliasName, cls)) {
          setTypeEncodingSymbol(env, cls, buildStructEncoding(info));
          return;
        }
      }
    }
  }
}

inline void installMacCompatShim(napi_env env) {
  const char* script = R"(
    (function (globalObject) {
      ["CGPoint", "CGSize", "CGRect"].forEach(function (name) {
        const constructor = globalObject[name + "Struct"];
        if (typeof globalObject[name] !== "function" &&
            typeof constructor === "function") {
          Object.defineProperty(globalObject, name, {
            configurable: true,
            enumerable: true,
            value: constructor
          });
        }
      });

      if (typeof globalObject.UIColor === "undefined" &&
          typeof globalObject.NSColor !== "undefined") {
        globalObject.UIColor = globalObject.NSColor;
      }

      const colorCtor = globalObject.UIColor || globalObject.NSColor;
      if (!colorCtor || !colorCtor.prototype) {
        return;
      }

      if (typeof colorCtor.prototype.initWithRedGreenBlueAlpha === "function") {
        return;
      }

      colorCtor.prototype.initWithRedGreenBlueAlpha = function (red, green, blue, alpha) {
        if (typeof this.initWithSRGBRedGreenBlueAlpha === "function") {
          return this.initWithSRGBRedGreenBlueAlpha(red, green, blue, alpha);
        }
        if (typeof this.initWithCalibratedRedGreenBlueAlpha === "function") {
          return this.initWithCalibratedRedGreenBlueAlpha(red, green, blue, alpha);
        }
        if (typeof colorCtor.colorWithSRGBRedGreenBlueAlpha === "function") {
          return colorCtor.colorWithSRGBRedGreenBlueAlpha(red, green, blue, alpha);
        }
        if (typeof colorCtor.colorWithCalibratedRedGreenBlueAlpha === "function") {
          return colorCtor.colorWithCalibratedRedGreenBlueAlpha(red, green, blue, alpha);
        }
        return this;
      };
    })(globalThis);
  )";

  napi_value shim = nullptr;
  napi_create_string_utf8(env, script, NAPI_AUTO_LENGTH, &shim);
  if (shim != nullptr) {
    napi_value result = nullptr;
    napi_run_script(env, shim, &result);
  }
}

inline void* resolveSymbolPointer(ObjCBridgeState* bridgeState, const char* symbolName) {
  if (bridgeState == nullptr || symbolName == nullptr || symbolName[0] == '\0') {
    return nullptr;
  }

  void* symbol = dlsym(bridgeState->self_dl, symbolName);
  if (symbol == nullptr) {
    symbol = dlsym(RTLD_DEFAULT, symbolName);
  }
  if (symbol == nullptr) {
    std::string underscored = "_";
    underscored += symbolName;
    symbol = dlsym(bridgeState->self_dl, underscored.c_str());
    if (symbol == nullptr) {
      symbol = dlsym(RTLD_DEFAULT, underscored.c_str());
    }
  }

  return symbol;
}

inline bool unwrapCompatNativeHandle(napi_env env, napi_value value, void** out) {
  return unwrapKnownNativeHandle(env, value, out);
}

inline napi_value createCompatDispatchQueueWrapper(napi_env env, dispatch_queue_t queue) {
  if (queue == nullptr) {
    napi_value nullValue = nullptr;
    napi_get_null(env, &nullValue);
    return nullValue;
  }

  return Pointer::create(env, reinterpret_cast<void*>(queue));
}

inline napi_value compat_dispatch_get_global_queue(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int64_t identifier = 0;
  if (argc > 0) {
    napi_valuetype identifierType = napi_undefined;
    if (napi_typeof(env, argv[0], &identifierType) == napi_ok && identifierType == napi_bigint) {
      bool lossless = false;
      if (napi_get_value_bigint_int64(env, argv[0], &identifier, &lossless) != napi_ok) {
        napi_throw_type_error(env, nullptr,
                              "dispatch_get_global_queue expects a numeric identifier.");
        return nullptr;
      }
    } else {
      napi_value coercedIdentifier = nullptr;
      if (napi_coerce_to_number(env, argv[0], &coercedIdentifier) != napi_ok ||
          napi_get_value_int64(env, coercedIdentifier, &identifier) != napi_ok) {
        napi_throw_type_error(env, nullptr,
                              "dispatch_get_global_queue expects a numeric identifier.");
        return nullptr;
      }
    }
  }

  uint64_t flags = 0;
  if (argc > 1) {
    napi_valuetype flagsType = napi_undefined;
    if (napi_typeof(env, argv[1], &flagsType) == napi_ok && flagsType == napi_bigint) {
      bool lossless = false;
      if (napi_get_value_bigint_uint64(env, argv[1], &flags, &lossless) != napi_ok) {
        napi_throw_type_error(env, nullptr, "dispatch_get_global_queue expects numeric flags.");
        return nullptr;
      }
    } else {
      napi_value coercedFlags = nullptr;
      int64_t signedFlags = 0;
      if (napi_coerce_to_number(env, argv[1], &coercedFlags) != napi_ok ||
          napi_get_value_int64(env, coercedFlags, &signedFlags) != napi_ok) {
        napi_throw_type_error(env, nullptr, "dispatch_get_global_queue expects numeric flags.");
        return nullptr;
      }
      flags = static_cast<uint64_t>(signedFlags);
    }
  }

  return createCompatDispatchQueueWrapper(env, dispatch_get_global_queue(identifier, flags));
}

inline napi_value compat_dispatch_get_current_queue(napi_env env, napi_callback_info info) {
  (void)info;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return createCompatDispatchQueueWrapper(env, dispatch_get_current_queue());
#pragma clang diagnostic pop
}

inline napi_value compat_dispatch_async(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 2) {
    napi_throw_type_error(env, nullptr, "dispatch_async expects a queue and callback.");
    return nullptr;
  }

  void* queueHandle = nullptr;
  if (!unwrapCompatNativeHandle(env, argv[0], &queueHandle) || queueHandle == nullptr) {
    napi_throw_type_error(env, nullptr, "dispatch_async expects a native queue handle.");
    return nullptr;
  }

  napi_valuetype callbackType = napi_undefined;
  if (napi_typeof(env, argv[1], &callbackType) != napi_ok || callbackType != napi_function) {
    napi_throw_type_error(env, nullptr, "dispatch_async expects a function callback.");
    return nullptr;
  }

  auto closure = new Closure(env, std::string("v"), true);
  id block = registerBlock(env, closure, argv[1]);
  dispatch_block_t dispatchBlock = (dispatch_block_t)block;

  dispatch_async(reinterpret_cast<dispatch_queue_t>(queueHandle), dispatchBlock);
  [block release];

  napi_value undefinedValue = nullptr;
  napi_get_undefined(env, &undefinedValue);
  return undefinedValue;
}

inline void registerCompatFunctionIfMissing(napi_env env, napi_value global,
                                            ObjCBridgeState* bridgeState, const char* functionName,
                                            const char* encoding) {
  if (hasNamedProperty(env, global, functionName)) {
    return;
  }

  void* fn = resolveSymbolPointer(bridgeState, functionName);
  if (fn == nullptr && strcmp(functionName, "CC_SHA256") == 0) {
    void* commonCrypto = dlopen("/usr/lib/system/libcommonCrypto.dylib", RTLD_NOW | RTLD_LOCAL);
    if (commonCrypto != nullptr) {
      fn = dlsym(commonCrypto, functionName);
      if (fn == nullptr) {
        fn = dlsym(commonCrypto, "_CC_SHA256");
      }
    }
  }

  if (fn == nullptr) {
    return;
  }

  napi_value wrapper = FunctionPointer::wrapWithEncoding(env, fn, encoding, false);
  if (wrapper != nullptr) {
    napi_set_named_property(env, global, functionName, wrapper);
  }
}

inline void registerCompatFunction(napi_env env, napi_value global, const char* functionName,
                                   napi_callback callback) {
  napi_value wrapper = nullptr;
  napi_create_function(env, functionName, NAPI_AUTO_LENGTH, callback, nullptr, &wrapper);
  if (wrapper != nullptr) {
    napi_value key = nullptr;
    napi_create_string_utf8(env, functionName, NAPI_AUTO_LENGTH, &key);
    if (key != nullptr) {
      bool deleted = false;
      napi_delete_property(env, global, key, &deleted);
      clearPendingException(env);
    }
    defineGlobalValue(env, global, functionName, wrapper);
  }
}

void registerLegacyCompatGlobals(napi_env env, napi_value global, ObjCBridgeState* bridgeState) {
#if TARGET_OS_OSX
  registerStructAlias(env, global, bridgeState, "CGPoint",
                      {"CGPoint", "_CGPoint", "NSPoint", "_NSPoint"});
  registerStructAlias(env, global, bridgeState, "CGSize",
                      {"CGSize", "_CGSize", "NSSize", "_NSSize"});
  registerStructAlias(env, global, bridgeState, "CGRect",
                      {"CGRect", "_CGRect", "NSRect", "_NSRect"});
  ensureSyntheticCGPoint(env, global, bridgeState);
  ensureConstructableStructAlias(
      env, global, bridgeState, "CGPoint",
      {"CGPointStruct", "NSPoint", "NSPointStruct", "_CGPoint", "_NSPoint", "CGPoint"});
  installMacCompatShim(env);
#endif

  // CommonCrypto compatibility used by historical runtime tests and apps.
  registerCompatFunctionIfMissing(env, global, bridgeState, "CC_SHA256", "^C^vQ^C");
  registerCompatFunctionIfMissing(env, global, bridgeState, "CGColorGetComponents", "^d^v");

  // Force known-good libdispatch globals on macOS. The metadata path can resolve these with an
  // incompatible call shape, which crashes when tests dispatch timers from a background queue.
  registerCompatFunction(env, global, "dispatch_async", compat_dispatch_async);
  registerCompatFunction(env, global, "dispatch_get_current_queue",
                         compat_dispatch_get_current_queue);
  registerCompatFunction(env, global, "dispatch_get_global_queue",
                         compat_dispatch_get_global_queue);
}

ObjCBridgeState::ObjCBridgeState(napi_env env, const char* metadata_path,
                                 const void* metadata_ptr) {
  this->env = env;
  napi_set_instance_data(env, this, finalize_bridge_data, nil);
  lifetimeToken = RegisterBridgeState(this);
  trackedObjectLiveness = [[NSMutableSet alloc] init];

  self_dl = dlopen(nullptr, RTLD_NOW);

  if (metadata_ptr && *((const char*)metadata_ptr) != '\0') {
#ifdef EMBED_METADATA_SIZE
    // NSLog(@"Ignoring metadata pointer due to embedded metadata");
    metadata = new MDMetadataReader((void*)embedded_metadata);
#else
    // NSLog(@"Using metadata from pointer: %p", metadata_ptr);
    metadata = new MDMetadataReader((void*)metadata_ptr);
#endif
  } else {
#ifdef EMBED_METADATA_SIZE
    if (metadata_path != nullptr) {
      // NSLog(@"Loading metadata from file: %s", metadata_path);
      metadata = loadMetadataFromFile(metadata_path);
    } else {
      // NSLog(@"Using embedded metadata");
      metadata = new MDMetadataReader((void*)embedded_metadata);
    }
#else
    unsigned long segmentSize = 0;
    auto segmentData = getsegmentdata((const mach_header_64*)_dyld_get_image_header(0),
                                      "__objc_metadata", &segmentSize);
    if (segmentData != nullptr) {
      metadata = new MDMetadataReader(segmentData);
    } else {
      metadata = loadMetadataFromFile(metadata_path);
    }
#endif
  }

  // objc_autoreleasePool = objc_autoreleasePoolPush();
}

ObjCBridgeState::~ObjCBridgeState() {
  drainFinalizers(true);
#if defined(TARGET_ENGINE_HERMES)
  auto pendingObjectFinalizers = std::move(objectFinalizers);
  objectFinalizers.clear();
  if (env != nullptr && objectFinalizationRegistry != nullptr) {
    napi_delete_reference(env, objectFinalizationRegistry);
    objectFinalizationRegistry = nullptr;
  }
  for (auto* context : pendingObjectFinalizers) {
    if (context == nullptr) {
      continue;
    }
    unregisterObjectIfRefMatches(context->object, context->ref);
    if (env != nullptr && context->ref != nullptr) {
      napi_delete_reference(env, context->ref);
    }
    delete context;
  }
#endif
  UnregisterBridgeState(this);

  auto deleteRef = [&](napi_ref& ref) {
    if (env != nullptr && ref != nullptr) {
      napi_delete_reference(env, ref);
      ref = nullptr;
    }
  };

  for (auto& pair : constructorsByPointer) {
    deleteRef(pair.second);
  }
  constructorsByPointer.clear();

  for (auto& pair : pointerCache) {
    deleteRef(pair.second.ref);
  }
  pointerCache.clear();

  for (auto& frame : roundTripCacheFrames) {
    for (auto& entry : frame) {
      ObjCBridgeState::releaseRoundTripEntry(env, entry.second);
    }
  }
  roundTripCacheFrames.clear();

  for (auto& entry : recentRoundTripCache) {
    ObjCBridgeState::releaseRoundTripEntry(env, entry.second);
  }
  recentRoundTripCache.clear();

  for (auto& entry : handleObjectRefs) {
    if (entry.second.ownsRef) {
      deleteRef(entry.second.ref);
    }
  }
  handleObjectRefs.clear();

  recentObjectWrappers.clear();

  std::unordered_set<napi_ref> classAndProtocolConstructorRefs;
  classAndProtocolConstructorRefs.reserve(classes.size() + protocols.size());
  for (const auto& pair : classes) {
    if (pair.second != nullptr && pair.second->constructor != nullptr) {
      classAndProtocolConstructorRefs.insert(pair.second->constructor);
    }
  }
  for (const auto& pair : protocols) {
    if (pair.second != nullptr && pair.second->constructor != nullptr) {
      classAndProtocolConstructorRefs.insert(pair.second->constructor);
    }
  }
  for (auto& pair : mdValueCache) {
    napi_ref& ref = pair.second;
    if (ref != nullptr &&
        classAndProtocolConstructorRefs.find(ref) == classAndProtocolConstructorRefs.end()) {
      deleteRef(ref);
    }
  }
  mdValueCache.clear();

  deleteRef(pointerClass);
  deleteRef(referenceClass);
  deleteRef(functionReferenceClass);
  deleteRef(createNativeProxy);
  deleteRef(createFastEnumeratorIterator);
  deleteRef(transferOwnershipToNative);

  // Clean up cached Cif objects
  for (auto& pair : cifs) {
    delete pair.second;
  }
  cifs.clear();

  for (auto& pair : mdMethodSignatureCache) {
    delete pair.second;
  }
  mdMethodSignatureCache.clear();

  for (auto& pair : mdBlockSignatureCache) {
    delete pair.second;
  }
  mdBlockSignatureCache.clear();

  // Clean up ObjCClass objects
  for (auto& pair : classes) {
    delete pair.second;
  }
  classes.clear();

  // Clean up ObjCProtocol objects
  for (auto& pair : protocols) {
    delete pair.second;
  }
  protocols.clear();

  // Clean up StructInfo objects
  for (auto& pair : structInfoCache) {
    deleteRef(pair.second->jsClass);
    delete pair.second;
  }
  structInfoCache.clear();

  if (syntheticCGPointInfo != nullptr) {
    deleteRef(syntheticCGPointInfo->jsClass);
    std::free(syntheticCGPointInfo->name);
    for (auto& field : syntheticCGPointInfo->fields) {
      std::free(field.name);
    }
    delete syntheticCGPointInfo;
    syntheticCGPointInfo = nullptr;
  }

  // Clean up CFunction objects
  for (auto& pair : cFunctionCache) {
    delete pair.second;
  }
  cFunctionCache.clear();

  for (auto& pair : mdFunctionSignatureCache) {
    delete pair.second;
  }
  mdFunctionSignatureCache.clear();

  NSMutableSet* trackedObjectTable = static_cast<NSMutableSet*>(trackedObjectLiveness);
  trackedObjectLiveness = nullptr;
  [trackedObjectTable release];

  clearStructTypeCaches(env);

  // if (objc_autoreleasePool != nullptr)
  //   objc_autoreleasePoolPop(objc_autoreleasePool);

  delete metadata;
  dlclose(self_dl);
}

napi_value ObjCBridgeState::proxyNativeObject(napi_env env, napi_value object, id nativeObject) {
  NS_OBJC_NAPI_PREAMBLE

  napi_value result = object;
  const bool nativeIsArray = [nativeObject isKindOfClass:NSArray.class];
  bool shouldProxyArray = nativeIsArray && !envFlagEnabled("NS_DISABLE_NAPI_ARRAY_PROXY");
  if (shouldProxyArray) {
    napi_value factory = get_ref_value(env, createNativeProxy);
    napi_value transferOwnershipFunc = get_ref_value(env, this->transferOwnershipToNative);
    napi_value global;
    napi_value args[3] = {object, nullptr, transferOwnershipFunc};
    napi_get_boolean(env, true, &args[1]);
    napi_get_global(env, &global);
    napi_call_function(env, global, factory, 3, args, &result);
  }

  napi_value nativePointer = Pointer::create(env, nativeObject);
  if (nativePointer != nullptr) {
    napi_set_named_property(env, result, kNativePointerProperty, nativePointer);
  }
  napi_ref ref = nullptr;
  napi_wrap(env, result, nativeObject, nullptr, nullptr, nullptr);

  auto* finalizerContext = new JSObjectFinalizerContext{
      .bridgeState = this,
      .bridgeStateToken = lifetimeToken,
      .object = nativeObject,
      .ref = nullptr,
  };
#if defined(TARGET_ENGINE_HERMES)
  NAPI_GUARD(napi_create_reference(env, result, 0, &ref)) {
    delete finalizerContext;
    NAPI_THROW_LAST_ERROR
    return nullptr;
  }
  finalizerContext->ref = ref;
  if (!registerObjectFinalizer(env, result, finalizerContext)) {
    napi_delete_reference(env, ref);
    delete finalizerContext;
    return nullptr;
  }
#else
  NAPI_GUARD(
      napi_add_finalizer(env, result, finalizerContext, finalize_objc_object, nullptr, &ref)) {
    delete finalizerContext;
    NAPI_THROW_LAST_ERROR
    return nullptr;
  }
  finalizerContext->ref = ref;
#endif

  storeObjectRef(nativeObject, ref);
  cacheHandleObjectRef(env, nativeObject, ref);
  cacheRecentObjectWrapper(env, nativeObject, result, ref);
  attachObjectLifecycleAssociation(env, nativeObject);
  trackObject(nativeObject);

  return result;
}

#if defined(TARGET_ENGINE_HERMES)
bool ObjCBridgeState::registerObjectFinalizer(napi_env env, napi_value object,
                                              JSObjectFinalizerContext* context) {
  if (objectFinalizationRegistry == nullptr) {
    napi_value global = nullptr;
    napi_value constructor = nullptr;
    napi_value callback = nullptr;
    napi_value registry = nullptr;
    if (napi_get_global(env, &global) != napi_ok ||
        napi_get_named_property(env, global, "FinalizationRegistry", &constructor) != napi_ok ||
        napi_create_function(env, "", 0, runObjectFinalizer, this, &callback) != napi_ok ||
        napi_new_instance(env, constructor, 1, &callback, &registry) != napi_ok ||
        napi_create_reference(env, registry, 1, &objectFinalizationRegistry) != napi_ok) {
      return false;
    }
  }

  napi_value registry = get_ref_value(env, objectFinalizationRegistry);
  napi_value registerFunction = nullptr;
  napi_value token = nullptr;
  napi_value args[2] = {object, nullptr};
  if (registry == nullptr ||
      napi_get_named_property(env, registry, "register", &registerFunction) != napi_ok ||
      napi_create_bigint_uint64(env, reinterpret_cast<uintptr_t>(context), &token) != napi_ok) {
    return false;
  }
  args[1] = token;
  if (napi_call_function(env, registry, registerFunction, 2, args, nullptr) != napi_ok) {
    return false;
  }

  objectFinalizers.insert(context);
  return true;
}

bool ObjCBridgeState::takeObjectFinalizer(JSObjectFinalizerContext* context) {
  return context != nullptr && objectFinalizers.erase(context) != 0;
}
#endif

void ObjCBridgeState::trackObject(id object) noexcept {
  if (object == nil) {
    return;
  }

  NSMutableSet* trackedObjectTable = static_cast<NSMutableSet*>(trackedObjectLiveness);
  if (trackedObjectTable == nil) {
    return;
  }

  NSNumber* objectKey = [NSNumber numberWithUnsignedLongLong:NormalizeHandleKey((void*)object)];
  std::lock_guard<std::mutex> lock(objectRefsMutex);
  [trackedObjectTable addObject:objectKey];
}

bool ObjCBridgeState::isTrackedObjectAlive(id object) const noexcept {
  if (object == nil) {
    return false;
  }

  NSMutableSet* trackedObjectTable = static_cast<NSMutableSet*>(trackedObjectLiveness);
  if (trackedObjectTable == nil) {
    return false;
  }

  NSNumber* objectKey = [NSNumber numberWithUnsignedLongLong:NormalizeHandleKey((void*)object)];
  std::lock_guard<std::mutex> lock(objectRefsMutex);
  return [trackedObjectTable containsObject:objectKey];
}

}  // namespace nativescript

using namespace nativescript;

NAPI_FUNCTION(getArrayBuffer) {
  NAPI_CALLBACK_BEGIN(2)

  void* ptr = Pointer::unwrap(env, argv[0])->data;
  int64_t length;
  napi_get_value_int64(env, argv[1], &length);

  napi_value arrayBuffer;
  if (length < 0) {
    napi_throw_error(env, nullptr, "Invalid ArrayBuffer length");
    return nullptr;
  }

  napi_create_external_arraybuffer(env, ptr, static_cast<size_t>(length), nullptr, nullptr,
                                   &arrayBuffer);

  return arrayBuffer;
}

NAPI_FUNCTION(init) {
  NAPI_CALLBACK_BEGIN(1)
  napi_valuetype type;
  napi_typeof(env, argv[0], &type);
  const char* metadata_path = nullptr;
  if (type == napi_string) {
    size_t len;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    metadata_path = (char*)malloc(len + 1);
    napi_get_value_string_utf8(env, argv[0], (char*)metadata_path, len + 1, &len);
  }
  nativescript_init(env, metadata_path, nullptr);
  return nullptr;
}

NAPI_EXPORT NAPI_MODULE_REGISTER {
  const napi_property_descriptor property = NAPI_FUNCTION_DESC(init);
  napi_define_properties(env, exports, 1, &property);
  return exports;
}

NAPI_EXPORT void nativescript_init(void* _env, const char* metadata_path,
                                   const void* metadata_ptr) {
  napi_env env = (napi_env)_env;

  ObjCBridgeState* bridgeState = new ObjCBridgeState(env, metadata_path, metadata_ptr);

  napi_value objc;
  napi_create_object(env, &objc);

  const napi_property_descriptor objcProperties[] = {
      NAPI_FUNCTION_DESC(registerClass),  NAPI_FUNCTION_DESC(registerBlock),
      NAPI_FUNCTION_DESC(import),         NAPI_FUNCTION_DESC(autoreleasepool),
      NAPI_FUNCTION_DESC(getArrayBuffer),
  };

  napi_define_properties(env, objc, 5, objcProperties);

  napi_value global;
  napi_get_global(env, &global);

  const napi_property_descriptor globalProperties[] = {{
                                                           .utf8name = "objc",
                                                           .method = nullptr,
                                                           .getter = nullptr,
                                                           .setter = nullptr,
                                                           .value = objc,
                                                           .attributes = napi_enumerable,
                                                           .data = nullptr,
                                                       },
                                                       {
                                                           .utf8name = "ObjectRef",
                                                           .method = nullptr,
                                                           .getter = nullptr,
                                                           .setter = nullptr,
                                                           .value = defineObjectRefClass(env),
                                                           .attributes = napi_enumerable,
                                                           .data = nullptr,
                                                       },
                                                       {
                                                           .utf8name = "NativeClass",
                                                           .method = JS_registerClass,
                                                           .getter = nullptr,
                                                           .setter = nullptr,
                                                           .value = nullptr,
                                                           .attributes = napi_enumerable,
                                                           .data = nullptr,
                                                       }};

  napi_define_properties(env, global, 3, globalProperties);

  setupObjCClassDecorator(env);

  initProxyFactory(env, bridgeState);
  initFastEnumeratorIteratorFactory(env, bridgeState);

  registerInterop(env, global);
  registerInlineFunctions(env);

  bridgeState->registerVarGlobals(env, global);
  bridgeState->registerEnumGlobals(env, global);
  bridgeState->registerStructGlobals(env, global);
  bridgeState->registerUnionGlobals(env, global);
  bridgeState->registerFunctionGlobals(env, global);
  bridgeState->registerClassGlobals(env, global);
  bridgeState->registerProtocolGlobals(env, global);
  registerLegacyCompatGlobals(env, global, bridgeState);
}
