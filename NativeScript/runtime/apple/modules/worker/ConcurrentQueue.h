#ifndef ConcurrentQueue_h
#define ConcurrentQueue_h

#include <CoreFoundation/CoreFoundation.h>

#include <mutex>
#include <queue>
#include <vector>

#include "runtime/apple/modules/worker/Message.h"

namespace nativescript {

struct ConcurrentQueue {
 public:
  void Initialize(CFRunLoopRef runLoop, void (*performWork)(void*), void* info);
  void Push(std::shared_ptr<worker::Message> message);
  std::vector<std::shared_ptr<worker::Message>> PopAll();
  void Terminate();

 private:
  std::queue<std::shared_ptr<worker::Message>> messagesQueue_;
  CFRunLoopSourceRef runLoopTasksSource_ = nullptr;
  CFRunLoopRef runLoop_ = nullptr;
  bool terminated = false;
  std::mutex mutex_;
  std::mutex initializationMutex_;
  void SignalAndWakeUp();
};

}  // namespace nativescript

#endif /* ConcurrentQueue_h */
