#ifndef NATIVE_NODE_VM_H
#define NATIVE_NODE_VM_H

#include "js_native_api_types.h"

namespace nativescript {

class VM {
 public:
  static napi_value CreateModule(napi_env env);
};

}  // namespace nativescript

#endif  // NATIVE_NODE_VM_H
