#include "CFunction.h"
#include <dispatch/dispatch.h>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "Block.h"
#include "CallbackThreading.h"
#include "ClassMember.h"
#include "Interop.h"
#include "ObjCBridge.h"
#include "SignatureDispatch.h"
#include "runtime/apple/NativeScriptException.h"
#include "ffi/objc/shared/Tasks.h"
#ifdef ENABLE_JS_RUNTIME
#include "jsr.h"
#endif

namespace nativescript {

namespace {

size_t getCifReturnStorageSize(Cif* cif) {
  size_t size = 0;
  if (cif != nullptr) {
    size = cif->rvalueLength;
    if (size == 0 && cif->cif.rtype != nullptr) {
      size = cif->cif.rtype->size;
    }
  }
  return size != 0 ? size : sizeof(void*);
}

class CFunctionReturnStorage final {
 public:
  explicit CFunctionReturnStorage(Cif* cif) {
    const size_t size = getCifReturnStorageSize(cif);

    if (size <= kInlineSize) {
      rvalue_ = inlineBuffer_;
      std::memset(rvalue_, 0, size);
      return;
    }

    rvalue_ = std::malloc(size);
    if (rvalue_ != nullptr) {
      std::memset(rvalue_, 0, size);
    }
  }

  ~CFunctionReturnStorage() {
    if (rvalue_ != nullptr && rvalue_ != inlineBuffer_) {
      std::free(rvalue_);
    }
  }

  CFunctionReturnStorage(const CFunctionReturnStorage&) = delete;
  CFunctionReturnStorage& operator=(const CFunctionReturnStorage&) = delete;

  bool isValid() const { return rvalue_ != nullptr; }
  void* rvalue() const { return rvalue_; }

 private:
  static constexpr size_t kInlineSize = 32;
  alignas(max_align_t) unsigned char inlineBuffer_[kInlineSize];
  void* rvalue_ = nullptr;
};

class CFunctionInvocationFrame final {
 public:
  explicit CFunctionInvocationFrame(Cif* cif)
      : avalues_(cif != nullptr ? cif->argc : 0, nullptr),
        argumentStorage_(cif != nullptr ? cif->argc : 0, nullptr) {
    if (cif == nullptr) {
      return;
    }

    rvalue_ = std::malloc(getCifReturnStorageSize(cif));
    if (rvalue_ == nullptr) {
      return;
    }

    valid_ = true;
    for (unsigned int i = 0; i < cif->argc; i++) {
      ffi_type* argType =
          cif->cif.arg_types != nullptr ? cif->cif.arg_types[i] : nullptr;
      const size_t argLength =
          argType != nullptr && argType->size > 0 ? argType->size : 1;
      void* storage = std::malloc(argLength);
      if (storage == nullptr) {
        valid_ = false;
        return;
      }
      argumentStorage_[i] = storage;
      avalues_[i] = storage;
    }
  }

  ~CFunctionInvocationFrame() {
    for (void* storage : argumentStorage_) {
      if (storage != nullptr) {
        std::free(storage);
      }
    }
    if (rvalue_ != nullptr) {
      std::free(rvalue_);
    }
  }

  CFunctionInvocationFrame(const CFunctionInvocationFrame&) = delete;
  CFunctionInvocationFrame& operator=(const CFunctionInvocationFrame&) = delete;

  bool isValid() const { return valid_ && rvalue_ != nullptr; }
  void* rvalue() const { return rvalue_; }
  void** avalues() { return avalues_.empty() ? nullptr : avalues_.data(); }

