#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "IR.h"
#include "MetadataWriter.h"

namespace metagen {

void MetadataFactory::process(CXCursor cursor, bool checkAvailability) {
  bool _previous_checkAvailability = _checkAvailability;
  _checkAvailability = checkAvailability;

  clang_visitChildren(
      cursor,
      [](CXCursor cursor, CXCursor, CXClientData clientData) {
        auto state = (MetadataFactory*)clientData;

        if (state->_checkAvailability && !isAvailable(cursor)) {
          return CXChildVisit_Continue;
        }

        CXCursorKind kind = clang_getCursorKind(cursor);

        switch (kind) {
          case CXCursor_VarDecl: {
            state->processVariable(cursor);
            break;
          }

          case CXCursor_EnumDecl: {
            state->processEnum(cursor);
            break;
          }

          case CXCursor_StructDecl: {
            state->processStruct(cursor);
            break;
          }

          case CXCursor_UnionDecl: {
            state->processUnion(cursor);
            break;
          }

          case CXCursor_FunctionDecl: {
            state->processFunction(cursor);
            break;
          }

          case CXCursor_ObjCInterfaceDecl: {
            state->processClass(cursor);
            break;
          }

          case CXCursor_ObjCProtocolDecl: {
            state->processProtocol(cursor);
            break;
          }

          case CXCursor_ObjCCategoryDecl: {
            state->processCategory(cursor);
            break;
          }

          default:
            break;
        }

        return CXChildVisit_Continue;
      },
      this);

  _checkAvailability = _previous_checkAvailability;
}

bool MetadataFactory::shouldProcess(CXCursor cursor, bool required) {
  if (required) {
    return true;
  }

  if (!isAvailable(cursor)) {
    return false;
  }

  CXSourceLocation srcloc = clang_getCursorLocation(cursor);
  CXFile file;
  clang_getFileLocation(srcloc, &file, nullptr, nullptr, nullptr);
  if (file == nullptr) {
    return false;
  }
  CXString fileName = clang_getFileName(file);
  const char* fileNameCStr = clang_getCString(fileName);
  std::string fileNameStr = fileNameCStr ? fileNameCStr : "";
  clang_disposeString(fileName);

  auto cached = shouldProcessCache.find(fileNameStr);
  bool shouldInclude = false;
  if (cached != shouldProcessCache.end()) {
    shouldInclude = cached->second;
  } else {
    for (const std::string& path : includePaths) {
      if (fileNameStr.find(path) != std::string::npos) {
        shouldInclude = true;
        break;
      }
    }
    shouldProcessCache.emplace(std::move(fileNameStr), shouldInclude);
  }

  if (!shouldInclude || !metadataFilter.active()) {
    return shouldInclude;
  }

  CXString cxSymbol = clang_getCursorSpelling(cursor);
  const char* symbolCString = clang_getCString(cxSymbol);
  std::string symbol = symbolCString ? symbolCString : "";
  clang_disposeString(cxSymbol);

  // Anonymous records and enums can contain multiple globally-visible child
  // symbols. Keep them conservatively; referenced named declarations are
  // also retained through the existing `required` dependency path.
  return symbol.empty() ||
         metadataFilter.isAllowed(getFrameworkName(cursor), symbol);
}

void MetadataFactory::implementClassProtocols(
    ClassDecl& decl, std::vector<std::string>& protocols) {
  for (std::string& name : protocols) {
    if (!decl.implementedProtocolNames.contains(name)) {
      decl.implementedProtocolNames.emplace(name);

      auto protocolIt = this->protocols.find(name);
      if (protocolIt != this->protocols.end()) {
        ProtocolDecl& protocol = protocolIt->second;
        decl.protocolRefs.emplace_back(&protocol);
        protocol.implementerRefs.emplace_back(&decl);
        for (MemberDecl& member : protocol.members) {
          decl.members.emplace_back(member);
        }
        implementClassProtocols(decl, protocol.protocolNames);
      }
    }
  }
}

void MetadataFactory::resolveRefs() {
  while (!referencedClasses.empty() || !referencedEnums.empty() ||
         !referencedProtocols.empty() || !referencedRecords.empty()) {
    processClassRefs();
    processProtocolRefs();
    processEnumRefs();
    processRecordRefs();
  }
}

void MetadataFactory::postProcess() {
  for (auto& kv : structs) {
    StructDecl& decl = kv.second;
    postProcessStruct(decl);
  }

  for (auto& kv : unions) {
    UnionDecl& decl = kv.second;
    postProcessUnion(decl);
  }

  for (FunctionDecl& decl : functions) {
    postProcessFunction(decl);
  }

  for (auto& kv : classes) {
    ClassDecl& decl = kv.second;
    postProcessClass(decl);
  }

  for (auto& kv : protocols) {
    ProtocolDecl& decl = kv.second;
    postProcessProtocol(decl);
  }

  // Processes the references to classes, protocols, enums and records
  // - resolving any indirect dependencies on other classes, etc.
  resolveRefs();

  // Push category members to their respective classes.

  for (CategoryDecl& category : categories) {
    if (classes.contains(category.className)) {
      ClassDecl& cls = classes[category.className];
      for (const std::string& protocolName : category.protocolNames) {
        if (!std::count(cls.protocolNames.begin(), cls.protocolNames.end(),
                        protocolName)) {
          cls.protocolNames.emplace_back(protocolName);
        }
      }
      category.processMembers(&cls.typeParameters);
      postProcessCategory(category);
      for (MemberDecl& member : category.members) {
        cls.members.emplace_back(member);
      }
    }
  }

  resolveRefs();

  // Rename protocols whose names conflict with classes.
  // If the immediate "...Protocol" name is already taken by another protocol,
  // keep appending a numeric suffix ("...Protocol2", "...Protocol3", ...).
  std::unordered_map<std::string, std::string> renamedProtocolMap;
  std::unordered_set<std::string> usedProtocolNames;
  for (const auto& kv : protocols) {
    usedProtocolNames.emplace(kv.first);
  }

  for (auto& kv : protocols) {
    ProtocolDecl& protocol = kv.second;
    const std::string originalName = protocol.name;
    if (!classes.contains(originalName)) {
      continue;
    }

    renamedProtocols.emplace(originalName);
    usedProtocolNames.erase(originalName);

    std::string newName = originalName + "Protocol";
    size_t suffix = 2;
    while (classes.contains(newName) || usedProtocolNames.contains(newName)) {
      newName = originalName + "Protocol" + std::to_string(suffix++);
    }

    protocol.name = newName;
    renamedProtocolMap.emplace(originalName, newName);
    usedProtocolNames.emplace(newName);
  }

  for (const auto& kv : renamedProtocolMap) {
    auto node = protocols.extract(kv.first);
    if (!node.empty()) {
      node.key() = kv.second;
      protocols.insert(std::move(node));
    }
  }

  for (auto& kv : protocols) {
    ProtocolDecl& protocol = kv.second;
    for (std::string& name : protocol.protocolNames) {
      auto renamedIt = renamedProtocolMap.find(name);
      if (renamedIt != renamedProtocolMap.end()) {
        name = renamedIt->second;
      }
    }
  }

  for (auto& kv : classes) {
    ClassDecl& cls = kv.second;
    for (std::string& name : cls.protocolNames) {
      auto renamedIt = renamedProtocolMap.find(name);
      if (renamedIt != renamedProtocolMap.end()) {
        name = renamedIt->second;
      }
    }
  }

  // Resolve references inside ClassDecls and ProtocolDecls.

  for (auto& kv : protocols) {
    ProtocolDecl& protocol = kv.second;
    for (std::string& name : protocol.protocolNames) {
      if (auto refIt = protocols.find(name); refIt != protocols.end()) {
        ProtocolDecl& ref = refIt->second;
        protocol.protocolRefs.emplace_back(&ref);
        ref.derivedProtocolRefs.emplace_back(&protocol);
      }
    }
  }

  for (auto& kv : classes) {
    ClassDecl& cls = kv.second;

    std::string resolvedSuperClassName = cls.superClassName;
    std::unordered_set<std::string> visitedSuperClasses;
    while (!resolvedSuperClassName.empty()) {
      if (visitedSuperClasses.contains(resolvedSuperClassName)) {
        resolvedSuperClassName.clear();
        break;
      }
      visitedSuperClasses.emplace(resolvedSuperClassName);

      auto superIt = classes.find(resolvedSuperClassName);
      if (superIt != classes.end()) {
        ClassDecl& ref = superIt->second;
        cls.superClassRef = &ref;
        cls.superClassName = resolvedSuperClassName;
        ref.derivedClassRefs.emplace_back(&cls);
        break;
      }

      auto skippedIt = skippedClasses.find(resolvedSuperClassName);
      if (skippedIt != skippedClasses.end() &&
          !skippedIt->second.superClassName.empty()) {
        resolvedSuperClassName = skippedIt->second.superClassName;
        continue;
      }

      resolvedSuperClassName.clear();
      break;
    }

    implementClassProtocols(cls, cls.protocolNames);
  }

  // Remove duplicate methods.

  for (auto& kv : classes) {
    ClassDecl& cls = kv.second;
    removeDuplicateMethods(cls.members);
    cls.postProcessMembers();
  }

  for (auto& kv : protocols) {
    ProtocolDecl& protocol = kv.second;
    removeDuplicateMethods(protocol.members);
    protocol.postProcessMembers();
  }

  // We need to do a second pass to propagate the changes further
  // in derived/implemented classes/protocols.
  // There should be a better way around this.

  for (auto& kv : classes) {
    ClassDecl& cls = kv.second;
    cls.postProcessMembers();
  }

  for (auto& kv : protocols) {
    ProtocolDecl& protocol = kv.second;
    protocol.postProcessMembers();
  }
}

void MetadataFactory::processVariable(CXCursor cursor) {
  if (!shouldProcess(cursor)) return;

  VariableDecl decl(cursor);
  auto [it, _] = variables.try_emplace(decl.name, std::move(decl));
  processType(it->second.type);
}

void MetadataFactory::processEnum(CXCursor cursor, bool required) {
  EnumDecl decl(cursor);

  if (decl.constants.empty()) {
    return;
  }

  if (!shouldProcess(cursor, required)) {
    auto it = skippedEnums.find(decl.name);
    if (it == skippedEnums.end() ||
        it->second.constants.size() < decl.constants.size()) {
      skippedEnums.insert_or_assign(decl.name, std::move(decl));
    }
    return;
  }

  // If the enum is unnamed, we'll just push the constants as global
  // constants.
  if (decl.name == "") {
    for (const auto& constant : decl.constants) {
      VariableDecl var(decl.framework, constant);
      variables.insert_or_assign(var.name, std::move(var));
    }

    return;
  }

  auto enumIt = enums.find(decl.name);
  if (enumIt != enums.end()) {
    if (enumIt->second.constants.size() < decl.constants.size()) {
      enumIt->second.constants = std::move(decl.constants);
    }
    return;
  }

  enums.try_emplace(decl.name, std::move(decl));
}

void MetadataFactory::processStruct(CXCursor cursor, bool required) {
  StructDecl decl(cursor);

  if (!shouldProcess(cursor, required)) {
    auto it = skippedStructs.find(decl.name);
    if (it == skippedStructs.end() ||
        it->second.fields.size() < decl.fields.size()) {
      skippedStructs.insert_or_assign(decl.name, std::move(decl));
    }
    return;
  }

  auto it = structs.find(decl.name);
  if (it != structs.end()) {
    if (it->second.fields.size() < decl.fields.size()) {
      it->second.fields = std::move(decl.fields);
    }
    process(cursor, true);
    return;
  }

  structs.try_emplace(decl.name, std::move(decl));
  process(cursor, true);
}

void MetadataFactory::postProcessStruct(StructDecl& decl) {
  for (StructFieldDecl& decl : decl.fields) {
    processType(decl.type);
  }
}

void MetadataFactory::processUnion(CXCursor cursor, bool required) {
  UnionDecl decl(cursor);

  if (decl.fields.empty()) {
    return;
  }

  if (!shouldProcess(cursor, required)) {
    auto it = skippedUnions.find(decl.name);
    if (it == skippedUnions.end() ||
        it->second.fields.size() < decl.fields.size()) {
      skippedUnions.insert_or_assign(decl.name, std::move(decl));
    }
    return;
  }

  auto it = unions.find(decl.name);
  if (it != unions.end()) {
    if (it->second.fields.size() < decl.fields.size()) {
      it->second.fields = std::move(decl.fields);
    }
    process(cursor, true);
    return;
  }

  unions.try_emplace(decl.name, std::move(decl));
  process(cursor, true);
}

void MetadataFactory::postProcessUnion(UnionDecl& decl) {
  for (UnionFieldDecl& decl : decl.fields) {
    processType(decl.type);
  }
}

void MetadataFactory::processFunction(CXCursor cursor) {
  // Skip if its inlined / defined in header, won't be available at
  // runtime.
  if (clang_Cursor_isFunctionInlined(cursor)) {
    return;
  }

  if (!shouldProcess(cursor)) return;

  FunctionDecl decl(cursor);
  functions.emplace_back(std::move(decl));
}

void MetadataFactory::postProcessFunction(FunctionDecl& decl) {
  for (ParameterDecl& parameter : decl.parameters) {
    processType(parameter.type);
  }

  processType(decl.returnType);
}

void MetadataFactory::processClass(CXCursor cursor, bool required) {
  ClassDecl decl(cursor);
  if (!isAvailable(cursor)) {
    decl.unavailable = true;
    skippedClasses.insert_or_assign(decl.name, std::move(decl));
    return;
  }

  if (!shouldProcess(cursor, required)) {
    skippedClasses.insert_or_assign(decl.name, std::move(decl));
    return;
  }

  classes.try_emplace(decl.name, std::move(decl));
}

void MetadataFactory::postProcessMember(MemberDecl& decl) {
  switch (decl.kind) {
    case kMemberMethod: {
      for (ParameterDecl& param : decl.parameters) {
        processType(param.type);
      }
      processType(decl.returnType);
      break;
    }

    case kMemberProperty: {
      processType(decl.propertyType);
      break;
    }
  }
}

void MetadataFactory::postProcessClass(ClassDecl& decl) {
  if (!decl.superClassName.empty()) {
    referencedClasses.emplace(decl.superClassName);
  }

  for (std::string& name : decl.protocolNames) {
    referencedProtocols.emplace(name);
  }

  for (MemberDecl& member : decl.members) {
    postProcessMember(member);
  }
}

void MetadataFactory::processProtocol(CXCursor cursor, bool required) {
  ProtocolDecl decl(cursor);

  if (!shouldProcess(cursor, required)) {
    skippedProtocols.insert_or_assign(decl.name, std::move(decl));
    return;
  }

  protocols.try_emplace(decl.name, std::move(decl));
}

void MetadataFactory::postProcessProtocol(ProtocolDecl& decl) {
  for (std::string& name : decl.protocolNames) {
    referencedProtocols.emplace(name);
  }

  for (MemberDecl& member : decl.members) {
    postProcessMember(member);
  }
}

void MetadataFactory::processCategory(CXCursor cursor) {
  if (!shouldProcess(cursor)) return;

  CategoryDecl decl(cursor);
  categories.emplace_back(std::move(decl));
}

void MetadataFactory::postProcessCategory(CategoryDecl& decl) {
  for (MemberDecl& member : decl.members) {
    postProcessMember(member);
  }
}

}  // namespace metagen
