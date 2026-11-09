#include "Struct.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include "Interop.h"
#include "ObjCBridge.h"
#include "TypeConv.h"
#include "Util.h"
#include "js_native_api.h"
#include "js_native_api_types.h"
#include "node_api_util.h"

#import <Foundation/Foundation.h>

namespace nativescript {

namespace {
std::string buildStructTypeEncoding(StructInfo* info) {
  if (info == nullptr || info->name == nullptr) {
    return "";
  }

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
}  // namespace

void ObjCBridgeState::registerStructGlobals(napi_env env, napi_value global) {
  MDSectionOffset offset = metadata->structsOffset;
  while (offset < metadata->unionsOffset) {
    // Sometimes there is padding after file ends.
    if (metadata->getOffset(offset) == 0) break;
    MDSectionOffset originalOffset = offset;
    auto name = metadata->getString(offset);
    offset += sizeof(MDSectionOffset);
    auto size = metadata->getArraySize(offset);
    offset += sizeof(uint16_t);

    std::string nameStr = name;
    structOffsets[nameStr] = originalOffset;

    bool next = true;
    while (next) {
      MDSectionOffset nameOffset = metadata->getOffset(offset);
      offset += sizeof(MDSectionOffset);
      next = (nameOffset & mdSectionOffsetNext) != 0;
      nameOffset &= ~mdSectionOffsetNext;
      if (nameOffset == MD_SECTION_OFFSET_NULL) break;
      auto name = metadata->resolveString(nameOffset);
      offset += sizeof(uint16_t);
      TypeConv::Make(env, metadata, &offset);
    }

    bool hasPrimaryName = false;
    napi_has_named_property(env, global, name, &hasPrimaryName);
    if (!hasPrimaryName) {
      napi_property_descriptor prop = {
          .utf8name = name,
          .method = nullptr,
          .getter = JS_structGetter,
          .setter = nullptr,
          .value = nullptr,
          .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
          .data = (void*)((size_t)originalOffset),
      };
      napi_define_properties(env, global, 1, &prop);
    }

    if (!nameStr.empty() && nameStr[0] == '_') {
      std::string alias = nameStr.substr(1);
      if (!alias.empty()) {
        bool hasAlias = false;
        napi_has_named_property(env, global, alias.c_str(), &hasAlias);
        if (!hasAlias) {
          napi_property_descriptor aliasProp = {
              .utf8name = alias.c_str(),
              .method = nullptr,
              .getter = JS_structGetter,
              .setter = nullptr,
              .value = nullptr,
              .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
              .data = (void*)((size_t)originalOffset),
          };
          napi_define_properties(env, global, 1, &aliasProp);
        }
      }
    }

    if (nameStr.size() < 6 || nameStr.compare(nameStr.size() - 6, 6, "Struct") != 0) {
      std::string alias = nameStr + "Struct";
      bool hasAlias = false;
      napi_has_named_property(env, global, alias.c_str(), &hasAlias);
      if (!hasAlias) {
        napi_property_descriptor aliasProp = {
            .utf8name = alias.c_str(),
            .method = nullptr,
            .getter = JS_structGetter,
            .setter = nullptr,
            .value = nullptr,
            .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
            .data = (void*)((size_t)originalOffset),
        };
        napi_define_properties(env, global, 1, &aliasProp);
      }
    }
  }
}

void ObjCBridgeState::registerUnionGlobals(napi_env env, napi_value global) {
  MDSectionOffset offset = metadata->unionsOffset;
  while (true) {
    // It's the last section, always ends with 4 bytes of 0.
    if (metadata->getOffset(offset) == 0) break;
    MDSectionOffset originalOffset = offset;
    auto name = metadata->getString(offset);
    offset += sizeof(MDSectionOffset);
    auto size = metadata->getArraySize(offset);
    offset += sizeof(uint16_t);

    std::string nameStr = name;
    unionOffsets[nameStr] = originalOffset;

    bool next = true;
    while (next) {
      MDSectionOffset nameOffset = metadata->getOffset(offset);
      next = (nameOffset & mdSectionOffsetNext) != 0;
      nameOffset &= ~mdSectionOffsetNext;
      auto name = metadata->resolveString(nameOffset);
      offset += sizeof(MDSectionOffset);
      TypeConv::Make(env, metadata, &offset);
    }

    napi_property_descriptor prop = {
        .utf8name = name,
        .method = nullptr,
        .getter = JS_unionGetter,
        .setter = nullptr,
        .value = nullptr,
        .attributes = (napi_property_attributes)(napi_enumerable | napi_configurable),
        .data = (void*)((size_t)originalOffset),
    };
    napi_define_properties(env, global, 1, &prop);
  }
}

NAPI_FUNCTION(structGetter) {
  NS_OBJC_NAPI_PREAMBLE

  void* data;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, nullptr, &data);
  MDSectionOffset offset = (MDSectionOffset)((size_t)data);

