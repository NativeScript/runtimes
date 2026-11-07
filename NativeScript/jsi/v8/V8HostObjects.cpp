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
            Value __receiver = Value::borrowed(runtime, info.Holder());
            HostObject::ReceiverScope __rs(*holder->hostObject, __receiver);
            Value result = holder->hostObject->get(
                runtime, PropNameID(isolate, property));
            if (!result.isUndefined()) {
              info.GetReturnValue().Set(result.local(runtime));
              return v8::Intercepted::kYes;
            }
          } catch (const JSError& error) {
            throwV8Exception(info.GetIsolate(), error);
            return v8::Intercepted::kYes;
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
          // Skip symbols, exactly as the getter above does.
          //
          // This setter used to convert the name through propertyNameToUtf8,
          // which spelled Symbol.iterator as the string "Symbol.iterator".
          // PropNameID now defers the conversion, and v8::String::Utf8Value on
          // a symbol swallows its own TypeError and yields "" -- so every
          // symbol-keyed write reached the host object under the empty name.
          // Silently writing the wrong property is worse than not intercepting.
          //
          // kNo is also what makes the pair coherent: the getter never
          // intercepts symbols, so a symbol stored here could never be read
          // back through it. Letting V8 store it as an ordinary property means
          // the write and the read agree. The other two interceptors already
          // guard this way.
          if (!property->IsString()) {
            return v8::Intercepted::kNo;
          }
          Runtime runtime(holder->state);
          try {
            Value __receiver = Value::borrowed(runtime, info.Holder());
            HostObject::ReceiverScope __rs(*holder->hostObject, __receiver);
            bool handled = holder->hostObject->set(
                runtime, PropNameID(info.GetIsolate(), property),
                Value(runtime, value));
            return handled ? v8::Intercepted::kYes : v8::Intercepted::kNo;
          } catch (const JSError& error) {
            throwV8Exception(info.GetIsolate(), error);
            return v8::Intercepted::kYes;
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
            Value __receiver = Value::borrowed(runtime, info.Holder());
            HostObject::ReceiverScope __rs(*holder->hostObject, __receiver);
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
          } catch (const JSError& error) {
            throwV8Exception(info.GetIsolate(), error);
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
            Value __receiver = Value::borrowed(runtime, info.Holder());
            HostObject::ReceiverScope __rs(*holder->hostObject, __receiver);
            Value result = holder->hostObject->getValueAtIndex(runtime, index);
            if (!result.isUndefined()) {
              info.GetReturnValue().Set(result.local(runtime));
              return v8::Intercepted::kYes;
            }
          } catch (const JSError& error) {
            throwV8Exception(info.GetIsolate(), error);
            return v8::Intercepted::kYes;
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
            Value __receiver = Value::borrowed(runtime, info.Holder());
            HostObject::ReceiverScope __rs(*holder->hostObject, __receiver);
            // Borrowed, not owned: an owned Value allocates a shared ValueStorage
            // and a v8::Global, which on `a[0] = x` is a heap allocation and a
            // global-handle create/destroy per element write. The value does not
            // outlive this call, and HostObject::setValueAtIndex's default
            // promotes it before handing it to the named setter, so a host object
            // that does not override the indexed form is unaffected.
            holder->hostObject->setValueAtIndex(runtime, index, Value::borrowed(runtime, value));
            return v8::Intercepted::kYes;
          } catch (const JSError& error) {
            throwV8Exception(info.GetIsolate(), error);
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
            Value __receiver = Value::borrowed(runtime, info.Holder());
            HostObject::ReceiverScope __rs(*holder->hostObject, __receiver);
            Value result = holder->hostObject->get(
                runtime, PropNameID(isolate, property));
            if (!result.isUndefined()) {
              info.GetReturnValue().Set(result.local(runtime));
              return v8::Intercepted::kYes;
            }
          } catch (const JSError& error) {
            throwV8Exception(info.GetIsolate(), error);
            return v8::Intercepted::kYes;
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
            Value __receiver = Value::borrowed(runtime, info.Holder());
            HostObject::ReceiverScope __rs(*holder->hostObject, __receiver);
            bool handled = holder->hostObject->set(
                runtime, PropNameID(isolate, property),
                Value(runtime, value));
            return handled ? v8::Intercepted::kYes : v8::Intercepted::kNo;
          } catch (const JSError& error) {
            throwV8Exception(info.GetIsolate(), error);
            return v8::Intercepted::kYes;
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
            Value __receiver = Value::borrowed(runtime, info.Holder());
            HostObject::ReceiverScope __rs(*holder->hostObject, __receiver);
            Value result = holder->hostObject->getValueAtIndex(runtime, index);
            if (!result.isUndefined()) {
              info.GetReturnValue().Set(result.local(runtime));
              return v8::Intercepted::kYes;
            }
          } catch (const JSError& error) {
            throwV8Exception(info.GetIsolate(), error);
            return v8::Intercepted::kYes;
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
            Value __receiver = Value::borrowed(runtime, info.Holder());
            HostObject::ReceiverScope __rs(*holder->hostObject, __receiver);
            // Borrowed; see the host-object template above.
            holder->hostObject->setValueAtIndex(runtime, index, Value::borrowed(runtime, value));
            return v8::Intercepted::kYes;
          } catch (const JSError& error) {
            throwV8Exception(info.GetIsolate(), error);
            return v8::Intercepted::kYes;
          } catch (const std::exception& exception) {
            throwV8Exception(info.GetIsolate(), exception);
            return v8::Intercepted::kYes;
          }
        },
        // Masking, unlike the named handler above -- and this is deliberate,
        // not an oversight. kNonMasking makes V8 complete a full property
        // lookup *before* consulting the interceptor, which is what makes it
        // correct for named properties: a Java field or method really does live
        // on the prototype and must win. No native instance ever has a real
        // indexed own or prototype property (the indexed setter always claims
        // the write, so V8 never stores one), so for indices that lookup can
        // only ever fail, and kNonMasking buys nothing but its cost -- measured
        // at 20-30% on every `javaArray[0]` read and write.
        //
        // This is also what the reference implementation does: the V8 Node-API
        // backend in vendor/v8/v8-api.cpp passes
        // kNonMasking for its named handler and default (masking) flags for its
        // indexed one.
        nullptr, nullptr, nullptr, v8::Local<v8::Value>(),
        v8::PropertyHandlerFlags::kNone));
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
        } catch (const JSError& error) {
          v8engine::throwV8Exception(info.GetIsolate(), error);
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

Function Function::createFromHostConstructor(Runtime& runtime, const PropNameID& name,
                                             unsigned int paramCount,
                                             HostFunctionType callback) {
  // v8::Function::New rather than a FunctionTemplate: see the header for why.
  // The holder/External/weak-callback lifetime handling is identical to
  // createFromHostFunction -- only the construction API differs.
  auto* holder = new v8engine::FunctionHolder(runtime.state(), std::move(callback));
  v8::Local<v8::External> data = v8::External::New(runtime.isolate(), holder, v8::kExternalPointerTypeTagDefault);
  v8::Local<v8::Function> function =
      v8::Function::New(
          runtime.context(),
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
              Value result = holder->callback(runtime, thisValue,
                                              args.size() == 0 ? nullptr : args.data(),
                                              args.size());
              info.GetReturnValue().Set(result.local(runtime));
            } catch (const JSError& error) {
              v8engine::throwV8Exception(info.GetIsolate(), error);
            } catch (const std::exception& exception) {
              v8engine::throwV8Exception(info.GetIsolate(), exception);
            }
          },
          data, static_cast<int>(paramCount), v8::ConstructorBehavior::kAllow)
          .ToLocalChecked();
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
