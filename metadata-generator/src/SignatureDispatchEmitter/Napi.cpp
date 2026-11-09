#include "SignatureDispatchEmitter/Shared.h"

#include <sstream>
#include <vector>

namespace metagen::signature_dispatch {

std::string makeNapiWrapperName(DispatchKind kind, size_t index) {
  std::ostringstream stream;
  stream << "dn";
  switch (kind) {
    case DispatchKind::ObjCMethod:
      stream << "o";
      break;
    case DispatchKind::CFunction:
      stream << "c";
      break;
    case DispatchKind::BlockInvoke:
      stream << "b";
      break;
  }
  stream << toBase36(index);
  return stream.str();
}
void writeFastNapiArgConversion(std::ostringstream& out, const MDTypeInfo* type,
                                size_t index, bool hasCleanupArgs) {
  const char* failCleanup = hasCleanupArgs ? "  cleanupManagedArgs();\n" : "";
  if (type == nullptr) {
    out << failCleanup;
    out << "  return false;\n";
    return;
  }

  switch (type->kind) {
    case mdTypeChar: {
      out << "  int32_t tmpArg" << index << " = 0;\n";
      out << "  if (napi_get_value_int32(env, argv[" << index << "], &tmpArg"
          << index << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
      out << "  arg" << index << " = static_cast<int8_t>(tmpArg" << index
          << ");\n";
      break;
    }
    case mdTypeUChar:
    case mdTypeUInt8: {
      out << "  uint32_t tmpArg" << index << " = 0;\n";
      out << "  if (napi_get_value_uint32(env, argv[" << index << "], &tmpArg"
          << index << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
      out << "  arg" << index << " = static_cast<uint8_t>(tmpArg" << index
          << ");\n";
      break;
    }
    case mdTypeSShort: {
      out << "  int32_t tmpArg" << index << " = 0;\n";
      out << "  if (napi_get_value_int32(env, argv[" << index << "], &tmpArg"
          << index << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
      out << "  arg" << index << " = static_cast<int16_t>(tmpArg" << index
          << ");\n";
      break;
    }
    case mdTypeUShort: {
      out << "  if (!TryFastConvertNapiUInt16Argument(env, argv[" << index
          << "], &arg" << index << ")) {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
      break;
    }
    case mdTypeSInt: {
      out << "  if (napi_get_value_int32(env, argv[" << index << "], &arg"
          << index << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
      break;
    }
    case mdTypeUInt: {
      out << "  if (napi_get_value_uint32(env, argv[" << index << "], &arg"
          << index << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
      break;
    }
    case mdTypeSLong:
    case mdTypeSInt64: {
      out << "  if (napi_get_value_int64(env, argv[" << index << "], &arg"
          << index << ") != napi_ok) {\n";
      out << "      bool lossless" << index << " = false;\n";
      out << "      if (napi_get_value_bigint_int64(env, argv[" << index
          << "], &arg" << index << ", &lossless" << index
          << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "        cleanupManagedArgs();\n";
      }
      out << "        return false;\n";
      out << "      }\n";
      out << "  }\n";
      break;
    }
    case mdTypeULong:
    case mdTypeUInt64: {
      out << "  bool lossless" << index << " = false;\n";
      out << "  if (napi_get_value_bigint_uint64(env, argv[" << index
          << "], &arg" << index << ", &lossless" << index
          << ") != napi_ok) {\n";
      out << "      int64_t signedValue" << index << " = 0;\n";
      out << "      if (napi_get_value_int64(env, argv[" << index
          << "], &signedValue" << index << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "        cleanupManagedArgs();\n";
      }
      out << "        return false;\n";
      out << "      }\n";
      out << "      arg" << index << " = static_cast<uint64_t>(signedValue"
          << index << ");\n";
      out << "  }\n";
      break;
    }
    case mdTypeFloat: {
      out << "  double tmpArg" << index << " = 0.0;\n";
      out << "  if (napi_get_value_double(env, argv[" << index << "], &tmpArg"
          << index << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
      out << "  arg" << index << " = static_cast<float>(tmpArg" << index
          << ");\n";
      break;
    }
    case mdTypeDouble: {
      out << "  if (napi_get_value_double(env, argv[" << index << "], &arg"
          << index << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
      out << "  if (std::isnan(arg" << index << ") || std::isinf(arg" << index
          << ")) {\n";
      out << "    arg" << index << " = 0.0;\n";
      out << "  }\n";
      break;
    }
    case mdTypeBool: {
      out << "  bool boolValue" << index << " = false;\n";
      out << "  if (napi_get_value_bool(env, argv[" << index << "], &boolValue"
          << index << ") != napi_ok) {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
      out << "  arg" << index << " = static_cast<uint8_t>(boolValue" << index
          << " ? 1 : 0);\n";
      break;
    }
    default:
      out << failCleanup;
      out << "  return false;\n";
      break;
  }
}

