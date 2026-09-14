#include "Enum.h"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <vector>
#include "ObjCBridge.h"

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

inline bool startsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
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

inline bool isNSComparisonResultOrderingName(const std::string& enumName,
                                             const std::string& member) {
  if (enumName != "NSComparisonResult") {
    return false;
  }
  return member == "Ascending" || member == "Same" || member == "Descending";
}
}  // namespace

void ObjCBridgeState::registerEnumGlobals(napi_env env, napi_value global) {
  MDSectionOffset offset = metadata->enumsOffset;
  while (offset < metadata->signaturesOffset) {
    MDSectionOffset originalOffset = offset;
    auto name = metadata->getString(offset);
    offset += sizeof(MDSectionOffset);

    bool next = true;
    while (next) {
      auto nameOffset = metadata->getOffset(offset);
      next = (nameOffset & mdSectionOffsetNext) != 0;
      nameOffset &= ~mdSectionOffsetNext;
      auto memberName = metadata->resolveString(nameOffset);
      offset += sizeof(MDSectionOffset);
      int64_t value = metadata->getEnumValue(offset);
      offset += sizeof(int64_t);

      napi_value member;
      napi_create_int64(env, value, &member);
      defineConstantIfMissing(env, global, memberName, member,
                              (napi_property_attributes)(napi_enumerable | napi_configurable));
    }

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
  offset += sizeof(MDSectionOffset);

  std::string strippedPrefix = stripEnumSuffix(enumNameStr);

  napi_value global;
  napi_get_global(env, &global);

  bool next = true;
  while (next) {
    auto nameOffset = bridgeState->metadata->getOffset(offset);
    next = (nameOffset & mdSectionOffsetNext) != 0;
    nameOffset &= ~mdSectionOffsetNext;
    auto name = bridgeState->metadata->resolveString(nameOffset);
    offset += sizeof(MDSectionOffset);
    int64_t value = bridgeState->metadata->getEnumValue(offset);
    offset += sizeof(int64_t);

    napi_value member;
    napi_create_int64(env, value, &member);

    std::string memberName = name;
    std::vector<std::string> aliases;
    aliases.push_back(memberName);

    if (!strippedPrefix.empty() && startsWith(memberName, strippedPrefix) &&
        memberName.size() > strippedPrefix.size()) {
      aliases.push_back(memberName.substr(strippedPrefix.size()));
    } else if (!strippedPrefix.empty() && !startsWith(memberName, strippedPrefix)) {
      aliases.push_back(strippedPrefix + memberName);
    }

    if (startsWith(enumNameStr, "NS") && !startsWith(memberName, "NS")) {
      aliases.push_back(std::string("NS") + memberName);
    }

    if (enumNameStr == "NSStringCompareOptions" && !memberName.ends_with("Search")) {
      aliases.push_back(memberName + "Search");
      aliases.push_back(std::string("NS") + memberName + "Search");
    }

    if (!startsWith(memberName, "k")) {
      aliases.push_back(std::string("k") + enumNameStr + memberName);
    }

    if (isNSComparisonResultOrderingName(enumNameStr, memberName)) {
      aliases.push_back(std::string("Ordered") + memberName);
      aliases.push_back(std::string("NSOrdered") + memberName);
    }

    std::vector<std::string> uniqueAliases;
    uniqueAliases.reserve(aliases.size());
    std::unordered_set<std::string> seenAliases;
    for (const auto& alias : aliases) {
      if (seenAliases.insert(alias).second) {
        uniqueAliases.push_back(alias);
      }
    }

    for (const auto& alias : uniqueAliases) {
      defineConstantIfMissing(env, result, alias, member);
      defineConstantIfMissing(env, global, alias, member,
                              (napi_property_attributes)(napi_enumerable | napi_configurable));
    }

    std::string reverseCanonical = uniqueAliases.size() > 1 ? uniqueAliases[1] : memberName;

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
  }

  bridgeState->mdValueCache[originalOffset] = make_ref(env, result);

  return result;
}

}  // namespace nativescript
