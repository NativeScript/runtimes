#include "Cif.h"
#include <Foundation/Foundation.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include "Metadata.h"
#include "MetadataReader.h"
#include "ObjCBridge.h"
#include "ffi/objc/shared/SignatureDispatchCore.h"
#include "TypeConv.h"
#include "Util.h"

namespace nativescript {
namespace {

inline bool typeRequiresSlowGeneratedNapiDispatch(const std::shared_ptr<TypeConv>& type) {
  if (type == nullptr) {
    return false;
  }

  switch (type->kind) {
    case mdTypeUChar:
    case mdTypeUInt8:
    case mdTypeString:
    case mdTypePointer:
    case mdTypeStruct:
    case mdTypeArray:
    case mdTypeBlock:
    case mdTypeFunctionPointer:
    case mdTypeVector:
    case mdTypeExtVector:
    case mdTypeComplex:
      return true;
    default:
      return false;
  }
}

inline bool typeKindMayUseRoundTripCache(MDTypeKind kind) {
  switch (kind) {
    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
      return true;
    default:
      return false;
  }
}

inline void updateGeneratedNapiDispatchCompatibility(Cif* cif) {
  if (cif == nullptr) {
    return;
  }

  cif->skipGeneratedNapiDispatch = false;
  cif->generatedDispatchHasRoundTripCacheArgument = false;
  cif->generatedDispatchUsesObjectReturnStorage = false;

  if (cif->returnType != nullptr) {
    cif->generatedDispatchUsesObjectReturnStorage =
        typeKindMayUseRoundTripCache(cif->returnType->kind);
  }

  cif->skipGeneratedNapiDispatch = typeRequiresSlowGeneratedNapiDispatch(cif->returnType);
  if (cif->skipGeneratedNapiDispatch) {
    return;
  }

  for (const auto& argType : cif->argTypes) {
    if (argType != nullptr && typeKindMayUseRoundTripCache(argType->kind)) {
      cif->generatedDispatchHasRoundTripCacheArgument = true;
    }
    if (typeRequiresSlowGeneratedNapiDispatch(argType)) {
      cif->skipGeneratedNapiDispatch = true;
      return;
    }
  }
}

}  // namespace

// Essentially, we cache libffi structures per unique method signature,
// this helps us avoid the overhead of creating them on the fly for each
// invocation.
Cif* ObjCBridgeState::getMethodCif(napi_env env, Method method) {
  auto encoding = std::string(method_getTypeEncoding(method));
  auto find = this->cifs[encoding];
  if (find != nullptr) {
    return find;
  }

  auto cif = new Cif(env, method);
  this->cifs[encoding] = cif;

  return cif;
}

Cif* ObjCBridgeState::getMethodCif(napi_env env, MDSectionOffset offset) {
  auto find = this->mdMethodSignatureCache[offset];
  if (find != nullptr) {
    return find;
  }

  auto cif = new Cif(env, metadata, offset, true, false);
  this->mdMethodSignatureCache[offset] = cif;

  return cif;
}

Cif* ObjCBridgeState::getBlockCif(napi_env env, MDSectionOffset offset) {
  auto find = this->mdBlockSignatureCache[offset];
  if (find != nullptr) {
    return find;
  }

  auto cif = new Cif(env, metadata, offset, false, true);
  this->mdBlockSignatureCache[offset] = cif;

  return cif;
}

Cif* ObjCBridgeState::getCFunctionCif(napi_env env, MDSectionOffset offset) {
  auto find = this->mdFunctionSignatureCache[offset];
  if (find != nullptr) {
    return find;
  }

  auto cif = new Cif(env, metadata, offset, false, false);
  this->mdFunctionSignatureCache[offset] = cif;

  return cif;
}

Cif::Cif(napi_env env, std::string encoding, unsigned int implicitArgc) {
  auto signature = [NSMethodSignature signatureWithObjCTypes:encoding.c_str()];
  unsigned long numberOfArguments = signature.numberOfArguments;
  unsigned long skippedArgs = std::min<unsigned long>(numberOfArguments, implicitArgc);
  this->argc = (int)(numberOfArguments - skippedArgs);
  this->argv = (napi_value*)malloc(sizeof(napi_value) * this->argc);

  unsigned int totalArgc = (unsigned int)numberOfArguments;

  const char* returnType = signature.methodReturnType;
  this->returnType = TypeConv::Make(env, &returnType);

  ffi_type* rtype = this->returnType->type;
  this->atypes = (ffi_type**)malloc(sizeof(ffi_type*) * totalArgc);

  unsigned long methodReturnLength = signature.methodReturnLength;
  unsigned long frameLength = signature.frameLength;

  this->rvalue = malloc(methodReturnLength);
  this->rvalueLength = methodReturnLength;
  this->frameLength = frameLength;

  this->avalues = this->argc > 0 ? (void**)malloc(sizeof(void*) * this->argc) : nullptr;
  if (this->avalues != nullptr) {
    memset(this->avalues, 0, sizeof(void*) * this->argc);
  }
  this->shouldFree = (bool*)malloc(sizeof(bool) * this->argc);
  memset(this->shouldFree, false, sizeof(bool) * this->argc);
  this->shouldFreeAny = false;
  this->avaluesAllocStart = 0;
  this->avaluesAllocCount = 0;

  for (int i = 0; i < numberOfArguments; i++) {
    const char* argenc = [signature getArgumentTypeAtIndex:i];

    auto argTypeInfo = TypeConv::Make(env, &argenc);
    this->atypes[i] = argTypeInfo->ffiTypeForArgument();

    if (i >= skippedArgs) {
      this->argTypes.push_back(argTypeInfo);
    }
  }

  ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, totalArgc, rtype, this->atypes);

