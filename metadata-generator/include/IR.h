#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Metadata.h"
#include "MetadataFilter.h"
#include "Util.h"
#include "clang-c/Index.h"

namespace metagen {

enum TypeSpecKind {
  kTypeUnknown,
  kTypeVoid,
  kTypeU8,
  kTypeU16,
  kTypeU32,
  kTypeU64,
  kTypeI8,
  kTypeI16,
  kTypeI32,
  kTypeI64,
  kTypeF32,
  kTypeF64,
  kTypeU128,
  kTypeI128,
  kTypeBool,
  kTypeString,
  kTypeSelector,
  kTypePointer,
  kTypeCallback,
  kTypeObject,
  kTypeInstanceObject,
  kTypeAnyObject,
  kTypeEnum,
  kTypeRecord,
  kTypeConstArray,
  kTypeIncompleteArray,
  kTypeVector,
  kTypeExtVector,
  kTypeComplex,
  kTypeFunctionPointer,
  kTypeF16,
  // unichar/UniChar: same width as kTypeU16 but projected to JS as a
  // single-character string rather than a number.
  kTypeUnichar,
};

class TypeSpec {
 public:
  TypeSpec() {}

  TypeSpec(CXType type,
           std::vector<std::string>* classTypeParameters = nullptr);

  TypeSpecKind kind;

  // kPointerType
  std::shared_ptr<TypeSpec> pointee;

  // kTypeCallback, kTypeFunctionPointer
  std::vector<TypeSpec> callbackArgs;
  std::shared_ptr<TypeSpec> callbackReturn;
  bool isVariadic = false;

  // kTypeObject, kTypeInstanceObject, kTypeAnyObject
  std::string className;
  std::string protocolName;
  bool isNullable = false;
  std::string typeParameterName;
  std::vector<TypeSpec> typeParameters;

  // kTypeEnum
  std::string enumName;

  // kTypeRecord
  std::string recordName;  // struct or union

  // kTypeConstArray, kTypeIncompleteArray
  std::shared_ptr<TypeSpec> arrayElement;
  // kTypeConstArray
  size_t constArraySize;

  // kTypeUnknown
  std::string unknownInfo;
};

enum VariableConstEvalKind {
  kEvalNone,
  kEvalI64,
  kEvalF64,
  kEvalString,
};

struct EnumConstDecl {
  std::string name;
  int64_t value;
  AvailabilityInfo availability;
};

class VariableDecl {
 public:
  VariableDecl() {}

  VariableDecl(CXCursor cursor);
  VariableDecl(std::string& framework, const EnumConstDecl& decl);

  std::string framework;
  AvailabilityInfo availability;

  std::string name;
  TypeSpec type;

  VariableConstEvalKind constEvalKind = kEvalNone;
  int64_t constEvalI64;
  double constEvalF64;
  std::string constEvalString;
};

class EnumDecl {
 public:
  EnumDecl() {}

  EnumDecl(CXCursor cursor);

  std::string framework;
  AvailabilityInfo availability;

  std::string name;
  std::vector<EnumConstDecl> constants;

  MDSectionOffset mdOffset = MD_SECTION_OFFSET_NULL;
};

struct StructFieldDecl {
  std::string name;
  uint16_t offset;
  TypeSpec type;
};

class StructDecl {
 public:
  StructDecl() {}

  StructDecl(CXCursor cursor);

  std::string framework;
  AvailabilityInfo availability;

  std::string name;
  uint16_t size;
  std::vector<StructFieldDecl> fields;

  MDSectionOffset mdOffset = MD_SECTION_OFFSET_NULL;
};

struct UnionFieldDecl {
  std::string name;
  TypeSpec type;
};

class UnionDecl {
 public:
  UnionDecl() {}

  UnionDecl(CXCursor cursor);

  std::string framework;
  AvailabilityInfo availability;

  std::string name;
  uint16_t size;
  std::vector<UnionFieldDecl> fields;

  MDSectionOffset mdOffset = MD_SECTION_OFFSET_NULL;
};

struct ParameterDecl {
  std::string name;
  TypeSpec type;
};

class FunctionDecl {
 public:
  FunctionDecl() {}

  FunctionDecl(CXCursor cursor);

  std::string framework;
  AvailabilityInfo availability;

  std::string name;
  TypeSpec returnType;
  std::vector<ParameterDecl> parameters;
  bool isVariadic;
  bool returnOwned = false;
};

enum MemberKind {
  kMemberProperty,
  kMemberMethod,
};

class MemberDecl {
 public:
  MemberDecl() {}

  MemberDecl(CXCursor cursor,
             std::vector<std::string>* classTypeParameters = nullptr);

  std::string toString();

  // Common
  MemberKind kind;
  std::string name;

  AvailabilityInfo availability;

  std::string parentClassName;
  std::string parentProtocolName;

  bool isStatic;

  // Method
  TypeSpec returnType;
  std::vector<ParameterDecl> parameters;
  std::string methodSelector;
  bool isVariadic;
  bool isInit = false;
  bool returnOwned = false;

  // Property
  TypeSpec propertyType;
  bool isReadonly;
  std::string setterName;
  std::string getterSelector;
  std::string setterSelector;

