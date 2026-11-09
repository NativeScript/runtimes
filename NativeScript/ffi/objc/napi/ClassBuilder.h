#ifndef CLASS_BUILDER_H
#define CLASS_BUILDER_H

#include "Class.h"
#include "ClassMember.h"
#include "objc/runtime.h"

@protocol ObjCBridgeClassBuilderProtocol

@end

namespace nativescript {

class ObjCProtocol;

class ClassBuilder : public ObjCClass {
 public:
  ClassBuilder(napi_env env, napi_value constructor, Class explicitSuperClass = nullptr);
  ~ClassBuilder();

  void addProtocol(ObjCProtocol* protocol);
  std::vector<MethodDescriptor*> lookupMethodDescriptors(std::string& name, bool setter = false);
  MethodDescriptor* lookupMethodDescriptor(std::string& name, bool setter = false);
  void addMethod(std::string& name, MethodDescriptor* desc, napi_value key,
                 napi_value func = nullptr);

  void build();

  // Static callback for extending native classes
  static napi_value ExtendCallback(napi_env env, napi_callback_info info);

  MethodMap exposedMethods;
  std::unordered_set<ObjCProtocol*> protocols;
  bool isFinal = false;
};

}  // namespace nativescript

#endif /* CLASS_BUILDER_H */
