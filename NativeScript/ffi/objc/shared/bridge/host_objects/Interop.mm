class NativeApiPointerHostObject final
    : public HostObject,
      public std::enable_shared_from_this<NativeApiPointerHostObject> {
 public:
  NativeApiPointerHostObject(std::shared_ptr<NativeApiBridge> bridge,
                             void* pointer, std::string kind = "pointer",
                             bool adopted = false,
                             std::shared_ptr<Value> backingValue = nullptr)
      : bridge_(std::move(bridge)),
        pointer_(pointer),
        kind_(std::move(kind)),
        adopted_(adopted),
        backingValue_(std::move(backingValue)) {}

  ~NativeApiPointerHostObject() override {
    if (adopted_ && pointer_ != nullptr) {
      if (bridge_ != nullptr) {
        bridge_->forgetPointerValue(pointer_);
      }
      free(pointer_);
      pointer_ = nullptr;
    }
  }

  void* pointer() const { return pointer_; }
  std::shared_ptr<Value> backingValue() const { return backingValue_; }
  void setBackingValue(Runtime& runtime, const Value& value) {
    backingValue_ = std::make_shared<Value>(runtime, value);
  }
  bool adopted() const { return adopted_; }
  void adopt() { adopted_ = true; }
  void clearWithoutFree() {
    if (bridge_ != nullptr) {
      bridge_->forgetPointerValue(pointer_);
    }
    pointer_ = nullptr;
    adopted_ = false;
    backingValue_.reset();
  }

  Value get(Runtime& runtime, const PropNameID& name) override {
    std::string property = name.utf8(runtime);
    if (property == "kind") {
      return makeString(runtime, kind_);
    }
    if (property == "address") {
      return static_cast<double>(reinterpret_cast<uintptr_t>(pointer_));
    }
    if (property == "adopted") {
      return adopted_;
    }
    if (property == "takeRetainedValue" || property == "takeUnretainedValue") {
      bool retained = property == "takeRetainedValue";
      std::weak_ptr<NativeApiPointerHostObject> weakSelf = shared_from_this();
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 0,
          [weakSelf, retained](Runtime& runtime, const Value&, const Value*,
                               size_t) -> Value {
            auto self = weakSelf.lock();
            if (!self || self->pointer_ == nullptr || self->consumed_) {
              throw JSError(runtime, "Unmanaged value has already been consumed.");
            }
            id object = static_cast<id>(self->pointer_);
            self->consumed_ = true;
            self->pointer_ = nullptr;
            self->adopted_ = false;
            self->backingValue_.reset();
            return makeNativeObjectValue(runtime, self->bridge_, object, retained);
          });
    }
    if (property == "add" || property == "subtract") {
      void* pointer = pointer_;
      bool add = property == "add";
      auto bridge = bridge_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 1,
          [bridge, pointer, add](Runtime& runtime, const Value&,
                                 const Value* args, size_t count) -> Value {
            if (count < 1 || !args[0].isNumber()) {
              throw JSError(runtime, "Pointer offset must be a number.");
            }
            intptr_t offset = static_cast<intptr_t>(args[0].getNumber());
            intptr_t base = reinterpret_cast<intptr_t>(pointer);
            void* result = reinterpret_cast<void*>(add ? base + offset : base - offset);
            return createPointer(runtime, bridge, result);
          });
    }
    if (property == "toNumber") {
      void* pointer = pointer_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "toNumber"), 0,
          [pointer](Runtime&, const Value&, const Value*, size_t) -> Value {
            return static_cast<double>(reinterpret_cast<uintptr_t>(pointer));
          });
    }
    if (property == "toBigInt") {
      void* pointer = pointer_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "toBigInt"), 0,
          [pointer](Runtime& runtime, const Value&, const Value*, size_t) -> Value {
            return BigInt::fromUint64(
                runtime,
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer)));
          });
    }
    if (property == "toHexString" || property == "toDecimalString") {
      void* pointer = pointer_;
      bool hex = property == "toHexString";
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, property.c_str()), 0,
          [pointer, hex](Runtime& runtime, const Value&, const Value*, size_t) -> Value {
            if (hex) {
              char text[2 + sizeof(uintptr_t) * 2 + 1] = {};
              snprintf(text, sizeof(text), "0x%llx",
                       static_cast<unsigned long long>(
                           reinterpret_cast<uintptr_t>(pointer)));
              return makeString(runtime, text);
            } else {
              char text[32] = {};
              snprintf(text, sizeof(text), "%lld",
                       static_cast<long long>(reinterpret_cast<intptr_t>(pointer)));
              return makeString(runtime, text);
            }
          });
    }
    if (property == "toString") {
      void* pointer = pointer_;
      std::string kind = kind_;
      return Function::createFromHostFunction(
          runtime, PropNameID::forAscii(runtime, "toString"), 0,
          [pointer, kind](Runtime& runtime, const Value&, const Value*,
                          size_t) -> Value {
            char address[32] = {};
            snprintf(address, sizeof(address), "%p", pointer);
            if (kind == "pointer") {
              return makeString(runtime,
                                "<Pointer: " + std::string(address) + ">");
            }
            return makeString(runtime, "[NativeApi " + kind + " " +
                                           std::string(address) + "]");
          });
    }
    return Value::undefined();
  }

  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override {
    std::vector<PropNameID> names;
    names.reserve(3);
    addPropertyName(runtime, names, "kind");
    addPropertyName(runtime, names, "address");
    addPropertyName(runtime, names, "adopted");
    addPropertyName(runtime, names, "takeRetainedValue");
    addPropertyName(runtime, names, "takeUnretainedValue");
    addPropertyName(runtime, names, "add");
    addPropertyName(runtime, names, "subtract");
    addPropertyName(runtime, names, "toNumber");
    addPropertyName(runtime, names, "toBigInt");
    addPropertyName(runtime, names, "toHexString");
    addPropertyName(runtime, names, "toDecimalString");
    addPropertyName(runtime, names, "toString");
    return names;
  }

 private:
  std::shared_ptr<NativeApiBridge> bridge_;
  void* pointer_ = nullptr;
  std::string kind_;
  bool adopted_ = false;
  bool consumed_ = false;
  std::shared_ptr<Value> backingValue_;
};

