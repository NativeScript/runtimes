#ifndef BRIDGED_CLASS_H
#define BRIDGED_CLASS_H

#include <string>
#include <unordered_set>

#include "ClassMember.h"
#include "MetadataReader.h"
#include "node_api_util.h"
#include "objc/message.h"
#include "objc/runtime.h"

using namespace metagen;

namespace nativescript {

void setupObjCClassDecorator(napi_env env);

void initFastEnumeratorIteratorFactory(napi_env env,
                                       ObjCBridgeState* bridgeState);

NAPI_FUNCTION(registerClass);
NAPI_FUNCTION(import);
NAPI_FUNCTION(classGetter);
NAPI_FUNCTION(BridgedConstructor);

class ObjCBridgeState;

class ObjCClass {
 public:
  ObjCClass() {}
  ObjCClass(napi_env env, MDSectionOffset offset);
  ~ObjCClass();

  ObjCBridgeState* bridgeState;
  uint64_t bridgeStateToken = 0;
  napi_env env;
  napi_ref constructor;
  napi_ref prototype;
  MDSectionOffset metadataOffset;
  std::string name;
  Class nativeClass;
  ObjCClass* superclass;
  ObjCClassMemberMap members;
};

}  // namespace nativescript

#endif /* BRIDGED_CLASS_H */
