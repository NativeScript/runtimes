#include "JSIRuntime.h"

#include <android/looper.h>
#include <jni.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <deque>
#include <mutex>
#include <unordered_map>

#include "V8Runtime.h"
#include "v8.h"

namespace {

using Task = std::function<void(facebook::jsi::Runtime&)>;

// Tasks waiting for the JS thread's ALooper. The eventfd is only written and
// closed under `mutex`, and only closed on the looper thread, so a wakeup can
// never reach a closed (or reused) fd and a callback never outlives the queue.
struct LooperQueue {
  std::weak_ptr<rnv8::V8Runtime> runtime;
  ALooper* looper = nullptr;
  int fd = -1;

  std::mutex mutex;
  std::deque<Task> tasks;
  bool closed = false;

  // Requires `mutex`.
  void Signal() {
    uint64_t value = 1;
    write(fd, &value, sizeof(value));
  }

  static int Drain(int fd, int /*events*/, void* data) {
    auto* holder = static_cast<std::shared_ptr<LooperQueue>*>(data);
    std::shared_ptr<LooperQueue> queue = *holder;

    std::deque<Task> tasks;
    {
      std::lock_guard<std::mutex> lock(queue->mutex);
      uint64_t value;
      read(fd, &value, sizeof(value));
      if (queue->closed) {
        ALooper_removeFd(queue->looper, fd);
        close(fd);
        ALooper_release(queue->looper);
        delete holder;
        return 0;
      }
      tasks.swap(queue->tasks);
    }

    if (auto runtime = queue->runtime.lock()) {
      for (const Task& task : tasks) {
        runtime->ExecuteTask(task);
      }
    }
    return 1;
  }
};

class LooperCallInvoker final : public tns::JSICallInvoker {
 public:
  // Must run on the JS thread. Returns nullptr when it has no ALooper.
  static std::shared_ptr<LooperCallInvoker> Create(
      std::weak_ptr<rnv8::V8Runtime> runtime) {
    ALooper* looper = ALooper_forThread();
    if (looper == nullptr) {
      return nullptr;
    }
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd == -1) {
      return nullptr;
    }

    auto queue = std::make_shared<LooperQueue>();
    queue->runtime = std::move(runtime);
    queue->looper = looper;
    queue->fd = fd;

    auto* holder = new std::shared_ptr<LooperQueue>(queue);
    if (ALooper_addFd(looper, fd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                      &LooperQueue::Drain, holder) != 1) {
      delete holder;
      close(fd);
      return nullptr;
    }
    ALooper_acquire(looper);
    return std::make_shared<LooperCallInvoker>(std::move(queue));
  }

  explicit LooperCallInvoker(std::shared_ptr<LooperQueue> queue)
      : queue_(std::move(queue)), thread_(pthread_self()) {}

  ~LooperCallInvoker() override {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->closed = true;
    queue_->tasks.clear();
    queue_->Signal();
  }

  void invokeAsync(Task&& task) override {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->tasks.push_back(std::move(task));
    queue_->Signal();
  }

  bool isOnJSThread() const override {
    return pthread_equal(pthread_self(), thread_) != 0;
  }

 private:
  std::shared_ptr<LooperQueue> queue_;
  pthread_t thread_;
};

struct IsolateEntry {
  std::shared_ptr<rnv8::V8Runtime> runtime;
  std::shared_ptr<tns::JSICallInvoker> callInvoker;
};

std::mutex g_jsiMutex;

// Leaked so no runtime is torn down by static destructors during exit, when
// its isolate may already be gone.
std::unordered_map<v8::Isolate*, IsolateEntry>& isolateEntries() {
  static auto* entries = new std::unordered_map<v8::Isolate*, IsolateEntry>();
  return *entries;
}

// Requires g_jsiMutex.
IsolateEntry* entryForCurrentIsolate() {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (isolate == nullptr) {
    return nullptr;
  }
  IsolateEntry& entry = isolateEntries()[isolate];
  if (!entry.runtime) {
    entry.runtime = std::make_shared<rnv8::V8Runtime>();
  }
  return &entry;
}

}  // namespace

namespace tns {

std::shared_ptr<facebook::jsi::Runtime> GetJSIRuntime() {
  std::lock_guard<std::mutex> lock(g_jsiMutex);
  IsolateEntry* entry = entryForCurrentIsolate();
  return entry ? entry->runtime : nullptr;
}

std::shared_ptr<JSICallInvoker> GetJSICallInvoker() {
  std::lock_guard<std::mutex> lock(g_jsiMutex);
  IsolateEntry* entry = entryForCurrentIsolate();
  if (entry == nullptr) {
    return nullptr;
  }
  if (!entry->callInvoker) {
    entry->callInvoker = LooperCallInvoker::Create(entry->runtime);
  }
  return entry->callInvoker;
}

void DisposeJSIRuntimeForCurrentIsolate() {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (isolate == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_jsiMutex);
  isolateEntries().erase(isolate);
}

}  // namespace tns

extern "C" facebook::jsi::Runtime* NativeScriptJSIRuntime(void) {
  auto rt = tns::GetJSIRuntime();
  return rt ? rt.get() : nullptr;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_nativescript_jsi_JSIRuntime_nativeRuntimePointer(JNIEnv* /*env*/,
                                                          jclass /*clazz*/) {
  facebook::jsi::Runtime* rt = NativeScriptJSIRuntime();
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(rt));
}

extern "C" JNIEXPORT void JNICALL
Java_org_nativescript_jsi_JSIRuntime_nativeDisposeRuntimeForCurrentIsolate(
    JNIEnv* /*env*/, jclass /*clazz*/) {
  tns::DisposeJSIRuntimeForCurrentIsolate();
}
