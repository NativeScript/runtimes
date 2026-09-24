#include "jsi/v8/V8Runtime.h"
#include "jsi/shared/NativeApiStackValueArray.h"

#ifdef TARGET_ENGINE_V8

namespace nativescript {
namespace engine {

namespace v8engine {

Value valueFromLocal(Runtime& runtime, v8::Local<v8::Value> value) { return Value(runtime, value); }

v8::Local<v8::ObjectTemplate> hostObjectTemplate(Runtime& runtime) {
  auto state = runtime.state();
  if (state->hostObjectTemplate.IsEmpty()) {
    v8::Local<v8::ObjectTemplate> objectTemplate = v8::ObjectTemplate::New(runtime.isolate());
    objectTemplate->SetInternalFieldCount(1);
    // toString must be own property to override Object.prototype.toString
    // when using kNonMasking interceptor.
    objectTemplate->Set(
        makeV8String(runtime.isolate(), "toString"),
        v8::FunctionTemplate::New(runtime.isolate(),
            [](const v8::FunctionCallbackInfo<v8::Value>& info) {
              v8::Local<v8::Object> self = info.This();
              if (self.IsEmpty() || self->InternalFieldCount() < 1) return;
              auto* holder = static_cast<HostObjectHolder*>(
                  self->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
              if (holder == nullptr || holder->hostObject == nullptr) return;
              Runtime rt(holder->state);
              try {
                Value toStr = holder->hostObject->get(rt, PropNameID("toString"));
                if (!toStr.isUndefined()) {
                  v8::Local<v8::Value> v8Val = toStr.local(rt);
                  if (v8Val->IsFunction()) {
                    v8::Local<v8::Value> result;
                    if (v8Val.As<v8::Function>()->Call(rt.context(), self, 0, nullptr)
                            .ToLocal(&result)) {
                      info.GetReturnValue().Set(result);
                      return;
                    }
                  }
                }
              } catch (...) {}
            }),
        v8::DontEnum);
    objectTemplate->SetHandler(v8::NamedPropertyHandlerConfiguration(
        [](v8::Local<v8::Name> property,
           const v8::PropertyCallbackInfo<v8::Value>& info) -> v8::Intercepted {
          auto* holder =
              static_cast<HostObjectHolder*>(info.Holder()->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
          if (holder == nullptr || holder->hostObject == nullptr) {
            return v8::Intercepted::kNo;
          }
          // Fast path: skip symbols entirely (they never match our properties).
          if (!property->IsString()) {
            return v8::Intercepted::kNo;
          }
          Runtime runtime(holder->state);
          try {
            v8::Isolate* isolate = info.GetIsolate();
            v8::String::Utf8Value utf8(isolate, property);
            if (*utf8 == nullptr) {
              return v8::Intercepted::kNo;
            }
            Value result = holder->hostObject->get(
                runtime, PropNameID(std::string(*utf8, utf8.length())));
            if (!result.isUndefined()) {
              info.GetReturnValue().Set(result.local(runtime));
              return v8::Intercepted::kYes;
            }
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
            return v8::Intercepted::kYes;
          }
          return v8::Intercepted::kNo;
        },
        // Unary + forces the lambda to decay to a function pointer. V8 14.9
        // constrains the setter parameter with requires(is_same_v<TSetter, ...>),
        // which a closure type fails even though it converts implicitly.
        +[](v8::Local<v8::Name> property, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) -> v8::Intercepted {
          auto* holder =
              static_cast<HostObjectHolder*>(info.Holder()->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
          if (holder == nullptr || holder->hostObject == nullptr) {
            return v8::Intercepted::kNo;
          }
          Runtime runtime(holder->state);
          try {
            bool handled = holder->hostObject->set(
                runtime, PropNameID(propertyNameToUtf8(info.GetIsolate(), property)),
                Value(runtime, value));
            return handled ? v8::Intercepted::kYes : v8::Intercepted::kNo;
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
            return v8::Intercepted::kYes;
          }
        },
        nullptr, nullptr,
        [](const v8::PropertyCallbackInfo<v8::Array>& info) {
          auto* holder =
              static_cast<HostObjectHolder*>(info.Holder()->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
          if (holder == nullptr || holder->hostObject == nullptr) {
            return;
          }
          Runtime runtime(holder->state);
          try {
            auto propertyNames = holder->hostObject->getPropertyNames(runtime);
            v8::Local<v8::Array> result =
                v8::Array::New(info.GetIsolate(), static_cast<int>(propertyNames.size()));
            for (size_t i = 0; i < propertyNames.size(); i++) {
              std::string name = propertyNames[i].utf8(runtime);
              result
                  ->Set(runtime.context(), static_cast<uint32_t>(i),
                        makeV8String(info.GetIsolate(), name))
                  .FromMaybe(false);
            }
            info.GetReturnValue().Set(result);
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
          }
        },
        v8::Local<v8::Value>(), v8::PropertyHandlerFlags::kNone));
    objectTemplate->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        [](uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info) -> v8::Intercepted {
          auto* holder =
              static_cast<HostObjectHolder*>(info.Holder()->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
          if (holder == nullptr || holder->hostObject == nullptr) {
            return v8::Intercepted::kNo;
          }
          Runtime runtime(holder->state);
          try {
            Value result = holder->hostObject->get(runtime, PropNameID(std::to_string(index)));
            if (!result.isUndefined()) {
              info.GetReturnValue().Set(result.local(runtime));
              return v8::Intercepted::kYes;
            }
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
            return v8::Intercepted::kYes;
          }
          return v8::Intercepted::kNo;
        },
        // Unary + forces the lambda to decay to a function pointer. V8 14.9
        // constrains the setter parameter with requires(is_same_v<TSetter, ...>),
        // which a closure type fails even though it converts implicitly.
        +[](uint32_t index, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) -> v8::Intercepted {
          auto* holder =
              static_cast<HostObjectHolder*>(info.Holder()->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
          if (holder == nullptr || holder->hostObject == nullptr) {
            return v8::Intercepted::kNo;
          }
          Runtime runtime(holder->state);
          try {
            holder->hostObject->set(runtime, PropNameID(std::to_string(index)),
                                    Value(runtime, value));
            return v8::Intercepted::kYes;
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
            return v8::Intercepted::kYes;
          }
        },
        nullptr, nullptr, nullptr, v8::Local<v8::Value>(),
        v8::PropertyHandlerFlags::kNone));
    state->hostObjectTemplate.Reset(runtime.isolate(), objectTemplate);
  }
  return state->hostObjectTemplate.Get(runtime.isolate());
}

// Template for native object instances — uses kNonMasking so V8 checks
// prototype chain first (methods/properties installed there are found
// without calling the interceptor).
v8::Local<v8::ObjectTemplate> nativeObjectTemplate(Runtime& runtime) {
  auto state = runtime.state();
  if (state->nativeObjectTemplate.IsEmpty()) {
    v8::Local<v8::ObjectTemplate> objectTemplate = v8::ObjectTemplate::New(runtime.isolate());
    objectTemplate->SetInternalFieldCount(1);
    // toString must be own property to override Object.prototype.toString
    objectTemplate->Set(
        makeV8String(runtime.isolate(), "toString"),
        v8::FunctionTemplate::New(runtime.isolate(),
            [](const v8::FunctionCallbackInfo<v8::Value>& info) {
              v8::Local<v8::Object> self = info.This();
              if (self.IsEmpty() || self->InternalFieldCount() < 1) return;
              auto* holder = static_cast<HostObjectHolder*>(
                  self->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
              if (holder == nullptr || holder->hostObject == nullptr) return;
              Runtime rt(holder->state);
              try {
                Value toStr = holder->hostObject->get(rt, PropNameID("toString"));
                if (!toStr.isUndefined()) {
                  v8::Local<v8::Value> v8Val = toStr.local(rt);
                  if (v8Val->IsFunction()) {
                    v8::Local<v8::Value> result;
                    if (v8Val.As<v8::Function>()->Call(rt.context(), self, 0, nullptr)
                            .ToLocal(&result)) {
                      info.GetReturnValue().Set(result);
                      return;
                    }
                  }
                }
              } catch (...) {}
            }),
        v8::DontEnum);
    objectTemplate->SetHandler(v8::NamedPropertyHandlerConfiguration(
        [](v8::Local<v8::Name> property,
           const v8::PropertyCallbackInfo<v8::Value>& info) -> v8::Intercepted {
          auto* holder =
              static_cast<HostObjectHolder*>(info.Holder()->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
          if (holder == nullptr || holder->hostObject == nullptr) {
            return v8::Intercepted::kNo;
          }
          if (!property->IsString()) {
            return v8::Intercepted::kNo;
          }
          Runtime runtime(holder->state);
          try {
            v8::Isolate* isolate = info.GetIsolate();
            v8::String::Utf8Value utf8(isolate, property);
            if (*utf8 == nullptr) {
              return v8::Intercepted::kNo;
            }
            Value result = holder->hostObject->get(
                runtime, PropNameID(std::string(*utf8, utf8.length())));
            if (!result.isUndefined()) {
              info.GetReturnValue().Set(result.local(runtime));
              return v8::Intercepted::kYes;
            }
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
            return v8::Intercepted::kYes;
          }
          return v8::Intercepted::kNo;
        },
        // Unary + forces the lambda to decay to a function pointer. V8 14.9
        // constrains the setter parameter with requires(is_same_v<TSetter, ...>),
        // which a closure type fails even though it converts implicitly.
        +[](v8::Local<v8::Name> property, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) -> v8::Intercepted {
          auto* holder =
              static_cast<HostObjectHolder*>(info.Holder()->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
          if (holder == nullptr || holder->hostObject == nullptr) {
            return v8::Intercepted::kNo;
          }
          if (!property->IsString()) {
            return v8::Intercepted::kNo;
          }
          Runtime runtime(holder->state);
          try {
            v8::Isolate* isolate = info.GetIsolate();
            v8::String::Utf8Value utf8(isolate, property);
            if (*utf8 == nullptr) {
              return v8::Intercepted::kNo;
            }
            bool handled = holder->hostObject->set(
                runtime, PropNameID(std::string(*utf8, utf8.length())),
                Value(runtime, value));
            return handled ? v8::Intercepted::kYes : v8::Intercepted::kNo;
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
            return v8::Intercepted::kYes;
          }
        },
        nullptr, nullptr, nullptr, v8::Local<v8::Value>(),
        v8::PropertyHandlerFlags::kNonMasking));
    objectTemplate->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        [](uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info) -> v8::Intercepted {
          auto* holder =
              static_cast<HostObjectHolder*>(info.Holder()->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
          if (holder == nullptr || holder->hostObject == nullptr) {
            return v8::Intercepted::kNo;
          }
          Runtime runtime(holder->state);
          try {
            Value result = holder->hostObject->get(runtime, PropNameID(std::to_string(index)));
            if (!result.isUndefined()) {
              info.GetReturnValue().Set(result.local(runtime));
              return v8::Intercepted::kYes;
            }
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
            return v8::Intercepted::kYes;
          }
          return v8::Intercepted::kNo;
        },
        // Unary + forces the lambda to decay to a function pointer. V8 14.9
        // constrains the setter parameter with requires(is_same_v<TSetter, ...>),
        // which a closure type fails even though it converts implicitly.
        +[](uint32_t index, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) -> v8::Intercepted {
          auto* holder =
              static_cast<HostObjectHolder*>(info.Holder()->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
          if (holder == nullptr || holder->hostObject == nullptr) {
            return v8::Intercepted::kNo;
          }
          Runtime runtime(holder->state);
          try {
            holder->hostObject->set(runtime, PropNameID(std::to_string(index)),
                                    Value(runtime, value));
            return v8::Intercepted::kYes;
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
            return v8::Intercepted::kYes;
          }
        },
        nullptr, nullptr, nullptr, v8::Local<v8::Value>(),
        v8::PropertyHandlerFlags::kNonMasking));
    state->nativeObjectTemplate.Reset(runtime.isolate(), objectTemplate);
  }
  return state->nativeObjectTemplate.Get(runtime.isolate());
}

void hostObjectWeakCallback(const v8::WeakCallbackInfo<HostObjectHolder>& info) {
  untrackRuntimeAllocation(info.GetIsolate(), info.GetParameter());
  delete info.GetParameter();
}

void functionWeakCallback(const v8::WeakCallbackInfo<FunctionHolder>& info) {
  untrackRuntimeAllocation(info.GetIsolate(), info.GetParameter());
  delete info.GetParameter();
}

}  // namespace v8engine

Object Object::createFromHostObjectWithToken(Runtime& runtime, std::shared_ptr<HostObject> host,
                                             const void* typeToken) {
  v8::Local<v8::Object> object =
      v8engine::hostObjectTemplate(runtime)->NewInstance(runtime.context()).ToLocalChecked();
  auto* holder = new v8engine::HostObjectHolder(runtime.state(), std::move(host), typeToken);
  v8engine::trackRuntimeAllocation(runtime.isolate(), holder);
  object->SetAlignedPointerInInternalField(
      0, holder, v8::kEmbedderDataTypeTagDefault);
  holder->object.Reset(runtime.isolate(), object);
  holder->object.SetWeak(holder, v8engine::hostObjectWeakCallback,
                         v8::WeakCallbackType::kParameter);
  return Object::fromValueStorage(Value(runtime, object).storage_);
}

Object Object::createNativeInstanceWithToken(Runtime& runtime, std::shared_ptr<HostObject> host,
                                             const void* typeToken) {
  v8::Local<v8::Object> object =
      v8engine::nativeObjectTemplate(runtime)
          ->NewInstance(runtime.context())
          .ToLocalChecked();
  auto* holder = new v8engine::HostObjectHolder(runtime.state(), std::move(host), typeToken);
  v8engine::trackRuntimeAllocation(runtime.isolate(), holder);
  object->SetAlignedPointerInInternalField(
      0, holder, v8::kEmbedderDataTypeTagDefault);
  holder->object.Reset(runtime.isolate(), object);
  holder->object.SetWeak(holder, v8engine::hostObjectWeakCallback,
                         v8::WeakCallbackType::kParameter);
  return Object::fromValueStorage(Value(runtime, object).storage_);
}

Function Function::createFromHostFunction(Runtime& runtime, const PropNameID& name, unsigned int,
                                          HostFunctionType callback) {
  auto* holder = new v8engine::FunctionHolder(runtime.state(), std::move(callback));
  v8engine::trackRuntimeAllocation(runtime.isolate(), holder);
  v8::Local<v8::External> data = v8::External::New(
      runtime.isolate(), holder, v8::kExternalPointerTypeTagDefault);
  v8::Local<v8::FunctionTemplate> functionTemplate = v8::FunctionTemplate::New(
      runtime.isolate(),
      [](const v8::FunctionCallbackInfo<v8::Value>& info) {
        auto* holder =
            static_cast<v8engine::FunctionHolder*>(info.Data().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
        Runtime runtime(holder->state);
        StackValueArray<Value, 8> args(static_cast<size_t>(info.Length()));
        for (int i = 0; i < info.Length(); i++) {
          args.emplace(static_cast<size_t>(i), Value::borrowed(runtime, info[i]));
        }
        try {
          Value thisValue = Value::borrowed(runtime, info.This());
          Value result = holder->callback(runtime, thisValue, args.size() == 0 ? nullptr : args.data(),
                                          args.size());
          info.GetReturnValue().Set(result.local(runtime));
        } catch (const std::exception& exception) {
          v8engine::throwV8Exception(info.GetIsolate(), exception);
        }
      },
      data);
  v8::Local<v8::Function> function =
      functionTemplate->GetFunction(runtime.context()).ToLocalChecked();
  std::string functionName = name.utf8(runtime);
  if (!functionName.empty()) {
    function->SetName(v8engine::makeV8String(runtime.isolate(), functionName));
  }
  holder->function.Reset(runtime.isolate(), function);
  holder->function.SetWeak(holder, v8engine::functionWeakCallback,
                           v8::WeakCallbackType::kParameter);
  return Function(Object::fromValueStorage(Value(runtime, function).storage_));
}

}  // namespace engine
}  // namespace nativescript

#endif  // TARGET_ENGINE_V8