 private:
  bool valid_ = false;
  void* rvalue_ = nullptr;
  std::vector<void*> avalues_;
  std::vector<void*> argumentStorage_;
};

inline bool unwrapCompatNativeHandleForCFunction(napi_env env, napi_value value, void** out) {
  return unwrapKnownNativeHandle(env, value, out);
}

inline napi_value createCompatDispatchQueueWrapperForCFunction(napi_env env,
                                                               dispatch_queue_t queue) {
  if (queue == nullptr) {
    napi_value nullValue = nullptr;
    napi_get_null(env, &nullValue);
    return nullValue;
  }

  return Pointer::create(env, reinterpret_cast<void*>(queue));
}

inline napi_value tryCallCompatLibdispatchFunction(napi_env env, size_t argc,
                                                   const napi_value* argv,
                                                   const char* functionName) {
  if (strcmp(functionName, "dispatch_get_global_queue") == 0) {
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

    return createCompatDispatchQueueWrapperForCFunction(
        env, dispatch_get_global_queue(identifier, flags));
  }

  if (strcmp(functionName, "dispatch_get_current_queue") == 0) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return createCompatDispatchQueueWrapperForCFunction(env, dispatch_get_current_queue());
#pragma clang diagnostic pop
  }

  if (strcmp(functionName, "dispatch_async") == 0) {
    if (argc < 2) {
      napi_throw_type_error(env, nullptr, "dispatch_async expects a queue and callback.");
      return nullptr;
    }

    void* queueHandle = nullptr;
    if (!unwrapCompatNativeHandleForCFunction(env, argv[0], &queueHandle) ||
        queueHandle == nullptr) {
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

  return nullptr;
}

}  // namespace

inline void ensureCFunctionDispatchLookup(CFunction* function, Cif* cif) {
  if (function == nullptr || cif == nullptr || cif->signatureHash == 0) {
    if (function != nullptr) {
      function->dispatchLookupCached = true;
      function->dispatchLookupSignatureHash = 0;
      function->dispatchId = 0;
      function->preparedInvoker = nullptr;
      function->napiInvoker = nullptr;
    }
    return;
  }

  if (function->dispatchLookupCached &&
      function->dispatchLookupSignatureHash == cif->signatureHash) {
    return;
  }

  function->dispatchLookupSignatureHash = cif->signatureHash;
  function->dispatchId = composeSignatureDispatchId(
      cif->signatureHash, SignatureCallKind::CFunction, function->dispatchFlags);
  function->preparedInvoker =
      reinterpret_cast<void*>(lookupCFunctionPreparedInvoker(function->dispatchId));
  function->napiInvoker = reinterpret_cast<void*>(lookupCFunctionNapiInvoker(function->dispatchId));
  function->dispatchLookupCached = true;
}

void ObjCBridgeState::registerFunctionGlobals(napi_env env, napi_value global) {
  MDSectionOffset offset = metadata->functionsOffset;
  while (offset < metadata->protocolsOffset) {
    MDSectionOffset originalOffset = offset;
    auto name = metadata->getString(offset);
    offset += sizeof(MDSectionOffset);
    offset += sizeof(MDSectionOffset);
    offset += sizeof(MDFunctionFlag);

    napi_property_descriptor prop = {
        .utf8name = name,
        .method = CFunction::jsCall,
        .getter = nullptr,
        .setter = nullptr,
        .value = nullptr,
        .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
        .data = (void*)((size_t)originalOffset),
    };

    napi_define_properties(env, global, 1, &prop);
  }
}

CFunction* ObjCBridgeState::getCFunction(napi_env env, MDSectionOffset offset) {
  auto cached = cFunctionCache.find(offset);
  if (cached != cFunctionCache.end()) {
    return cached->second;
  }

  auto sigOffset =
      metadata->signaturesOffset + metadata->getOffset(offset + sizeof(MDSectionOffset));
  MDFunctionFlag functionFlags = metadata->getFunctionFlag(offset + sizeof(MDSectionOffset) * 2);

  const char* name = metadata->getString(offset);
  auto cFunction = new CFunction(dlsym(self_dl, name));
  cFunction->bridgeState = this;
  cFunction->cif = getCFunctionCif(env, sigOffset);
  cFunction->dispatchFlags = (functionFlags & mdFunctionReturnOwned) != 0 ? 1 : 0;
  cFunctionCache[offset] = cFunction;

  return cFunction;
}

napi_value CFunction::jsCall(napi_env env, napi_callback_info cbinfo) {
  void* _offset = nullptr;
  size_t actualArgc = 16;
  napi_value stackArgs[16];

  napi_get_cb_info(env, cbinfo, &actualArgc, stackArgs, nullptr, &_offset);

  MDSectionOffset offset = (MDSectionOffset)((size_t)_offset);

  if (actualArgc > 16) {
    std::vector<napi_value> dynamicArgs(actualArgc);
    size_t retryArgc = actualArgc;
    napi_get_cb_info(env, cbinfo, &retryArgc, dynamicArgs.data(), nullptr, nullptr);
    dynamicArgs.resize(retryArgc);
    return jsCallDirect(env, offset, retryArgc, dynamicArgs.data());
  }

  return jsCallDirect(env, offset, actualArgc, stackArgs);
}

napi_value CFunction::jsCallDirect(napi_env env, MDSectionOffset offset,
                                   size_t actualArgc,
                                   const napi_value* callArgs) {
  auto bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr) {
    napi_throw_error(env, "NativeScriptException", "Missing Objective-C bridge state.");
    return nullptr;
  }