  if (status != FFI_OK) {
    std::cout << "Failed to prepare CIF, libffi returned error:" << status << std::endl;
    return;
  }

  for (unsigned int i = 0; i < this->argc; i++) {
    this->avalues[i] = malloc(cif.arg_types[i + skippedArgs]->size);
    this->avaluesAllocCount++;
  }

  updateGeneratedNapiDispatchCompatibility(this);
}

Cif::Cif(napi_env env, Method method) {
  const unsigned int totalArgc = method_getNumberOfArguments(method);
  this->argc = totalArgc >= 2 ? totalArgc - 2 : 0;
  this->argv = this->argc > 0 ? (napi_value*)malloc(sizeof(napi_value) * this->argc) : nullptr;

  char* returnTypeEnc = method_copyReturnType(method);
  const char* returnTypePtr = returnTypeEnc;
  this->returnType = TypeConv::Make(env, &returnTypePtr);
  if (returnTypeEnc != nullptr) {
    free(returnTypeEnc);
  }

  ffi_type* rtype = this->returnType->type;
  this->atypes = (ffi_type**)malloc(sizeof(ffi_type*) * totalArgc);

  this->rvalueLength = std::max<size_t>(1, rtype->size);
  this->rvalue = malloc(this->rvalueLength);
  this->frameLength = 0;

  this->avalues = this->argc > 0 ? (void**)malloc(sizeof(void*) * this->argc) : nullptr;
  if (this->avalues != nullptr) {
    memset(this->avalues, 0, sizeof(void*) * this->argc);
  }

  this->shouldFree = this->argc > 0 ? (bool*)malloc(sizeof(bool) * this->argc) : nullptr;
  if (this->shouldFree != nullptr) {
    memset(this->shouldFree, false, sizeof(bool) * this->argc);
  }
  this->shouldFreeAny = false;
  this->avaluesAllocStart = 0;
  this->avaluesAllocCount = 0;

  for (unsigned int i = 0; i < totalArgc; i++) {
    char* argEnc = method_copyArgumentType(method, i);
    const char* argEncPtr = argEnc;
    auto argTypeInfo = TypeConv::Make(env, &argEncPtr);
    if (argEnc != nullptr) {
      free(argEnc);
    }

    this->atypes[i] = argTypeInfo->ffiTypeForArgument();
    if (i >= 2) {
      this->argTypes.push_back(argTypeInfo);
    }
  }

  ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, totalArgc, rtype, this->atypes);
  if (status != FFI_OK) {
    std::cout << "Failed to prepare CIF, libffi returned error:" << status << std::endl;
    return;
  }

  for (unsigned int i = 0; i < this->argc; i++) {
    this->avalues[i] = malloc(cif.arg_types[i + 2]->size);
    this->avaluesAllocCount++;
  }

  updateGeneratedNapiDispatchCompatibility(this);
}

