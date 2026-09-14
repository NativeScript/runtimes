#ifndef METHOD_CIF_H
#define METHOD_CIF_H

#include <cstdint>
#include <string>

#include "MetadataReader.h"
#include "TypeConv.h"
#include "ffi.h"
#include "objc/message.h"
#include "objc/runtime.h"

namespace nativescript {

class Cif {
 public:
  ffi_cif cif;
  unsigned int argc;
  size_t frameLength;
  size_t rvalueLength;
  bool isVariadic = false;
  uint64_t signatureHash = 0;
  bool skipGeneratedNapiDispatch = false;
  bool generatedDispatchHasRoundTripCacheArgument = false;
  bool generatedDispatchUsesObjectReturnStorage = false;

  void* rvalue;
  void** avalues;
  ffi_type** atypes = nullptr;
  unsigned int avaluesAllocStart = 0;
  unsigned int avaluesAllocCount = 0;

  std::shared_ptr<TypeConv> returnType;
  std::vector<std::shared_ptr<TypeConv>> argTypes;

  napi_value* argv;
  bool shouldFreeAny;
  bool* shouldFree;

  Cif(napi_env env, std::string typeEncoding, unsigned int implicitArgc = 2);
  Cif(napi_env env, Method method);
  Cif(napi_env env, MDMetadataReader* reader, MDSectionOffset offset,
      bool isMethod = false, bool isBlock = false);

  ~Cif();
};

}  // namespace nativescript

#endif /* METHOD_CIF_H */
