#include "Block.h"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "Interop.h"
#include "runtime/apple/NativeScriptException.h"
#include "ObjCBridge.h"
#include "SignatureDispatch.h"
#include "TypeConv.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "node_api_util.h"
#include "objc/runtime.h"

struct Block_descriptor_1 {
  unsigned long int reserved;  // NULL
  unsigned long int size;      // sizeof(struct Block_literal_1)
  // optional helper functions
  void (*copy_helper)(void* dst, void* src);  // IFF (1<<25)
  void (*dispose_helper)(void* src);          // IFF (1<<25)
  // required ABI.2010.3.16
  const char* signature;  // IFF (1<<30)
};

struct Block_literal_1 {
  void* isa;  // initialized to &_NSConcreteStackBlock or &_NSConcreteGlobalBlock
  int flags;
  int reserved;
  void* invoke;
  Block_descriptor_1* descriptor;
  // imported variables
  nativescript::Closure* closure;
};

namespace {

constexpr int kBlockNeedsFree = (1 << 24);
constexpr int kBlockHasCopyDispose = (1 << 25);
constexpr int kBlockRefCountOne = (1 << 1);
constexpr int kBlockHasSignature = (1 << 30);

struct BlockJsFunctionEntry {
  napi_ref ref = nullptr;
  napi_env env = nullptr;
  nativescript::ObjCBridgeState* bridgeState = nullptr;
  uint64_t bridgeStateToken = 0;
  std::thread::id jsThreadId;
  CFRunLoopRef jsRunLoop = nullptr;
};

std::unordered_map<void*, BlockJsFunctionEntry> g_blockToJsFunction;
std::mutex g_blockToJsFunctionMutex;

inline bool removeCachedBlockJsFunctionEntry(void* blockPtr, BlockJsFunctionEntry* entry) {
  std::lock_guard<std::mutex> lock(g_blockToJsFunctionMutex);
  auto it = g_blockToJsFunction.find(blockPtr);
  if (it == g_blockToJsFunction.end()) {
    return false;
  }
  if (entry != nullptr) {
    *entry = it->second;
  }
  g_blockToJsFunction.erase(it);
  return true;
}

inline void deleteBlockReferenceOnOwningLoop(const BlockJsFunctionEntry& entry) {
  nativescript::DeleteReferenceOnOwningThread(entry.env, entry.bridgeState, entry.bridgeStateToken,
                                              entry.ref);
}

inline nativescript::BlockPreparedInvoker ensureFunctionPointerPreparedInvoker(
    nativescript::FunctionPointer* ref, nativescript::SignatureCallKind kind) {
  if (ref == nullptr || ref->cif == nullptr || ref->cif->signatureHash == 0) {
    if (ref != nullptr) {
      ref->dispatchLookupCached = true;
      ref->dispatchLookupSignatureHash = 0;
      ref->dispatchId = 0;
      ref->preparedInvoker = nullptr;
    }
    return nullptr;
  }

  if (!ref->dispatchLookupCached ||
      ref->dispatchLookupSignatureHash != ref->cif->signatureHash) {
    ref->dispatchLookupSignatureHash = ref->cif->signatureHash;
    ref->dispatchId =
        nativescript::composeSignatureDispatchId(ref->cif->signatureHash, kind, 0);
    if (kind == nativescript::SignatureCallKind::BlockInvoke) {
      ref->preparedInvoker =
          reinterpret_cast<void*>(nativescript::lookupBlockPreparedInvoker(ref->dispatchId));
    } else {
      ref->preparedInvoker =
          reinterpret_cast<void*>(nativescript::lookupCFunctionPreparedInvoker(ref->dispatchId));
    }
    ref->dispatchLookupCached = true;
  }

  return reinterpret_cast<nativescript::BlockPreparedInvoker>(ref->preparedInvoker);
}

inline const napi_value* prepareFunctionPointerInvocationArgs(napi_env env, nativescript::Cif* cif,
                                                              size_t actualArgc,
                                                              const napi_value* rawArgs,
                                                              napi_value* stackArgs,
                                                              size_t stackCapacity,
                                                              std::vector<napi_value>* heapArgs) {
  if (cif == nullptr || cif->argc == 0) {
    return nullptr;
  }

  if (actualArgc == cif->argc && rawArgs != nullptr) {
    return rawArgs;
  }

  napi_value jsUndefined = nullptr;
  napi_get_undefined(env, &jsUndefined);

  if (cif->argc <= stackCapacity) {
    for (unsigned int i = 0; i < cif->argc; i++) {
      stackArgs[i] = i < actualArgc && rawArgs != nullptr ? rawArgs[i] : jsUndefined;
    }
    return stackArgs;
  }

  heapArgs->assign(cif->argc, jsUndefined);
  const size_t copyArgc = std::min(actualArgc, static_cast<size_t>(cif->argc));
  if (copyArgc > 0 && rawArgs != nullptr) {
    memcpy(heapArgs->data(), rawArgs, copyArgc * sizeof(napi_value));
  }
  return heapArgs->data();
}

napi_value callFunctionPointerAsCFunctionDirect(napi_env env, nativescript::FunctionPointer* ref,
                                                size_t actualArgc,
                                                const napi_value* rawArgs) {
  if (ref == nullptr || ref->cif == nullptr || ref->function == nullptr) {
    napi_throw_error(env, "NativeScriptException", "Missing native function pointer.");
    return nullptr;
  }

  auto cif = ref->cif;
  napi_value stackInvocationArgs[16];
  std::vector<napi_value> heapInvocationArgs;
  const napi_value* invocationArgs = prepareFunctionPointerInvocationArgs(
      env, cif, actualArgc, rawArgs, stackInvocationArgs, 16, &heapInvocationArgs);

  void* stackAValues[16];
  std::vector<void*> heapAValues;
  void** avalues = stackAValues;
  if (cif->argc > 16) {
    heapAValues.resize(cif->argc);
    avalues = heapAValues.data();
  }

  bool shouldFreeAny = false;
  uint8_t stackShouldFree[16] = {};
  std::vector<uint8_t> heapShouldFree;
  uint8_t* shouldFree = stackShouldFree;
  if (cif->argc > 16) {
    heapShouldFree.assign(cif->argc, false);
    shouldFree = heapShouldFree.data();
  }

  for (unsigned int i = 0; i < cif->argc; i++) {
    avalues[i] = cif->avalues[i];
    bool shouldFreeArg = false;
    cif->argTypes[i]->toNative(env, invocationArgs[i], avalues[i], &shouldFreeArg,
                               &shouldFreeAny);
    shouldFree[i] = shouldFreeArg ? 1 : 0;
    if (nativescript::ConsumeNapiArgumentConversionFailure(env)) {
      for (unsigned int converted = 0; converted <= i; converted++) {
        if (shouldFree[converted]) {
          cif->argTypes[converted]->free(env, *((void**)avalues[converted]));
        }
      }
      return nullptr;
    }
  }

  void* rvalue = cif->rvalue;
  auto preparedInvoker =
      ensureFunctionPointerPreparedInvoker(ref, nativescript::SignatureCallKind::CFunction);

  @try {
    if (preparedInvoker != nullptr) {
      preparedInvoker(ref->function, avalues, rvalue);
    } else {
      ffi_call(&cif->cif, FFI_FN(ref->function), rvalue, avalues);
    }
  } @catch (NSException* exception) {
    std::string message = exception.description.UTF8String;
    nativescript::NativeScriptException nativeScriptException(message);
    nativeScriptException.ReThrowToJS(env);
    return nullptr;
  }

  if (shouldFreeAny) {
    for (unsigned int i = 0; i < cif->argc; i++) {
      if (shouldFree[i]) {
        cif->argTypes[i]->free(env, *((void**)avalues[i]));
      }
    }
  }

  return cif->returnType->toJS(env, rvalue);
}

napi_value callFunctionPointerAsBlockDirect(napi_env env, nativescript::FunctionPointer* ref,
                                            size_t actualArgc,
                                            const napi_value* rawArgs) {
  if (ref == nullptr || ref->cif == nullptr || ref->function == nullptr) {
    napi_throw_error(env, "NativeScriptException", "Missing native block pointer.");
    return nullptr;
  }

  auto block = static_cast<Block_literal_1*>(ref->function);
  if (block == nullptr || block->invoke == nullptr) {
    napi_throw_error(env, "NativeScriptException", "Missing native block invoke pointer.");
    return nullptr;
  }

  auto cif = ref->cif;
  napi_value stackInvocationArgs[16];
  std::vector<napi_value> heapInvocationArgs;
  const napi_value* invocationArgs = prepareFunctionPointerInvocationArgs(
      env, cif, actualArgc, rawArgs, stackInvocationArgs, 16, &heapInvocationArgs);

  void* stackAValues[17];
  std::vector<void*> heapAValues;
  void** avalues = stackAValues;
  if (cif->cif.nargs > 17) {
    heapAValues.resize(cif->cif.nargs);
    avalues = heapAValues.data();
  }

  bool shouldFreeAny = false;
  uint8_t stackShouldFree[16] = {};
  std::vector<uint8_t> heapShouldFree;
  uint8_t* shouldFree = stackShouldFree;
  if (cif->argc > 16) {
    heapShouldFree.assign(cif->argc, false);
    shouldFree = heapShouldFree.data();
  }

  avalues[0] = &block;
  for (unsigned int i = 0; i < cif->argc; i++) {
    avalues[i + 1] = cif->avalues[i];
    bool shouldFreeArg = false;
    cif->argTypes[i]->toNative(env, invocationArgs[i], avalues[i + 1], &shouldFreeArg,
                               &shouldFreeAny);
    shouldFree[i] = shouldFreeArg ? 1 : 0;
    if (nativescript::ConsumeNapiArgumentConversionFailure(env)) {
      for (unsigned int converted = 0; converted <= i; converted++) {
        if (shouldFree[converted]) {
          cif->argTypes[converted]->free(env, *((void**)avalues[converted + 1]));
        }
      }
      return nullptr;
    }
  }

  void* rvalue = cif->rvalue;
  nativescript::BlockPreparedInvoker preparedInvoker = ensureFunctionPointerPreparedInvoker(
      ref, nativescript::SignatureCallKind::BlockInvoke);

  @try {
    if (preparedInvoker != nullptr) {
      preparedInvoker(block->invoke, avalues, rvalue);
    } else {
      ffi_call(&cif->cif, FFI_FN(block->invoke), rvalue, avalues);
    }
  } @catch (NSException* exception) {
    std::string message = exception.description.UTF8String;
    nativescript::NativeScriptException nativeScriptException(message);
    nativeScriptException.ReThrowToJS(env);
    return nullptr;
  }

  if (shouldFreeAny) {
    for (unsigned int i = 0; i < cif->argc; i++) {
      if (shouldFree[i]) {
        cif->argTypes[i]->free(env, *((void**)avalues[i + 1]));
      }
    }
  }

  return cif->returnType->toJS(env, rvalue);
}

void block_copy(void* dest, void* src) {
  auto dst = static_cast<Block_literal_1*>(dest);
  auto source = static_cast<Block_literal_1*>(src);
  dst->closure = source->closure;
  if (dst->closure != nullptr) {
    dst->closure->retain();
  }
}

void block_release(void* src) {
  auto block = static_cast<Block_literal_1*>(src);
  if (block == nullptr) {
    return;
  }

  BlockJsFunctionEntry entry;
  if (removeCachedBlockJsFunctionEntry(block, &entry)) {
    deleteBlockReferenceOnOwningLoop(entry);
  }

  if (block->closure != nullptr) {
    block->closure->release();
  }
  block->closure = nullptr;
}

Block_descriptor_1 kBlockDescriptor = {
    .reserved = 0,
    .size = sizeof(Block_literal_1),
    .copy_helper = block_copy,
    .dispose_helper = block_release,
    .signature = "v@?",
};

inline napi_value getCachedBlockJsFunction(napi_env env, void* blockPtr) {
  BlockJsFunctionEntry removedEntry;
  bool shouldDelete = false;
  napi_value value = nullptr;

  {
    std::lock_guard<std::mutex> lock(g_blockToJsFunctionMutex);
    auto it = g_blockToJsFunction.find(blockPtr);
    if (it == g_blockToJsFunction.end()) {
      return nullptr;
    }

    value = nativescript::get_ref_value(env, it->second.ref);
    if (value == nullptr) {
      removedEntry = it->second;
      g_blockToJsFunction.erase(it);
      shouldDelete = true;
    }
  }

  if (shouldDelete) {
    deleteBlockReferenceOnOwningLoop(removedEntry);
  }

  return value;
}

inline void cacheBlockJsFunction(napi_env env, void* blockPtr, napi_value jsFunction,
                                 nativescript::Closure* closure) {
  if (blockPtr == nullptr || jsFunction == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_blockToJsFunctionMutex);
  if (g_blockToJsFunction.find(blockPtr) != g_blockToJsFunction.end()) {
    return;
  }
  // Keep this weak so callback identity can round-trip without preventing GC.
  BlockJsFunctionEntry entry;
  entry.ref = nativescript::make_ref(env, jsFunction, 0);
  entry.env = env;
  entry.bridgeState = nativescript::ObjCBridgeState::InstanceData(env);
  entry.bridgeStateToken = entry.bridgeState != nullptr ? entry.bridgeState->lifetimeToken : 0;
  entry.jsThreadId = closure != nullptr ? closure->jsThreadId : std::this_thread::get_id();
  entry.jsRunLoop = closure != nullptr ? closure->jsRunLoop : CFRunLoopGetCurrent();
  g_blockToJsFunction[blockPtr] = entry;
}

}  // namespace