  auto name = bridgeState->metadata->getString(offset);

  if (strcmp(name, "dispatch_async") == 0 || strcmp(name, "dispatch_get_current_queue") == 0 ||
      strcmp(name, "dispatch_get_global_queue") == 0) {
    return tryCallCompatLibdispatchFunction(env, actualArgc, callArgs, name);
  }

  auto func = bridgeState->getCFunction(env, offset);

  auto cif = func->cif;
  ensureCFunctionDispatchLookup(func, cif);
  auto preparedInvoker = reinterpret_cast<CFunctionPreparedInvoker>(func->preparedInvoker);
  auto napiInvoker = reinterpret_cast<CFunctionNapiInvoker>(func->napiInvoker);

  MDFunctionFlag functionFlags =
      bridgeState->metadata->getFunctionFlag(offset + sizeof(MDSectionOffset) * 2);

  const napi_value* invocationArgs = nullptr;
  std::vector<napi_value> paddedArgs;
  if (cif->argc > 0) {
    invocationArgs = callArgs;
    if (actualArgc != cif->argc) {
      napi_value jsUndefined = nullptr;
      napi_get_undefined(env, &jsUndefined);
      paddedArgs.assign(cif->argc, jsUndefined);
      size_t copyArgc = actualArgc < cif->argc ? actualArgc : cif->argc;
      if (copyArgc > 0) {
        memcpy(paddedArgs.data(), callArgs, copyArgc * sizeof(napi_value));
      }
      invocationArgs = paddedArgs.data();
    }
  }

  uint32_t toJSFlags = kCStringAsReference;
  if ((functionFlags & mdFunctionReturnOwned) != 0) {
    toJSFlags |= kReturnOwned;
  }

  const bool isMainEntrypoint =
      strcmp(name, "UIApplicationMain") == 0 || strcmp(name, "NSApplicationMain") == 0;

  if (napiInvoker != nullptr && !cif->skipGeneratedNapiDispatch &&
      !isMainEntrypoint) {
    CFunctionReturnStorage returnStorage(cif);
    if (!returnStorage.isValid()) {
      napi_throw_error(env, "NativeScriptException",
                       "Unable to allocate C function return storage.");
      return nullptr;
    }

    void* rvalue = returnStorage.rvalue();
    @try {
      NativeCallRuntimeUnlockScope unlockRuntime(env);
      bool invoked = napiInvoker(env, cif, func->fnptr, invocationArgs, rvalue);
      if (!invoked) {
        return nullptr;
      }
    } @catch (NSException* exception) {
      std::string message = exception.description.UTF8String;
      NSLog(@"ObjC->JS: Exception in CFunction (napi): %s", message.c_str());
      nativescript::NativeScriptException nativeScriptException(message);
      nativeScriptException.ReThrowToJS(env);
      return nullptr;
    }
    return cif->returnType->toJS(env, rvalue, toJSFlags);
  }

