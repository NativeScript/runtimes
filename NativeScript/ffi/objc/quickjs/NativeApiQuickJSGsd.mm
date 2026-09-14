// --- GSD (Generated Signature Dispatch) for QuickJS ---
// GsdObjCContext is the engine-neutral interface the generated invokers use:
// it reads JS arguments and writes the JS return value via the QuickJS API.
// Readers require an actual JS number so coercion edge cases defer to the
// fully correct generic path.
struct GsdObjCContext;
using ObjCGsdInvoker = bool (*)(GsdObjCContext&);
struct ObjCGsdDispatchEntry {
  uint64_t dispatchId;
  ObjCGsdInvoker invoker;
};

struct GsdObjCContext {
  Runtime& runtime;
  const std::shared_ptr<NativeApiBridge>& bridge;
  id self;
  SEL selector;
  JSContext* context;
  JSValueConst* arguments;
  const NativeApiType& returnType;
  JSValue result = JS_UNDEFINED;
  const Value* valueArguments = nullptr;
  bool materializeValueResult = false;
  Value valueResult = Value::undefined();

  template <typename Invocation>
  void invokeNative(Invocation&& invocation) {
    performGeneratedObjCInvocation(runtime, bridge, [&]() { invocation(); });
  }

  bool readNumber(size_t i, double* out) {
    if (valueArguments != nullptr) {
      const Value& v = valueArguments[i];
      if (!v.isNumber()) return false;
      *out = v.getNumber();
      return true;
    }
    JSValueConst v = arguments[i];
    if (!JS_IsNumber(v)) return false;
    return quickJSNumberValue(context, v, out);
  }
  bool readBool(size_t i, uint8_t* out) {
    if (valueArguments != nullptr) {
      const Value& v = valueArguments[i];
      if (!v.isBool()) return false;
      *out = v.getBool() ? 1 : 0;
      return true;
    }
    JSValueConst v = arguments[i];
    if (!JS_IsBool(v)) return false;
    *out = JS_ToBool(context, v) != 0 ? 1 : 0;
    return true;
  }
  template <class T>
  bool readSigned(size_t i, T* out) {
    double tmp = 0;
    if (!readNumber(i, &tmp)) return false;
    *out = static_cast<T>(tmp);
    return true;
  }
  template <class T>
  bool readUnsigned(size_t i, T* out) {
    double tmp = 0;
    if (!readNumber(i, &tmp)) return false;
    *out = static_cast<T>(tmp);
    return true;
  }
  bool readFloat(size_t i, float* out) {
    double tmp = 0;
    if (!readNumber(i, &tmp)) return false;
    *out = static_cast<float>(tmp);
    return true;
  }
  bool readDouble(size_t i, double* out) { return readNumber(i, out); }
  bool readSelector(size_t i, SEL* out) {
    if (valueArguments != nullptr) {
      return readFastEngineSelectorArgument(runtime, valueArguments[i], out);
    }
    return readQuickJSEngineSelectorArgument(runtime, arguments[i], out);
  }
  bool readClass(size_t i, Class* out) {
    if (valueArguments != nullptr) {
      Class cls = classFromEngineValue(runtime, valueArguments[i]);
      if (cls == Nil) return false;
      *out = cls;
      return true;
    }
    if (auto* c = quickJSHostObjectRaw<NativeApiClassHostObject>(
            runtime, arguments[i])) {
      *out = c->nativeClass();
      return true;
    }
    Class cls = quickJSNativeClassArgument(runtime, arguments[i]);
    if (cls == Nil) return false;
    *out = cls;
    return true;
  }
  bool readObject(size_t i, id* out) {
    if (valueArguments != nullptr) {
      const Value& v = valueArguments[i];
      if (v.isNull() || v.isUndefined()) {
        *out = nil;
        return true;
      }
      if (!v.isObject()) return false;
      Object object = v.asObject(runtime);
      if (object.isHostObject<NativeApiObjectHostObject>(runtime)) {
        *out = object.getHostObject<NativeApiObjectHostObject>(runtime)->object();
        return true;
      }
      if (object.isHostObject<NativeApiClassHostObject>(runtime)) {
        *out = static_cast<id>(
            object.getHostObject<NativeApiClassHostObject>(runtime)->nativeClass());
        return true;
      }
      Class cls = classFromEngineValue(runtime, v);
      if (cls != Nil) {
        *out = static_cast<id>(cls);
        return true;
      }
      if (object.isHostObject<NativeApiProtocolHostObject>(runtime)) {
        *out = static_cast<id>(
            object.getHostObject<NativeApiProtocolHostObject>(runtime)
                ->nativeProtocol());
        return true;
      }
      return false;
    }
    JSValueConst v = arguments[i];
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
      *out = nil;
      return true;
    }
    if (auto* h = quickJSHostObjectRaw<NativeApiObjectHostObject>(runtime, v)) {
      *out = h->object();
      return true;
    }
    if (auto* c = quickJSHostObjectRaw<NativeApiClassHostObject>(runtime, v)) {
      *out = static_cast<id>(c->nativeClass());
      return true;
    }
    if (JS_IsObject(v)) {
      Class cls = quickJSNativeClassArgument(runtime, v);
      if (cls != Nil) {
        *out = static_cast<id>(cls);
        return true;
      }
    }
    if (auto* p =
            quickJSHostObjectRaw<NativeApiProtocolHostObject>(runtime, v)) {
      *out = static_cast<id>(p->nativeProtocol());
      return true;
    }
    return false;
  }

  void setVoid() {
    if (materializeValueResult) {
      valueResult = Value::undefined();
      return;
    }
    result = JS_UNDEFINED;
  }
  void setBool(bool v) {
    if (materializeValueResult) {
      valueResult = Value(v);
      return;
    }
    result = JS_NewBool(context, v);
  }
  void setInt32(int32_t v) {
    if (materializeValueResult) {
      valueResult = Value(static_cast<double>(v));
      return;
    }
    result = JS_NewInt32(context, v);
  }
  void setUInt32(uint32_t v) {
    if (materializeValueResult) {
      valueResult = Value(static_cast<double>(v));
      return;
    }
    result = JS_NewUint32(context, v);
  }
  void setUInt16(uint16_t v) {
    if (materializeValueResult) {
      valueResult = Value(static_cast<double>(v));
      return;
    }
    result = JS_NewUint32(context, v);
  }
  void setInt64(int64_t v) {
    if (materializeValueResult) {
      valueResult = signedInteger64ToEngineValue(runtime, v);
      return;
    }
    result = quickJSInteger64Value(runtime, v);
  }
  void setUInt64(uint64_t v) {
    if (materializeValueResult) {
      valueResult = unsignedInteger64ToEngineValue(runtime, v);
      return;
    }
    result = quickJSUnsignedInteger64Value(runtime, v);
  }
  void setDouble(double v) {
    if (materializeValueResult) {
      valueResult = Value(v);
      return;
    }
    result = JS_NewFloat64(context, v);
  }
  void setSelector(SEL v) {
    const char* name = v != nullptr ? sel_getName(v) : nullptr;
    if (materializeValueResult) {
      valueResult = name != nullptr ? makeString(runtime, name) : Value::null();
      return;
    }
    result = name == nullptr ? JS_NULL : JS_NewString(context, name);
  }
  void setClass(Class v) {
    if (materializeValueResult) {
      if (v == nil) {
        valueResult = Value::null();
        return;
      }
      const char* name = class_getName(v);
      NativeApiSymbol symbol{
          .kind = NativeApiSymbolKind::Class,
          .offset = MD_SECTION_OFFSET_NULL,
          .name = name != nullptr ? name : "",
          .runtimeName = name != nullptr ? name : "",
      };
      if (const NativeApiSymbol* found = bridge->findClass(symbol.name)) {
        symbol = *found;
      }
      valueResult = makeNativeClassValue(runtime, bridge, std::move(symbol));
      return;
    }
    if (v == nil) {
      result = JS_NULL;
      return;
    }
    const char* name = class_getName(v);
    NativeApiSymbol symbol{
        .kind = NativeApiSymbolKind::Class,
        .offset = MD_SECTION_OFFSET_NULL,
        .name = name != nullptr ? name : "",
        .runtimeName = name != nullptr ? name : "",
    };
    if (const NativeApiSymbol* found = bridge->findClass(symbol.name)) {
      symbol = *found;
    }
    Value classValue = makeNativeClassValue(runtime, bridge, std::move(symbol));
    result = classValue.local(runtime);
  }
  void setObject(id obj) {
    if (materializeValueResult) {
      valueResult = convertNativeReturnValue(runtime, bridge, returnType, &obj);
      return;
    }
    result = setQuickJSEngineObjectReturn(runtime, bridge, returnType, obj);
  }
};

// Close the anonymous namespace so the generated dispatch table lives in
// namespace nativescript; GsdObjCContext/ObjCGsdDispatchEntry stay reachable
// via the unnamed namespace's implicit using-directive.
}  // namespace (temporary close for GSD .inc)

#if defined(__has_include)
#if __has_include("../shared/GeneratedGsdSignatureDispatch.inc")
#include "../shared/GeneratedGsdSignatureDispatch.inc"
#endif
#endif

#ifndef NS_HAS_GENERATED_SIGNATURE_GSD_DISPATCH
inline constexpr ObjCGsdDispatchEntry kGeneratedObjCGsdDispatchEntries[] = {
    {0, nullptr}};
#endif

ObjCGsdInvoker lookupObjCGsdInvoker(uint64_t dispatchId) {
  if (!isGeneratedDispatchEnabled()) {
    return nullptr;
  }
  return lookupDispatchInvoker<ObjCGsdDispatchEntry, ObjCGsdInvoker>(
      kGeneratedObjCGsdDispatchEntries, dispatchId);
}

namespace {  // reopen anonymous namespace

// --- End GSD ---
