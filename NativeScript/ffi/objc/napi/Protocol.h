#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <string>
#include <unordered_set>

#include "ClassMember.h"
#include "MetadataReader.h"
#include "node_api_util.h"

using namespace metagen;

namespace nativescript {

class ObjCBridgeState;

NAPI_FUNCTION(protocolGetter);

class ObjCProtocol {
 public:
  static napi_value jsConstructor(napi_env env, napi_callback_info cbinfo);

  ObjCProtocol(napi_env env, MDSectionOffset offset);
  ~ObjCProtocol();

  napi_env env;
  ObjCBridgeState* bridgeState = nullptr;
  uint64_t bridgeStateToken = 0;
  MDSectionOffset metadataOffset;
  std::string name;
  napi_ref constructor;
  MDSectionOffset membersOffset;
  ObjCClassMemberMap members;
  std::unordered_set<ObjCProtocol*> protocols;
};

}  // namespace nativescript

#endif /* PROTOCOL_H */
