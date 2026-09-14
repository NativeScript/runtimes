#include "Closure.h"
#include "AutoreleasePool.h"
#include "CallbackThreading.h"
#include "Metadata.h"
#include "MetadataReader.h"
#include "ObjCBridge.h"
#include "TypeConv.h"
#include "Util.h"
#include "runtime/apple/NativeScriptException.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#ifdef ENABLE_JS_RUNTIME
#include "jsr.h"
#endif
#include "node_api_util.h"
#include "objc/message.h"

#include <CoreFoundation/CFRunLoop.h>
#include <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace nativescript {

namespace {

thread_local std::vector<NapiNativeCallbackExceptionCapture*>
    gNativeCallbackExceptionCaptureStack;

inline void deleteClosureOnOwningThread(Closure* closure) {
  if (closure == nullptr) {
    return;
  }

#ifdef ENABLE_JS_RUNTIME
  CFRunLoopRef runloop = closure->jsRunLoop;
  if (runloop == nullptr) {
    runloop = CFRunLoopGetMain();
  }

  if (closure->jsThreadId == std::this_thread::get_id()) {
    delete closure;
    return;
  }

  if (runloop != nullptr) {
    CFRetain(runloop);
    CFRunLoopPerformBlock(runloop, kCFRunLoopCommonModes, ^{
      delete closure;
      CFRelease(runloop);
    });
    CFRunLoopWakeUp(runloop);
    return;
  }

#endif  // ENABLE_JS_RUNTIME

  delete closure;
}

}  // namespace

NapiNativeCallbackExceptionCapture::~NapiNativeCallbackExceptionCapture() {
  clear();
}

void NapiNativeCallbackExceptionCapture::clear() {
  if (env != nullptr && errorRef != nullptr) {
    napi_delete_reference(env, errorRef);
  }
  env = nullptr;
  errorRef = nullptr;
}

ScopedNapiNativeCallbackExceptionCapture::
    ScopedNapiNativeCallbackExceptionCapture(
        NapiNativeCallbackExceptionCapture* capture)
    : capture_(capture) {
  gNativeCallbackExceptionCaptureStack.push_back(capture_);
}

ScopedNapiNativeCallbackExceptionCapture::
    ~ScopedNapiNativeCallbackExceptionCapture() {
  if (!gNativeCallbackExceptionCaptureStack.empty() &&
      gNativeCallbackExceptionCaptureStack.back() == capture_) {
    gNativeCallbackExceptionCaptureStack.pop_back();
  }
}

bool recordNapiNativeCallbackException(napi_env env, napi_value error) {
  if (env == nullptr || error == nullptr ||
      gNativeCallbackExceptionCaptureStack.empty()) {
    return false;
  }

  auto* capture = gNativeCallbackExceptionCaptureStack.back();
  if (capture == nullptr || capture->errorRef != nullptr) {
    return capture != nullptr;
  }

  capture->env = env;
  return napi_create_reference(env, error, 1, &capture->errorRef) == napi_ok;
}

bool rethrowNapiNativeCallbackException(
    napi_env env, NapiNativeCallbackExceptionCapture& capture) {
  if (env == nullptr || capture.errorRef == nullptr) {
    return false;
  }

  napi_value error = nullptr;
  napi_ref errorRef = capture.errorRef;
  capture.errorRef = nullptr;
  napi_get_reference_value(env, errorRef, &error);
  napi_delete_reference(env, errorRef);
  capture.env = nullptr;

  if (error != nullptr) {
    NativeScriptException nativeScriptException(
        env, error, "JS implemented closure threw an exception");
    nativeScriptException.ReThrowToJS(env);
  } else {
    NativeScriptException nativeScriptException(
        "Unable to obtain the error thrown by the JS implemented closure");
    nativeScriptException.ReThrowToJS(env);
  }
  return true;
}

void Closure::destroyOnOwningThread(Closure* closure) { deleteClosureOnOwningThread(closure); }

inline bool selectorEndsWithErrorParam(SEL selector) {
  if (selector == nullptr) {
    return false;
  }

  const char* selectorName = sel_getName(selector);
  if (selectorName == nullptr) {
    return false;
  }

  size_t selectorLength = strlen(selectorName);
  const char* suffix = "error:";
  size_t suffixLength = strlen(suffix);
  if (selectorLength < suffixLength) {
    return false;
  }

  return strcmp(selectorName + selectorLength - suffixLength, suffix) == 0;
}

