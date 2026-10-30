#include <assert.h>

#include <unordered_set>

#include "IR.h"
#include "Util.h"
#include "clang-c/Index.h"

static const std::unordered_set<std::string> KNOWN_BRIDGED_TYPES = {
#define CF_TYPE(NAME) #NAME,
#define NON_CF_TYPE(NAME)
#include "CFDatabase.def"
#undef CF_TYPE
#undef NON_CF_TYPE
};

namespace metagen {

// The canonical function type of a block has lost its parameters' typedef
// names (CMSampleBufferRef becomes struct opaqueCMSampleBuffer *), and with
// them the CF bridging, so parameters are read from the sugared type.
static CXType sugaredBlockFunctionType(CXType type, CXType canonicalPointee) {
  while (type.kind != CXType_BlockPointer) {
    if (type.kind == CXType_Typedef) {
      type = clang_getTypedefDeclUnderlyingType(clang_getTypeDeclaration(type));
    } else if (type.kind == CXType_Attributed) {
      type = clang_Type_getModifiedType(type);
    } else if (type.kind == CXType_Elaborated) {
      type = clang_Type_getNamedType(type);
    } else {
      return canonicalPointee;
    }
  }

  CXType pointee = clang_getPointeeType(type);
  if (clang_getNumArgTypes(pointee) != clang_getNumArgTypes(canonicalPointee)) {
    return canonicalPointee;
  }
  return pointee;
}

TypeSpec::TypeSpec(CXType type, std::vector<std::string>* classTypeParameters) {
  auto canonicalType = clang_getCanonicalType(type);

  CXString nameStr = clang_getTypeSpelling(type);
  std::string name = clang_getCString(nameStr);
  clang_disposeString(nameStr);

  if (name.find(" _Nullable *") != std::string::npos) {
    std::string strippedName = name.substr(0, name.find(" _Nullable *"));
    strippedName = rtrim(strippedName);
    if (KNOWN_BRIDGED_TYPES.contains(strippedName)) {
      kind = kTypePointer;
      auto pointeeType = clang_getPointeeType(type);
      pointee = std::make_shared<TypeSpec>(pointeeType, classTypeParameters);
      return;
    }
  }

  if (KNOWN_BRIDGED_TYPES.contains(name)) {
    kind = kTypeAnyObject;
    return;
  }

  CXString canonicalNameStr = clang_getTypeSpelling(canonicalType);
  std::string canonicalName = stripCInfo(clang_getCString(canonicalNameStr));
  clang_disposeString(canonicalNameStr);

  if (classTypeParameters != nullptr &&
      std::find(classTypeParameters->begin(), classTypeParameters->end(),
                name) != classTypeParameters->end()) {
    typeParameterName = name;
    kind = kTypeAnyObject;
    return;
  }

  switch (canonicalType.kind) {
    case CXType_Void:
      kind = kTypeVoid;
      break;

    case CXType_Bool:
      kind = kTypeBool;
      break;

    case CXType_UChar: {
      kind = kTypeU8;
      break;
    }
    case CXType_UShort: {
      const std::string strippedName = stripCInfo(name);
      if (strippedName == "unichar" || strippedName == "UniChar") {
        kind = kTypeUnichar;
      } else {
        kind = kTypeU16;
      }
      break;
    }
    case CXType_UInt:
      kind = kTypeU32;
      break;
    case CXType_Char_S:
      kind = kTypeI8;
      break;
    case CXType_SChar:
      kind = kTypeI8;
      break;
    case CXType_WChar:
      kind = kTypeI16;
      break;
    case CXType_Short:
      kind = kTypeI16;
      break;
    case CXType_Int:
      kind = kTypeI32;
      break;
    case CXType_Float:
      kind = kTypeF32;
      break;
    case CXType_Float16:
      kind = kTypeF16;
      break;
    case CXType_Double:
      kind = kTypeF64;
      break;
    case CXType_LongDouble:
      kind = kTypeF64;
      break;
    case CXType_ULong:
      kind = kTypeU64;
      break;
    case CXType_ULongLong:
      kind = kTypeU64;
      break;
    case CXType_UInt128:
      kind = kTypeU128;
      break;
    case CXType_Long:
      kind = kTypeI64;
      break;
    case CXType_LongLong:
      kind = kTypeI64;
      break;
    case CXType_Int128:
      kind = kTypeI128;
      break;

    case CXType_Pointer: {
      unknownInfo = name;

      auto pointeeType = clang_getPointeeType(canonicalType);

      if (pointeeType.kind == CXType_Char_S) {
        kind = kTypeString;
      } else if (pointeeType.kind == CXType_ObjCSel) {
        kind = kTypeSelector;
      } else if (pointeeType.kind == CXType_FunctionProto) {
        auto resultType = clang_getResultType(pointeeType);
        callbackReturn =
            std::make_shared<TypeSpec>(resultType, classTypeParameters);
        auto argc = clang_getNumArgTypes(pointeeType);
        if (argc > 0) {
          callbackArgs.reserve(static_cast<size_t>(argc));
        }
        for (int i = 0; i < argc; i++) {
          auto argType = clang_getArgType(pointeeType, i);
          callbackArgs.emplace_back(argType, classTypeParameters);
        }
        kind = kTypeFunctionPointer;
      } else if (pointeeType.kind == CXType_FunctionNoProto) {
        auto resultType = clang_getResultType(pointeeType);
        callbackReturn =
            std::make_shared<TypeSpec>(resultType, classTypeParameters);
        kind = kTypeFunctionPointer;
      } else {
        kind = kTypePointer;
        pointee = std::make_shared<TypeSpec>(pointeeType, classTypeParameters);
      }
      break;
    }

    case CXType_FunctionProto: {
      auto resultType = clang_getResultType(canonicalType);
      callbackReturn =
          std::make_shared<TypeSpec>(resultType, classTypeParameters);
      auto argc = clang_getNumArgTypes(canonicalType);
      if (argc > 0) {
        callbackArgs.reserve(static_cast<size_t>(argc));
      }
      for (int i = 0; i < argc; i++) {
        auto argType = clang_getArgType(canonicalType, i);
        callbackArgs.emplace_back(argType, classTypeParameters);
      }
      kind = kTypeFunctionPointer;
      break;
    }

    case CXType_ObjCSel:
      kind = kTypeSelector;
      break;

    case CXType_BlockPointer: {
      auto pointeeType =
          sugaredBlockFunctionType(type, clang_getPointeeType(canonicalType));
      auto resultType = clang_getResultType(pointeeType);
      callbackReturn =
          std::make_shared<TypeSpec>(resultType, classTypeParameters);
      auto argc = clang_getNumArgTypes(pointeeType);
      if (argc > 0) {
        callbackArgs.reserve(static_cast<size_t>(argc));
      }
      for (int i = 0; i < argc; i++) {
        auto argType = clang_getArgType(pointeeType, i);
        callbackArgs.emplace_back(argType, classTypeParameters);
      }
      kind = kTypeCallback;
      break;
    }

    case CXType_ObjCObjectPointer: {
      CXType pointeeType = clang_getPointeeType(canonicalType);
      int numTypeParams = clang_Type_getNumTemplateArguments(pointeeType);
      for (int i = 0; i < numTypeParams; i++) {
        CXType typeArg = clang_Type_getTemplateArgumentAsType(pointeeType, i);
        typeParameters.emplace_back(typeArg, classTypeParameters);
      }

      if (canonicalName == "id") {
        if (name == "instancetype") {
          kind = kTypeInstanceObject;
        } else {
          kind = kTypeAnyObject;
        }
      } else if (canonicalName == "Class") {
        kind = kTypePointer;
        pointee = std::make_shared<TypeSpec>();
        pointee->kind = kTypeVoid;
      } else if (canonicalName == "Protocol *" ||
                 canonicalName == "Protocol*" || canonicalName == "Protocol") {
        kind = kTypePointer;
        pointee = std::make_shared<TypeSpec>();
        pointee->kind = kTypeVoid;
      } else if (canonicalName.find("id<") == 0 ||
                 canonicalName.find("Class<") == 0) {
        // Just ID but this time we have protocols specified, which we can add
        // onto the object prototype.
        auto commaOffset = canonicalName.find(",");
        auto endOffset = canonicalName.find(">");
        auto skip = canonicalName.find("id<") == 0 ? 3 : 6;
        if (commaOffset < endOffset) {
          endOffset = commaOffset;
        }
        protocolName = canonicalName.substr(skip, endOffset - skip);
        kind = kTypeObject;
      } else if (canonicalName.find("<") < canonicalName.size()) {
        // We have protocols specified, but we also have a class name.
        auto cls = canonicalName.substr(0, canonicalName.find("<"));
        kind = kTypeObject;
        className = rtrim(cls);
      } else {
        // We have a class name.
        auto cls = canonicalName.substr(0, canonicalName.find("*"));
        kind = kTypeObject;
        className = rtrim(cls);
      }
      break;
    }

    case CXType_Enum: {
      CXString name = clang_getTypeSpelling(canonicalType);
      enumName = stripCInfo(clang_getCString(name));
      clang_disposeString(name);
      kind = kTypeEnum;
      break;
    }

    case CXType_Record: {
      CXString name = clang_getTypeSpelling(canonicalType);
      recordName = transformStructName(stripCInfo(clang_getCString(name)));
      clang_disposeString(name);
      kind = kTypeRecord;
      break;
    }

    case CXType_ConstantArray: {
      auto element = clang_getArrayElementType(canonicalType);
      arrayElement = std::make_shared<TypeSpec>(element, classTypeParameters);
      constArraySize = clang_getArraySize(canonicalType);
      kind = kTypeConstArray;
      break;
    }

    case CXType_IncompleteArray: {
      auto element = clang_getArrayElementType(canonicalType);
      arrayElement = std::make_shared<TypeSpec>(element, classTypeParameters);
      kind = kTypeIncompleteArray;
      break;
    }

    case CXType_Vector: {
      kind = kTypeVector;
      auto element = clang_getElementType(canonicalType);
      arrayElement = std::make_shared<TypeSpec>(element, classTypeParameters);
      long long elementsCount = clang_getNumElements(canonicalType);
      constArraySize =
          elementsCount > 0 ? static_cast<size_t>(elementsCount) : 0;
      break;
    }

    case CXType_ExtVector: {
      kind = kTypeExtVector;
      auto element = clang_getElementType(canonicalType);
      arrayElement = std::make_shared<TypeSpec>(element, classTypeParameters);
      long long elementsCount = clang_getNumElements(canonicalType);
      constArraySize =
          elementsCount > 0 ? static_cast<size_t>(elementsCount) : 0;
      break;
    }

    case CXType_Complex: {
      kind = kTypeComplex;
      auto element = clang_getElementType(canonicalType);
      arrayElement = std::make_shared<TypeSpec>(element, classTypeParameters);
      constArraySize = 2;
      break;
    }

    default:
      kind = kTypeUnknown;
      CXString typeName = clang_getTypeSpelling(canonicalType);
      unknownInfo = clang_getCString(typeName);
      clang_disposeString(typeName);
      unknownInfo +=
          " (CXTypeKind: " + std::to_string(canonicalType.kind) + ")";
      break;
  }
}

void MetadataFactory::processType(TypeSpec& type) {
  switch (type.kind) {
    case kTypeObject: {
      if (!type.className.empty()) {
        if (!classes.contains(type.className)) {
          referencedClasses.emplace(type.className);
        }
      } else if (!type.protocolName.empty() &&
                 !protocols.contains(type.protocolName)) {
        referencedProtocols.emplace(type.protocolName);
      }
      for (TypeSpec& p : type.typeParameters) {
        processType(p);
      }
      break;
    }

    case kTypeConstArray:
    case kTypeIncompleteArray:
      processType(*type.arrayElement);
      break;

    case kTypeVector:
    case kTypeExtVector:
    case kTypeComplex:
      if (type.arrayElement != nullptr) {
        processType(*type.arrayElement);
      }
      break;

    case kTypePointer:
      processType(*type.pointee);
      break;

    case kTypeCallback:
    case kTypeFunctionPointer:
      for (auto& arg : type.callbackArgs) {
        processType(arg);
      }
      processType(*type.callbackReturn);
      break;

    case kTypeEnum:
      if (!enums.contains(type.enumName))
        referencedEnums.emplace(type.enumName);
      break;

    case kTypeRecord:
      if (!structs.contains(type.recordName) &&
          !unions.contains(type.recordName))
        referencedRecords.emplace(type.recordName);
      break;

    default:
      break;
  }
}

}  // namespace metagen
