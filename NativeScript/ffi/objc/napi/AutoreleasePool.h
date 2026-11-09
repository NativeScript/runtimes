#ifndef AUTORELEASEPOOL_H
#define AUTORELEASEPOOL_H

#include "node_api_util.h"

extern "C" {
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);
}

namespace nativescript {

NAPI_FUNCTION(autoreleasepool);

}  // namespace nativescript

#endif  // AUTORELEASEPOOL_H