namespace {
void block_finalize_now(napi_env env, void* data, void* hint) {
  (void)env;
  (void)hint;
  auto block = static_cast<Block_literal_1*>(data);
  if (block == nullptr) {
    return;
  }

  BlockJsFunctionEntry entry;
  if (removeCachedBlockJsFunctionEntry(block, &entry)) {
    deleteBlockReferenceOnOwningLoop(entry);
  }

  if (block->closure != nullptr) {
    block->closure->release();
  }
  block->closure = nullptr;

  free(block);
}

void finalizeFunctionPointerNow(napi_env env, void* finalize_data, void* finalize_hint) {
  auto ref = static_cast<nativescript::FunctionPointer*>(finalize_data);
  if (ref == nullptr) {
    return;
  }
  if (ref->ownsCif && ref->cif != nullptr) {
    delete ref->cif;
    ref->cif = nullptr;
  }
  delete ref;
}
}  // namespace

void block_finalize(napi_env env, void* data, void* hint) {
  if (nativescript::PostFinalizer(env, block_finalize_now, data, hint)) {
    return;
  }

  block_finalize_now(env, data, hint);
}

namespace nativescript {

void* mallocBlockISA = nullptr;
void* stackBlockISA = nullptr;

id registerBlock(napi_env env, Closure* closure, napi_value callback) {
  auto block = static_cast<Block_literal_1*>(malloc(sizeof(Block_literal_1)));
  memset(block, 0, sizeof(Block_literal_1));

  if (mallocBlockISA == nullptr) {
    mallocBlockISA = dlsym(RTLD_DEFAULT, "_NSConcreteMallocBlock");
  }

  if (stackBlockISA == nullptr) {
    stackBlockISA = dlsym(RTLD_DEFAULT, "_NSConcreteStackBlock");
  }

  block->isa = mallocBlockISA != nullptr ? mallocBlockISA : stackBlockISA;
  block->flags = kBlockNeedsFree | kBlockHasCopyDispose | kBlockRefCountOne | kBlockHasSignature;
  block->reserved = 0;
  block->invoke = closure->fnptr;
  block->descriptor = &kBlockDescriptor;
  block->closure = closure;

  closure->func = make_ref(env, callback, 1);

  // Expose the native block pointer on the JS callback so interop.handleof/sizeof
  // can resolve pointers for blocks that round-trip through Objective-C.
  napi_value ptrExternal;
  napi_create_external(env, block, nullptr, nullptr, &ptrExternal);
  napi_set_named_property(env, callback, "__ns_native_ptr", ptrExternal);

  auto bridgeState = ObjCBridgeState::InstanceData(env);

#ifndef ENABLE_JS_RUNTIME
  if (napiSupportsThreadsafeFunctions(bridgeState->self_dl)) {
    napi_value workName;
    napi_create_string_utf8(env, "Block", NAPI_AUTO_LENGTH, &workName);
    napi_create_threadsafe_function(env, nullptr, nullptr, workName, 0, 1, nullptr, nullptr,
                                    closure, Closure::callBlockFromMainThread, &closure->tsfn);
    if (closure->tsfn) napi_unref_threadsafe_function(env, closure->tsfn);
  }
#endif  // ENABLE_JS_RUNTIME

  cacheBlockJsFunction(env, block, callback, closure);

  return (id)block;
}

napi_value getCachedBlockCallback(napi_env env, void* blockPtr) {
  return getCachedBlockJsFunction(env, blockPtr);
}

bool isObjCBlockObject(id obj) {
  if (obj == nil) {
    return false;
  }

  Class cls = object_getClass(obj);
  if (cls == nil) {
    return false;
  }

  static thread_local std::unordered_map<Class, bool> blockClassCache;
  auto cached = blockClassCache.find(cls);
  if (cached != blockClassCache.end()) {
    return cached->second;
  }

  const char* className = class_getName(cls);
  if (className == nullptr) {
    blockClassCache.emplace(cls, false);
    return false;
  }

  // Runtime block classes are typically internal names like
  // __NSGlobalBlock__, __NSMallocBlock__, __NSStackBlock__.
  bool isBlock = className[0] == '_' && className[1] == '_' && strstr(className, "Block") != nullptr;
  blockClassCache.emplace(cls, isBlock);
  return isBlock;
}

const char* getObjCBlockSignature(void* blockPtr) {
  auto block = static_cast<Block_literal_1*>(blockPtr);
  if (block == nullptr || block->descriptor == nullptr) {
    return nullptr;
  }

  if ((block->flags & kBlockHasSignature) == 0) {
    return nullptr;
  }

  // Descriptor layout:
  // unsigned long reserved;
  // unsigned long size;
  // [copy_helper, dispose_helper] if BLOCK_HAS_COPY_DISPOSE
  // const char* signature if BLOCK_HAS_SIGNATURE
  auto descriptorCursor = reinterpret_cast<uint8_t*>(block->descriptor);
  descriptorCursor += sizeof(unsigned long) * 2;
  if ((block->flags & kBlockHasCopyDispose) != 0) {
    descriptorCursor += sizeof(void*) * 2;
  }

  return *reinterpret_cast<const char**>(descriptorCursor);
}

NAPI_FUNCTION(registerBlock) {
  NAPI_CALLBACK_BEGIN(2)

  char enc[256];
  NAPI_GUARD(napi_get_value_string_utf8(env, argv[0], enc, 256, nullptr)) {
    NAPI_THROW_LAST_ERROR
    return nullptr;
  }

  napi_value callback = argv[1];

  auto closure = new Closure(env, enc, true);
  registerBlock(env, closure, callback);

  return callback;
}

napi_value FunctionPointer::wrap(napi_env env, void* function, metagen::MDSectionOffset offset,
                                 bool isBlock) {
  if (isBlock) {
    napi_value cached = getCachedBlockJsFunction(env, function);
    if (cached != nullptr) {
      return cached;
    }
  }

  FunctionPointer* ref = new FunctionPointer();
  ref->function = function;
  ref->offset = offset;

  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);