  auto bridgeState = ObjCBridgeState::InstanceData(env);
  auto structInfo = bridgeState->getStructInfo(env, offset);
  return StructObject::getJSClass(env, structInfo);
}

NAPI_FUNCTION(unionGetter) {
  NS_OBJC_NAPI_PREAMBLE

  void* data;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, nullptr, &data);
  MDSectionOffset offset = (MDSectionOffset)((size_t)data);

  auto bridgeState = ObjCBridgeState::InstanceData(env);
  auto structInfo = bridgeState->getUnionInfo(env, offset);
  return StructObject::getJSClass(env, structInfo);
}

StructInfo* getStructInfoFromMetadata(napi_env env, MDMetadataReader* metadata,
                                      MDSectionOffset offset) {
  auto originalOffset = offset;

  auto structInfo = new StructInfo();
  auto name = metadata->getString(offset);
  structInfo->name = name;
  offset += sizeof(MDSectionOffset);
  structInfo->size = metadata->getArraySize(offset);
  offset += sizeof(uint16_t);

  bool next = true;
  while (next) {
    MDSectionOffset nameOffset = metadata->getOffset(offset);
    next = (nameOffset & mdSectionOffsetNext) != 0;
    if (next) {
      nameOffset &= ~mdSectionOffsetNext;
    }
    if (nameOffset == MD_SECTION_OFFSET_NULL) {
      break;
    }
    auto fieldInfo = StructFieldInfo();
    fieldInfo.name = metadata->resolveString(nameOffset);
    offset += sizeof(MDSectionOffset);
    fieldInfo.offset = metadata->getArraySize(offset);
    offset += sizeof(uint16_t);
    fieldInfo.type = TypeConv::Make(env, metadata, &offset);
    structInfo->fields.push_back(fieldInfo);
  }

  return structInfo;
}

StructInfo* getStructInfoFromUnionMetadata(napi_env env, MDMetadataReader* metadata,
                                           MDSectionOffset offset) {
  auto originalOffset = offset;

  auto structInfo = new StructInfo();
  auto name = metadata->getString(offset);
  structInfo->name = name;
  offset += sizeof(MDSectionOffset);
  structInfo->size = metadata->getArraySize(offset);
  offset += sizeof(uint16_t);

  bool next = true;
  while (next) {
    MDSectionOffset nameOffset = metadata->getOffset(offset);
    offset += sizeof(MDSectionOffset);
    next = (nameOffset & mdSectionOffsetNext) != 0;
    if (next) {
      nameOffset &= ~mdSectionOffsetNext;
    }
    if (nameOffset == 0) {
      break;
    }
    auto fieldInfo = StructFieldInfo();
    fieldInfo.name = metadata->resolveString(nameOffset);
    fieldInfo.offset = 0;
    fieldInfo.type = TypeConv::Make(env, metadata, &offset);
    structInfo->fields.push_back(fieldInfo);
  }

  return structInfo;
}

namespace {
constexpr napi_type_tag kStructObjectTypeTag = {
    0x93d4bc8be2d74d2dULL,
    0xa3c150244f745f84ULL,
};

void StructObject_finalize_now(napi_env env, void* data, void* hint) {
  auto structObject = (StructObject*)data;
  delete structObject;
}
}  // namespace

void StructObject_finalize(napi_env env, void* data, void* hint) {
  if (PostFinalizer(env, StructObject_finalize_now, data, hint)) {
    return;
  }

  StructObject_finalize_now(env, data, hint);
}

