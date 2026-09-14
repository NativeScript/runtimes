class NativeApiStructObjectHostObject final : public HostObject {
 public:
  using PointerBackingValues =
      std::unordered_map<const void*, std::shared_ptr<Value>>;

  NativeApiStructObjectHostObject(
      std::shared_ptr<NativeApiBridge> bridge,
      std::shared_ptr<NativeApiAggregateInfo> info,
      const void* data = nullptr, bool ownsData = true,
      std::shared_ptr<std::vector<unsigned char>> storageOwner = nullptr,
      std::shared_ptr<Value> backingValue = nullptr,
      std::shared_ptr<PointerBackingValues> pointerBackingValues = nullptr)
      : bridge_(std::move(bridge)),
        info_(std::move(info)),
        ownedData_(std::move(storageOwner)),
        backingValue_(std::move(backingValue)),
        pointerBackingValues_(
            pointerBackingValues != nullptr
                ? std::move(pointerBackingValues)
                : std::make_shared<PointerBackingValues>()),
        ownsData_(ownsData) {
    size_t size = info_ != nullptr ? info_->size : 0;
    if (ownedData_ != nullptr) {
      data_ = const_cast<void*>(data);
      ownsData_ = false;
    } else if (ownsData_) {
      ownedData_ = std::make_shared<std::vector<unsigned char>>(size, 0);
      if (data != nullptr && size > 0) {
        std::memcpy(ownedData_->data(), data, size);
      }
      data_ = ownedData_->empty() ? nullptr : ownedData_->data();
    } else {
      data_ = const_cast<void*>(data);
    }
  }

  void* data() const { return data_; }
  std::shared_ptr<NativeApiAggregateInfo> info() const { return info_; }
  std::shared_ptr<std::vector<unsigned char>> storageOwner() const {
    return ownedData_;
  }
  std::shared_ptr<Value> backingValue() const { return backingValue_; }
  std::shared_ptr<PointerBackingValues> pointerBackingValues() const {
    return pointerBackingValues_;
  }

  Value get(Runtime& runtime, const PropNameID& name) override;
  NativeApiHostSetResult set(Runtime& runtime, const PropNameID& name, const Value& value) override;
  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override;

 private:
  std::shared_ptr<NativeApiBridge> bridge_;
  std::shared_ptr<NativeApiAggregateInfo> info_;
  std::shared_ptr<std::vector<unsigned char>> ownedData_;
  std::shared_ptr<Value> backingValue_;
  std::shared_ptr<PointerBackingValues> pointerBackingValues_;
  void* data_ = nullptr;
  bool ownsData_ = true;
};

#ifdef TARGET_ENGINE_HERMES
class NativeApiStructObjectState final : public NativeApiNativeState {
 public:
  explicit NativeApiStructObjectState(
      std::shared_ptr<NativeApiStructObjectHostObject> host)
      : host_(std::move(host)) {}

  std::shared_ptr<NativeApiStructObjectHostObject> host() const {
    return host_;
  }

 private:
  std::shared_ptr<NativeApiStructObjectHostObject> host_;
};
#endif

std::shared_ptr<NativeApiStructObjectHostObject> getNativeStructHostObject(
    Runtime& runtime, const Object& object) {
  if (object.isHostObject<NativeApiStructObjectHostObject>(runtime)) {
    return object.getHostObject<NativeApiStructObjectHostObject>(runtime);
  }
#ifdef TARGET_ENGINE_HERMES
  if (!object.hasNativeState<NativeApiStructObjectState>(runtime)) {
    return nullptr;
  }
  return object.getNativeState<NativeApiStructObjectState>(runtime)->host();
#else
  return nullptr;
#endif
}

Object createNativeStructHostObject(
    Runtime& runtime, std::shared_ptr<NativeApiStructObjectHostObject> host) {
#ifdef TARGET_ENGINE_HERMES
  Object objectConstructor =
      runtime.global().getPropertyAsObject(runtime, "Object");
  Function defineProperty =
      objectConstructor.getPropertyAsFunction(runtime, "defineProperty");
  Object object(runtime);
  object.setNativeState(runtime,
                        std::make_shared<NativeApiStructObjectState>(host));
  std::weak_ptr<NativeApiStructObjectHostObject> weakHost = host;

  for (const auto& propertyName : host->getPropertyNames(runtime)) {
    std::string property = propertyName.utf8(runtime);
    const auto info = host->info();
    const bool isField = info != nullptr && std::any_of(
        info->fields.begin(), info->fields.end(),
        [&](const NativeApiAggregateField& field) {
          return field.name == property;
        });
    if (!isField) {
      object.setProperty(runtime, property.c_str(),
                         host->get(runtime, propertyName));
      continue;
    }

    Object descriptor(runtime);
    descriptor.setProperty(runtime, "configurable", false);
    descriptor.setProperty(runtime, "enumerable", true);
    descriptor.setProperty(
        runtime, "get",
        Function::createFromHostFunction(
            runtime, PropNameID::forUtf8(runtime, property), 0,
            [weakHost, property](Runtime& runtime, const Value& receiver,
                                 const Value*, size_t) -> Value {
              auto host = weakHost.lock();
              if (host == nullptr || !receiver.isObject()) {
                throw JSError(runtime, "Invalid struct receiver");
              }
              Object receiverObject = receiver.asObject(runtime);
              if (getNativeStructHostObject(runtime, receiverObject).get() !=
                  host.get()) {
                throw JSError(runtime, "Invalid struct receiver");
              }
              return host->get(runtime,
                               PropNameID::forUtf8(runtime, property));
            }));

    descriptor.setProperty(
        runtime, "set",
        Function::createFromHostFunction(
            runtime, PropNameID::forUtf8(runtime, property), 1,
            [weakHost, property](Runtime& runtime, const Value& receiver,
                                 const Value* args, size_t count) -> Value {
              auto host = weakHost.lock();
              if (host == nullptr || !receiver.isObject() || count < 1) {
                throw JSError(runtime, "Invalid struct receiver");
              }
              Object receiverObject = receiver.asObject(runtime);
              if (getNativeStructHostObject(runtime, receiverObject).get() !=
                  host.get()) {
                throw JSError(runtime, "Invalid struct receiver");
              }
              host->set(runtime, PropNameID::forUtf8(runtime, property),
                        args[0]);
              return Value::undefined();
            }));
    defineProperty.call(runtime, object, makeString(runtime, property),
                        descriptor);
  }
  return object;
#else
  return Object::createFromHostObject(runtime, host);
#endif
}
