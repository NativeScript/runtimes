// Included by NativeApiJSCSelectorGroups.mm inside the NativeScript anonymous namespace.

std::string jscValueToUtf8(Runtime& runtime, JSValueRef value) {
  JSValueRef exception = nullptr;
  JSStringRef string = JSValueToStringCopy(runtime.context(), value, &exception);
  if (string == nullptr || exception != nullptr) {
    if (string != nullptr) {
      JSStringRelease(string);
    }
    return {};
  }
  std::string result = engine::jscengine::stringToUtf8(string);
  JSStringRelease(string);
  return result;
}

bool jscNumberValue(Runtime& runtime, JSValueRef value, double* result) {
  if (result == nullptr) {
    return false;
  }
  JSValueRef exception = nullptr;
  double converted = JSValueToNumber(runtime.context(), value, &exception);
  if (exception != nullptr) {
    return false;
  }
  *result = converted;
  return true;
}

template <typename T>
std::shared_ptr<T> jscHostObject(Runtime& runtime, JSValueRef value) {
  if (value == nullptr || !JSValueIsObject(runtime.context(), value)) {
    return nullptr;
  }
  JSValueRef exception = nullptr;
  JSObjectRef object = JSValueToObject(runtime.context(), value, &exception);
  if (exception != nullptr || object == nullptr) {
    return nullptr;
  }
  auto* holder = static_cast<engine::jscengine::HostObjectHolder*>(
      JSObjectGetPrivate(object));
  if (holder == nullptr ||
      holder->typeToken != engine::jscengine::hostObjectTypeToken<T>()) {
    return nullptr;
  }
  return std::static_pointer_cast<T>(holder->hostObject);
}

template <typename T>
T* jscHostObjectRaw(Runtime& runtime, JSValueRef value) {
  if (value == nullptr || !JSValueIsObject(runtime.context(), value)) {
    return nullptr;
  }
  JSValueRef exception = nullptr;
  JSObjectRef object = JSValueToObject(runtime.context(), value, &exception);
  if (exception != nullptr || object == nullptr) {
    return nullptr;
  }
  auto* holder = static_cast<engine::jscengine::HostObjectHolder*>(
      JSObjectGetPrivate(object));
  if (holder == nullptr ||
      holder->typeToken != engine::jscengine::hostObjectTypeToken<T>()) {
    return nullptr;
  }
  return static_cast<T*>(holder->hostObject.get());
}