inline bool isNSErrorMethodCallback(Closure* closure, ffi_cif* cif) {
  if (closure == nullptr || cif == nullptr || cif->nargs < 3 || closure->returnType == nullptr) {
    return false;
  }

  if (closure->returnType->kind != mdTypeBool) {
    return false;
  }

  if (closure->argTypes.size() != cif->nargs) {
    return false;
  }

  auto lastArgType = closure->argTypes[cif->nargs - 1];
  if (lastArgType == nullptr || lastArgType->kind != mdTypePointer) {
    return false;
  }

  return selectorEndsWithErrorParam(closure->selector);
}

inline void JSCallbackInner(Closure* closure, napi_value func, napi_value thisArg, napi_value* argv,
                            size_t argc, bool* done, void* ret) {
  napi_env env = closure->env;

  napi_value result;

  napi_get_and_clear_last_exception(env, &result);

  napi_status status = napi_call_function(env, thisArg, func, argc, argv, &result);

  if (done != nullptr) {
    *done = true;
  }

  // If the call failed, we need to create an error object and throw it in native
  // Likely it will circle back to JS. We have try/catch around all native calls from JS,
  // so those are likely to catch it.
  if (status != napi_ok) {
    napi_get_and_clear_last_exception(env, &result);
    napi_valuetype resultType;
    napi_typeof(env, result, &resultType);

    if (resultType != napi_object) {
      napi_value code, msg;
      napi_create_string_utf8(env, "NativeScriptException", NAPI_AUTO_LENGTH, &code);
      napi_create_string_utf8(env,
                              "Unable to obtain the error thrown by the JS implemented closure",
                              NAPI_AUTO_LENGTH, &msg);
      napi_create_error(env, code, msg, &result);
    }

    if (recordNapiNativeCallbackException(env, result)) {
      napi_get_undefined(env, &result);
    } else {
      NativeScriptException::OnUncaughtError(env, result);
    }
  }

  // Even if call was failed and result is just undefined, let's still try to
  // fill the return value memory with something so that it doesn't crash.
  bool shouldFree;
  closure->returnType->toNative(env, result, ret, &shouldFree, &shouldFree);
  ConsumeNapiArgumentConversionFailure(env);
}