  // For protocols
  bool optional;

  bool tsIgnore = false;

  // Method overloads for TS emission (used to preserve superclass signatures).
  std::vector<MemberDecl> overloads;
  std::vector<std::string> overloadSignatureKeys;

  void addOverloadFrom(const MemberDecl& member);
};

void removeDuplicateMethods(std::vector<MemberDecl>& members);

class ProtocolDecl;

class ClassDecl {
 public:
  ClassDecl() {}

  ClassDecl(CXCursor cursor);

  MemberDecl* getMemberNamed(const std::string& name);

  void postProcessMembers();

  std::string framework;
  AvailabilityInfo availability;

  std::string name;
  std::string runtimeName;
  std::string superClassName;
  std::vector<std::string> protocolNames;

  std::vector<std::string> typeParameters;

  std::vector<MemberDecl> members;

  ClassDecl* superClassRef = nullptr;
  std::vector<ClassDecl*> derivedClassRefs;
  std::vector<ProtocolDecl*> protocolRefs;
  std::unordered_set<std::string> implementedProtocolNames;

  bool tsIgnore = false;
  bool unavailable = false;

  MDSectionOffset mdOffset = MD_SECTION_OFFSET_NULL;
};

class ProtocolDecl {
 public:
  ProtocolDecl() {}

  ProtocolDecl(CXCursor cursor);

  MemberDecl* getMemberNamed(const std::string& name);

  void postProcessMembers();

  std::string framework;
  AvailabilityInfo availability;

  std::string name;
  std::vector<std::string> protocolNames;

  std::vector<MemberDecl> members;

  std::vector<ProtocolDecl*> protocolRefs;
  std::vector<ProtocolDecl*> derivedProtocolRefs;
  std::vector<ClassDecl*> implementerRefs;

  bool tsIgnore = false;

  MDSectionOffset mdOffset = MD_SECTION_OFFSET_NULL;
};

void convertMethodToPropertyIfNeeded(MemberDecl& member,
                                     MemberDecl* memberInSuperclass,
                                     bool isSuperclass);

class CategoryDecl {
 public:
  CategoryDecl() {}

  CategoryDecl(CXCursor cursor);

  void processMembers(std::vector<std::string>* classTypeParameters = nullptr);

  CXCursor cursor;

  std::string framework;

  std::string name;
  std::string className;
  std::vector<std::string> protocolNames;

  std::vector<MemberDecl> members;

  std::vector<std::string>* _classTypeParameters;
};

class MetadataFactory {
 public:
  MetadataFactory() {}

  void process(CXCursor cursor, bool checkAvailability = false);
  void resolveRefs();
  void postProcess();

  void implementClassProtocols(ClassDecl& decl,
                               std::vector<std::string>& protocols);

  bool shouldProcess(CXCursor cursor, bool required = false);

  void processVariable(CXCursor cursor);
  void processEnum(CXCursor cursor, bool required = false);
  void processStruct(CXCursor cursor, bool required = false);
  void processUnion(CXCursor cursor, bool required = false);
  void processFunction(CXCursor cursor);
  void processClass(CXCursor cursor, bool required = false);
  void processProtocol(CXCursor cursor, bool required = false);
  void processCategory(CXCursor cursor);
  void processType(TypeSpec& type);

  void postProcessStruct(StructDecl& decl);
  void postProcessUnion(UnionDecl& decl);
  void postProcessFunction(FunctionDecl& decl);
  void postProcessMember(MemberDecl& decl);
  void postProcessClass(ClassDecl& decl);
  void postProcessProtocol(ProtocolDecl& decl);
  void postProcessCategory(CategoryDecl& decl);

  void processEnumRefs();
  void processRecordRefs();
  void processClassRefs();
  void processProtocolRefs();

  std::unordered_set<std::string> includePaths;
  MetadataFilter metadataFilter;

  std::unordered_map<std::string, VariableDecl> variables;
  std::unordered_map<std::string, EnumDecl> enums;
  std::unordered_map<std::string, StructDecl> structs;
  std::unordered_map<std::string, UnionDecl> unions;
  std::vector<FunctionDecl> functions;
  std::unordered_map<std::string, ClassDecl> classes;
  std::unordered_map<std::string, ProtocolDecl> protocols;
  std::vector<CategoryDecl> categories;

  std::unordered_set<std::string> referencedEnums;
  std::unordered_set<std::string> referencedRecords;
  std::unordered_set<std::string> referencedClasses;
  std::unordered_set<std::string> referencedProtocols;

  std::unordered_set<std::string> renamedProtocols;
 std::unordered_set<std::string> missingClasses;

 private:
  bool _checkAvailability = false;
  std::unordered_map<std::string, bool> shouldProcessCache;

  std::unordered_map<std::string, EnumDecl> skippedEnums;
  std::unordered_map<std::string, StructDecl> skippedStructs;
  std::unordered_map<std::string, UnionDecl> skippedUnions;
  std::unordered_map<std::string, ClassDecl> skippedClasses;
  std::unordered_map<std::string, ProtocolDecl> skippedProtocols;
};

}  // namespace metagen
