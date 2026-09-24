// Included by NativeApiQuickJSSelectorGroups.mm inside the NativeScript anonymous namespace.

std::string quickJSValueToUtf8(JSContext* context, JSValueConst value) {
  size_t length = 0;
  const char* text = JS_ToCStringLen(context, &length, value);
  if (text == nullptr) {
    return {};
  }
  std::string result(text, length);
  JS_FreeCString(context, text);
  return result;
}

bool quickJSNumberValue(JSContext* context, JSValueConst value,
                        double* result) {
  if (result == nullptr) {
    return false;
  }
  double converted = 0;
  if (JS_ToFloat64(context, &converted, value) < 0) {
    return false;
  }
  *result = converted;
  return true;
}

template <typename T>
std::shared_ptr<T> quickJSHostObject(Runtime& runtime, JSValueConst value) {
  if (!JS_IsObject(value)) {
    return nullptr;
  }
  engine::quickjsengine::ensureClasses(runtime);
  auto* holder = static_cast<engine::quickjsengine::HostObjectHolder*>(
      JS_GetOpaque(value, engine::quickjsengine::gHostClassId));
  if (holder == nullptr ||
      holder->typeToken != engine::quickjsengine::hostObjectTypeToken<T>()) {
    return nullptr;
  }
  return std::static_pointer_cast<T>(holder->hostObject);
}

template <typename T>
T* quickJSHostObjectRaw(Runtime& runtime, JSValueConst value) {
  if (!JS_IsObject(value)) {
    return nullptr;
  }
  engine::quickjsengine::ensureClasses(runtime);
  auto* holder = static_cast<engine::quickjsengine::HostObjectHolder*>(
      JS_GetOpaque(value, engine::quickjsengine::gHostClassId));
  if (holder == nullptr ||
      holder->typeToken != engine::quickjsengine::hostObjectTypeToken<T>()) {
    return nullptr;
  }
  return static_cast<T*>(holder->hostObject.get());
}

id quickJSNativeObjectArgument(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, JSValueConst value,
    NativeApiArgumentFrame& frame) {
  JSContext* context = runtime.context();
  if (JS_IsNull(value) || JS_IsUndefined(value)) {
    return nil;
  }
  if (JS_IsString(value)) {
    std::string utf8 = quickJSValueToUtf8(context, value);
    id string = type.kind == metagen::mdTypeNSMutableStringObject
                    ? [[NSMutableString alloc] initWithBytes:utf8.data()
                                                      length:utf8.size()
                                                    encoding:NSUTF8StringEncoding]
                    : [[NSString alloc] initWithBytes:utf8.data()
                                               length:utf8.size()
                                             encoding:NSUTF8StringEncoding];
    if (string != nil) {
      frame.addObject(string);
    }
    return string;
  }
  if (JS_IsBool(value)) {
    return [NSNumber numberWithBool:JS_ToBool(context, value) != 0];
  }
  if (JS_IsNumber(value) || JS_IsBigInt(value)) {
    double converted = 0;
    if (quickJSNumberValue(context, value, &converted)) {
      return [NSNumber numberWithDouble:converted];
    }
  }
  if (!JS_IsObject(value)) {
    return nil;
  }
  if (auto objectHost =
          quickJSHostObject<NativeApiObjectHostObject>(runtime, value)) {
    return objectHost->object();
  }
  if (auto classHost =
          quickJSHostObject<NativeApiClassHostObject>(runtime, value)) {
    return static_cast<id>(classHost->nativeClass());
  }
  if (auto protocolHost =
          quickJSHostObject<NativeApiProtocolHostObject>(runtime, value)) {
    return static_cast<id>(protocolHost->nativeProtocol());
  }
  if (auto pointerHost =
          quickJSHostObject<NativeApiPointerHostObject>(runtime, value)) {
    return static_cast<id>(pointerHost->pointer());
  }
  if (auto referenceHost =
          quickJSHostObject<NativeApiReferenceHostObject>(runtime, value)) {
    return static_cast<id>(referenceHost->data());
  }
  if (auto structHost =
          quickJSHostObject<NativeApiStructObjectHostObject>(runtime, value)) {
    return static_cast<id>(structHost->data());
  }

  JSValue wrappedClassValue =
      JS_GetPropertyStr(context, value, "__nativeApiClass");
  if (!JS_IsException(wrappedClassValue)) {
    if (auto classHost = quickJSHostObject<NativeApiClassHostObject>(
            runtime, wrappedClassValue)) {
      JS_FreeValue(context, wrappedClassValue);
      return static_cast<id>(classHost->nativeClass());
    }
  }
  JS_FreeValue(context, wrappedClassValue);

  Value wrapped = Value::borrowed(runtime, value);
  return objectFromEngineValue(runtime, bridge, wrapped, frame,
                               type.kind ==
                                   metagen::mdTypeNSMutableStringObject);
}