  if (isBlock) {
    ref->cif = bridgeState->getBlockCif(env, offset);
  } else {
    ref->cif = bridgeState->getCFunctionCif(env, offset);
  }

  napi_value result;
  napi_create_function(env, isBlock ? "objcBlockWrapper" : "cFunctionWrapper", NAPI_AUTO_LENGTH,
                       isBlock ? jsCallAsBlock : jsCallAsCFunction, ref, &result);

  // Allow fast pointer extraction when JS function wrappers are passed back to native.
  napi_ref nativePointerRef;
  napi_wrap(env, result, function, nullptr, nullptr, &nativePointerRef);
  (void)nativePointerRef;

  // Keep raw pointer metadata without overriding the function callback data.
  // Overriding callback data breaks JS invocation for wrapped function pointers.
  napi_value ptrExternal;
  napi_create_external(env, function, nullptr, nullptr, &ptrExternal);
  napi_property_descriptor ptrProp = {
      .utf8name = "__ns_native_ptr",
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = ptrExternal,
      .attributes = napi_default,
      .data = nullptr,
  };
  napi_define_properties(env, result, 1, &ptrProp);

  napi_ref jsRef;
  napi_add_finalizer(env, result, ref, FunctionPointer::finalize, nullptr, &jsRef);

  return result;
}

