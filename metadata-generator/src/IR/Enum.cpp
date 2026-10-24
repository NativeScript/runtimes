#include "IR.h"

namespace metagen {

// Transforms verbose enum member names into shorter ones, as we fit them inside
// a JS object instead of making top level object.
// so enum like: NSBackgroundActivityResult
// - NSBackgroundActivityResultFinished
// - NSBackgroundActivityResultDeferred
// becomes:
// NSBackgroundActivityResult
// - Finished
// - Deferred
// Basically, split the name into parts (assuming camel case), and then remove
// the repeating parts from the beginning and end of the name.
void transformEnumMemberNames(std::vector<EnumConstDecl>& members,
                              std::string& prefix, std::string& suffix) {
  std::vector<std::vector<std::string>> parts;
  parts.reserve(members.size());
  size_t skip_begin = 0, skip_end = 0;
  std::vector<std::string> largestPart;

  for (const auto& member : members) {
    auto split = splitCamelCase(member.name);
    parts.emplace_back(split);
    if (split.size() > largestPart.size()) {
      largestPart = split;
    }
  }

  skip_begin = skip_end = largestPart.size();

  for (size_t i = 0; i < parts.size(); i++) {
    const auto& part = parts[i];
    size_t skip = 0;

    for (size_t j = 0; j < part.size(); j++) {
      if (j >= largestPart.size()) {
        break;
      }

      if (part[j] == largestPart[j]) {
        skip++;
      } else {
        break;
      }
    }

    if (skip < skip_begin) {
      skip_begin = skip;
    }

    skip = 0;

    for (size_t k = 0; k < part.size(); k++) {
      if (k >= largestPart.size()) {
        break;
      }

      const size_t j = part.size() - 1 - k;
      if (part[j] == largestPart[largestPart.size() - 1 - skip]) {
        skip++;
      } else {
        break;
      }
    }

    if (skip < skip_end) {
      skip_end = skip;
    }
  }

  for (size_t j = 0; j < skip_begin; j++) {
    prefix += largestPart[j];
  }
  for (size_t j = largestPart.size() - skip_end; j < largestPart.size(); j++) {
    suffix += largestPart[j];
  }

  for (size_t i = 0; i < members.size(); i++) {
    const auto& part = parts[i];
    std::string name;
    name.reserve(members[i].name.size());

    for (size_t j = skip_begin; j < part.size() - skip_end; j++) {
      name += part[j];
    }

    if (name == "") {
      name = members[i].name;
    }

    members[i].name = name;
  }
}

EnumDecl::EnumDecl(CXCursor cursor) {
  framework = getFrameworkName(cursor);
  if (gCaptureAvailability) {
    availability = getAvailabilityInfo(cursor);
  }

  CXString cxname = clang_getCursorSpelling(cursor);
  name = clang_getCString(cxname);
  clang_disposeString(cxname);

  if (name.find("(unnamed") != std::string::npos) {
    name = "";
  }

  clang_visitChildren(
      cursor,
      [](CXCursor cursor, CXCursor, CXClientData clientData) {
        if (!isAvailable(cursor)) {
          return CXChildVisit_Continue;
        }

        auto state = (EnumDecl*)clientData;

        CXCursorKind kind = clang_getCursorKind(cursor);

        switch (kind) {
          case CXCursor_EnumConstantDecl: {
            CXString name = clang_getCursorSpelling(cursor);
            std::string nameStr = clang_getCString(name);
            clang_disposeString(name);
            auto value = clang_getEnumConstantDeclValue(cursor);
            EnumConstDecl decl;
            decl.name = nameStr;
            decl.cName = nameStr;
            decl.value = value;
            if (gCaptureAvailability) {
              decl.availability = getAvailabilityInfo(cursor);
            }
            state->constants.emplace_back(decl);
            break;
          }

          default:
            break;
        }

        return CXChildVisit_Continue;
      },
      this);

  if (!name.empty())
    transformEnumMemberNames(constants, memberPrefix, memberSuffix);
}

void MetadataFactory::processEnumRefs() {
  std::unordered_set<std::string> refs;
  refs.swap(referencedEnums);

  for (const std::string& ref : refs) {
    if (enums.contains(ref)) {
      continue;
    }

    auto skippedIt = skippedEnums.find(ref);
    if (skippedIt != skippedEnums.end()) {
      enums.insert_or_assign(skippedIt->second.name, skippedIt->second);
    } else {
      std::cerr << "processEnumRef: Enum " << ref << " not found" << std::endl;
    }
  }
}

}  // namespace metagen