Cif::Cif(napi_env env, MDMetadataReader* reader, MDSectionOffset offset, bool isMethod,
         bool isBlock) {
  MDSectionOffset signatureStart = offset;
  auto returnTypeKind = reader->getTypeKind(offset);
  bool next = ((MDTypeFlag)returnTypeKind & mdTypeFlagNext) != 0;
  isVariadic = ((MDTypeFlag)returnTypeKind & mdTypeFlagVariadic) != 0;

  returnType = TypeConv::Make(env, reader, &offset);

  auto implicitArgs = isMethod ? 2 : isBlock ? 1 : 0;

  shouldFreeAny = false;
  atypes = nullptr;
  avaluesAllocStart = 0;
  avaluesAllocCount = 0;

  if (next || isMethod || isBlock) {
    while (next) {
      auto argTypeKind = reader->getTypeKind(offset);
      next = ((MDTypeFlag)argTypeKind & mdTypeFlagNext) != 0;
      auto argTypeInfo = TypeConv::Make(env, reader, &offset);
      std::string enc;
      argTypeInfo->encode(&enc);
      argTypes.push_back(argTypeInfo);
    }

    argc = (int)argTypes.size();

    auto totalArgc = argc + implicitArgs;

    argv = (napi_value*)malloc(sizeof(napi_value) * argc);
    shouldFree = (bool*)malloc(sizeof(bool) * argc);

    atypes = (ffi_type**)malloc(sizeof(ffi_type*) * totalArgc);
    avalues = (void**)malloc(sizeof(void*) * argc);
    memset(avalues, 0, sizeof(void*) * argc);

    if (isMethod) {
      atypes[0] = &ffi_type_pointer;
      atypes[1] = &ffi_type_pointer;
    }

    if (isBlock) {
      atypes[0] = &ffi_type_pointer;
    }

    for (int i = 0; i < argc; i++) {
      atypes[i + implicitArgs] = argTypes[i]->ffiTypeForArgument();
      shouldFree[i] = false;
    }
  } else {
    argc = 0;
    argv = nullptr;
    avalues = nullptr;
    shouldFree = nullptr;
  }

  ffi_status status =
      ffi_prep_cif(&cif, FFI_DEFAULT_ABI, argc + implicitArgs, returnType->type, atypes);

  if (status != FFI_OK) {
    std::cout << "Failed to prepare CIF, libffi returned error: " << status << std::endl;
    return;
  }

  for (int i = 0; i < argc; i++) {
    avalues[i] = malloc(cif.arg_types[i + implicitArgs]->size);
    avaluesAllocCount++;
  }

  rvalue = malloc(cif.rtype->size);
  rvalueLength = cif.rtype->size;

  signatureHash = metadataSignatureHash(reader, signatureStart);

  updateGeneratedNapiDispatchCompatibility(this);
}

Cif::~Cif() {
  if (rvalue != nullptr) {
    free(rvalue);
  }
  if (argv != nullptr) {
    free(argv);
  }
  if (avalues != nullptr) {
    for (unsigned int i = 0; i < avaluesAllocCount; i++) {
      auto index = avaluesAllocStart + i;
      if (avalues[index] != nullptr) {
        free(avalues[index]);
      }
    }
    free(avalues);
  }
  if (atypes != nullptr) {
    free(atypes);
  }
  if (shouldFree != nullptr) {
    free(shouldFree);
  }
}

}  // namespace nativescript
