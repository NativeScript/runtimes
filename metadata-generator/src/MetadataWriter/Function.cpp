#include "MetadataWriter.h"

namespace metagen {

void MDMetadataWriter::write(FunctionDecl &decl) {
  MDFunction *mdFunction = new MDFunction();

  mdFunction->name = strings.add(decl.name, decl.name);
  mdFunction->flags = mdFunctionFlagNone;
  if (decl.returnOwned) {
    mdFunction->flags = (MDFunctionFlag)(mdFunction->flags | mdFunctionReturnOwned);
  }

  MDSignature *mdSignature = new MDSignature();
  mdSignature->returnType = getTypeInfo(decl.returnType);
  mdSignature->arguments.reserve(decl.parameters.size());
  for (const auto& param : decl.parameters) {
    mdSignature->arguments.push_back(getTypeInfo(param.type));
  }
  mdSignature->isVariadic = decl.isVariadic;

  MDSignatureResolvable res{mdSignature, &mdFunction->signature};
  signatureResolvables.emplace_back(res);

  functions.add(mdFunction, decl.name);
}

size_t MDSignatureSerde::size(MDSignature *value) {
  size_t size = 0;
  // Return type
  auto typeInfoSerde = MDTypeInfoSerde();
  auto returnTypeSize = typeInfoSerde.size(value->returnType);
  size += returnTypeSize;
  // Arguments
  for (MDTypeInfo *arg : value->arguments) {
    auto argTypeSize = typeInfoSerde.size(arg);
    size += argTypeSize;
  }
  return size;
}

void MDSignatureSerde::serialize(MDSignature *value, void *data) {
  // Return type
  auto typeInfoSerde = MDTypeInfoSerde();
  auto returnTypeSize = typeInfoSerde.size(value->returnType);
  typeInfoSerde.serialize(value->returnType, data);
  MDTypeKind *data_kind = (MDTypeKind *)data;
  uint8_t returnKind = static_cast<uint8_t>(value->returnType->kind);
  if (value->arguments.size() > 0) {
    returnKind |= static_cast<uint8_t>(mdTypeFlagNext);
  }
  if (value->isVariadic) {
    returnKind |= static_cast<uint8_t>(mdTypeFlagVariadic);
  }
  data_kind[0] = static_cast<MDTypeKind>(returnKind);
  ptr_add(&data, returnTypeSize);
  // Arguments
  size_t argumentsSize = value->arguments.size();
  for (size_t i = 0; i < argumentsSize; i++) {
    MDTypeInfo *arg = value->arguments[i];
    auto argTypeSize = typeInfoSerde.size(arg);
    typeInfoSerde.serialize(arg, data);
    if (i != argumentsSize - 1) {
      // If this is not the last argument, we'll write the next flag.
      MDTypeKind *data_kind = (MDTypeKind *)data;
      data_kind[0] = static_cast<MDTypeKind>(
          static_cast<uint8_t>(arg->kind) |
          static_cast<uint8_t>(mdTypeFlagNext));
    }
    ptr_add(&data, argTypeSize);
  }
}

std::string MDSignatureSerde::encode(MDSignature *signature) {
  MDTypeInfoSerde typeInfoSerde;
  std::string result = typeInfoSerde.encode(signature->returnType) + "@:";
  for (MDTypeInfo *arg : signature->arguments) {
    result += typeInfoSerde.encode(arg);
  }
  if (signature->isVariadic) {
    result += "...";
  }
  return result;
}

size_t MDFunctionSerde::size(MDFunction *value) {
  size_t size = 0;
  // Name
  addsize(value->name);
  // Signature
  addsize(value->signature);
  // Flags
  addsize(value->flags);
  return size;
}

void MDFunctionSerde::serialize(MDFunction *value, void *data) {
  // Name
  binwrite(value->name);
  // Signature
  binwrite(value->signature);
  // Flags
  binwrite(value->flags);
}

} // namespace metagen
