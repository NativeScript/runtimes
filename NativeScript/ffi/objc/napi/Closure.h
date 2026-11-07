#ifndef CLOSURE_H
#define CLOSURE_H

#include <CoreFoundation/CFRunLoop.h>

#include <atomic>
#include <string>
#include <thread>

#include "MetadataReader.h"
#include "TypeConv.h"
#include "ffi.h"
#include "node_api_util.h"
#include "objc/runtime.h"

namespace nativescript {

class ObjCBridgeState;

struct NapiNativeCallbackExceptionCapture {
  napi_env env = nullptr;
  napi_ref errorRef = nullptr;

  ~NapiNativeCallbackExceptionCapture();
  void clear();
};

class ScopedNapiNativeCallbackExceptionCapture {
 public:
  explicit ScopedNapiNativeCallbackExceptionCapture(
      NapiNativeCallbackExceptionCapture* capture);
  ~ScopedNapiNativeCallbackExceptionCapture();

  ScopedNapiNativeCallbackExceptionCapture(
      const ScopedNapiNativeCallbackExceptionCapture&) = delete;
  ScopedNapiNativeCallbackExceptionCapture& operator=(
      const ScopedNapiNativeCallbackExceptionCapture&) = delete;

 private:
  NapiNativeCallbackExceptionCapture* capture_ = nullptr;
};

bool recordNapiNativeCallbackException(napi_env env, napi_value error);
bool rethrowNapiNativeCallbackException(
    napi_env env, NapiNativeCallbackExceptionCapture& capture);

class Closure {
 public:
  static void callBlockFromMainThread(napi_env env, napi_value js_cb,
                                      void* context, void* data);
  static void destroyOnOwningThread(Closure* closure);

  Closure(napi_env env, std::string typeEncoding, bool isBlock, bool isMethod = false);
  Closure(napi_env env, MDMetadataReader* reader, MDSectionOffset offset,
          bool isBlock = false, std::string* encoding = nullptr,
          bool isMethod = false, bool isGetter = false, bool isSetter = false);

  ~Closure();
  void retain();
  void release();

  napi_env env = nullptr;
  ObjCBridgeState* bridgeState = nullptr;
  uint64_t bridgeStateToken = 0;
  napi_ref thisConstructor;
  napi_ref func = nullptr;
  bool isGetter = false;
  bool isSetter = false;
  std::string propertyName;
  SEL selector = nullptr;
  napi_threadsafe_function tsfn = nullptr;

  std::thread::id jsThreadId = std::this_thread::get_id();
  CFRunLoopRef jsRunLoop = CFRunLoopGetCurrent();
  std::atomic<int> retainCount{1};

  ffi_cif cif;
  ffi_closure* closure;
  void* fnptr;
  ffi_type** atypes = nullptr;  // Track malloc'd atypes array

  std::shared_ptr<TypeConv> returnType;
  std::vector<std::shared_ptr<TypeConv>> argTypes;
};

}  // namespace nativescript

#endif /* CLOSURE_H */