// Bridge calls from Objective-C to JavaScript.
// Opposite of what native_call.cc does - but a lot of type conversion logic
// is reused, just in reverse.
void JSMethodCallback(ffi_cif* cif, void* ret, void* args[], void* data) {
  Closure* closure = (Closure*)data;
  napi_env env = closure->env;

#ifdef ENABLE_JS_RUNTIME
  NapiScope scope(env);
#endif

  auto bridgeState = ObjCBridgeState::InstanceData(env);

  napi_value constructor = get_ref_value(env, closure->thisConstructor);

  id self = *(id*)args[0];
  napi_value thisArg = bridgeState->getObject(env, self, constructor);
  if (thisArg == nil) {
    NSLog(@"ObjC->JS: thisArg is nil, the JS object was probably garbage "
          @"collected");
  }

  napi_value func;
  if (closure->func != nullptr) {
    func = get_ref_value(env, closure->func);
  } else {
    napi_get_named_property(env, thisArg, closure->propertyName.c_str(), &func);
  }

  napi_valuetype funcType;
  napi_typeof(env, func, &funcType);
  if (funcType != napi_function) {
    std::string errmsg = "Property " + closure->propertyName + " is not a function, cannot call it";
    napi_throw_error(env, nullptr, errmsg.c_str());
    return;
  }

  napi_value argv[cif->nargs - 2];
  for (int i = 2; i < cif->nargs; i++) {
    argv[i - 2] = closure->argTypes[i]->toJS(env, args[i], 0);
  }

  napi_value result;
  napi_get_and_clear_last_exception(env, &result);

  napi_status status = napi_call_function(env, thisArg, func, cif->nargs - 2, argv, &result);
  if (status != napi_ok) {
    napi_get_and_clear_last_exception(env, &result);

    if (isNSErrorMethodCallback(closure, cif)) {
      if (ret != nullptr && cif->rtype != nullptr && cif->rtype->size > 0) {
        memset(ret, 0, cif->rtype->size);
      }

      void* outArgValue = args[cif->nargs - 1];
      NSError** outError = outArgValue != nullptr ? *((NSError***)outArgValue) : nullptr;
      if (outError != nullptr) {
        std::string message = "JS error";
        napi_valuetype resultType = napi_undefined;
        if (result != nullptr && napi_typeof(env, result, &resultType) == napi_ok) {
          if (resultType == napi_object) {
            napi_value messageValue = nullptr;
            bool hasMessage = false;
            if (napi_has_named_property(env, result, "message", &hasMessage) == napi_ok &&
                hasMessage &&
                napi_get_named_property(env, result, "message", &messageValue) == napi_ok) {
              napi_valuetype messageType = napi_undefined;
              if (napi_typeof(env, messageValue, &messageType) == napi_ok &&
                  messageType == napi_string) {
                size_t messageLength = 0;
                napi_get_value_string_utf8(env, messageValue, nullptr, 0, &messageLength);
                std::vector<char> messageBuffer(messageLength + 1);
                napi_get_value_string_utf8(env, messageValue, messageBuffer.data(),
                                           messageBuffer.size(), &messageLength);
                message.assign(messageBuffer.data(), messageLength);
              }
            }
          } else if (resultType == napi_string) {
            size_t messageLength = 0;
            napi_get_value_string_utf8(env, result, nullptr, 0, &messageLength);
            std::vector<char> messageBuffer(messageLength + 1);
            napi_get_value_string_utf8(env, result, messageBuffer.data(), messageBuffer.size(),
                                       &messageLength);
            message.assign(messageBuffer.data(), messageLength);
          }
        }

        NSString* nsMessage = [NSString stringWithUTF8String:message.c_str()];
        NSDictionary* userInfo = nsMessage != nil ? @{NSLocalizedDescriptionKey : nsMessage} : nil;
        *outError = [NSError errorWithDomain:@"TNSErrorDomain" code:1 userInfo:userInfo];
      }

      return;
    }

    napi_valuetype resultType = napi_undefined;
    napi_typeof(env, result, &resultType);

    if (resultType != napi_object) {
      napi_value code, msg;
      napi_create_string_utf8(env, "NativeScriptException", NAPI_AUTO_LENGTH, &code);
      napi_create_string_utf8(env,
                              "Unable to obtain the error thrown by the JS implemented closure",
                              NAPI_AUTO_LENGTH, &msg);
      napi_create_error(env, code, msg, &result);
    }

    if (recordNapiNativeCallbackException(env, result)) {
      napi_get_undefined(env, &result);
    } else {
      NativeScriptException::OnUncaughtError(env, result);
    }
  }

  bool shouldFree;
  closure->returnType->toNative(env, result, ret, &shouldFree, &shouldFree);
  ConsumeNapiArgumentConversionFailure(env);
}

void JSFunctionCallback(ffi_cif* cif, void* ret, void* args[], void* data) {
  Closure* closure = (Closure*)data;
  napi_env env = closure->env;

#ifdef ENABLE_JS_RUNTIME
  NativeCallbackScope scope(env);
#endif

  napi_value func = get_ref_value(env, closure->func);

  napi_valuetype funcType = napi_undefined;
  napi_typeof(env, func, &funcType);
  if (funcType != napi_function) {
    napi_throw_error(env, nullptr, "Function reference is not callable");
    return;
  }

  napi_value thisArg;
  napi_get_global(env, &thisArg);

  napi_value argv[cif->nargs];
  for (int i = 0; i < cif->nargs; i++) {
    argv[i] = closure->argTypes[i]->toJS(env, args[i], 0);
  }

  JSCallbackInner(closure, func, thisArg, argv, cif->nargs, nullptr, ret);
}

struct JSBlockCallContext {
  ffi_cif* cif;
  void* ret;
  void** args;
  std::mutex mutex;
  std::condition_variable cv;
  bool done;
  bool useCondvar;
};

