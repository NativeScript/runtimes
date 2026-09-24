// HostObject::set returns bool on engines whose interceptors can defer an
// unhandled set to the JS prototype chain. JSI's HostObject::set is void, so
// the Hermes backend defines NATIVESCRIPT_NATIVE_API_HOST_SET_VOID and the
// set overrides below collapse their return type/values accordingly.
#ifdef NATIVESCRIPT_NATIVE_API_HOST_SET_VOID
using NativeApiHostSetResult = void;
#define NATIVE_API_SET_RETURN(handled) return
#else
using NativeApiHostSetResult = bool;
#define NATIVE_API_SET_RETURN(handled) return (handled)
#endif

// Engine-neutral factory for native object instance wrappers. V8 uses its
// kNonMasking native instance template (fast prototype-based property access);
// every other engine uses its standard host-object creation. Selected at
// compile time so the shared bridge code stays engine-agnostic.
template <typename T>
Object createNativeInstanceHostObject(Runtime& runtime, std::shared_ptr<T> host) {
#if defined(TARGET_ENGINE_V8) || defined(TARGET_ENGINE_JSC)
  return Object::createNativeInstanceHostObject(runtime, std::move(host));
#else
  return Object::createFromHostObject(runtime, std::move(host));
#endif
}

class NativeApiObjectLifetimeState final {
 public:
  explicit NativeApiObjectLifetimeState(id object)
      : object_(reinterpret_cast<void*>(object)) {}

  id object() const {
    return reinterpret_cast<id>(object_.load(std::memory_order_relaxed));
  }

  void setObject(id object) {
    object_.store(reinterpret_cast<void*>(object), std::memory_order_relaxed);
  }

  void clear() { object_.store(nullptr, std::memory_order_relaxed); }

 private:
  std::atomic<void*> object_{nullptr};
};


#include "host_objects/Interop.mm"

#include "host_objects/Struct.mm"

#include "host_objects/Appearance.mm"

#include "host_objects/Object.mm"

#include "host_objects/Class.mm"

#include "host_objects/Protocol.mm"
