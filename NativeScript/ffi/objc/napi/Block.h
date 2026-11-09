#ifndef BLOCK_H
#define BLOCK_H

#include <cstdint>
#include <cstdlib>

#include "Cif.h"
#include "Closure.h"
#include "node_api_util.h"

namespace nativescript {

class FunctionPointer {
 public:
  void* function;
  metagen::MDSectionOffset offset;
  Cif* cif;
  bool ownsCif = false;
  bool dispatchLookupCached = false;
  uint64_t dispatchLookupSignatureHash = 0;
  uint64_t dispatchId = 0;
  void* preparedInvoker = nullptr;

  static napi_value wrap(napi_env env, void* function,
                         metagen::MDSectionOffset offset, bool isBlock);
  static napi_value wrapWithEncoding(napi_env env, void* function,
                                     const char* encoding, bool isBlock);
  static void finalize(napi_env env, void* finalize_data, void* finalize_hint);

  static napi_value jsCallAsCFunction(napi_env env, napi_callback_info cbinfo);
  static napi_value jsCallAsBlock(napi_env env, napi_callback_info cbinfo);
};

id registerBlock(napi_env env, Closure* closure, napi_value callback);
napi_value getCachedBlockCallback(napi_env env, void* blockPtr);
bool isObjCBlockObject(id obj);
const char* getObjCBlockSignature(void* blockPtr);

NAPI_FUNCTION(registerBlock);

}  // namespace nativescript

#endif /* BLOCK_H */
