// Included by NativeApiV8.mm inside the NativeScript anonymous namespace.

struct NativeApiLazyGlobalData {
  NativeApiLazyGlobalData(v8::Isolate* isolate, const std::string& name,
                            const std::string& kind) {
    nameValue.Reset(isolate, engine::v8engine::makeV8String(isolate, name));
    kindValue.Reset(isolate, engine::v8engine::makeV8String(isolate, kind));
  }

  ~NativeApiLazyGlobalData() {
    external.Reset();
    nameValue.Reset();
    kindValue.Reset();
  }

  v8::Global<v8::External> external;
  v8::Global<v8::String> nameValue;
  v8::Global<v8::String> kindValue;
};

void NativeApiLazyGlobalWeakCallback(
    const v8::WeakCallbackInfo<NativeApiLazyGlobalData>& info) {
  engine::v8engine::untrackRuntimeAllocation(info.GetIsolate(),
                                             info.GetParameter());
  delete info.GetParameter();
}

std::shared_ptr<Runtime> retainNativeApiRuntime(Runtime& runtime) {
  return std::make_shared<Runtime>(runtime.state());
}

class NativeApiRuntimeScope final {
 public:
  explicit NativeApiRuntimeScope(Runtime& runtime)
      : locker_(runtime.isolate()),
        isolateScope_(runtime.isolate()),
        handleScope_(runtime.isolate()),
        context_(runtime.context()),
        contextScope_(context_) {}

 private:
  v8::Locker locker_;
  v8::Isolate::Scope isolateScope_;
  v8::HandleScope handleScope_;
  v8::Local<v8::Context> context_;
  v8::Context::Scope contextScope_;
};

void NativeApiLazyGlobalGetter(v8::Local<v8::Name>,
                                 const v8::PropertyCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::HandleScope handleScope(isolate);
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (!info.Data()->IsExternal()) {
    return;
  }

  auto* data = static_cast<NativeApiLazyGlobalData*>(info.Data().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
  if (data == nullptr) {
    return;
  }
  v8::Local<v8::String> nameValue = data->nameValue.Get(isolate);
  v8::Local<v8::String> kindValue = data->kindValue.Get(isolate);

  v8::Local<v8::Object> global = context->Global();
  v8::Local<v8::Value> resolverValue;
  if (!global
           ->Get(context, engine::v8engine::makeV8String(
                              isolate, "__nativeScriptResolveNativeApiLazyGlobal"))
           .ToLocal(&resolverValue) ||
      !resolverValue->IsFunction()) {
    return;
  }

  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Value> args[] = {nameValue, kindValue};
  v8::Local<v8::Value> result;
  if (!resolverValue.As<v8::Function>()->Call(context, global, 2, args).ToLocal(&result)) {
    if (tryCatch.HasCaught()) {
      isolate->ThrowException(tryCatch.Exception());
    }
    return;
  }
  if (global->Delete(context, nameValue).FromMaybe(false)) {
    global->DefineOwnProperty(context, nameValue, result, v8::DontEnum).FromMaybe(false);
  }
  info.GetReturnValue().Set(result);
}

bool InstallNativeApiLazyGlobal(Runtime& runtime, std::shared_ptr<NativeApiBridge>,
                                      const std::string& name, const std::string& kind,
                                      bool force) {
  if (name.empty() || kind.empty()) {
    return false;
  }

  v8::Isolate* isolate = runtime.isolate();
  v8::EscapableHandleScope handleScope(isolate);
  v8::Local<v8::Context> context = runtime.context();
  v8::Local<v8::Object> global = context->Global();
  v8::Local<v8::String> property = engine::v8engine::makeV8String(isolate, name);
  if (!force && global->HasOwnProperty(context, property).FromMaybe(false)) {
    return false;
  }

  auto* data = new NativeApiLazyGlobalData(isolate, name, kind);
  engine::v8engine::trackRuntimeAllocation(isolate, data);
  v8::Local<v8::External> external = v8::External::New(
      isolate, data, v8::kExternalPointerTypeTagDefault);

  bool installed = global
                       ->SetNativeDataProperty(context, property, NativeApiLazyGlobalGetter,
                                               nullptr, external, v8::DontEnum)
                       .FromMaybe(false);
  if (installed) {
    data->external.Reset(isolate, external);
    data->external.SetWeak(data, NativeApiLazyGlobalWeakCallback,
                           v8::WeakCallbackType::kParameter);
  } else {
    engine::v8engine::untrackRuntimeAllocation(isolate, data);
    delete data;
  }
  return installed;
}

void SetNativeApiObjectPrototype(Runtime& runtime, Object& object,
                                       const Object& prototype) {
  v8::TryCatch tryCatch(runtime.isolate());
  if (!object.local(runtime)
           ->SetPrototypeV2(runtime.context(), prototype.local(runtime))
           .FromMaybe(false)) {
    throw JSError(runtime,
                  engine::v8engine::currentExceptionMessage(runtime.isolate(),
                                                            tryCatch));
  }
}
