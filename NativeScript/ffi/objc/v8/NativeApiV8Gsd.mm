// --- GSD (Generated Signature Dispatch) for V8 ---
// GsdObjCContext is the engine-neutral interface the generated invokers use:
// it reads JS arguments and writes the JS return value via the V8 API. The
// readers mirror V8's generic argument/return conversions exactly; any value
// that is not in the fast representation makes a reader return false so the
// invoker falls back to the fully correct generic path.
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
  const v8::FunctionCallbackInfo<v8::Value>& info;
  v8::Isolate* isolate;
  v8::Local<v8::Context> jsContext;
  const NativeApiType& returnType;

  template <typename Invocation>
  void invokeNative(Invocation&& invocation) {
    performGeneratedObjCInvocation(runtime, bridge, [&]() { invocation(); });
  }

  v8::Local<v8::Value> arg(size_t i) const {
    return info[static_cast<int>(i)];
  }

  bool readBool(size_t i, uint8_t* out) {
    *out = arg(i)->BooleanValue(isolate) ? 1 : 0;
    return true;
  }
  template <class T>
  bool readSigned(size_t i, T* out) {
    v8::Local<v8::Value> v = arg(i);
    if (v->IsInt32()) {
      *out = static_cast<T>(v.As<v8::Int32>()->Value());
      return true;
    }
    if constexpr (sizeof(T) <= 4) {
      int32_t tmp = 0;
      if (!v->Int32Value(jsContext).To(&tmp)) return false;
      *out = static_cast<T>(tmp);
    } else {
      if (v->IsBigInt()) {
        bool lossless = false;
        *out = static_cast<T>(v.As<v8::BigInt>()->Int64Value(&lossless));
      } else {
        int64_t tmp = 0;
        if (!v->IntegerValue(jsContext).To(&tmp)) return false;
        *out = static_cast<T>(tmp);
      }
    }
    return true;
  }
  template <class T>
  bool readUnsigned(size_t i, T* out) {
    v8::Local<v8::Value> v = arg(i);
    if (v->IsUint32()) {
      *out = static_cast<T>(v.As<v8::Uint32>()->Value());
      return true;
    }
    if (v->IsInt32()) {
      *out = static_cast<T>(v.As<v8::Int32>()->Value());
      return true;
    }
    if constexpr (sizeof(T) <= 4) {
      uint32_t tmp = 0;
      if (!v->Uint32Value(jsContext).To(&tmp)) return false;
      *out = static_cast<T>(tmp);
    } else {
      if (v->IsBigInt()) {
        bool lossless = false;
        *out = static_cast<T>(v.As<v8::BigInt>()->Uint64Value(&lossless));
      } else {
        int64_t tmp = 0;
        if (!v->IntegerValue(jsContext).To(&tmp)) return false;
        *out = static_cast<T>(static_cast<uint64_t>(tmp));
      }
    }
    return true;
  }
  bool readFloat(size_t i, float* out) {
    double tmp = 0.0;
    if (!readDouble(i, &tmp)) return false;
    *out = static_cast<float>(tmp);
    return true;
  }
  bool readDouble(size_t i, double* out) {
    v8::Local<v8::Value> v = arg(i);
    if (v->IsNumber()) {
      *out = v.As<v8::Number>()->Value();
      return true;
    }
    return v->NumberValue(jsContext).To(out);
  }
  bool readSelector(size_t i, SEL* out) {
    return readV8EngineSelectorArgument(runtime, arg(i), out);
  }
  bool readClass(size_t i, Class* out) {
    Class cls = v8NativeClassArgument(runtime, arg(i));
    if (cls == Nil) return false;
    *out = cls;
    return true;
  }
  bool readObject(size_t i, id* out) {
    v8::Local<v8::Value> v = arg(i);
    if (v.IsEmpty() || v->IsNullOrUndefined()) {
      *out = nil;
      return true;
    }
    if (!v->IsObject()) return false;
    if (auto* h = v8HostObjectRaw<NativeApiObjectHostObject>(v)) {
      *out = h->object();
      return true;
    }
    if (auto* c = v8HostObjectRaw<NativeApiClassHostObject>(v)) {
      *out = static_cast<id>(c->nativeClass());
      return true;
    }
    Class cls = v8NativeClassArgument(runtime, v);
    if (cls != Nil) {
      *out = static_cast<id>(cls);
      return true;
    }
    if (auto* p = v8HostObjectRaw<NativeApiProtocolHostObject>(v)) {
      *out = static_cast<id>(p->nativeProtocol());
      return true;
    }
    return false;
  }

  void setVoid() {}
  void setBool(bool v) {
    info.GetReturnValue().Set(v8::Boolean::New(isolate, v));
  }
  void setInt32(int32_t v) {
    info.GetReturnValue().Set(v8::Integer::New(isolate, v));
  }
  void setUInt32(uint32_t v) {
    info.GetReturnValue().Set(v8::Integer::NewFromUnsigned(isolate, v));
  }
  void setUInt16(uint16_t v) {
    info.GetReturnValue().Set(v8::Integer::NewFromUnsigned(isolate, v));
  }
  void setInt64(int64_t v) {
    info.GetReturnValue().Set(v8Integer64Value(isolate, v));
  }
  void setUInt64(uint64_t v) {
    info.GetReturnValue().Set(v8UnsignedInteger64Value(isolate, v));
  }
  void setDouble(double v) {
    info.GetReturnValue().Set(v8::Number::New(isolate, v));
  }
  void setSelector(SEL v) {
    const char* name = v != nullptr ? sel_getName(v) : nullptr;
    if (name == nullptr) {
      info.GetReturnValue().Set(v8::Null(isolate));
    } else {
      info.GetReturnValue().Set(engine::v8engine::makeV8String(isolate, name));
    }
  }
  void setClass(Class v) {
    if (v == nil) {
      info.GetReturnValue().Set(v8::Null(isolate));
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
    Value result = makeNativeClassValue(runtime, bridge, std::move(symbol));
    info.GetReturnValue().Set(result.local(runtime));
  }
  void setObject(id obj) {
    setV8EngineObjectReturn(runtime, bridge, returnType, obj, info);
  }
};

// Close the anonymous namespace so the generated dispatch table lives in
// namespace nativescript (visible to lookupObjCGsdInvoker). GsdObjCContext is
// reachable from there via the unnamed namespace's implicit using-directive.
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