napi_value FunctionPointer::wrapWithEncoding(napi_env env, void* function, const char* encoding,
                                             bool isBlock) {
  if (function == nullptr || encoding == nullptr || encoding[0] == '\0') {
    napi_value nullValue;
    napi_get_null(env, &nullValue);
    return nullValue;
  }

  if (isBlock) {
    napi_value cached = getCachedBlockJsFunction(env, function);
    if (cached != nullptr) {
      return cached;
    }
  }

  FunctionPointer* ref = new FunctionPointer();
  ref->function = function;
  ref->offset = 0;
  ref->ownsCif = true;
  ref->cif = new Cif(env, encoding, isBlock ? 1 : 0);

  napi_value result;
  napi_create_function(env, isBlock ? "objcBlockWrapper" : "cFunctionWrapper", NAPI_AUTO_LENGTH,
                       isBlock ? jsCallAsBlock : jsCallAsCFunction, ref, &result);

  // Allow fast pointer extraction when JS function wrappers are passed back to native.
  napi_ref nativePointerRef;
  napi_wrap(env, result, function, nullptr, nullptr, &nativePointerRef);
  (void)nativePointerRef;

  // Keep raw pointer metadata without overriding the function callback data.
  // Overriding callback data breaks JS invocation for wrapped function pointers.
  napi_value ptrExternal;
  napi_create_external(env, function, nullptr, nullptr, &ptrExternal);
  napi_property_descriptor ptrProp = {
      .utf8name = "__ns_native_ptr",
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = ptrExternal,
      .attributes = napi_default,
      .data = nullptr,
  };
  napi_define_properties(env, result, 1, &ptrProp);

  napi_ref jsRef;
  napi_add_finalizer(env, result, ref, FunctionPointer::finalize, nullptr, &jsRef);

  return result;
}

