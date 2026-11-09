//
//  Created by Eduardo Speroni on 11/22/23.
//  Copyright © 2023 Progress. All rights reserved.
//

#ifndef Message_hpp
#define Message_hpp

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <vector>
#include "js_native_api_types.h"

namespace nativescript {

template <typename T>
inline T* Malloc(size_t n) {
  T* ret = malloc(n);
  return ret;
}

template <typename T>
T* UncheckedRealloc(T* pointer, size_t n) {
  size_t full_size = sizeof(T) * n;

  if (full_size == 0) {
    free(pointer);
    return nullptr;
  }

  void* allocated = realloc(pointer, full_size);

  //  if (UNLIKELY(allocated == nullptr)) {
  //    // Tell V8 that memory is low and retry.
  //    LowMemoryNotification();
  //    allocated = realloc(pointer, full_size);
  //  }

  return static_cast<T*>(allocated);
}

template <typename T>
struct MallocedBuffer {
  T* data;
  size_t size;

  T* release() {
    T* ret = data;
    data = nullptr;
    return ret;
  }

  void Truncate(size_t new_size) { size = new_size; }

  void Realloc(size_t new_size) {
    Truncate(new_size);
    data = UncheckedRealloc(data, new_size);
  }

  bool is_empty() const { return data == nullptr; }

  MallocedBuffer() : data(nullptr), size(0) {}
  explicit MallocedBuffer(size_t size) : data(Malloc<T>(size)), size(size) {}
  MallocedBuffer(T* data, size_t size) : data(data), size(size) {}
  MallocedBuffer(MallocedBuffer&& other) : data(other.data), size(other.size) {
    other.data = nullptr;
  }
  MallocedBuffer& operator=(MallocedBuffer&& other) {
    this->~MallocedBuffer();
    return *new (this) MallocedBuffer(std::move(other));
  }
  ~MallocedBuffer() {
    free(data);
  }
  MallocedBuffer(const MallocedBuffer&) = delete;
  MallocedBuffer& operator=(const MallocedBuffer&) = delete;
};

namespace worker {

class Message {
 public:
  Message(MallocedBuffer<char>&& payload = MallocedBuffer<char>());
  Message(Message&& other) noexcept;
  Message& operator=(Message&& other) noexcept;
  ~Message();
  Message& operator=(const Message&) = delete;
  Message(const Message&) = delete;
  bool Serialize(napi_env env, napi_value input);
  napi_value Deserialize(napi_env env);

 private:
  MallocedBuffer<char> main_message_buf_;
#ifdef TARGET_ENGINE_QUICKJS
  bool is_quickjs_native_message_ = false;
  std::vector<uint8_t*> quickjs_shared_array_buffers_;
  void ReleaseQuickJSSharedArrayBuffers();
#endif
};
};  // namespace worker
}  // namespace nativescript

#endif /* Message_hpp */