Class quickJSNativeClassArgument(Runtime& runtime, JSValueConst value) {
  if (JS_IsNull(value) || JS_IsUndefined(value)) {
    return Nil;
  }
  if (auto classHost =
          quickJSHostObject<NativeApiClassHostObject>(runtime, value)) {
    return classHost->nativeClass();
  }
  if (JS_IsObject(value)) {
    JSValue wrappedClassValue =
        JS_GetPropertyStr(runtime.context(), value, "__nativeApiClass");
    if (!JS_IsException(wrappedClassValue)) {
      if (auto classHost = quickJSHostObject<NativeApiClassHostObject>(
              runtime, wrappedClassValue)) {
        JS_FreeValue(runtime.context(), wrappedClassValue);
        return classHost->nativeClass();
      }
    }
    JS_FreeValue(runtime.context(), wrappedClassValue);
  }
  Value wrapped = Value::borrowed(runtime, value);
  return classFromEngineValue(runtime, wrapped);
}

bool readQuickJSEngineSelectorArgument(Runtime& runtime, JSValueConst value,
                                       SEL* result) {
  if (result == nullptr) {
    return false;
  }
  if (JS_IsNull(value) || JS_IsUndefined(value)) {
    *result = nullptr;
    return true;
  }
  if (!JS_IsString(value)) {
    return false;
  }
  std::string selectorName = quickJSValueToUtf8(runtime.context(), value);
  *result = sel_registerName(selectorName.c_str());
  return true;
}

template <typename T>
bool writeQuickJSNumber(JSContext* context, JSValueConst value, void* target) {
  double converted = 0;
  if (!quickJSNumberValue(context, value, &converted)) {
    return false;
  }
  *static_cast<T*>(target) = static_cast<T>(converted);
  return true;
}

bool prepareQuickJSEngineArgument(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, JSValueConst value,
    NativeApiArgumentFrame& frame, size_t index) {
  ffi_type* ffiType = ffiTypeForEngineArgument(type);
  size_t size =
      ffiType != nullptr && ffiType->size > 0 ? ffiType->size : nativeSizeForType(type);
  void* target = frame.storageAt(index, size);
  JSContext* context = runtime.context();

  switch (type.kind) {
    case metagen::mdTypeBool:
      if (!JS_IsBool(value)) {
        return false;
      }
      *static_cast<uint8_t*>(target) = JS_ToBool(context, value) != 0 ? 1 : 0;
      return true;
    case metagen::mdTypeChar:
      return writeQuickJSNumber<int8_t>(context, value, target);
    case metagen::mdTypeUChar:
    case metagen::mdTypeUInt8:
      return writeQuickJSNumber<uint8_t>(context, value, target);
    case metagen::mdTypeSShort:
      return writeQuickJSNumber<int16_t>(context, value, target);
    case metagen::mdTypeUShort:
    case metagen::mdTypeUnichar:
      if (JS_IsString(value)) {
        std::string text = quickJSValueToUtf8(context, value);
        if (text.size() != 1) {
          return false;
        }
        *static_cast<uint16_t*>(target) =
            static_cast<uint16_t>(static_cast<unsigned char>(text[0]));
        return true;
      }
      return writeQuickJSNumber<uint16_t>(context, value, target);
    case metagen::mdTypeSInt: {
      int32_t converted = 0;
      if (JS_ToInt32(context, &converted, value) < 0) {
        return false;
      }
      *static_cast<int32_t*>(target) = converted;
      return true;
    }
    case metagen::mdTypeUInt: {
      uint32_t converted = 0;
      if (JS_ToUint32(context, &converted, value) < 0) {
        return false;
      }
      *static_cast<uint32_t*>(target) = converted;
      return true;
    }
    case metagen::mdTypeSLong:
    case metagen::mdTypeSInt64: {
      int64_t converted = 0;
      if (JS_ToInt64Ext(context, &converted, value) < 0) {
        return false;
      }
      *static_cast<int64_t*>(target) = converted;
      return true;
    }
    case metagen::mdTypeULong:
    case metagen::mdTypeUInt64: {
      uint64_t converted = 0;
      if (JS_IsBigInt(value)) {
        if (JS_ToBigUint64(context, &converted, value) < 0) {
          return false;
        }
      } else {
        int64_t signedValue = 0;
        if (JS_ToInt64Ext(context, &signedValue, value) < 0) {
          return false;
        }
        converted = static_cast<uint64_t>(signedValue);
      }
      *static_cast<uint64_t*>(target) = converted;
      return true;
    }
    case metagen::mdTypeFloat:
      return writeQuickJSNumber<float>(context, value, target);
    case metagen::mdTypeDouble:
      return writeQuickJSNumber<double>(context, value, target);
    case metagen::mdTypeSelector:
      return readQuickJSEngineSelectorArgument(runtime, value,
                                               static_cast<SEL*>(target));
    case metagen::mdTypeClass: {
      Class cls = quickJSNativeClassArgument(runtime, value);
      if (cls == Nil) {
        return false;
      }
      *static_cast<Class*>(target) = cls;
      return true;
    }
    case metagen::mdTypeAnyObject:
    case metagen::mdTypeProtocolObject:
    case metagen::mdTypeClassObject:
    case metagen::mdTypeInstanceObject:
    case metagen::mdTypeNSStringObject:
    case metagen::mdTypeNSMutableStringObject:
      *static_cast<id*>(target) =
          quickJSNativeObjectArgument(runtime, bridge, type, value, frame);
      return true;
    default:
      break;
  }

  Value wrapped = Value::borrowed(runtime, value);
  convertEngineFfiArgument(runtime, bridge, type, wrapped, target, frame);
  return true;
}