void FunctionPointer::finalize(napi_env env, void* finalize_data, void* finalize_hint) {
  if (PostFinalizer(env, finalizeFunctionPointerNow, finalize_data, finalize_hint)) {
    return;
  }

  finalizeFunctionPointerNow(env, finalize_data, finalize_hint);
}

napi_value FunctionPointer::jsCallAsCFunction(napi_env env, napi_callback_info cbinfo) {
  FunctionPointer* ref = nullptr;
  size_t actualArgc = 16;
  napi_value stackArgs[16];
  napi_get_cb_info(env, cbinfo, &actualArgc, stackArgs, nullptr, (void**)&ref);

  if (actualArgc > 16) {
    std::vector<napi_value> heapArgs(actualArgc);
    size_t retryArgc = actualArgc;
    napi_get_cb_info(env, cbinfo, &retryArgc, heapArgs.data(), nullptr, nullptr);
    return callFunctionPointerAsCFunctionDirect(env, ref, retryArgc, heapArgs.data());
  }

  return callFunctionPointerAsCFunctionDirect(env, ref, actualArgc, stackArgs);
}

napi_value FunctionPointer::jsCallAsBlock(napi_env env, napi_callback_info cbinfo) {
  FunctionPointer* ref = nullptr;
  size_t actualArgc = 16;
  napi_value stackArgs[16];
  napi_get_cb_info(env, cbinfo, &actualArgc, stackArgs, nullptr, (void**)&ref);

  if (actualArgc > 16) {
    std::vector<napi_value> heapArgs(actualArgc);
    size_t retryArgc = actualArgc;
    napi_get_cb_info(env, cbinfo, &retryArgc, heapArgs.data(), nullptr, nullptr);
    return callFunctionPointerAsBlockDirect(env, ref, retryArgc, heapArgs.data());
  }

  return callFunctionPointerAsBlockDirect(env, ref, actualArgc, stackArgs);
}

}  // namespace nativescript