void Closure::callBlockFromMainThread(napi_env env, napi_value js_cb, void* context, void* data) {
  auto closure = (Closure*)context;
  auto ctx = (JSBlockCallContext*)data;

  napi_value func = js_cb;
  if (func == nullptr && closure->func != nullptr) {
    func = get_ref_value(env, closure->func);
  }

  napi_value thisArg;
  napi_get_global(env, &thisArg);

  napi_value argv[ctx->cif->nargs - 1];
  for (int i = 0; i < ctx->cif->nargs - 1; i++) {
    argv[i] = closure->argTypes[i]->toJS(env, ctx->args[i + 1], 0);
  }

  JSCallbackInner(closure, func, thisArg, argv, ctx->cif->nargs - 1, nullptr, ctx->ret);
#ifdef TARGET_ENGINE_HERMES
  if (!isNativeCallerThreadCallbackActive()) {
    js_execute_pending_jobs(env);
  }
#endif
  if (ctx->useCondvar) {
    {
      std::lock_guard<std::mutex> lock(ctx->mutex);
      ctx->done = true;
    }
    ctx->cv.notify_one();
  }
}

void JSBlockCallback(ffi_cif* cif, void* ret, void* args[], void* data) {
  Closure* closure = (Closure*)data;
  napi_env env = closure->env;
  closure->retain();
  struct ClosureRetainGuard {
    Closure* closure;
    ~ClosureRetainGuard() {
      if (closure != nullptr) {
        closure->release();
      }
    }
  } retainGuard{closure};

  auto currentThreadId = std::this_thread::get_id();

  JSBlockCallContext ctx;
  ctx.cif = cif;
  ctx.ret = ret;
  ctx.args = args;
  ctx.done = false;
  ctx.useCondvar = false;

  if (currentThreadId == closure->jsThreadId || shouldInvokeCallbackOnNativeCallerThread()) {
#ifdef ENABLE_JS_RUNTIME
    NativeCallbackScope scope(env);
#endif
    Closure::callBlockFromMainThread(env, get_ref_value(env, closure->func), closure, &ctx);
  } else {
#ifndef ENABLE_JS_RUNTIME
    if (!closure->tsfn) {
      assert(false && "Threadsafe functions are not supported");
    }

    ctx.useCondvar = true;
    napi_acquire_threadsafe_function(closure->tsfn);
    std::unique_lock<std::mutex> lock(ctx.mutex);
    napi_call_threadsafe_function(closure->tsfn, &ctx, napi_tsfn_blocking);
    ctx.cv.wait(lock, [&ctx] { return ctx.done; });
    napi_release_threadsafe_function(closure->tsfn, napi_tsfn_release);
#else
    auto runloop = closure->jsRunLoop;
    if (runloop == nullptr) {
      runloop = CFRunLoopGetMain();
    }

    if (runloop == nullptr) {
      NapiScope scope(env);
      Closure::callBlockFromMainThread(env, get_ref_value(env, closure->func), closure, &ctx);
    } else {
      JSBlockCallContext* ctxPtr = &ctx;
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      CFRunLoopPerformBlock(runloop, kCFRunLoopCommonModes, ^{
        NapiScope scope(env);
        Closure::callBlockFromMainThread(env, get_ref_value(env, closure->func), closure, ctxPtr);
        dispatch_semaphore_signal(done);
      });
      CFRunLoopWakeUp(runloop);
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    }
#endif  // ENABLE_JS_RUNTIME
  }
}

Closure::Closure(napi_env env, std::string encoding, bool isBlock, bool isMethod) {
  this->env = env;
  this->bridgeState = ObjCBridgeState::InstanceData(env);
  this->bridgeStateToken = this->bridgeState != nullptr ? this->bridgeState->lifetimeToken : 0;
  auto signature = [NSMethodSignature signatureWithObjCTypes:encoding.c_str()];
  size_t argc = signature.numberOfArguments;

  const char* rtypeEncoding = signature.methodReturnType;
  returnType = TypeConv::Make(env, &rtypeEncoding);

  int skipArgs = isBlock ? 1 : 0;

  ffi_type* rtype = this->returnType->type;
  this->atypes = (ffi_type**)malloc(sizeof(ffi_type*) * (argc + skipArgs));

  if (isBlock) {
    this->atypes[0] = &ffi_type_pointer;
  }

  for (int i = 0; i < argc; i++) {
    const char* argenc = [signature getArgumentTypeAtIndex:i];
    auto argTypeInfo = TypeConv::Make(env, &argenc);
    this->atypes[i + skipArgs] = argTypeInfo->ffiTypeForArgument();
    this->argTypes.push_back(argTypeInfo);
  }

  ffi_status status =
      ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (int)argc + skipArgs, rtype, this->atypes);

  if (status != FFI_OK) {
    std::cout << "ffi_prep_cif failed" << std::endl;
    return;
  }

  closure = (ffi_closure*)ffi_closure_alloc(sizeof(ffi_closure), &fnptr);

  ffi_prep_closure_loc(
      closure, &cif, isBlock ? JSBlockCallback : (isMethod ? JSMethodCallback : JSFunctionCallback),
      this, fnptr);
}