JSValue quickJSInteger64Value(Runtime& runtime, int64_t value) {
  constexpr int64_t maxSafeInteger = 9007199254740991LL;
  constexpr int64_t minSafeInteger = -9007199254740991LL;
  if (value >= minSafeInteger && value <= maxSafeInteger) {
    return JS_NewFloat64(runtime.context(), static_cast<double>(value));
  }
  return JS_NewBigInt64(runtime.context(), value);
}

JSValue quickJSUnsignedInteger64Value(Runtime& runtime, uint64_t value) {
  constexpr uint64_t maxSafeInteger = 9007199254740991ULL;
  if (value <= maxSafeInteger) {
    return JS_NewFloat64(runtime.context(), static_cast<double>(value));
  }
  return JS_NewBigUint64(runtime.context(), value);
}

JSValue setQuickJSEngineObjectReturn(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, id object) {
  JSContext* context = runtime.context();
  if (object == nil) {
    return JS_NULL;
  }
  Value roundTrip =
      findCachedNativeObjectReturn(runtime, bridge, type, object);
  if (!roundTrip.isUndefined()) {
    JSValue result = roundTrip.local(runtime);
    if (type.returnOwned) {
      [object release];
    }
    return result;
  }
  if (nativeObjectReturnMayCoerceToString(type) &&
      nativeObjectIsStringLike(object)) {
    std::string utf8 = utf8StringFromNSString(static_cast<NSString*>(object));
    if (type.returnOwned) {
      [object release];
    }
    return JS_NewStringLen(context, utf8.data(), utf8.size());
  }
  if ([object isKindOfClass:[NSNull class]]) {
    if (type.returnOwned) {
      [object release];
    }
    return JS_NULL;
  }
  if ([object isKindOfClass:[NSNumber class]] &&
      ![object isKindOfClass:[NSDecimalNumber class]]) {
    NSNumber* number = static_cast<NSNumber*>(object);
    const char* objCType = [number objCType];
    bool isBool = CFGetTypeID((__bridge CFTypeRef)number) ==
                      CFBooleanGetTypeID() ||
                  (objCType != nullptr &&
                   std::strcmp(objCType, @encode(BOOL)) == 0);
    JSValue result = isBool ? JS_NewBool(context, [number boolValue])
                            : JS_NewFloat64(context, [number doubleValue]);
    if (type.returnOwned) {
      [object release];
    }
    return result;
  }

  if (const NativeApiSymbol* classSymbol =
          bridge->findClassForRuntimePointer((void*)object)) {
    Value result = makeNativeClassValue(runtime, bridge, *classSymbol);
    if (type.returnOwned) {
      [object release];
    }
    return result.local(runtime);
  }
  if (const NativeApiSymbol* protocolSymbol =
          bridge->findProtocolForRuntimePointer((void*)object)) {
    Value result = makeNativeProtocolValue(runtime, bridge, *protocolSymbol);
    if (type.returnOwned) {
      [object release];
    }
    return result.local(runtime);
  }
  Value result = makeNativeObjectValue(runtime, bridge, object, type.returnOwned);
  return result.local(runtime);
}

