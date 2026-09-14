#ifndef OBJECT_H
#define OBJECT_H

#include "ObjCBridge.h"

namespace nativescript {

void initProxyFactory(napi_env env, ObjCBridgeState* bridgeState);
void attachObjectLifecycleAssociation(napi_env env, id object);
void transferOwnershipToNative(napi_env env, napi_value value, id object);

}  // namespace nativescript

#endif /* OBJECT_H */
