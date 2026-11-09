#ifndef JS_OBJECT_H
#define JS_OBJECT_H

#include "js_native_api.h"
#include "objc/runtime.h"

namespace nativescript {

id jsObjectToId(napi_env env, napi_value value);
napi_value idToJsObject(napi_env env, id obj);

}  // namespace nativescript

#endif /* JS_OBJECT_H */