NAPI_FUNCTION(StructConstructor) {
  NS_OBJC_NAPI_PREAMBLE

  napi_value jsThis;
  napi_value argv[1];
  StructInfo* info;
  size_t argc = 1;

  napi_get_cb_info(env, cbinfo, &argc, argv, &jsThis, (void**)&info);

  napi_valuetype thisType = napi_undefined;
  if (jsThis == nullptr || napi_typeof(env, jsThis, &thisType) != napi_ok ||
      (thisType != napi_object && thisType != napi_function)) {
    napi_create_object(env, &jsThis);

    if (info != nullptr) {
      napi_value structCtor = StructObject::getJSClass(env, info);
      napi_value structPrototype = nullptr;
      if (structCtor != nullptr &&
          napi_get_named_property(env, structCtor, "prototype", &structPrototype) == napi_ok &&
          structPrototype != nullptr) {
        napi_value global = nullptr;
        napi_value objectCtor = nullptr;
        napi_value setPrototypeOf = nullptr;
        napi_get_global(env, &global);
        napi_get_named_property(env, global, "Object", &objectCtor);
        napi_get_named_property(env, objectCtor, "setPrototypeOf", &setPrototypeOf);
        napi_value setPrototypeArgs[2] = {jsThis, structPrototype};
        napi_call_function(env, objectCtor, setPrototypeOf, 2, setPrototypeArgs, nullptr);
      }
    }
  }

  napi_value arg;
  if (argc > 0) {
    arg = argv[0];
  } else {
    napi_get_undefined(env, &arg);
  }

  napi_valuetype argType;
  napi_typeof(env, arg, &argType);

  StructObject* object;

  if (argType == napi_object) {
    if (Pointer::isInstance(env, arg)) {
      Pointer* pointer = Pointer::unwrap(env, arg);
      if (pointer == nullptr) {
        napi_throw_error(env, nullptr, "Invalid pointer-backed struct argument");
        return nullptr;
      }
      object = new StructObject(info, pointer->data, env, arg);
    } else if (Reference::isInstance(env, arg)) {
      Reference* reference = Reference::unwrap(env, arg);
      if (reference == nullptr || reference->data == nullptr) {
        napi_throw_error(env, nullptr, "Reference is not initialized");
        return nullptr;
      }
      object = new StructObject(info, reference->data, env, arg);
    } else {
      object = new StructObject(env, info, arg);
    }
  } else {
    object = new StructObject(info);
  }

  napi_ref* wrapperRef = nullptr;
#if defined(TARGET_ENGINE_HERMES)
  wrapperRef = &object->wrapperRef;
#endif
  if (napi_wrap(env, jsThis, object, StructObject_finalize, nullptr, wrapperRef) != napi_ok) {
    delete object;
    return nullptr;
  }
  if (napi_type_tag_object(env, jsThis, &kStructObjectTypeTag) != napi_ok) {
    void* removed = nullptr;
    napi_remove_wrap(env, jsThis, &removed);
    delete static_cast<StructObject*>(removed);
    return nullptr;
  }

  return jsThis;
}

NAPI_FUNCTION(StructPropertyGetter) {
  NS_OBJC_NAPI_PREAMBLE

  napi_value jsThis;
  StructFieldInfo* info;

  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &jsThis, (void**)&info);

  auto object = StructObject::unwrap(env, jsThis);
  if (object == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid struct receiver");
    return nullptr;
  }
  auto value = object->get(env, info);

  if (StructObject::isInstance(env, value)) {
    napi_set_named_property(env, value, "__ns_parent_struct", jsThis);
  } else {
    bool isArray = false;
    if (napi_is_array(env, value, &isArray) == napi_ok && isArray) {
      napi_set_named_property(env, value, "__ns_parent_struct", jsThis);
    }
  }

  return value;
}

NAPI_FUNCTION(StructPropertySetter) {
  NS_OBJC_NAPI_PREAMBLE

  napi_value jsThis, arg;
  StructFieldInfo* info;
  size_t argc = 1;

  napi_get_cb_info(env, cbinfo, &argc, &arg, &jsThis, (void**)&info);

  auto object = StructObject::unwrap(env, jsThis);
  if (object == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid struct receiver");
    return nullptr;
  }
  object->set(env, info, arg);

  return nullptr;
}

NAPI_FUNCTION(StructCustomInspect) {
  napi_value jsThis;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &jsThis, nullptr);

  auto object = StructObject::unwrap(env, jsThis);
  if (object == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid struct receiver");
    return nullptr;
  }
  std::string str = "struct ";
  str += object->info->name;
  str += " {}";

  napi_value result;
  napi_create_string_utf8(env, str.c_str(), str.length(), &result);
  return result;
}