  auto invocationFrame = std::make_shared<CFunctionInvocationFrame>(cif);
  if (!invocationFrame->isValid()) {
    napi_throw_error(env, "NativeScriptException",
                     "Unable to allocate C function invocation storage.");
    return nullptr;
  }
  void* rvalue = invocationFrame->rvalue();
  void** avalues = invocationFrame->avalues();

  bool shouldFreeAny = false;
  std::vector<uint8_t> shouldFree(cif->argc, 0);

  if (cif->argc > 0) {
    for (unsigned int i = 0; i < cif->argc; i++) {
      bool argShouldFree = false;
      cif->argTypes[i]->toNative(env, invocationArgs[i], avalues[i], &argShouldFree,
                                 &shouldFreeAny);
      shouldFree[i] = argShouldFree ? 1 : 0;
      if (ConsumeNapiArgumentConversionFailure(env)) {
        for (unsigned int converted = 0; converted <= i; converted++) {
          if (shouldFree[converted]) {
            cif->argTypes[converted]->free(env, *((void**)avalues[converted]));
          }
        }
        return nullptr;
      }
    }
  }

#ifdef ENABLE_JS_RUNTIME
  if (isMainEntrypoint) {
    Tasks::Register([env, cif, func, preparedInvoker, invocationFrame]() {
      void** avalues = invocationFrame->avalues();
      void* rvalue = invocationFrame->rvalue();
      @try {
        if (preparedInvoker != nullptr) {
          preparedInvoker(func->fnptr, avalues, rvalue);
        } else {
          ffi_call(&cif->cif, FFI_FN(func->fnptr), rvalue, avalues);
        }
      } @catch (NSException* exception) {
        NapiScope scope(env);
        std::string message = exception.description.UTF8String;
        NSLog(@"ObjC->JS: Exception in CFunction (task): %s", message.c_str());
        nativescript::NativeScriptException nativeScriptException(message);
        nativeScriptException.ReThrowToJS(env);
      }
    });

    return nullptr;
  }
#endif

  @try {
    NativeCallRuntimeUnlockScope unlockRuntime(env);
    if (preparedInvoker != nullptr) {
      preparedInvoker(func->fnptr, avalues, rvalue);
    } else {
      ffi_call(&cif->cif, FFI_FN(func->fnptr), rvalue, avalues);
    }
  } @catch (NSException* exception) {
    std::string message = exception.description.UTF8String;
    NSLog(@"ObjC->JS: Exception in CFunction: %s", message.c_str());
    nativescript::NativeScriptException nativeScriptException(message);
    nativeScriptException.ReThrowToJS(env);
    return nullptr;
  }

  if (shouldFreeAny) {
    void* returnPointerValue = nullptr;
    bool returnIsPointer = cif->returnType != nullptr && cif->returnType->type == &ffi_type_pointer;
    if (returnIsPointer && rvalue != nullptr) {
      returnPointerValue = *((void**)rvalue);
    }

    for (unsigned int i = 0; i < cif->argc; i++) {
      if (shouldFree[i]) {
        if (returnPointerValue != nullptr && avalues[i] != nullptr) {
          void* argPointerValue = *((void**)avalues[i]);
          if (argPointerValue == returnPointerValue) {
            continue;
          }
        }
        cif->argTypes[i]->free(env, *((void**)avalues[i]));
      }
    }
  }

  return cif->returnType->toJS(env, rvalue, toJSFlags);
}

CFunction::~CFunction() { cif = nullptr; }

}  // namespace nativescript