class NativeApiReferenceHostObject final : public HostObject {
 public:
  NativeApiReferenceHostObject(std::shared_ptr<NativeApiBridge> bridge,
                               NativeApiType type, void* data, bool ownsData,
                               size_t byteLength = 0,
                               std::shared_ptr<Value> pendingValue = nullptr,
                               std::shared_ptr<Value> backingValue = nullptr)
      : bridge_(std::move(bridge)),
        type_(std::move(type)),
        data_(data),
        ownsData_(ownsData),
        byteLength_(byteLength),
        pendingValue_(std::move(pendingValue)),
        backingValue_(std::move(backingValue)) {}

  ~NativeApiReferenceHostObject() override {
    for (id object : retainedObjects_) {
      [object release];
    }
    if (ownsData_ && data_ != nullptr) {
      free(data_);
      data_ = nullptr;
    }
  }

  void* data() const { return data_; }
  const NativeApiType& type() const { return type_; }
  std::shared_ptr<Value> backingValue() const { return backingValue_; }
  void setBackingValue(std::shared_ptr<Value> backingValue) {
    backingValue_ = std::move(backingValue);
  }
  void ensureStorage(Runtime& runtime, NativeApiType type,
                     NativeApiArgumentFrame& frame, size_t elements = 1);
  void retainObjectSlot(size_t index, id object);

  Value get(Runtime& runtime, const PropNameID& name) override;
  NativeApiHostSetResult set(Runtime& runtime, const PropNameID& name, const Value& value) override;
  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override {
    std::vector<PropNameID> names;
    addPropertyName(runtime, names, "kind");
    addPropertyName(runtime, names, "value");
    addPropertyName(runtime, names, "address");
    addPropertyName(runtime, names, "toString");
    return names;
  }

 private:
  std::shared_ptr<NativeApiBridge> bridge_;
  NativeApiType type_;
  void* data_ = nullptr;
  bool ownsData_ = false;
  size_t byteLength_ = 0;
  std::shared_ptr<Value> pendingValue_;
  std::shared_ptr<Value> backingValue_;
  std::vector<id> retainedObjects_;
};
