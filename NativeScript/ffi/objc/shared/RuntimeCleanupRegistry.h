#ifndef NATIVESCRIPT_FFI_OBJC_RUNTIME_CLEANUP_REGISTRY_H
#define NATIVESCRIPT_FFI_OBJC_RUNTIME_CLEANUP_REGISTRY_H

#include <unordered_map>
#include <utility>

namespace nativescript::engine {

class RuntimeCleanupRegistry {
 public:
  using Cleanup = void (*)(void*);

  void track(void* pointer, Cleanup cleanup) { actions_[pointer] = cleanup; }

  template <typename T>
  void track(T* pointer) {
    track(pointer, [](void* value) { delete static_cast<T*>(value); });
  }

  void untrack(void* pointer) { actions_.erase(pointer); }
  bool empty() const { return actions_.empty(); }

  void cleanup() {
    while (!actions_.empty()) {
      auto action = actions_.begin();
      void* pointer = action->first;
      Cleanup cleanup = action->second;
      actions_.erase(action);
      cleanup(pointer);
    }
  }

 private:
  std::unordered_map<void*, Cleanup> actions_;
};

}  // namespace nativescript::engine

#endif  // NATIVESCRIPT_FFI_OBJC_RUNTIME_CLEANUP_REGISTRY_H
