#include "JSObject.h"
#include "ObjCBridge.h"
#include "js_native_api.h"

#import <Foundation/Foundation.h>

@interface JSObject : NSObject {
  napi_env env;
  napi_ref ref;
  nativescript::ObjCBridgeState* bridgeState;
  uint64_t bridgeStateToken;
}

- (instancetype)initWithEnv:(napi_env)env value:(napi_value)value;
- (napi_value)value;

@end

@implementation JSObject

- (instancetype)initWithEnv:(napi_env)_env value:(napi_value)value {
  [super init];
  self->env = _env;
  napi_create_reference(env, value, 1, &ref);
  napi_wrap(env, value, self, nullptr, nullptr, nullptr);
  bridgeState = nativescript::ObjCBridgeState::InstanceData(env);
  bridgeStateToken = bridgeState != nullptr ? bridgeState->lifetimeToken : 0;
  return self;
}

- (napi_value)value {
  if (env == nullptr || ref == nullptr) {
    return nullptr;
  }

  napi_value result;
  napi_get_reference_value(env, ref, &result);
  return result;
}

- (void)dealloc {
  if (env != nullptr && ref != nullptr &&
      (bridgeState == nullptr || nativescript::IsBridgeStateLive(bridgeState, bridgeStateToken))) {
    nativescript::DeleteReferenceOnOwningThread(env, bridgeState, bridgeStateToken, ref);
    ref = nullptr;
  }
  bridgeState = nullptr;
  bridgeStateToken = 0;
  env = nullptr;
  [super dealloc];
}

@end

@protocol Test

@optional
@property(nonatomic, readonly) bool optionalString;

@end

namespace nativescript {

id jsObjectToId(napi_env env, napi_value value) {
  return [[[JSObject alloc] initWithEnv:env value:value] autorelease];
}

napi_value idToJsObject(napi_env env, id obj) {
  if (obj == nil) {
    return nullptr;
  }
  if ([obj isKindOfClass:[JSObject class]]) {
    return [((JSObject*)obj) value];
  }
  return nil;
}

}  // namespace nativescript
