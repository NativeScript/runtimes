// --- GSD (Generated Signature Dispatch) for Hermes/JSI ---
// GsdObjCContext is the engine-neutral interface the generated invokers use:
// it reads jsi::Value arguments and writes the jsi::Value return value using
// the shared engine-neutral conversion helpers (which already operate on the
// jsi value type). Readers require the fast representation; anything else
// makes a reader return false so the invoker falls back to the generic path.
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
  const Value* arguments;
  const NativeApiType& returnType;
  Value result = Value::undefined();

  template <typename Invocation>
  void invokeNative(Invocation&& invocation) {
    performGeneratedObjCInvocation(runtime, bridge, [&]() { invocation(); });
  }

  bool readNumber(size_t i, double* out) {
    const Value& v = arguments[i];
    if (!v.isNumber()) return false;
    *out = v.asNumber();
    return true;
  }
  bool readBool(size_t i, uint8_t* out) {
    const Value& v = arguments[i];
    if (!v.isBool()) return false;
    *out = v.getBool() ? 1 : 0;
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
    return readFastEngineSelectorArgument(runtime, arguments[i], out);
  }
  bool readClass(size_t i, Class* out) {
    Class cls = classFromEngineValue(runtime, arguments[i]);
    if (cls == Nil) return false;
    *out = cls;
    return true;
  }
  bool readObject(size_t i, id* out) {
    const Value& v = arguments[i];
    if (v.isNull() || v.isUndefined()) {
      *out = nil;
      return true;
    }
    if (!v.isObject()) return false;
    Object o = v.getObject(runtime);
    if (o.isHostObject<NativeApiObjectHostObject>(runtime)) {
      *out = o.getHostObject<NativeApiObjectHostObject>(runtime)->object();
      return true;
    }
    if (o.isHostObject<NativeApiClassHostObject>(runtime)) {
      *out = static_cast<id>(
          o.getHostObject<NativeApiClassHostObject>(runtime)->nativeClass());
      return true;
    }
    Class cls = classFromEngineValue(runtime, v);
    if (cls != Nil) {
      *out = static_cast<id>(cls);
      return true;
    }
    if (o.isHostObject<NativeApiProtocolHostObject>(runtime)) {
      *out = static_cast<id>(
          o.getHostObject<NativeApiProtocolHostObject>(runtime)
              ->nativeProtocol());
      return true;
    }
    return false;
  }

  void setVoid() { result = Value::undefined(); }
  void setBool(bool v) { result = Value(v); }
  void setInt32(int32_t v) { result = Value(static_cast<double>(v)); }
  void setUInt32(uint32_t v) { result = Value(static_cast<double>(v)); }
  void setUInt16(uint16_t v) {
    result = Value(static_cast<double>(v));
  }
  void setInt64(int64_t v) { result = signedInteger64ToEngineValue(runtime, v); }
  void setUInt64(uint64_t v) {
    result = unsignedInteger64ToEngineValue(runtime, v);
  }
  void setDouble(double v) { result = Value(v); }
  void setSelector(SEL v) {
    const char* name = v != nullptr ? sel_getName(v) : nullptr;
    result = name != nullptr ? makeString(runtime, name) : Value::null();
  }
  void setClass(Class v) {
    if (v == nil) {
      result = Value::null();
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
    result = makeNativeClassValue(runtime, bridge, std::move(symbol));
  }
  void setObject(id obj) {
    result = convertNativeReturnValue(runtime, bridge, returnType, &obj);
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