id jscNativeObjectArgument(Runtime& runtime,
                           const std::shared_ptr<NativeApiBridge>& bridge,
                           const NativeApiType& type, JSValueRef value,
                           NativeApiArgumentFrame& frame) {
  if (value == nullptr || JSValueIsNull(runtime.context(), value) ||
      JSValueIsUndefined(runtime.context(), value)) {
    return nil;
  }
  if (JSValueIsString(runtime.context(), value)) {
    std::string utf8 = jscValueToUtf8(runtime, value);
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
  if (JSValueIsBoolean(runtime.context(), value)) {
    return [NSNumber numberWithBool:JSValueToBoolean(runtime.context(), value)];
  }
  if (JSValueIsNumber(runtime.context(), value)) {
    double converted = 0;
    if (jscNumberValue(runtime, value, &converted)) {
      return [NSNumber numberWithDouble:converted];
    }
  }
  if (!JSValueIsObject(runtime.context(), value)) {
    return nil;
  }
  if (auto objectHost = jscHostObject<NativeApiObjectHostObject>(runtime, value)) {
    return objectHost->object();
  }
  if (auto classHost = jscHostObject<NativeApiClassHostObject>(runtime, value)) {
    return static_cast<id>(classHost->nativeClass());
  }
  if (auto protocolHost =
          jscHostObject<NativeApiProtocolHostObject>(runtime, value)) {
    return static_cast<id>(protocolHost->nativeProtocol());
  }
  if (auto pointerHost =
          jscHostObject<NativeApiPointerHostObject>(runtime, value)) {
    return static_cast<id>(pointerHost->pointer());
  }
  if (auto referenceHost =
          jscHostObject<NativeApiReferenceHostObject>(runtime, value)) {
    return static_cast<id>(referenceHost->data());
  }
  if (auto structHost =
          jscHostObject<NativeApiStructObjectHostObject>(runtime, value)) {
    return static_cast<id>(structHost->data());
  }

  JSValueRef exception = nullptr;
  JSObjectRef object = JSValueToObject(runtime.context(), value, &exception);
  if (exception == nullptr && object != nullptr) {
    JSStringRef property = engine::jscengine::makeJSString("__nativeApiClass");
    JSValueRef wrappedClassValue =
        JSObjectGetProperty(runtime.context(), object, property, nullptr);
    JSStringRelease(property);
    if (auto classHost =
            jscHostObject<NativeApiClassHostObject>(runtime,
                                                    wrappedClassValue)) {
      return static_cast<id>(classHost->nativeClass());
    }
  }

  Value wrapped = Value::borrowed(runtime, value);
  return objectFromEngineValue(runtime, bridge, wrapped, frame,
                               type.kind ==
                                   metagen::mdTypeNSMutableStringObject);
}

Class jscNativeClassArgument(Runtime& runtime, JSValueRef value) {
  if (value == nullptr || JSValueIsNull(runtime.context(), value) ||
      JSValueIsUndefined(runtime.context(), value)) {
    return Nil;
  }
  if (auto classHost = jscHostObject<NativeApiClassHostObject>(runtime, value)) {
    return classHost->nativeClass();
  }
  if (JSValueIsObject(runtime.context(), value)) {
    JSValueRef exception = nullptr;
    JSObjectRef object = JSValueToObject(runtime.context(), value, &exception);
    if (exception == nullptr && object != nullptr) {
      JSStringRef property = engine::jscengine::makeJSString("__nativeApiClass");
      JSValueRef wrappedClassValue =
          JSObjectGetProperty(runtime.context(), object, property, nullptr);
      JSStringRelease(property);
      if (auto classHost =
              jscHostObject<NativeApiClassHostObject>(runtime,
                                                      wrappedClassValue)) {
        return classHost->nativeClass();
      }
    }
  }
  Value wrapped = Value::borrowed(runtime, value);
  return classFromEngineValue(runtime, wrapped);
}

bool readJSCEngineSelectorArgument(Runtime& runtime, JSValueRef value,
                                   SEL* result) {
  if (result == nullptr) {
    return false;
  }
  if (value == nullptr || JSValueIsNull(runtime.context(), value) ||
      JSValueIsUndefined(runtime.context(), value)) {
    *result = nullptr;
    return true;
  }
  if (!JSValueIsString(runtime.context(), value)) {
    return false;
  }
  std::string selectorName = jscValueToUtf8(runtime, value);
  *result = sel_registerName(selectorName.c_str());
  return true;
}

template <typename T>
bool writeJSCNumber(Runtime& runtime, JSValueRef value, void* target) {
  double converted = 0;
  if (!jscNumberValue(runtime, value, &converted)) {
    return false;
  }
  *static_cast<T*>(target) = static_cast<T>(converted);
  return true;
}

bool prepareJSCEngineArgument(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, JSValueRef value,
    NativeApiArgumentFrame& frame, size_t index) {
  ffi_type* ffiType = ffiTypeForEngineArgument(type);
  size_t size =
      ffiType != nullptr && ffiType->size > 0 ? ffiType->size : nativeSizeForType(type);
  void* target = frame.storageAt(index, size);

  switch (type.kind) {
    case metagen::mdTypeBool:
      if (!JSValueIsBoolean(runtime.context(), value)) {
        return false;
      }
      *static_cast<uint8_t*>(target) =
          JSValueToBoolean(runtime.context(), value) ? 1 : 0;
      return true;
    case metagen::mdTypeChar:
      return writeJSCNumber<int8_t>(runtime, value, target);
    case metagen::mdTypeUChar:
    case metagen::mdTypeUInt8:
      return writeJSCNumber<uint8_t>(runtime, value, target);
    case metagen::mdTypeSShort:
      return writeJSCNumber<int16_t>(runtime, value, target);
    case metagen::mdTypeUShort:
    case metagen::mdTypeUnichar:
      if (JSValueIsString(runtime.context(), value)) {
        std::string text = jscValueToUtf8(runtime, value);
        if (text.size() != 1) {
          return false;
        }
        *static_cast<uint16_t*>(target) =
            static_cast<uint16_t>(static_cast<unsigned char>(text[0]));
        return true;
      }
      return writeJSCNumber<uint16_t>(runtime, value, target);
    case metagen::mdTypeSInt:
      return writeJSCNumber<int32_t>(runtime, value, target);
    case metagen::mdTypeUInt:
      return writeJSCNumber<uint32_t>(runtime, value, target);
    case metagen::mdTypeSLong:
    case metagen::mdTypeSInt64:
      return writeJSCNumber<int64_t>(runtime, value, target);
    case metagen::mdTypeULong:
    case metagen::mdTypeUInt64:
      return writeJSCNumber<uint64_t>(runtime, value, target);
    case metagen::mdTypeFloat:
      return writeJSCNumber<float>(runtime, value, target);
    case metagen::mdTypeDouble:
      return writeJSCNumber<double>(runtime, value, target);
    case metagen::mdTypeSelector:
      return readJSCEngineSelectorArgument(runtime, value,
                                           static_cast<SEL*>(target));
    case metagen::mdTypeClass: {
      Class cls = jscNativeClassArgument(runtime, value);
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
          jscNativeObjectArgument(runtime, bridge, type, value, frame);
      return true;
    default:
      break;
  }

  Value wrapped = Value::borrowed(runtime, value);
  convertEngineFfiArgument(runtime, bridge, type, wrapped, target, frame);
  return true;
}

JSValueRef jscInteger64Value(Runtime& runtime, int64_t value) {
  constexpr int64_t maxSafeInteger = 9007199254740991LL;
  constexpr int64_t minSafeInteger = -9007199254740991LL;
  if (value >= minSafeInteger && value <= maxSafeInteger) {
    return JSValueMakeNumber(runtime.context(), static_cast<double>(value));
  }
  Value bigint = BigInt::fromInt64(runtime, value);
  return bigint.local(runtime);
}

JSValueRef jscUnsignedInteger64Value(Runtime& runtime, uint64_t value) {
  constexpr uint64_t maxSafeInteger = 9007199254740991ULL;
  if (value <= maxSafeInteger) {
    return JSValueMakeNumber(runtime.context(), static_cast<double>(value));
  }
  Value bigint = BigInt::fromUint64(runtime, value);
  return bigint.local(runtime);
}

JSValueRef setJSCEngineObjectReturn(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, id object) {
  if (object == nil) {
    return JSValueMakeNull(runtime.context());
  }
  Value roundTrip =
      findCachedNativeObjectReturn(runtime, bridge, type, object);
  if (!roundTrip.isUndefined()) {
    JSValueRef result = roundTrip.local(runtime);
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
    JSStringRef string = engine::jscengine::makeJSString(utf8);
    JSValueRef result = JSValueMakeString(runtime.context(), string);
    JSStringRelease(string);
    return result;
  }
  if ([object isKindOfClass:[NSNull class]]) {
    if (type.returnOwned) {
      [object release];
    }
    return JSValueMakeNull(runtime.context());
  }
  if ([object isKindOfClass:[NSNumber class]] &&
      ![object isKindOfClass:[NSDecimalNumber class]]) {
    NSNumber* number = static_cast<NSNumber*>(object);
    const char* objCType = [number objCType];
    bool isBool = CFGetTypeID((__bridge CFTypeRef)number) ==
                      CFBooleanGetTypeID() ||
                  (objCType != nullptr &&
                   std::strcmp(objCType, @encode(BOOL)) == 0);
    JSValueRef result =
        isBool ? JSValueMakeBoolean(runtime.context(), [number boolValue])
               : JSValueMakeNumber(runtime.context(), [number doubleValue]);
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

JSValueRef setJSCEngineReturnValue(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    NativeApiType type, void* value, const std::string& selectorName) {
  switch (type.kind) {
    case metagen::mdTypeVoid:
      return JSValueMakeUndefined(runtime.context());
    case metagen::mdTypeBool:
      return JSValueMakeBoolean(runtime.context(),
                                *static_cast<uint8_t*>(value) != 0);
    case metagen::mdTypeChar:
      return JSValueMakeNumber(runtime.context(),
                               *static_cast<int8_t*>(value));
    case metagen::mdTypeUChar:
    case metagen::mdTypeUInt8:
      return JSValueMakeNumber(runtime.context(),
                               *static_cast<uint8_t*>(value));
    case metagen::mdTypeSShort:
      return JSValueMakeNumber(runtime.context(),
                               *static_cast<int16_t*>(value));
    case metagen::mdTypeUShort:
      return JSValueMakeNumber(runtime.context(), *static_cast<uint16_t*>(value));
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
      JSStringRef string = engine::jscengine::makeJSString(std::string(buffer, length));
      JSValueRef result = JSValueMakeString(runtime.context(), string);
      JSStringRelease(string);
      return result;
    }
    case metagen::mdTypeSInt:
      return JSValueMakeNumber(runtime.context(),
                               *static_cast<int32_t*>(value));
    case metagen::mdTypeUInt:
      return JSValueMakeNumber(runtime.context(),
                               *static_cast<uint32_t*>(value));
    case metagen::mdTypeSLong:
    case metagen::mdTypeSInt64:
      return jscInteger64Value(runtime, *static_cast<int64_t*>(value));
    case metagen::mdTypeULong:
    case metagen::mdTypeUInt64:
      return jscUnsignedInteger64Value(runtime,
                                       *static_cast<uint64_t*>(value));
    case metagen::mdTypeFloat:
      return JSValueMakeNumber(runtime.context(), *static_cast<float*>(value));
    case metagen::mdTypeDouble:
      return JSValueMakeNumber(runtime.context(), *static_cast<double*>(value));
    case metagen::mdTypeClass: {
      Class cls = *static_cast<Class*>(value);
      if (cls == nil) {
        return JSValueMakeNull(runtime.context());
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
      return setJSCEngineObjectReturn(runtime, bridge, type,
                                      *static_cast<id*>(value));
    case metagen::mdTypeSelector: {
      SEL selector = *static_cast<SEL*>(value);
      const char* selectorNameValue =
          selector != nullptr ? sel_getName(selector) : nullptr;
      if (selectorNameValue == nullptr) {
        return JSValueMakeNull(runtime.context());
      }
      JSStringRef string = engine::jscengine::makeJSString(selectorNameValue);
      JSValueRef result = JSValueMakeString(runtime.context(), string);
      JSStringRelease(string);
      return result;
    }
    default:
      break;
  }
  Value result = convertNativeReturnValue(runtime, bridge, type, value);
  return result.local(runtime);
}
