#ifndef PERFORMANCE_H
#define PERFORMANCE_H

#include "native_api_util.h"

namespace nativescript {

class Performance {
 public:
  static JS_CLASS_INIT(Init);

  static JS_METHOD(Constructor);

  static JS_METHOD(Now);
};

}  // namespace nativescript

#endif  // PERFORMANCE_H
