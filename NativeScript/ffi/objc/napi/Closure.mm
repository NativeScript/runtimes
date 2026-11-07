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

#include <Block.h>
#include <CoreFoundation/CFRunLoop.h>
#include <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <unordered_map>
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

void reportNativeCallbackError(napi_env env, napi_value error) {
  if (!recordNapiNativeCallbackException(env, error)) {
    NativeScriptException::OnUncaughtError(env, error);
  }
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

    reportNativeCallbackError(env, result);
    napi_get_undefined(env, &result);
  }

  // Even if call was failed and result is just undefined, let's still try to
  // fill the return value memory with something so that it doesn't crash.
  bool shouldFree;
  closure->returnType->toNative(env, result, ret, &shouldFree, &shouldFree);
}

// Bridge calls from Objective-C to JavaScript.
// Opposite of what native_call.cc does - but a lot of type conversion logic
// is reused, just in reverse.
void invokeJSMethodCallback(ffi_cif* cif, void* ret, void* args[], void* data) {
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

    reportNativeCallbackError(env, result);
    napi_get_undefined(env, &result);
  }

  bool shouldFree;
  closure->returnType->toNative(env, result, ret, &shouldFree, &shouldFree);
}

void invokeJSFunctionCallback(ffi_cif* cif, void* ret, void* args[], void* data) {
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

#ifndef ENABLE_JS_RUNTIME
using ClosureInvoker = void (*)(ffi_cif* cif, void* ret, void* args[], void* data);

struct ClosureInvocation {
  ClosureInvoker invoke;
  ffi_cif* cif;
  void* ret;
  void** args;
  void* data;
};

void runClosureInvocation(void* data) {
  auto invocation = static_cast<ClosureInvocation*>(data);
  invocation->invoke(invocation->cif, invocation->ret, invocation->args, invocation->data);
}

void invokeInNativeCallbackScope(napi_env env, ClosureInvoker invoke, ffi_cif* cif, void* ret,
                                 void* args[], void* data) {
  ClosureInvocation invocation{invoke, cif, ret, args, data};
  if (!NativeCallbackScope::run(env, runClosureInvocation, &invocation) &&
      cif->rtype->type != FFI_TYPE_VOID) {
    memset(ret, 0, cif->rtype->size);
  }
}

// A closure invoked on a thread other than the JS thread is forwarded to it
// through one threadsafe function per env. A void callback whose arguments
// can all be captured is queued and its native caller returns at once:
// waiting deadlocks whenever the JS thread is itself blocked on that caller
// (e.g. a synchronous native call whose completion handler fires on a worker
// thread). Every other call waits for JS to return.
//
// Limits of queued calls:
// - The queue is unbounded and every pending call keeps its object arguments
//   retained. When JS falls behind a producer such as a capture stream, the
//   retained buffers pile up and can exhaust the producer's buffer pool.
// - Object arguments are retained, not copied. JS sees the state of an object
//   at the time the call runs, not when it was queued, so native code that
//   recycles or mutates an object after the callback returns (a reused
//   buffer, for one) shows JS the later contents.
// - A queued call has no native caller left to receive an exception. JS errors
//   still go through the closure uncaught-error handling, and the Objective-C
//   exception that handling raises is logged.
struct ForeignThreadCall {
  ClosureInvoker invoke;
  ffi_cif* cif;
  void* ret;
  void** args;
  void* data;
  bool queued = false;

  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  NSException* exception = nil;
};

enum class CapturedArg { Copy, Retain, CopyBlock };

bool isPlainValue(ffi_type* type) {
  if (type->type == FFI_TYPE_POINTER) {
    return false;
  }
  if (type->type == FFI_TYPE_STRUCT || type->type == FFI_TYPE_COMPLEX) {
    for (ffi_type** element = type->elements; element != nullptr && *element != nullptr;
         element++) {
      if (!isPlainValue(*element)) {
        return false;
      }
    }
  }
  return true;
}

// Only values that stay valid after the native caller returns can be
// captured; anything that may point into memory the caller owns cannot.
bool capturedArgHandling(TypeConv* type, ffi_type* ffiType, CapturedArg* handling) {
  switch (type->kind) {
    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
      *handling = CapturedArg::Retain;
      return true;
    case mdTypeBlock:
      *handling = CapturedArg::CopyBlock;
      return true;
    case mdTypeChar:
    case mdTypeSInt:
    case mdTypeSShort:
    case mdTypeSLong:
    case mdTypeSInt64:
    case mdTypeUChar:
    case mdTypeUInt:
    case mdTypeUShort:
    case mdTypeULong:
    case mdTypeUInt64:
    case mdTypeFloat:
    case mdTypeDouble:
    case mdTypeUInt8:
    case mdTypeBool:
    case mdTypeUnichar:
    case mdTypeF16:
    case mdTypeUInt128:
    case mdTypeLongDouble:
    case mdTypeComplex:
    case mdTypeVector:
    case mdTypeExtVector:
    // Classes and selectors live as long as the process.
    case mdTypeClass:
    case mdTypeSelector:
      *handling = CapturedArg::Copy;
      return true;
    case mdTypeStruct:
      *handling = CapturedArg::Copy;
      return isPlainValue(ffiType);
    default:
      return false;
  }
}

// The block literal (argument 0 of a block invoke) has no TypeConv; it is not
// passed to JS.
TypeConv* foreignCallArgType(Closure* closure, bool isBlock, unsigned index) {
  if (isBlock) {
    if (index == 0) {
      return nullptr;
    }
    index -= 1;
  }
  return index < closure->argTypes.size() ? closure->argTypes[index].get() : nullptr;
}

bool canQueueForeignCall(Closure* closure, bool isBlock, ffi_cif* cif) {
  // A setter waits, so a getter that follows on the same thread sees the value.
  if (closure->returnType == nullptr || closure->returnType->kind != mdTypeVoid ||
      closure->isSetter) {
    return false;
  }
  for (unsigned i = isBlock ? 1 : 0; i < cif->nargs; i++) {
    TypeConv* type = foreignCallArgType(closure, isBlock, i);
    CapturedArg handling;
    if (type == nullptr || !capturedArgHandling(type, cif->arg_types[i], &handling)) {
      return false;
    }
  }
  return true;
}

struct QueuedForeignThreadCall : ForeignThreadCall {
  Closure* closure = nullptr;
  std::vector<std::max_align_t> argStorage;
  std::vector<void*> argValues;
  std::vector<std::pair<void*, CapturedArg>> ownedArgs;
  std::max_align_t returnStorage[2];

  QueuedForeignThreadCall(Closure* closure, ClosureInvoker invoke, bool isBlock, ffi_cif* cif,
                          void* sourceArgs[])
      : closure(closure) {
    this->invoke = invoke;
    this->cif = cif;
    this->data = closure;
    this->ret = returnStorage;
    queued = true;
    closure->retain();

    const size_t slotSize = sizeof(std::max_align_t);
    size_t slots = 0;
    for (unsigned i = 0; i < cif->nargs; i++) {
      slots += (cif->arg_types[i]->size + slotSize - 1) / slotSize;
    }
    argStorage.resize(slots > 0 ? slots : 1);
    argValues.resize(cif->nargs);

    std::max_align_t* cursor = argStorage.data();
    for (unsigned i = 0; i < cif->nargs; i++) {
      size_t size = cif->arg_types[i]->size;
      memcpy(cursor, sourceArgs[i], size);
      argValues[i] = cursor;

      TypeConv* type = foreignCallArgType(closure, isBlock, i);
      CapturedArg handling = CapturedArg::Copy;
      if (type != nullptr) {
        capturedArgHandling(type, cif->arg_types[i], &handling);
      }
      void** value = (void**)cursor;
      if (handling == CapturedArg::Retain && *value != nullptr) {
        CFRetain((CFTypeRef)*value);
        ownedArgs.emplace_back(*value, handling);
      } else if (handling == CapturedArg::CopyBlock && *value != nullptr) {
        *value = Block_copy(*value);
        ownedArgs.emplace_back(*value, handling);
      }

      cursor += (size + slotSize - 1) / slotSize;
    }
    args = argValues.data();
  }

  ~QueuedForeignThreadCall() {
    for (auto& owned : ownedArgs) {
      if (owned.second == CapturedArg::CopyBlock) {
        Block_release(owned.first);
      } else {
        CFRelease((CFTypeRef)owned.first);
      }
    }
    closure->release();
  }
};

std::mutex gForeignThreadDispatchersMutex;
std::unordered_map<napi_env, napi_threadsafe_function> gForeignThreadDispatchers;

void runForeignThreadCall(napi_env env, napi_value /*jsCallback*/, void* /*context*/, void* data) {
  auto call = static_cast<ForeignThreadCall*>(data);
  bool ran = false;
  NSException* exception = nil;
  // env is null when the env is torn down with calls still pending.
  if (env != nullptr) {
    // The JS thread of a Node-API host has no autorelease pool of its own.
    @autoreleasepool {
      @try {
        call->invoke(call->cif, call->ret, call->args, call->data);
        ran = true;
      } @catch (NSException* caught) {
        exception = [caught retain];
      }
    }
  }

  if (call->queued) {
    if (exception != nil) {
      NSLog(@"Uncaught exception in a JS callback invoked from a background thread: %@",
            exception);
      [exception release];
    }
    delete static_cast<QueuedForeignThreadCall*>(call);
    return;
  }

  call->exception = exception;
  if (!ran && call->cif->rtype->type != FFI_TYPE_VOID) {
    memset(call->ret, 0, call->cif->rtype->size);
  }
  {
    std::lock_guard<std::mutex> lock(call->mutex);
    call->done = true;
  }
  call->cv.notify_one();
}

void finalizeForeignThreadDispatcher(napi_env /*env*/, void* finalizeData, void* /*hint*/) {
  std::lock_guard<std::mutex> lock(gForeignThreadDispatchersMutex);
  gForeignThreadDispatchers.erase(static_cast<napi_env>(finalizeData));
}

bool postForeignThreadCall(napi_env env, ForeignThreadCall* call) {
  std::lock_guard<std::mutex> lock(gForeignThreadDispatchersMutex);
  auto it = gForeignThreadDispatchers.find(env);
  return it != gForeignThreadDispatchers.end() &&
         napi_call_threadsafe_function(it->second, call, napi_tsfn_blocking) == napi_ok;
}

void callFromForeignThread(Closure* closure, ClosureInvoker invoke, bool isBlock, ffi_cif* cif,
                           void* ret, void* args[]) {
  if (canQueueForeignCall(closure, isBlock, cif)) {
    auto call = new QueuedForeignThreadCall(closure, invoke, isBlock, cif, args);
    if (!postForeignThreadCall(closure->env, call)) {
      delete call;
    }
    return;
  }

  ForeignThreadCall call;
  call.invoke = invoke;
  call.cif = cif;
  call.ret = ret;
  call.args = args;
  call.data = closure;

  std::unique_lock<std::mutex> lock(call.mutex);
  if (!postForeignThreadCall(closure->env, &call)) {
    lock.unlock();
    if (cif->rtype->type != FFI_TYPE_VOID) {
      memset(ret, 0, cif->rtype->size);
    }
    return;
  }
  call.cv.wait(lock, [&call] { return call.done; });
  lock.unlock();

  if (call.exception != nil) {
    @throw [call.exception autorelease];
  }
}
#endif  // ENABLE_JS_RUNTIME

void JSMethodCallback(ffi_cif* cif, void* ret, void* args[], void* data) {
#ifdef ENABLE_JS_RUNTIME
  invokeJSMethodCallback(cif, ret, args, data);
#else
  Closure* closure = (Closure*)data;
  if (std::this_thread::get_id() != closure->jsThreadId) {
    callFromForeignThread(closure, invokeJSMethodCallback, false, cif, ret, args);
    return;
  }
  invokeInNativeCallbackScope(closure->env, invokeJSMethodCallback, cif, ret, args, data);
#endif
}

void JSFunctionCallback(ffi_cif* cif, void* ret, void* args[], void* data) {
#ifdef ENABLE_JS_RUNTIME
  invokeJSFunctionCallback(cif, ret, args, data);
#else
  Closure* closure = (Closure*)data;
  if (std::this_thread::get_id() != closure->jsThreadId) {
    callFromForeignThread(closure, invokeJSFunctionCallback, false, cif, ret, args);
    return;
  }
  invokeInNativeCallbackScope(closure->env, invokeJSFunctionCallback, cif, ret, args, data);
#endif
}

struct JSBlockCallContext {
  ffi_cif* cif;
  void* ret;
  void** args;
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
}

#ifndef ENABLE_JS_RUNTIME
void invokeJSBlockCallback(ffi_cif* cif, void* ret, void* args[], void* data) {
  Closure* closure = (Closure*)data;
  JSBlockCallContext ctx{cif, ret, args};
  Closure::callBlockFromMainThread(closure->env, get_ref_value(closure->env, closure->func),
                                   closure, &ctx);
}
#endif  // ENABLE_JS_RUNTIME

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

  JSBlockCallContext ctx{cif, ret, args};

  if (currentThreadId == closure->jsThreadId || shouldInvokeCallbackOnNativeCallerThread()) {
#ifdef ENABLE_JS_RUNTIME
    NativeCallbackScope scope(env);
    Closure::callBlockFromMainThread(env, get_ref_value(env, closure->func), closure, &ctx);
#else
    invokeInNativeCallbackScope(env, invokeJSBlockCallback, cif, ret, args, closure);
#endif
  } else {
#ifndef ENABLE_JS_RUNTIME
    callFromForeignThread(closure, invokeJSBlockCallback, true, cif, ret, args);
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
      ffi_prep_cif(&cif, FFI_DEFAULT_ABI, argTypes.size() + skipArgs, rtype, this->atypes);

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

#ifndef ENABLE_JS_RUNTIME
void initForeignThreadCallbacks(napi_env env) {
  auto bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr || !napiSupportsThreadsafeFunctions(bridgeState->self_dl)) {
    return;
  }

  napi_value name = nullptr;
  napi_threadsafe_function tsfn = nullptr;
  napi_create_string_utf8(env, "NativeScriptForeignThreadCallback", NAPI_AUTO_LENGTH, &name);
  if (napi_create_threadsafe_function(env, nullptr, nullptr, name, 0, 1, env,
                                      finalizeForeignThreadDispatcher, nullptr,
                                      runForeignThreadCall, &tsfn) != napi_ok) {
    return;
  }
  napi_unref_threadsafe_function(env, tsfn);

  std::lock_guard<std::mutex> lock(gForeignThreadDispatchersMutex);
  gForeignThreadDispatchers[env] = tsfn;
}
#endif  // ENABLE_JS_RUNTIME

}  // namespace nativescript
