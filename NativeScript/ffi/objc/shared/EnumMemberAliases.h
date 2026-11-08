#ifndef NATIVESCRIPT_FFI_OBJC_SHARED_ENUM_MEMBER_ALIASES_H
#define NATIVESCRIPT_FFI_OBJC_SHARED_ENUM_MEMBER_ALIASES_H

#include <string>
#include <unordered_set>
#include <vector>

namespace nativescript {
namespace enum_members {

struct Alias {
  std::string name;
  // Only the C name may become a global; the other aliases are enum-object
  // conveniences and can shadow unrelated globals (e.g. Center, Zone).
  bool isCName = false;
};

inline bool startsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

// The first alias is always the stored member name. cName is empty when the
// metadata does not record C names; then no alias is tagged as the C name.
inline std::vector<Alias> aliasesFor(const std::string& enumName,
                                     const std::string& strippedEnumName,
                                     const std::string& storedName,
                                     const std::string& cName) {
  std::vector<std::string> names;
  names.push_back(storedName);

  if (!strippedEnumName.empty() && startsWith(storedName, strippedEnumName) &&
      storedName.size() > strippedEnumName.size()) {
    names.push_back(storedName.substr(strippedEnumName.size()));
  } else if (!strippedEnumName.empty() &&
             !startsWith(storedName, strippedEnumName)) {
    names.push_back(strippedEnumName + storedName);
  }

  if (startsWith(enumName, "NS") && !startsWith(storedName, "NS")) {
    names.push_back(std::string("NS") + storedName);
  }

  if (enumName == "NSStringCompareOptions" && !endsWith(storedName, "Search")) {
    names.push_back(storedName + "Search");
    names.push_back(std::string("NS") + storedName + "Search");
  }

  if (!startsWith(storedName, "k")) {
    names.push_back(std::string("k") + enumName + storedName);
  }

  if (enumName == "NSComparisonResult" &&
      (storedName == "Ascending" || storedName == "Same" ||
       storedName == "Descending")) {
    names.push_back(std::string("Ordered") + storedName);
    names.push_back(std::string("NSOrdered") + storedName);
  }

  if (!cName.empty()) {
    names.push_back(cName);
  }

  std::vector<Alias> aliases;
  aliases.reserve(names.size());
  std::unordered_set<std::string> seen;
  for (auto& name : names) {
    if (!name.empty() && seen.insert(name).second) {
      bool isCName = name == cName;
      aliases.push_back(Alias{std::move(name), isCName});
    }
  }
  return aliases;
}

}  // namespace enum_members
}  // namespace nativescript

#endif  // NATIVESCRIPT_FFI_OBJC_SHARED_ENUM_MEMBER_ALIASES_H
