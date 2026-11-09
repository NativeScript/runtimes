#ifndef TIMERS_H
#define TIMERS_H

#include "native_api_util.h"

namespace nativescript {

class Timers {
 public:
  static JS_CLASS_INIT(Init);

  static JS_METHOD(SetTimeout);
  static JS_METHOD(SetInterval);
  static JS_METHOD(ClearTimer);
  static JS_METHOD(QueueMicrotask);
  static JS_METHOD(ActiveTimerCount);

  static bool HasActiveTimers();
};

}  // namespace nativescript

#endif  // TIMERS_H
