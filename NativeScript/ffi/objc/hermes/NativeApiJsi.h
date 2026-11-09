#ifndef NATIVE_API_JSI_H_FFI_FORWARD
#define NATIVE_API_JSI_H_FFI_FORWARD

// Moved to jsi/hermes/NativeApiJsi.h. Hermes is the one backend that exposes
// the real facebook::jsi API rather than reimplementing the nativescript
// engine shape, and this declaration is already platform-neutral, so Android's
// Hermes backend can share it verbatim.
//
// This forwarding header keeps the Apple include paths unchanged.

#include "jsi/hermes/NativeApiJsi.h"

#endif  // NATIVE_API_JSI_H_FFI_FORWARD