void writeNapiWrapper(std::ostringstream& out, DispatchKind kind,
                      const std::string& wrapperName,
                      const MDSignature* signature) {
  std::string returnType;
  if (!mapTypeToCpp(signature->returnType, &returnType, true)) {
    return;
  }

  std::vector<const MDTypeInfo*> argTypeInfos;
  std::vector<std::string> argTypes;
  argTypes.reserve(signature->arguments.size());
  argTypeInfos.reserve(signature->arguments.size());
  for (const auto* arg : signature->arguments) {
    std::string argType;
    if (!mapTypeToCpp(arg, &argType, false)) {
      return;
    }
    argTypeInfos.push_back(arg);
    argTypes.push_back(argType);
  }

  out << "static inline bool " << wrapperName
      << "(napi_env env, Cif* cif, void* fnptr, ";
  if (kind == DispatchKind::ObjCMethod) {
    out << "id self, SEL selector, ";
  }
  out << "const napi_value* argv, void* rvalue) {\n";

  out << "  using Fn = " << returnType << " (*)(";
  bool first = true;
  if (kind == DispatchKind::ObjCMethod) {
    out << "id, SEL";
    first = false;
  }
  for (const auto& argType : argTypes) {
    if (!first) {
      out << ", ";
    }
    out << argType;
    first = false;
  }
  out << ");\n";
  out << "  auto fn = reinterpret_cast<Fn>(fnptr);\n";
  std::vector<size_t> cleanupArgIndexes;
  std::vector<size_t> noCleanupManagedArgIndexes;
  cleanupArgIndexes.reserve(argTypes.size());
  noCleanupManagedArgIndexes.reserve(argTypes.size());
  for (size_t i = 0; i < argTypes.size(); i++) {
    if (!isFastPrimitiveDispatchKind(argTypeInfos[i]->kind)) {
      if (argKindMayNeedCleanup(argTypeInfos[i]->kind)) {
        cleanupArgIndexes.push_back(i);
      } else {
        noCleanupManagedArgIndexes.push_back(i);
      }
    }
  }
  const bool hasCleanupArgs = !cleanupArgIndexes.empty();
  if (hasCleanupArgs) {
    out << "  bool shouldFreeAny = false;\n";
  }
  if (!noCleanupManagedArgIndexes.empty()) {
    out << "  bool ignoredShouldFree = false;\n";
    out << "  bool ignoredShouldFreeAny = false;\n";
  }
  if (returnType != "void") {
    out << "  " << returnType << " nativeResult{};\n";
  }

  for (size_t i = 0; i < argTypes.size(); i++) {
    out << "  " << argTypes[i] << " arg" << i << "{};\n";
    if (!isFastPrimitiveDispatchKind(argTypeInfos[i]->kind) &&
        argKindMayNeedCleanup(argTypeInfos[i]->kind)) {
      out << "  bool shouldFree" << i << " = false;\n";
    }
  }

  if (hasCleanupArgs) {
    out << "  auto cleanupManagedArgs = [&]() {\n";
    out << "    if (shouldFreeAny) {\n";
    if (kind == DispatchKind::CFunction && returnType != "void") {
      out << "      void* returnPointerValue = nullptr;\n";
      out << "      if (cif->returnType != nullptr && cif->returnType->type == "
             "&ffi_type_pointer) {\n";
      out << "        returnPointerValue = "
             "*reinterpret_cast<void**>(&nativeResult);\n";
      out << "      }\n";
    }
    for (const auto i : cleanupArgIndexes) {
      out << "      if (shouldFree" << i << ") {\n";
      if (kind == DispatchKind::CFunction && returnType != "void") {
        out << "        if (returnPointerValue != nullptr && "
               "*reinterpret_cast<void**>(&arg"
            << i << ") == returnPointerValue) {\n";
        out << "          // Returning an argument pointer keeps ownership "
               "with "
               "the return value.\n";
        out << "        } else {\n";
        out << "          cif->argTypes[" << i
            << "]->free(env, *reinterpret_cast<void**>(&arg" << i << "));\n";
        out << "        }\n";
      } else {
        out << "        cif->argTypes[" << i
            << "]->free(env, *reinterpret_cast<void**>(&arg" << i << "));\n";
      }
      out << "      }\n";
    }
    out << "    }\n";
    out << "  };\n";
  }

  for (size_t i = 0; i < argTypes.size(); i++) {
    if (isFastPrimitiveDispatchKind(argTypeInfos[i]->kind)) {
      writeFastNapiArgConversion(out, argTypeInfos[i], i, hasCleanupArgs);
    } else if (isFastReferenceDispatchKind(argTypeInfos[i]->kind)) {
      out << "  if (!TryFastConvertNapiArgument(env, static_cast<MDTypeKind>("
          << static_cast<int>(argTypeInfos[i]->kind) << "), argv[" << i
          << "], &arg" << i << ")) {\n";
      if (argKindMayNeedCleanup(argTypeInfos[i]->kind)) {
        out << "    cif->argTypes[" << i << "]->toNative(env, argv[" << i
            << "], &arg" << i << ", &shouldFree" << i << ", &shouldFreeAny);\n";
      } else {
        out << "    ignoredShouldFree = false;\n";
        out << "    ignoredShouldFreeAny = false;\n";
        out << "    cif->argTypes[" << i << "]->toNative(env, argv[" << i
            << "], &arg" << i
            << ", &ignoredShouldFree, &ignoredShouldFreeAny);\n";
      }
      out << "    bool conversionFailed" << i << " = false;\n";
      out << "    if (ConsumeNapiArgumentConversionFailure(env) || "
             "napi_is_exception_pending(env, &conversionFailed"
          << i << ") != napi_ok || conversionFailed" << i << ") {\n";
      if (hasCleanupArgs) {
        out << "      cleanupManagedArgs();\n";
      }
      out << "      return false;\n";
      out << "    }\n";
      out << "  }\n";
    } else {
      if (argKindMayNeedCleanup(argTypeInfos[i]->kind)) {
        out << "  cif->argTypes[" << i << "]->toNative(env, argv[" << i
            << "], &arg" << i << ", &shouldFree" << i << ", &shouldFreeAny);\n";
      } else {
        out << "  ignoredShouldFree = false;\n";
        out << "  ignoredShouldFreeAny = false;\n";
        out << "  cif->argTypes[" << i << "]->toNative(env, argv[" << i
            << "], &arg" << i
            << ", &ignoredShouldFree, &ignoredShouldFreeAny);\n";
      }
      out << "  bool conversionFailed" << i << " = false;\n";
      out << "  if (ConsumeNapiArgumentConversionFailure(env) || "
             "napi_is_exception_pending(env, &conversionFailed"
          << i << ") != napi_ok || conversionFailed" << i << ") {\n";
      if (hasCleanupArgs) {
        out << "    cleanupManagedArgs();\n";
      }
      out << "    return false;\n";
      out << "  }\n";
    }
  }

  std::ostringstream callExpr;
  callExpr << "fn(";
  bool hasAnyCallArg = false;
  if (kind == DispatchKind::ObjCMethod) {
    callExpr << "self, selector";
    hasAnyCallArg = true;
  }
  for (size_t i = 0; i < argTypes.size(); i++) {
    if (hasAnyCallArg) {
      callExpr << ", ";
    }
    callExpr << "arg" << i;
    hasAnyCallArg = true;
  }
  callExpr << ")";

  if (returnType == "void") {
    out << "  " << callExpr.str() << ";\n";
  } else {
    out << "  nativeResult = " << callExpr.str() << ";\n";
    out << "  *reinterpret_cast<" << returnType
        << "*>(rvalue) = nativeResult;\n";
  }
  if (hasCleanupArgs) {
    out << "  cleanupManagedArgs();\n";
  }

  out << "  return true;\n";
  out << "}\n\n";
}

}  // namespace metagen::signature_dispatch
