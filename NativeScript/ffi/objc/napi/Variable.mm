#include "Variable.h"
#include "MetadataReader.h"
#include "ObjCBridge.h"
#include "js_native_api.h"

namespace nativescript {

namespace {

const char* getValidatedConstantString(MDMetadataReader* metadata,
                                       MDSectionOffset stringOffsetRef) {
  if (metadata == nullptr || metadata->data == nullptr) {
    return nullptr;
  }

  if (stringOffsetRef < metadata->constantsOffset ||
      stringOffsetRef + sizeof(MDSectionOffset) > metadata->enumsOffset) {
    return nullptr;
  }

  const auto stringsStart = static_cast<const char*>(metadata->data) + metadata->stringsOffset;
  const auto stringsSize = metadata->constantsOffset - metadata->stringsOffset;
  if (stringsSize == 0) {
    return nullptr;
  }

  const auto stringOffset = metadata->getOffset(stringOffsetRef);
  if (stringOffset >= stringsSize) {
    return nullptr;
  }

  const char* value = stringsStart + stringOffset;
  const auto remaining = stringsSize - stringOffset;
  if (memchr(value, '\0', remaining) == nullptr) {
    return nullptr;
  }

  return value;
}

}  // namespace

void ObjCBridgeState::registerVarGlobals(napi_env env, napi_value global) {
  auto offset = metadata->constantsOffset;
  while (offset < metadata->enumsOffset) {
    MDSectionOffset originalOffset = offset;
    auto name = getValidatedConstantString(metadata, offset);
    offset += sizeof(MDSectionOffset);
    auto evalKind = metadata->getVariableEvalKind(offset);
    offset += sizeof(MDVariableEvalKind);

    if (name != nullptr && name[0] != '\0') {
      napi_property_descriptor prop = {
          .utf8name = name,
          .method = nullptr,
          .getter = JS_variableGetter,
          .setter = nullptr,
          .value = nullptr,
          .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
          .data = (void*)((size_t)originalOffset),
      };

      napi_define_properties(env, global, 1, &prop);
    }

    switch (evalKind) {
      case mdEvalDouble: {
        offset += sizeof(double);
        break;
      }

      case mdEvalInt64: {
        offset += sizeof(int64_t);
        break;
      }

      case mdEvalString: {
        offset += sizeof(MDSectionOffset);
        break;
      }

      default:
        TypeConv::Make(env, metadata, &offset);
        break;
    }
  }
}

NAPI_FUNCTION(variableGetter) {
  void* data;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, nullptr, &data);
  MDSectionOffset offset = (MDSectionOffset)((size_t)data);
  MDSectionOffset originalOffset = offset;
  auto bridgeState = ObjCBridgeState::InstanceData(env);

  auto cached = bridgeState->mdValueCache[offset];
  if (cached != nullptr) {
    return get_ref_value(env, cached);
  }

  napi_value result = nullptr;

  // Name
  auto name = getValidatedConstantString(bridgeState->metadata, offset);
  if (name == nullptr || name[0] == '\0') {
    napi_get_null(env, &result);
    return result;
  }
  offset += sizeof(MDSectionOffset);

  // Eval kind
  auto evalKind = bridgeState->metadata->getVariableEvalKind(offset);
  offset += sizeof(MDVariableEvalKind);

  switch (evalKind) {
    case mdEvalDouble: {
      auto value = bridgeState->metadata->getDouble(offset);
      napi_create_double(env, value, &result);
      break;
    }

    case mdEvalInt64: {
      auto value = bridgeState->metadata->getInt64(offset);
      napi_create_int64(env, value, &result);
      break;
    }

    case mdEvalString: {
      auto value = bridgeState->metadata->getString(offset);
      napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &result);
      if (result != nullptr) {
        bridgeState->mdValueCache[originalOffset] = make_ref(env, result);
      }
      break;
    }

    default: {
      auto type = TypeConv::Make(env, bridgeState->metadata, &offset);
      auto value = dlsym(bridgeState->self_dl, name);
      if (value == nullptr) {
        value = dlsym(RTLD_DEFAULT, name);
      }
      if (value == nullptr) {
        napi_get_null(env, &result);
      } else {
        result = type->toJS(env, value);
      }
      return result;
    }
  }

  return result;
}

}  // namespace nativescript