NAPI_FUNCTION(StructEquals) {
  napi_value jsThis, argv[2];
  size_t argc = 2;
  void* data = nullptr;
  napi_get_cb_info(env, cbinfo, &argc, argv, &jsThis, &data);

  StructInfo* info = static_cast<StructInfo*>(data);
  if (info == nullptr || argc < 2) {
    napi_value result;
    napi_get_boolean(env, false, &result);
    return result;
  }

  auto serialize = [&](napi_value value, std::vector<uint8_t>& out) -> bool {
    out.assign(info->size, 0);

    StructObject* structObject = StructObject::unwrap(env, value);
    if (structObject != nullptr) {
      size_t copySize =
          std::min(static_cast<size_t>(info->size), static_cast<size_t>(structObject->info->size));
      memcpy(out.data(), structObject->data, copySize);
      return true;
    }

    napi_valuetype type = napi_undefined;
    napi_typeof(env, value, &type);
    if (type != napi_object) {
      return false;
    }

    StructObject(env, info, value, out.data());
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    return !pending;
  };

  std::vector<uint8_t> left;
  std::vector<uint8_t> right;
  bool okLeft = serialize(argv[0], left);
  bool okRight = serialize(argv[1], right);

  napi_value result;
  napi_get_boolean(env, okLeft && okRight && memcmp(left.data(), right.data(), info->size) == 0,
                   &result);
  return result;
}

inline StructObject::StructObject(StructInfo* info, void* data, napi_env env,
                                  napi_value backingValue) {
  this->info = info;
  this->env = env;
  this->bridgeState = env != nullptr ? ObjCBridgeState::InstanceData(env) : nullptr;
  this->bridgeStateToken = this->bridgeState != nullptr ? this->bridgeState->lifetimeToken : 0;
  if (data == nullptr) {
    this->data = malloc(info->size);
    memset(this->data, 0, this->info->size);
    this->owned = true;
  } else {
    this->data = data;
    this->owned = false;
    if (env != nullptr && backingValue != nullptr) {
      napi_create_reference(env, backingValue, 1, &this->backingRef);
    }
  }
}

StructObject::StructObject(napi_env env, StructInfo* info, napi_value object, void* memory) {
  this->info = info;
  this->env = env;
  this->bridgeState = env != nullptr ? ObjCBridgeState::InstanceData(env) : nullptr;
  this->bridgeStateToken = this->bridgeState != nullptr ? this->bridgeState->lifetimeToken : 0;

  if (memory == nullptr) {
    this->owned = true;
    this->data = malloc(info->size);
  } else {
    this->owned = false;
    this->data = memory;
  }

  memset(this->data, 0, this->info->size);

  for (auto& field : info->fields) {
    bool hasProp = false;
    napi_has_named_property(env, object, field.name, &hasProp);
    if (!hasProp) {
      continue;
    }
    napi_value property;
    napi_get_named_property(env, object, field.name, &property);
    set(env, &field, property);
  }
}

StructObject::~StructObject() {
#if defined(TARGET_ENGINE_HERMES)
  if (this->wrapperRef != nullptr && this->env != nullptr &&
      (this->bridgeState == nullptr ||
       IsBridgeStateLive(this->bridgeState, this->bridgeStateToken))) {
    DeleteReferenceOnOwningThread(this->env, this->bridgeState, this->bridgeStateToken,
                                  this->wrapperRef);
  }
  this->wrapperRef = nullptr;
#endif
  if (this->backingRef != nullptr && this->env != nullptr &&
      (this->bridgeState == nullptr ||
       IsBridgeStateLive(this->bridgeState, this->bridgeStateToken))) {
    DeleteReferenceOnOwningThread(this->env, this->bridgeState, this->bridgeStateToken,
                                  this->backingRef);
  }
  this->backingRef = nullptr;
  this->bridgeState = nullptr;
  this->bridgeStateToken = 0;
  if (this->owned) free(this->data);
}

napi_value StructObject::get(napi_env env, StructFieldInfo* field) {
  auto data = (char*)this->data + field->offset;
  return field->type->toJS(env, data, kStructZeroCopy);
}

void StructObject::set(napi_env env, StructFieldInfo* field, napi_value value) {
  auto data = (char*)this->data + field->offset;
  bool shouldFree = false;
  field->type->toNative(env, value, data, &shouldFree, &shouldFree);
  ConsumeNapiArgumentConversionFailure(env);
}

StructObject* StructObject::unwrap(napi_env env, napi_value object) {
  bool matches = false;
  if (napi_check_object_type_tag(env, object, &kStructObjectTypeTag, &matches) != napi_ok ||
      !matches) {
    return nullptr;
  }

  StructObject* result = nullptr;
  auto status = napi_unwrap(env, object, (void**)&result);
  if (status != napi_ok) return nullptr;
#if defined(TARGET_ENGINE_HERMES)
  if (result == nullptr || result->wrapperRef == nullptr) {
    return nullptr;
  }

  napi_value wrapper = nullptr;
  bool same = false;
  if (napi_get_reference_value(env, result->wrapperRef, &wrapper) != napi_ok ||
      wrapper == nullptr || napi_strict_equals(env, object, wrapper, &same) != napi_ok ||
      !same) {
    return nullptr;
  }
#endif
  return result;
}

