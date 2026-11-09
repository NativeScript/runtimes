#ifndef C_FUNCTION_H
#define C_FUNCTION_H

#include <cstdint>

#include "Cif.h"

namespace nativescript {

class ObjCBridgeState;

class CFunction {
 public:
  static napi_value jsCall(napi_env env, napi_callback_info cbinfo);
  static napi_value jsCallDirect(napi_env env, MDSectionOffset offset,
                                 size_t actualArgc,
                                 const napi_value* callArgs);

  CFunction(void* fnptr) : fnptr(fnptr) {}
  ~CFunction();

  void* fnptr;
  ObjCBridgeState* bridgeState = nullptr;
  Cif* cif = nullptr;
  uint8_t dispatchFlags = 0;
  bool dispatchLookupCached = false;
  uint64_t dispatchLookupSignatureHash = 0;
  uint64_t dispatchId = 0;
  void* preparedInvoker = nullptr;
  void* napiInvoker = nullptr;
};

}  // namespace nativescript

#endif /* C_FUNCTION_H */
