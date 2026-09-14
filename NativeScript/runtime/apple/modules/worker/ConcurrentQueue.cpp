#include "ConcurrentQueue.h"

namespace nativescript {

void ConcurrentQueue::Initialize(CFRunLoopRef runLoop,
                                 void (*performWork)(void*), void* info) {
  std::unique_lock<std::mutex> lock(initializationMutex_);
  if (terminated) {
    return;
  }
  this->runLoop_ = runLoop;
  CFRunLoopSourceContext sourceContext = {0, info, 0, 0, 0,
                                          0, 0,    0, 0, performWork};
  this->runLoopTasksSource_ =
      CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &sourceContext);
  CFRunLoopAddSource(this->runLoop_, this->runLoopTasksSource_,
                     kCFRunLoopCommonModes);
}

void ConcurrentQueue::Push(std::shared_ptr<worker::Message> message) {
  std::unique_lock<std::mutex> initializationLock(initializationMutex_);
  if (terminated) {
    return;
  }

  {
    std::unique_lock<std::mutex> mlock(this->mutex_);
    this->messagesQueue_.push(message);
  }

  if (this->runLoopTasksSource_ != nullptr &&
      CFRunLoopSourceIsValid(this->runLoopTasksSource_)) {
    this->SignalAndWakeUp();
  }
}

std::vector<std::shared_ptr<worker::Message>> ConcurrentQueue::PopAll() {
  std::unique_lock<std::mutex> mlock(this->mutex_);
  std::vector<std::shared_ptr<worker::Message>> messages;

  while (!this->messagesQueue_.empty()) {
    std::shared_ptr<worker::Message> message = this->messagesQueue_.front();
    this->messagesQueue_.pop();
    messages.push_back(message);
  }

  return messages;
}

void ConcurrentQueue::SignalAndWakeUp() {
  if (this->runLoopTasksSource_ != nullptr) {
    assert(CFRunLoopSourceIsValid(this->runLoopTasksSource_));
    CFRunLoopSourceSignal(this->runLoopTasksSource_);
  }

  if (this->runLoop_ != nullptr) {
    CFRunLoopWakeUp(this->runLoop_);
  }
}

void ConcurrentQueue::Terminate() {
  std::unique_lock<std::mutex> lock(initializationMutex_);
  terminated = true;
  if (this->runLoop_) {
    CFRunLoopStop(this->runLoop_);
  }

  if (this->runLoopTasksSource_) {
    CFRunLoopRemoveSource(this->runLoop_, this->runLoopTasksSource_,
                          kCFRunLoopCommonModes);
    CFRunLoopSourceInvalidate(this->runLoopTasksSource_);
    CFRelease(this->runLoopTasksSource_);
    this->runLoopTasksSource_ = nullptr;
  }
  this->runLoop_ = nullptr;
}

}  // namespace nativescript
