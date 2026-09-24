#pragma once

#include <cstddef>
#include <new>
#include <utility>

namespace nativescript::engine {

template <typename ValueType, size_t InlineCount>
class StackValueArray {
 public:
  explicit StackValueArray(size_t count) : count_(count) {
    values_ =
        count_ > InlineCount
            ? static_cast<ValueType*>(
                  ::operator new(sizeof(ValueType) * count_))
            : reinterpret_cast<ValueType*>(inlineStorage_);
  }

  ~StackValueArray() {
    for (size_t i = 0; i < constructed_; i++) {
      values_[i].~ValueType();
    }
    if (count_ > InlineCount) {
      ::operator delete(values_);
    }
  }

  StackValueArray(const StackValueArray&) = delete;
  StackValueArray& operator=(const StackValueArray&) = delete;

  void emplace(size_t index, ValueType&& value) {
    new (&values_[index]) ValueType(std::move(value));
    constructed_++;
  }

  ValueType* data() { return count_ == 0 ? nullptr : values_; }
  size_t size() const { return count_; }

 private:
  size_t count_ = 0;
  size_t constructed_ = 0;
  ValueType* values_ = nullptr;
  alignas(ValueType) unsigned char inlineStorage_[sizeof(ValueType) * InlineCount];
};

}  // namespace nativescript::engine
