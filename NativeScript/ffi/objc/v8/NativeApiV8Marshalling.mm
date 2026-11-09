// Included by NativeApiV8SelectorGroups.mm inside the NativeScript anonymous namespace.

std::string v8StringToUtf8(v8::Isolate* isolate,
                           v8::Local<v8::Value> value) {
  v8::String::Utf8Value utf8(isolate, value);
  return *utf8 != nullptr ? std::string(*utf8, utf8.length()) : std::string();
}

template <typename T>
std::shared_ptr<T> v8HostObject(Runtime& runtime, v8::Local<v8::Value> value) {
  if (value.IsEmpty() || !value->IsObject()) {
    return nullptr;
  }
  v8::Local<v8::Object> object = value.As<v8::Object>();
  if (object->InternalFieldCount() < 1) {
    return nullptr;
  }
  auto* holder = static_cast<engine::v8engine::HostObjectHolder*>(
      object->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
  if (holder == nullptr ||
      holder->typeToken != engine::v8engine::hostObjectTypeToken<T>()) {
    return nullptr;
  }
  return std::static_pointer_cast<T>(holder->hostObject);
}

// Fast version that returns raw pointer (no atomic ref count).
// Only safe when the caller guarantees the object stays alive.
template <typename T>
T* v8HostObjectRaw(v8::Local<v8::Value> value) {
  if (value.IsEmpty() || !value->IsObject()) {
    return nullptr;
  }
  v8::Local<v8::Object> object = value.As<v8::Object>();
  if (object->InternalFieldCount() < 1) {
    return nullptr;
  }
  auto* holder = static_cast<engine::v8engine::HostObjectHolder*>(
      object->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
  if (holder == nullptr ||
      holder->typeToken != engine::v8engine::hostObjectTypeToken<T>()) {
    return nullptr;
  }
  return static_cast<T*>(holder->hostObject.get());
}

id v8NativeObjectArgument(Runtime& runtime,
                          const std::shared_ptr<NativeApiBridge>& bridge,
                          const NativeApiType& type,
                          v8::Local<v8::Value> value,
                          NativeApiArgumentFrame& frame) {
  v8::Isolate* isolate = runtime.isolate();
  if (value.IsEmpty() || value->IsNullOrUndefined()) {
    return nil;
  }
  if (value->IsString()) {
    std::string utf8 = v8StringToUtf8(isolate, value);
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
  if (value->IsBoolean()) {
    return [NSNumber numberWithBool:value->BooleanValue(isolate)];
  }
  if (value->IsNumber()) {
    return [NSNumber numberWithDouble:value->NumberValue(runtime.context())
                                          .FromMaybe(0)];
  }
  if (!value->IsObject()) {
    return nil;
  }
  if (auto objectHost =
          v8HostObject<NativeApiObjectHostObject>(runtime, value)) {
    return objectHost->object();
  }
  if (auto classHost = v8HostObject<NativeApiClassHostObject>(runtime, value)) {
    return static_cast<id>(classHost->nativeClass());
  }
  if (auto protocolHost =
          v8HostObject<NativeApiProtocolHostObject>(runtime, value)) {
    return static_cast<id>(protocolHost->nativeProtocol());
  }
  if (auto pointerHost =
          v8HostObject<NativeApiPointerHostObject>(runtime, value)) {
    return static_cast<id>(pointerHost->pointer());
  }
  if (auto referenceHost =
          v8HostObject<NativeApiReferenceHostObject>(runtime, value)) {
    return static_cast<id>(referenceHost->data());
  }
  if (auto structHost =
          v8HostObject<NativeApiStructObjectHostObject>(runtime, value)) {
    return static_cast<id>(structHost->data());
  }

  v8::Local<v8::Value> wrappedClassValue;
  if (value.As<v8::Object>()
          ->Get(runtime.context(),
                engine::v8engine::makeV8String(isolate, "__nativeApiClass"))
          .ToLocal(&wrappedClassValue)) {
    if (auto classHost =
            v8HostObject<NativeApiClassHostObject>(runtime, wrappedClassValue)) {
      return static_cast<id>(classHost->nativeClass());
    }
  }

  Value wrapped = Value::borrowed(runtime, value);
  return objectFromEngineValue(runtime, bridge, wrapped, frame,
                               type.kind ==
                                   metagen::mdTypeNSMutableStringObject);
}

Class v8NativeClassArgument(Runtime& runtime, v8::Local<v8::Value> value) {
  if (value.IsEmpty() || value->IsNullOrUndefined()) {
    return Nil;
  }
  auto* state = runtime.rawState();
  if (state != nullptr && value->IsObject()) {
    // The cache holds the Class opaquely so the engine layer stays platform
    // neutral; cast back on the way out.
    if (state->nativeClassArgumentLast.nativeClass != nullptr &&
        !state->nativeClassArgumentLast.value.IsEmpty() &&
        state->nativeClassArgumentLast.value.Get(runtime.isolate()) == value) {
      return (Class)state->nativeClassArgumentLast.nativeClass;
    }
    for (auto& entry : state->nativeClassArgumentCache) {
      if (entry.nativeClass != nullptr && !entry.value.IsEmpty() &&
          entry.value.Get(runtime.isolate()) == value) {
        state->nativeClassArgumentLast.value.Reset(runtime.isolate(), value);
        state->nativeClassArgumentLast.nativeClass = entry.nativeClass;
        return (Class)entry.nativeClass;
      }
    }
  }

  Class result = Nil;
  if (auto classHost = v8HostObject<NativeApiClassHostObject>(runtime, value)) {
    result = classHost->nativeClass();
  } else if (value->IsObject()) {
    v8::Local<v8::Value> wrappedClassValue;
    if (value.As<v8::Object>()
            ->Get(runtime.context(),
                  engine::v8engine::makeV8String(runtime.isolate(),
                                                 "__nativeApiClass"))
            .ToLocal(&wrappedClassValue)) {
      if (auto classHost =
              v8HostObject<NativeApiClassHostObject>(runtime,
                                                     wrappedClassValue)) {
        result = classHost->nativeClass();
      }
    }
  }

  if (result == Nil) {
    Value wrapped = Value::borrowed(runtime, value);
    result = classFromEngineValue(runtime, wrapped);
  }

  if (result != Nil && state != nullptr && value->IsObject()) {
    constexpr size_t cacheSize =
        sizeof(state->nativeClassArgumentCache) /
        sizeof(state->nativeClassArgumentCache[0]);
    auto& entry = state->nativeClassArgumentCache[
        state->nativeClassArgumentCacheNext++ % cacheSize];
    entry.value.Reset(runtime.isolate(), value);
    entry.nativeClass = result;
    state->nativeClassArgumentLast.value.Reset(runtime.isolate(), value);
    state->nativeClassArgumentLast.nativeClass = result;
  }
  return result;
}

bool readV8EngineSelectorArgument(Runtime& runtime, v8::Local<v8::Value> value,
                                  SEL* result) {
  if (result == nullptr) {
    return false;
  }
  if (value.IsEmpty() || value->IsNullOrUndefined()) {
    *result = nullptr;
    return true;
  }
  if (!value->IsString()) {
    return false;
  }
  auto* state = runtime.rawState();
  if (state != nullptr) {
    if (state->nativeSelectorArgumentLast.selector != nullptr &&
        !state->nativeSelectorArgumentLast.value.IsEmpty() &&
        state->nativeSelectorArgumentLast.value.Get(runtime.isolate()) ==
            value) {
      *result = (SEL)state->nativeSelectorArgumentLast.selector;
      return true;
    }
    for (auto& entry : state->nativeSelectorArgumentCache) {
      if (entry.selector != nullptr && !entry.value.IsEmpty() &&
          entry.value.Get(runtime.isolate()) == value) {
        *result = (SEL)entry.selector;
        state->nativeSelectorArgumentLast.value.Reset(runtime.isolate(), value);
        state->nativeSelectorArgumentLast.selector = entry.selector;
        return true;
      }
    }
  }

  v8::Isolate* isolate = runtime.isolate();
  v8::Local<v8::String> string = value.As<v8::String>();
  char stackBuffer[128];
  if (string->Utf8LengthV2(isolate) + 1 <= sizeof(stackBuffer)) {
    string->WriteUtf8V2(isolate, stackBuffer, sizeof(stackBuffer),
                        v8::String::WriteFlags::kNullTerminate);
    *result = sel_registerName(stackBuffer);
  } else {
    std::string selectorName = v8StringToUtf8(isolate, value);
    *result = sel_registerName(selectorName.c_str());
  }
  if (*result != nullptr && state != nullptr) {
    constexpr size_t cacheSize =
        sizeof(state->nativeSelectorArgumentCache) /
        sizeof(state->nativeSelectorArgumentCache[0]);
    auto& entry = state->nativeSelectorArgumentCache[
        state->nativeSelectorArgumentCacheNext++ % cacheSize];
    entry.value.Reset(isolate, value);
    entry.selector = *result;
    state->nativeSelectorArgumentLast.value.Reset(isolate, value);
    state->nativeSelectorArgumentLast.selector = *result;
  }
  return true;
}

bool prepareV8EngineArgument(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, v8::Local<v8::Value> value,
    NativeApiArgumentFrame& frame, size_t index) {
  ffi_type* ffiType = ffiTypeForEngineArgument(type);
  size_t size =
      ffiType != nullptr && ffiType->size > 0 ? ffiType->size : nativeSizeForType(type);
  void* target = frame.storageAt(index, size);

  switch (type.kind) {
    case metagen::mdTypeBool:
      if (!value->IsBoolean()) {
        return false;
      }
      *static_cast<uint8_t*>(target) =
          value->BooleanValue(runtime.isolate()) ? 1 : 0;
      return true;
    case metagen::mdTypeChar: {
      int32_t converted = 0;
      if (!value->Int32Value(runtime.context()).To(&converted)) {
        return false;
      }
      *static_cast<int8_t*>(target) = static_cast<int8_t>(converted);
      return true;
    }
    case metagen::mdTypeUChar:
    case metagen::mdTypeUInt8: {
      uint32_t converted = 0;
      if (!value->Uint32Value(runtime.context()).To(&converted)) {
        return false;
      }
      *static_cast<uint8_t*>(target) = static_cast<uint8_t>(converted);
      return true;
    }
    case metagen::mdTypeSShort: {
      int32_t converted = 0;
      if (!value->Int32Value(runtime.context()).To(&converted)) {
        return false;
      }
      *static_cast<int16_t*>(target) = static_cast<int16_t>(converted);
      return true;
    }
    case metagen::mdTypeUShort:
    case metagen::mdTypeUnichar: {
      if (value->IsString()) {
        std::string text = v8StringToUtf8(runtime.isolate(), value);
        if (text.size() != 1) {
          return false;
        }
        *static_cast<uint16_t*>(target) =
            static_cast<uint16_t>(static_cast<unsigned char>(text[0]));
        return true;
      }
      uint32_t converted = 0;
      if (!value->Uint32Value(runtime.context()).To(&converted)) {
        return false;
      }
      *static_cast<uint16_t*>(target) = static_cast<uint16_t>(converted);
      return true;
    }
    case metagen::mdTypeSInt:
      return value->Int32Value(runtime.context()).To(
          static_cast<int32_t*>(target));
    case metagen::mdTypeUInt:
      return value->Uint32Value(runtime.context()).To(
          static_cast<uint32_t*>(target));
    case metagen::mdTypeSLong:
    case metagen::mdTypeSInt64: {
      if (value->IsBigInt()) {
        bool lossless = false;
        *static_cast<int64_t*>(target) =
            value.As<v8::BigInt>()->Int64Value(&lossless);
        return true;
      }
      return value->IntegerValue(runtime.context()).To(
          static_cast<int64_t*>(target));
    }
    case metagen::mdTypeULong:
    case metagen::mdTypeUInt64: {
      if (value->IsBigInt()) {
        bool lossless = false;
        *static_cast<uint64_t*>(target) =
            value.As<v8::BigInt>()->Uint64Value(&lossless);
        return true;
      }
      int64_t converted = 0;
      if (!value->IntegerValue(runtime.context()).To(&converted)) {
        return false;
      }
      *static_cast<uint64_t*>(target) = static_cast<uint64_t>(converted);
      return true;
    }
    case metagen::mdTypeFloat: {
      double converted = 0;
      if (!value->NumberValue(runtime.context()).To(&converted)) {
        return false;
      }
      *static_cast<float*>(target) = static_cast<float>(converted);
      return true;
    }
    case metagen::mdTypeDouble:
      return value->NumberValue(runtime.context()).To(
          static_cast<double*>(target));
    case metagen::mdTypeSelector:
      return readV8EngineSelectorArgument(runtime, value,
                                          static_cast<SEL*>(target));
    case metagen::mdTypeClass: {
      Class cls = v8NativeClassArgument(runtime, value);
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
          v8NativeObjectArgument(runtime, bridge, type, value, frame);
      return true;
    default:
      break;
  }

  Value wrapped = Value::borrowed(runtime, value);
  convertEngineFfiArgument(runtime, bridge, type, wrapped, target, frame);
  return true;
}

v8::Local<v8::Value> v8Integer64Value(v8::Isolate* isolate, int64_t value) {
  constexpr int64_t maxSafeInteger = 9007199254740991LL;
  constexpr int64_t minSafeInteger = -9007199254740991LL;
  if (value >= minSafeInteger && value <= maxSafeInteger) {
    return v8::Number::New(isolate, static_cast<double>(value));
  }
  return v8::BigInt::New(isolate, value);
}

v8::Local<v8::Value> v8UnsignedInteger64Value(v8::Isolate* isolate,
                                              uint64_t value) {
  constexpr uint64_t maxSafeInteger = 9007199254740991ULL;
  if (value <= maxSafeInteger) {
    return v8::Number::New(isolate, static_cast<double>(value));
  }
  return v8::BigInt::NewFromUnsigned(isolate, value);
}

bool setV8EngineObjectReturn(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    const NativeApiType& type, id object,
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = runtime.isolate();
  if (object == nil) {
    info.GetReturnValue().Set(v8::Null(isolate));
    return true;
  }
  Value roundTrip =
      findCachedNativeObjectReturn(runtime, bridge, type, object);
  if (!roundTrip.isUndefined()) {
    info.GetReturnValue().Set(roundTrip.local(runtime));
    if (type.returnOwned) {
      [object release];
    }
    return true;
  }
  if (nativeObjectReturnMayCoerceToString(type) &&
      nativeObjectIsStringLike(object)) {
    std::string utf8 = utf8StringFromNSString(static_cast<NSString*>(object));
    if (type.returnOwned) {
      [object release];
    }
    info.GetReturnValue().Set(engine::v8engine::makeV8String(isolate, utf8));
    return true;
  }
  if ([object isKindOfClass:[NSNull class]]) {
    if (type.returnOwned) {
      [object release];
    }
    info.GetReturnValue().Set(v8::Null(isolate));
    return true;
  }
  if ([object isKindOfClass:[NSNumber class]] &&
      ![object isKindOfClass:[NSDecimalNumber class]]) {
    NSNumber* number = static_cast<NSNumber*>(object);
    const char* objCType = [number objCType];
    bool isBool = CFGetTypeID((__bridge CFTypeRef)number) ==
                      CFBooleanGetTypeID() ||
                  (objCType != nullptr &&
                   std::strcmp(objCType, @encode(BOOL)) == 0);
    if (isBool) {
      info.GetReturnValue().Set(v8::Boolean::New(isolate, [number boolValue]));
    } else {
      info.GetReturnValue().Set(v8::Number::New(isolate, [number doubleValue]));
    }
    if (type.returnOwned) {
      [object release];
    }
    return true;
  }

  if (const NativeApiSymbol* classSymbol =
          bridge->findClassForRuntimePointer((void*)object)) {
    Value result = makeNativeClassValue(runtime, bridge, *classSymbol);
    info.GetReturnValue().Set(result.local(runtime));
    if (type.returnOwned) {
      [object release];
    }
    return true;
  }
  if (const NativeApiSymbol* protocolSymbol =
          bridge->findProtocolForRuntimePointer((void*)object)) {
    Value result = makeNativeProtocolValue(runtime, bridge, *protocolSymbol);
    info.GetReturnValue().Set(result.local(runtime));
    if (type.returnOwned) {
      [object release];
    }
    return true;
  }
  Value result = makeNativeObjectValue(runtime, bridge, object, type.returnOwned);
  info.GetReturnValue().Set(result.local(runtime));
  return true;
}

bool setV8EngineReturnValue(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    NativeApiType type, void* value, const std::string& selectorName,
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = runtime.isolate();
  switch (type.kind) {
    case metagen::mdTypeVoid:
      info.GetReturnValue().Set(v8::Undefined(isolate));
      return true;
    case metagen::mdTypeBool:
      info.GetReturnValue().Set(
          v8::Boolean::New(isolate, *static_cast<uint8_t*>(value) != 0));
      return true;
    case metagen::mdTypeChar:
      info.GetReturnValue().Set(
          v8::Integer::New(isolate, *static_cast<int8_t*>(value)));
      return true;
    case metagen::mdTypeUChar:
    case metagen::mdTypeUInt8:
      info.GetReturnValue().Set(v8::Integer::NewFromUnsigned(
          isolate, *static_cast<uint8_t*>(value)));
      return true;
    case metagen::mdTypeSShort:
      info.GetReturnValue().Set(
          v8::Integer::New(isolate, *static_cast<int16_t*>(value)));
      return true;
    case metagen::mdTypeUShort:
      info.GetReturnValue().Set(
          v8::Integer::NewFromUnsigned(isolate, *static_cast<uint16_t*>(value)));
      return true;
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
      info.GetReturnValue().Set(
          engine::v8engine::makeV8String(isolate, std::string(buffer, length)));
      return true;
    }
    case metagen::mdTypeSInt:
      info.GetReturnValue().Set(
          v8::Integer::New(isolate, *static_cast<int32_t*>(value)));
      return true;
    case metagen::mdTypeUInt:
      info.GetReturnValue().Set(v8::Integer::NewFromUnsigned(
          isolate, *static_cast<uint32_t*>(value)));
      return true;
    case metagen::mdTypeSLong:
    case metagen::mdTypeSInt64:
      info.GetReturnValue().Set(
          v8Integer64Value(isolate, *static_cast<int64_t*>(value)));
      return true;
    case metagen::mdTypeULong:
    case metagen::mdTypeUInt64:
      info.GetReturnValue().Set(
          v8UnsignedInteger64Value(isolate, *static_cast<uint64_t*>(value)));
      return true;
    case metagen::mdTypeFloat:
      info.GetReturnValue().Set(
          v8::Number::New(isolate, *static_cast<float*>(value)));
      return true;
    case metagen::mdTypeDouble:
      info.GetReturnValue().Set(
          v8::Number::New(isolate, *static_cast<double*>(value)));
      return true;
    case metagen::mdTypeClass: {
      Class cls = *static_cast<Class*>(value);
      if (cls == nil) {
        info.GetReturnValue().Set(v8::Null(isolate));
        return true;
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
      info.GetReturnValue().Set(result.local(runtime));
      return true;
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
      return setV8EngineObjectReturn(runtime, bridge, type,
                                     *static_cast<id*>(value), info);
    case metagen::mdTypeSelector: {
      SEL selector = *static_cast<SEL*>(value);
      const char* selectorNameValue =
          selector != nullptr ? sel_getName(selector) : nullptr;
      if (selectorNameValue == nullptr) {
        info.GetReturnValue().Set(v8::Null(isolate));
      } else {
        info.GetReturnValue().Set(
            engine::v8engine::makeV8String(isolate, selectorNameValue));
      }
      return true;
    }
    default:
      break;
  }
  Value result = convertNativeReturnValue(runtime, bridge, type, value);
  info.GetReturnValue().Set(result.local(runtime));
  return true;
}
