#ifndef BRIDGED_METHOD_H
#define BRIDGED_METHOD_H

#include <cstdint>
#include <iostream>
#include <vector>

#include "Cif.h"
#include "objc/runtime.h"

namespace nativescript {

class ObjCBridgeState;

enum MethodDescriptorKind : uint8_t {
  kMethodDescSignatureOffset,
  kMethodDescEncoding,
};

class MethodDescriptor {
 public:
  SEL selector;

  MethodDescriptorKind kind;

  MDSectionOffset signatureOffset;
  uint8_t dispatchFlags = 0;
  std::string encoding;
  bool isProperty = false;
  bool dispatchLookupCached = false;
  uint64_t dispatchLookupSignatureHash = 0;
  uint8_t dispatchLookupFlags = 0;
  uint64_t dispatchId = 0;
  void* preparedInvoker = nullptr;
  void* napiInvoker = nullptr;
  bool nserrorOutSignatureCached = false;
  bool nserrorOutSignature = false;

  MethodDescriptor() {}

  MethodDescriptor(SEL selector, MDSectionOffset offset)
      : selector(selector),
        kind(kMethodDescSignatureOffset),
        signatureOffset(offset) {}

  MethodDescriptor(SEL selector, char* encoding)
      : selector(selector), kind(kMethodDescEncoding), encoding(encoding) {}

  MethodDescriptor(SEL selector, std::string encoding)
      : kind(kMethodDescEncoding), encoding(encoding) {
    this->selector = selector;
  }
};

typedef std::unordered_map<std::string, MethodDescriptor> MethodMap;

class ObjCClassMember;

// Static and instance members may share a JS name with different selectors
// and signatures (e.g. NSEvent.modifierFlags); keying by name alone makes the
// last-defined variant's selector/cif win for both, so the key must carry
// staticness.
struct ObjCClassMemberKey {
  std::string name;
  bool isStatic = false;

  bool operator==(const ObjCClassMemberKey& other) const {
    return isStatic == other.isStatic && name == other.name;
  }
};

struct ObjCClassMemberKeyHash {
  size_t operator()(const ObjCClassMemberKey& key) const {
    return std::hash<std::string>()(key.name) ^
           (key.isStatic ? 0x9e3779b97f4a7c15ull : 0);
  }
};

typedef std::unordered_map<ObjCClassMemberKey, ObjCClassMember,
                           ObjCClassMemberKeyHash>
    ObjCClassMemberMap;

class ObjCClass;

struct ObjCClassMemberOverload {
  MethodDescriptor method;
  Cif* cif = nullptr;

  ObjCClassMemberOverload(SEL selector, MDSectionOffset offset,
                          uint8_t dispatchFlags)
      : method(selector, offset) {
    method.dispatchFlags = dispatchFlags;
  }
};

class ObjCClassMember {
 public:
  static void defineMembers(napi_env env, ObjCClassMemberMap& memberMap,
                            MDSectionOffset offset, napi_value constructor,
                            ObjCClass* cls = nullptr);

  static napi_value jsCall(napi_env env, napi_callback_info cbinfo);
  static napi_value jsCallInit(napi_env env, napi_callback_info cbinfo);
  static napi_value jsGetter(napi_env env, napi_callback_info cbinfo);
  static napi_value jsReadOnlySetter(napi_env env, napi_callback_info cbinfo);
  static napi_value jsSetter(napi_env env, napi_callback_info cbinfo);
  static napi_value jsCallDirect(napi_env env, ObjCClassMember* method,
                                 napi_value jsThis, size_t actualArgc,
                                 const napi_value* callArgs);
  static napi_value jsGetterDirect(napi_env env, ObjCClassMember* method,
                                   napi_value jsThis);
  static napi_value jsSetterDirect(napi_env env, ObjCClassMember* method,
                                   napi_value jsThis, napi_value value);
  static napi_value jsReadOnlySetterDirect(napi_env env);
  void addOverload(SEL selector, MDSectionOffset offset, uint8_t dispatchFlags);

  ObjCClassMember(ObjCBridgeState* bridgeState, SEL selector,
                  MDSectionOffset offset, MDMemberFlag flags)
      : bridgeState(bridgeState),
        methodOrGetter(MethodDescriptor(selector, offset)),
        returnOwned((flags & metagen::mdMemberReturnOwned) != 0),
        classMethod((flags & metagen::mdMemberStatic) != 0),
        cls(nullptr) {
    methodOrGetter.dispatchFlags = returnOwned ? 1 : 0;
  }

  ObjCClassMember(ObjCBridgeState* bridgeState, SEL getterSelector,
                  SEL setterSelector, MDSectionOffset getterOffset,
                  MDSectionOffset setterOffset, MDMemberFlag flags)
      : bridgeState(bridgeState),
        methodOrGetter(MethodDescriptor(getterSelector, getterOffset)),
        setter(MethodDescriptor(setterSelector, setterOffset)),
        returnOwned((flags & metagen::mdMemberReturnOwned) != 0),
        classMethod((flags & metagen::mdMemberStatic) != 0),
        cls(nullptr) {
    methodOrGetter.isProperty = true;
    setter.isProperty = true;
    methodOrGetter.dispatchFlags = returnOwned ? 1 : 0;
    setter.dispatchFlags = 0;
  }

  ObjCBridgeState* bridgeState;
  MethodDescriptor methodOrGetter;
  MethodDescriptor setter;
  Cif* cif = nullptr;
  Cif* setterCif = nullptr;
  bool returnOwned;
  bool classMethod;
  ObjCClass* cls;
  std::vector<ObjCClassMemberOverload> overloads;
};

}  // namespace nativescript

#endif /* BRIDGED_METHOD_H */
