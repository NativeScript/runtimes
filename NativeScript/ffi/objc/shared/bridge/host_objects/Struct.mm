class NativeApiStructObjectHostObject final : public HostObject {
 public:
  NativeApiStructObjectHostObject(
      std::shared_ptr<NativeApiBridge> bridge,
      std::shared_ptr<NativeApiAggregateInfo> info,
      const void* data = nullptr, bool ownsData = true,
      std::shared_ptr<std::vector<unsigned char>> storageOwner = nullptr,
      std::shared_ptr<Value> backingValue = nullptr)
      : bridge_(std::move(bridge)),
        info_(std::move(info)),
        ownedData_(std::move(storageOwner)),
        backingValue_(std::move(backingValue)),
        ownsData_(ownsData) {
    if (bridge_ != nullptr) {
      bridge_->holdRuntimeValue(backingValue_);
    }
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

  Value get(Runtime& runtime, const PropNameID& name) override;
  NativeApiHostSetResult set(Runtime& runtime, const PropNameID& name, const Value& value) override;
  std::vector<PropNameID> getPropertyNames(Runtime& runtime) override;

 private:
  std::shared_ptr<NativeApiBridge> bridge_;
  std::shared_ptr<NativeApiAggregateInfo> info_;
  std::shared_ptr<std::vector<unsigned char>> ownedData_;
  std::shared_ptr<Value> backingValue_;
  void* data_ = nullptr;
  bool ownsData_ = true;
};