Closure::Closure(napi_env env, MDMetadataReader* reader, MDSectionOffset offset, bool isBlock,
                 std::string* encoding, bool isMethod, bool isGetter, bool isSetter) {
  this->env = env;
  this->bridgeState = ObjCBridgeState::InstanceData(env);
  this->bridgeStateToken = this->bridgeState != nullptr ? this->bridgeState->lifetimeToken : 0;
  this->isGetter = isGetter;
  this->isSetter = isSetter;

  auto returnTypeKind = reader->getTypeKind(offset);
  bool next = ((MDTypeFlag)returnTypeKind & mdTypeFlagNext) != 0;

  returnType = TypeConv::Make(env, reader, &offset);

  if (encoding != nullptr) returnType->encode(encoding);

  ffi_type* rtype = returnType->type;

  if (isMethod && encoding != nullptr) {
    const char* argenc = "@";
    *encoding += argenc;
    argTypes.push_back(TypeConv::Make(env, &argenc));
    argenc = ":";
    *encoding += argenc;
    argTypes.push_back(TypeConv::Make(env, &argenc));
  }

  while (next) {
    auto argTypeKind = reader->getTypeKind(offset);
    next = ((MDTypeFlag)argTypeKind & mdTypeFlagNext) != 0;
    auto argTypeInfo = TypeConv::Make(env, reader, &offset);
    if (encoding != nullptr) argTypeInfo->encode(encoding);
    argTypes.push_back(argTypeInfo);
  }

  auto skipArgs = isBlock ? 1 : 0;

  if (!argTypes.empty() || isBlock) {
    this->atypes = (ffi_type**)malloc(sizeof(ffi_type*) * (argTypes.size() + skipArgs));
    if (isBlock) {
      this->atypes[0] = &ffi_type_pointer;
    }
    for (int i = 0; i < argTypes.size(); i++) {
      this->atypes[i + skipArgs] = argTypes[i]->ffiTypeForArgument();
    }
  }

  ffi_status status =
      ffi_prep_cif(&cif, FFI_DEFAULT_ABI,
                   static_cast<unsigned int>(argTypes.size() + skipArgs), rtype,
                   this->atypes);

  if (status != FFI_OK) {
    std::cout << "Failed to prepare CIF, libffi returned error:" << status << std::endl;
  }

  closure = (ffi_closure*)ffi_closure_alloc(sizeof(ffi_closure), &fnptr);

  ffi_prep_closure_loc(
      closure, &cif, isBlock ? JSBlockCallback : (isMethod ? JSMethodCallback : JSFunctionCallback),
      this, fnptr);
}

Closure::~Closure() {
  if (func != nullptr) {
    if (env != nullptr &&
        (bridgeState == nullptr || IsBridgeStateLive(bridgeState, bridgeStateToken))) {
#ifdef ENABLE_JS_RUNTIME
      ReleaseAndDeleteReferenceOnOwningThread(env, bridgeState, bridgeStateToken, func);
#else
      DeleteReferenceOnOwningThread(env, bridgeState, bridgeStateToken, func);
#endif
    }
    func = nullptr;
  }
  bridgeState = nullptr;
  bridgeStateToken = 0;
#ifndef ENABLE_JS_RUNTIME
  if (tsfn != nullptr) {
    napi_release_threadsafe_function(tsfn, napi_tsfn_abort);
  }
#endif  // ENABLE_JS_RUNTIME
  if (atypes != nullptr) {
    free(atypes);
  }
  ffi_closure_free(closure);
}

void Closure::retain() { retainCount.fetch_add(1, std::memory_order_relaxed); }

void Closure::release() {
  int previous = retainCount.fetch_sub(1, std::memory_order_acq_rel);
  if (previous == 1) {
    deleteClosureOnOwningThread(this);
  }
}

}  // namespace nativescript