JSValue setQuickJSEngineReturnValue(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    NativeApiType type, void* value, const std::string& selectorName) {
  JSContext* context = runtime.context();
  switch (type.kind) {
    case metagen::mdTypeVoid:
      return JS_UNDEFINED;
    case metagen::mdTypeBool:
      return JS_NewBool(context, *static_cast<uint8_t*>(value) != 0);
    case metagen::mdTypeChar:
      return JS_NewInt32(context, *static_cast<int8_t*>(value));
    case metagen::mdTypeUChar:
    case metagen::mdTypeUInt8:
      return JS_NewUint32(context, *static_cast<uint8_t*>(value));
    case metagen::mdTypeSShort:
      return JS_NewInt32(context, *static_cast<int16_t*>(value));
    case metagen::mdTypeUShort:
      return JS_NewUint32(context, *static_cast<uint16_t*>(value));
    case metagen::mdTypeUnichar: {
      const char16_t unit = *static_cast<char16_t*>(value);
      // UTF-8 encode one UTF-16 code unit (1-3 bytes; unpaired surrogates
      // fall back to U+FFFD).
      char buffer[4] = {0};
      size_t length = 0;
      if (unit < 0x80) {
        buffer[length++] = static_cast<char>(unit);
      } else if (unit < 0x800) {
        buffer[length++] = static_cast<char>(0xC0 | (unit >> 6));
        buffer[length++] = static_cast<char>(0x80 | (unit & 0x3F));
      } else if (unit >= 0xD800 && unit <= 0xDFFF) {
        buffer[length++] = static_cast<char>(0xEF);
        buffer[length++] = static_cast<char>(0xBF);
        buffer[length++] = static_cast<char>(0xBD);
      } else {
        buffer[length++] = static_cast<char>(0xE0 | (unit >> 12));
        buffer[length++] = static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
        buffer[length++] = static_cast<char>(0x80 | (unit & 0x3F));
      }
      return JS_NewStringLen(context, buffer, length);
    }
    case metagen::mdTypeSInt:
      return JS_NewInt32(context, *static_cast<int32_t*>(value));
    case metagen::mdTypeUInt:
      return JS_NewUint32(context, *static_cast<uint32_t*>(value));
    case metagen::mdTypeSLong:
    case metagen::mdTypeSInt64:
      return quickJSInteger64Value(runtime, *static_cast<int64_t*>(value));
    case metagen::mdTypeULong:
    case metagen::mdTypeUInt64:
      return quickJSUnsignedInteger64Value(runtime,
                                           *static_cast<uint64_t*>(value));
    case metagen::mdTypeFloat:
      return JS_NewFloat64(context, *static_cast<float*>(value));
    case metagen::mdTypeDouble:
      return JS_NewFloat64(context, *static_cast<double*>(value));
    case metagen::mdTypeClass: {
      Class cls = *static_cast<Class*>(value);
      if (cls == nil) {
        return JS_NULL;
      }
      const char* name = class_getName(cls);
      NativeApiSymbol symbol{
          .kind = NativeApiSymbolKind::Class,
          .offset = MD_SECTION_OFFSET_NULL,
          .name = name != nullptr ? name : "",
          .runtimeName = name != nullptr ? name : "",
      };
      if (const NativeApiSymbol* found = bridge->findClass(symbol.name)) {
        symbol = *found;
      }
      Value result = makeNativeClassValue(runtime, bridge, std::move(symbol));
      return result.local(runtime);
    }
    case metagen::mdTypeAnyObject:
    case metagen::mdTypeProtocolObject:
    case metagen::mdTypeClassObject:
    case metagen::mdTypeInstanceObject:
    case metagen::mdTypeNSStringObject:
    case metagen::mdTypeNSMutableStringObject:
      if ((selectorName == "valueForKey:" ||
           selectorName == "valueForKeyPath:") &&
          isObjectiveCObjectType(type)) {
        type.kind = metagen::mdTypeAnyObject;
      }
      return setQuickJSEngineObjectReturn(runtime, bridge, type,
                                          *static_cast<id*>(value));
    case metagen::mdTypeSelector: {
      SEL selector = *static_cast<SEL*>(value);
      const char* selectorNameValue =
          selector != nullptr ? sel_getName(selector) : nullptr;
      if (selectorNameValue == nullptr) {
        return JS_NULL;
      }
      return JS_NewString(context, selectorNameValue);
    }
    default:
      break;
  }
  Value result = convertNativeReturnValue(runtime, bridge, type, value);
  return result.local(runtime);
}
