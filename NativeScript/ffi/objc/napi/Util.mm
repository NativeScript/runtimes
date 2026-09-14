#include "Util.h"
#include <vector>
#include "Metadata.h"
#include "ObjCBridge.h"
#include "js_native_api.h"
#include "js_native_api_types.h"

using namespace metagen;

namespace nativescript {

inline std::string getStructEncodingForValue(napi_env env, napi_value value) {
  ObjCBridgeState* bridgeState = ObjCBridgeState::InstanceData(env);
  if (bridgeState == nullptr || value == nullptr) {
    return "";
  }

  napi_valuetype valueType = napi_undefined;
  napi_typeof(env, value, &valueType);
  if (valueType != napi_function && valueType != napi_object) {
    return "";
  }

  napi_value typeSymbol = jsSymbolFor(env, "type");
  if (typeSymbol != nullptr) {
    bool hasTypeEncoding = false;
    if (napi_has_property(env, value, typeSymbol, &hasTypeEncoding) == napi_ok && hasTypeEncoding) {
      napi_value typeEncodingValue = nullptr;
      if (napi_get_property(env, value, typeSymbol, &typeEncodingValue) == napi_ok &&
          typeEncodingValue != nullptr) {
        napi_valuetype typeEncodingType = napi_undefined;
        napi_typeof(env, typeEncodingValue, &typeEncodingType);
        if (typeEncodingType == napi_string) {
          size_t length = 0;
          napi_get_value_string_utf8(env, typeEncodingValue, nullptr, 0, &length);
          std::vector<char> buffer(length + 1, '\0');
          if (napi_get_value_string_utf8(env, typeEncodingValue, buffer.data(), buffer.size(),
                                         &length) == napi_ok) {
            return std::string(buffer.data(), length);
          }
        }
      }
    }
  }

  if (!bridgeState->structOffsets.empty()) {
    napi_value nameValue = nullptr;
    if (napi_get_named_property(env, value, "name", &nameValue) == napi_ok &&
        nameValue != nullptr) {
      napi_valuetype nameType = napi_undefined;
      napi_typeof(env, nameValue, &nameType);
      if (nameType == napi_string) {
        size_t nameLength = 0;
        napi_get_value_string_utf8(env, nameValue, nullptr, 0, &nameLength);
        std::vector<char> nameBuffer(nameLength + 1, '\0');
        if (napi_get_value_string_utf8(env, nameValue, nameBuffer.data(), nameBuffer.size(),
                                       &nameLength) == napi_ok) {
          std::string candidateName(nameBuffer.data(), nameLength);
          auto structIt = bridgeState->structOffsets.find(candidateName);
          if (structIt != bridgeState->structOffsets.end()) {
            StructInfo* info = bridgeState->getStructInfo(env, structIt->second);
            if (info != nullptr && info->name != nullptr) {
              std::string encoding = "{";
              encoding += info->name;
              encoding += "=";
              for (const auto& field : info->fields) {
                if (field.type == nullptr) {
                  return "";
                }
                field.type->encode(&encoding);
              }
              encoding += "}";
              return encoding;
            }
          }
        }
      }
    }
  }

  return "";
}

std::string implicitSetterSelector(std::string name) {
  std::string setter;
  setter += "set";
  setter += toupper(name[0]);
  setter += name.substr(1);
  setter += ":";
  return setter;
}

std::string jsifySelector(std::string selector) {
  std::string jsifiedSelector;
  bool nextupper = false;
  for (auto c : selector) {
    if (c == ':') {
      nextupper = true;
    } else if (nextupper) {
      jsifiedSelector += toupper(c);
      nextupper = false;
    } else {
      jsifiedSelector += c;
    }
  }
  return jsifiedSelector;
}

napi_value jsSymbolFor(napi_env env, const char* string) {
  napi_value global, Symbol, SymbolFor, symbol, symbolString;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Symbol", &Symbol);
  napi_get_named_property(env, Symbol, "for", &SymbolFor);
  napi_create_string_utf8(env, string, NAPI_AUTO_LENGTH, &symbolString);
  napi_call_function(env, global, SymbolFor, 1, &symbolString, &symbol);
  return symbol;
}

std::string getEncodedType(napi_env env, napi_value value) {
  napi_valuetype type;
  napi_typeof(env, value, &type);

  switch (type) {
    case napi_number: {
      int32_t number = -1;
      napi_get_value_int32(env, value, &number);

      switch (number) {
        case mdTypeVoid:
          return "v";

        case mdTypeBool:
          return "B";

        case mdTypeChar:
          return "c";

        case mdTypeUInt8:
          return "C";

        case mdTypeSShort:
          return "s";

        case mdTypeUShort:
        case mdTypeUnichar:
          return "S";

        case mdTypeSInt:
          return "i";

        case mdTypeUInt:
          return "I";

        case mdTypeSInt64:
          return "q";

        case mdTypeUInt64:
          return "Q";

        case mdTypeFloat:
          return "f";

        case mdTypeDouble:
          return "d";

        case mdTypeString:
          return "*";

        case mdTypeAnyObject:
          return "@";

        case mdTypePointer:
          return "^v";

        case mdTypeSelector:
          return ":";

        default:
          napi_throw_error(env, nullptr, "Invalid type");
          return "v";
      }
    }

    case napi_function:
    case napi_object: {
      std::string structEncoding = getStructEncodingForValue(env, value);
      if (!structEncoding.empty()) {
        return structEncoding;
      }

      auto bridgeState = ObjCBridgeState::InstanceData(env);
      if (bridgeState != nullptr) {
        id bridgedType = nil;
        if (bridgeState->tryResolveBridgedTypeConstructor(env, value, &bridgedType) &&
            bridgedType != nil) {
          return "@";
        }
      }

      if (type == napi_function) {
        // Native class constructor like NSObject.
        return "@";
      }

      napi_throw_error(env, nullptr, "Invalid type");
      return "v";
    }

    default:
      napi_throw_error(env, nullptr, "Invalid type");
      return "v";
  }
}

}  // namespace nativescript
