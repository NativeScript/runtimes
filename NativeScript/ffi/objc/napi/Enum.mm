#include "Enum.h"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <vector>
#include "ObjCBridge.h"
#include "ffi/objc/shared/EnumMemberAliases.h"

namespace nativescript {

namespace {
inline void defineConstantIfMissing(napi_env env, napi_value object, const std::string& name,
                                    napi_value value,
                                    napi_property_attributes attributes = napi_enumerable) {
  bool hasProperty = false;
  napi_has_named_property(env, object, name.c_str(), &hasProperty);
  if (hasProperty) {
    return;
  }

  napi_property_descriptor prop = {
      .utf8name = name.c_str(),
      .method = nullptr,
      .getter = nullptr,
      .setter = nullptr,
      .value = value,
      .attributes = attributes,
      .data = nullptr,
  };
  napi_define_properties(env, object, 1, &prop);
}

inline std::string stripEnumSuffix(const std::string& enumName) {
  static const std::vector<std::string> suffixes = {
      "Options", "Option", "Enums", "Enum",   "Result", "Direction", "Orientation",
      "Style",   "Mask",   "Type",  "Status", "Modes",  "Mode",      "s"};

  for (const auto& suffix : suffixes) {
    if (enumName.size() > suffix.size() &&
        enumName.compare(enumName.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return enumName.substr(0, enumName.size() - suffix.size());
    }
  }

  return enumName;
}
}  // namespace

void ObjCBridgeState::registerEnumGlobals(napi_env env, napi_value global) {
  MDSectionOffset offset = metadata->enumsOffset;
  while (offset < metadata->signaturesOffset) {
    MDSectionOffset originalOffset = offset;
    auto name = metadata->getString(offset);

    offset = metadata->forEachEnumMember(
        originalOffset, [&](const char*, const std::string& cName, int64_t value) {
          if (cName.empty()) {
            return;
          }
          napi_value member;
          napi_create_int64(env, value, &member);
          defineConstantIfMissing(env, global, cName, member,
                                  (napi_property_attributes)(napi_enumerable | napi_configurable));
        });

    napi_property_descriptor prop = {
        .utf8name = name,
        .method = nullptr,
        .getter = JS_enumGetter,
        .setter = nullptr,
        .value = nullptr,
        .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
        .data = (void*)((size_t)originalOffset),
    };
    napi_define_properties(env, global, 1, &prop);
  }
}

NAPI_FUNCTION(enumGetter) {
  void* data;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, nullptr, &data);
  MDSectionOffset offset = (MDSectionOffset)((size_t)data);
  MDSectionOffset originalOffset = offset;
  auto bridgeState = ObjCBridgeState::InstanceData(env);

  auto cached = bridgeState->mdValueCache[offset];
  if (cached != nullptr) {
    return get_ref_value(env, cached);
  }

  napi_value result;
  napi_create_object(env, &result);

  // enum name
  auto enumName = bridgeState->metadata->getString(offset);
  std::string enumNameStr = enumName;

  std::string strippedPrefix = stripEnumSuffix(enumNameStr);

  bridgeState->metadata->forEachEnumMember(originalOffset, [&](const char* name,
                                                               const std::string& cName,
                                                               int64_t value) {
    napi_value member;
    napi_create_int64(env, value, &member);

    std::string memberName = name;
    auto aliases = enum_members::aliasesFor(enumNameStr, strippedPrefix, memberName, cName);

    for (const auto& alias : aliases) {
      defineConstantIfMissing(env, result, alias.name, member);
    }

    const std::string& reverseCanonical = aliases.size() > 1 ? aliases[1].name : memberName;

    // reverse mapping enum[value] -> canonical member name
    char valueKey[32];
    snprintf(valueKey, sizeof(valueKey), "%lld", (long long)value);
    bool hasReverseMapping = false;
    napi_has_named_property(env, result, valueKey, &hasReverseMapping);
    if (!hasReverseMapping) {
      napi_value reverseName;
      napi_create_string_utf8(env, reverseCanonical.c_str(), NAPI_AUTO_LENGTH, &reverseName);
      napi_property_descriptor reverseProp = {
          .utf8name = valueKey,
          .method = nullptr,
          .getter = nullptr,
          .setter = nullptr,
          .value = reverseName,
          .attributes = napi_enumerable,
          .data = nullptr,
      };
      napi_define_properties(env, result, 1, &reverseProp);
    }
  });

  bridgeState->mdValueCache[originalOffset] = make_ref(env, result);

  return result;
}

}  // namespace nativescript
