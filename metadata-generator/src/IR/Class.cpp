#include <unordered_map>
#include <unordered_set>

#include "IR.h"
#include "clang-c/Index.h"

namespace metagen {

ClassDecl::ClassDecl(CXCursor cursor) {
  framework = getFrameworkName(cursor);
  if (gCaptureAvailability) {
    availability = getAvailabilityInfo(cursor);
  }

  CXString cxname = clang_getCursorSpelling(cursor);
  name = clang_getCString(cxname);
  clang_disposeString(cxname);

  CXPrintingPolicy policy = clang_getCursorPrintingPolicy(cursor);
  CXString printed = clang_getCursorPrettyPrinted(cursor, policy);
  std::string prettyText = clang_getCString(printed);
  clang_disposeString(printed);
  clang_PrintingPolicy_dispose(policy);

  std::string pattern = "objc_runtime_name(\"";
  size_t pos = prettyText.find(pattern);
  if (pos != std::string::npos) {
    size_t start = pos + pattern.length();
    size_t end = prettyText.find('"', start);
    if (end != std::string::npos) {
      std::string runtime = prettyText.substr(start, end - start);
      this->runtimeName = runtime;
    }
  }

  struct ClassVisitorState {
    ClassDecl* cls;
    size_t memberCount = 0;
  } state = {this};

  clang_visitChildren(
      cursor,
      [](CXCursor cursor, CXCursor, CXClientData clientData) {
        auto* state = static_cast<ClassVisitorState*>(clientData);
        auto* cls = state->cls;

        CXCursorKind kind = clang_getCursorKind(cursor);

        // Preserve superclass references even when the immediate superclass is
        // unavailable, so we can later collapse the chain to the nearest
        // available ancestor.
        if (kind != CXCursor_ObjCSuperClassRef && !isAvailable(cursor)) {
          return CXChildVisit_Continue;
        }

        switch (kind) {
          case CXCursor_ObjCSuperClassRef: {
            CXString name = clang_getCursorSpelling(cursor);
            std::string nameStr = clang_getCString(name);
            cls->superClassName = nameStr;
            clang_disposeString(name);
            break;
          }

          case CXCursor_ObjCProtocolRef: {
            CXString name = clang_getCursorSpelling(cursor);
            std::string nameStr = clang_getCString(name);
            cls->protocolNames.emplace_back(nameStr);
            clang_disposeString(name);
            break;
          }

          case CXCursor_TemplateTypeParameter: {
            CXString name = clang_getCursorSpelling(cursor);
            std::string nameStr = clang_getCString(name);
            cls->typeParameters.emplace_back(nameStr);
            clang_disposeString(name);
            break;
          }

          case CXCursor_ObjCPropertyDecl:
          case CXCursor_ObjCClassMethodDecl:
          case CXCursor_ObjCInstanceMethodDecl: {
            state->memberCount++;
            break;
          }

          default:
            break;
        }

        return CXChildVisit_Continue;
      },
      &state);

  members.reserve(state.memberCount);

  clang_visitChildren(
      cursor,
      [](CXCursor cursor, CXCursor, CXClientData clientData) {
        if (!isAvailable(cursor)) {
          return CXChildVisit_Continue;
        }

        auto cls = (ClassDecl*)clientData;

        CXCursorKind kind = clang_getCursorKind(cursor);

        switch (kind) {
          case CXCursor_ObjCPropertyDecl:
          case CXCursor_ObjCClassMethodDecl:
          case CXCursor_ObjCInstanceMethodDecl: {
            auto member = MemberDecl(cursor, &cls->typeParameters);
            member.parentClassName = cls->name;
            cls->members.emplace_back(std::move(member));
            break;
          }
          default:
            break;
        }

        return CXChildVisit_Continue;
      },
      this);
}

MemberDecl* ClassDecl::getMemberNamed(const std::string& name) {
  for (auto& member : members) {
    if (member.name == name) {
      return &member;
    }
  }
  return nullptr;
}

void removeDuplicateMethods(std::vector<MemberDecl>& members) {
  std::vector<MemberDecl> filteredMembers;
  filteredMembers.reserve(members.size());

  // Remove getter methods in favor of properties

  std::unordered_set<std::string> instanceFiltered, classFiltered,
      instanceFilteredProperties, classFilteredProperties;
  std::unordered_map<std::string, size_t> instancePropertyIndex,
      classPropertyIndex;

  for (auto& member : members) {
    auto& filtered =
        member.isStatic ? classFilteredProperties : instanceFilteredProperties;
    if (member.kind == kMemberProperty) {
      filtered.insert(member.name);
      // Keep setter methods hidden when a property with the same setter exists.
      if (!member.setterName.empty()) {
        filtered.insert(member.setterName);
      }
    }
  }

  // Check whether we should declare

  for (auto& member : members) {
    auto& filtered = member.isStatic ? classFiltered : instanceFiltered;
    auto& filteredProps =
        member.isStatic ? classFilteredProperties : instanceFilteredProperties;

    // Prevent property accessors from being emitted as methods when a matching
    // property exists.
    if (member.kind == kMemberMethod) {
      if (filteredProps.contains(member.name)) {
        continue;
      }
      std::string key = "M:";
      key += member.name;
      key += "|";
      key += member.methodSelector;
      if (filtered.contains(key)) {
        continue;
      }
      filteredMembers.emplace_back(member);
      filtered.insert(key);
    } else {
      auto& propertyIndex =
          member.isStatic ? classPropertyIndex : instancePropertyIndex;
      auto existing = propertyIndex.find(member.name);
      if (existing == propertyIndex.end()) {
        propertyIndex.emplace(member.name, filteredMembers.size());
        filteredMembers.emplace_back(member);
      } else {
        MemberDecl& previous = filteredMembers[existing->second];
        if (previous.isReadonly && !member.isReadonly) {
          previous = member;
        }
      }
    }
  }

  members = std::move(filteredMembers);
}

void MetadataFactory::processClassRefs() {
  while (!referencedClasses.empty()) {
    std::unordered_set<std::string> refs;
    refs.swap(referencedClasses);

    for (const std::string& name : refs) {
      if (classes.contains(name)) {
        continue;
      }

      auto skippedIt = skippedClasses.find(name);
      if (skippedIt != skippedClasses.end()) {
        if (skippedIt->second.unavailable) {
          if (!skippedIt->second.superClassName.empty()) {
            referencedClasses.emplace(skippedIt->second.superClassName);
          }
          continue;
        }
        auto [inserted, _] = classes.try_emplace(name, skippedIt->second);
        postProcessClass(inserted->second);
      } else {
        std::cerr << "ERROR: Unknown class " << name << std::endl;
        missingClasses.emplace(name);
      }
    }
  }
}

void convertMethodToPropertyIfNeeded(MemberDecl& member,
                                     MemberDecl* memberInSuperclass,
                                     bool isSuperclass) {
  if (memberInSuperclass != nullptr) {
    if (memberInSuperclass->isStatic != member.isStatic) {
      return;
    }

    if (memberInSuperclass->kind == member.kind &&
        memberInSuperclass->toString() != member.toString() && isSuperclass) {
      if (member.kind == kMemberMethod) {
        member.addOverloadFrom(*memberInSuperclass);
      } else {
        member.tsIgnore = true;
      }
    }

    if (member.kind == kMemberProperty) return;

    if (memberInSuperclass->kind == kMemberProperty) {
      // If the superclass has a property with the same name, we need to
      // convert this method into a property.
      member.kind = kMemberProperty;
      member.propertyType = member.returnType;
      if (member.propertyType.kind == kTypeInstanceObject && member.isStatic) {
        member.propertyType.kind = kTypeObject;
        member.propertyType.className = member.parentClassName;
      }
      member.parameters.clear();
      member.isReadonly = memberInSuperclass->isReadonly;
      member.setterName = memberInSuperclass->setterName;
      member.getterSelector = memberInSuperclass->getterSelector;
      member.setterSelector = memberInSuperclass->setterSelector;
    }
  }
}

void processDerivedClassRefs(MemberDecl& member,
                             std::vector<ClassDecl*>& derived) {
  for (ClassDecl* cls : derived) {
    convertMethodToPropertyIfNeeded(member, cls->getMemberNamed(member.name),
                                    false);
    processDerivedClassRefs(member, cls->derivedClassRefs);
  }
}

void ClassDecl::postProcessMembers() {
  for (MemberDecl& member : members) {
    ClassDecl* currentSuperClass = superClassRef;

    while (currentSuperClass != nullptr) {
      convertMethodToPropertyIfNeeded(
          member, currentSuperClass->getMemberNamed(member.name), true);
      currentSuperClass = currentSuperClass->superClassRef;
    }

    processDerivedClassRefs(member, derivedClassRefs);

    for (ProtocolDecl* protocol : protocolRefs) {
      convertMethodToPropertyIfNeeded(
          member, protocol->getMemberNamed(member.name), false);
    }

    if (member.tsIgnore && !tsIgnore) {
      tsIgnore = true;
    }
  }
}

}  // namespace metagen