napi_value StructObject::defineJSClass(napi_env env, StructInfo* info) {
  auto properties = (napi_property_descriptor*)malloc((info->fields.size() + 2) *
                                                      sizeof(napi_property_descriptor));

  for (int i = 0; i < info->fields.size(); i++) {
    auto field = info->fields[i];
    auto prop = &properties[i];
    prop->utf8name = field.name;
    prop->name = nullptr;
    prop->method = nullptr;
    prop->value = nullptr;
    prop->attributes = (napi_property_attributes)(napi_enumerable | napi_writable);
    prop->data = &info->fields[i];
    prop->getter = JS_StructPropertyGetter;
    prop->setter = JS_StructPropertySetter;
  }

  auto prop = &properties[info->fields.size()];
  prop->utf8name = nullptr;
  prop->name = jsSymbolFor(env, "nodejs.util.inspect.custom");
  prop->method = JS_StructCustomInspect;
  prop->getter = nullptr;
  prop->setter = nullptr;
  prop->value = nullptr;
  prop->attributes = napi_default;
  prop->data = nullptr;

  napi_value size;
  napi_create_int32(env, info->size, &size);

  auto sizeofProp = &properties[info->fields.size() + 1];
  sizeofProp->utf8name = nullptr;
  sizeofProp->name = jsSymbolFor(env, "sizeof");
  sizeofProp->method = nullptr;
  sizeofProp->getter = nullptr;
  sizeofProp->setter = nullptr;
  sizeofProp->value = size;
  sizeofProp->attributes = napi_enumerable;
  sizeofProp->data = nullptr;

  napi_value result;
  napi_define_class(env, info->name, NAPI_AUTO_LENGTH, JS_StructConstructor, (void*)info,
                    info->fields.size() + 2, properties, &result);

  const napi_property_descriptor classProps[] = {
      {
          .utf8name = "equals",
          .method = JS_StructEquals,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_default,
          .data = info,
      },
      {
          .utf8name = nullptr,
          .name = jsSymbolFor(env, "sizeof"),
          .method = nullptr,
          .getter = nullptr,
          .setter = nullptr,
          .value = size,
          .attributes = napi_enumerable,
          .data = nullptr,
      },
  };
  napi_define_properties(env, result, 2, classProps);

  std::string typeEncoding = buildStructTypeEncoding(info);
  if (!typeEncoding.empty()) {
    napi_value typeSymbol = jsSymbolFor(env, "type");
    napi_value typeValue = nullptr;
    napi_create_string_utf8(env, typeEncoding.c_str(), NAPI_AUTO_LENGTH, &typeValue);
    napi_set_property(env, result, typeSymbol, typeValue);

    napi_value prototype = nullptr;
    if (napi_get_named_property(env, result, "prototype", &prototype) == napi_ok &&
        prototype != nullptr) {
      napi_set_property(env, prototype, typeSymbol, typeValue);
    }
  }

  free(properties);

  return result;
}

bool StructObject::isInstance(napi_env env, napi_value object) {
  bool matches = false;
  return object != nullptr &&
         napi_check_object_type_tag(env, object, &kStructObjectTypeTag,
                                    &matches) == napi_ok &&
         matches && StructObject::unwrap(env, object) != nullptr;
}

napi_value StructObject::getJSClass(napi_env env, StructInfo* info) {
  if (info->jsClass != nullptr) {
    return get_ref_value(env, info->jsClass);
  }

  auto result = defineJSClass(env, info);
  info->jsClass = make_ref(env, result);

  return result;
}

napi_value StructObject::fromNative(napi_env env, StructInfo* info, void* data, bool owned) {
  napi_value result;
  napi_value cls = getJSClass(env, info);
  napi_new_instance(env, cls, 0, nullptr, &result);
  auto object = StructObject::unwrap(env, result);
  if (object == nullptr) {
    return result;
  }

  if (owned) {
    if (object->owned) {
      memcpy(object->data, data, info->size);
    } else {
      object->data = malloc(info->size);
      memcpy(object->data, data, info->size);
      object->owned = true;
    }
  } else {
    if (object->owned && object->data != nullptr) {
      free(object->data);
    }
    object->data = data;
    object->owned = false;
  }
  return result;
}

}  // namespace nativescript
