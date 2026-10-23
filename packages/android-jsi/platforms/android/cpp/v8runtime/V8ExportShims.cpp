#include "v8.h"

// Inline V8 header code (e.g. Object::GetInternalField) calls this V8_EXPORT
// function, but libNativeScript.so does not export it. V8 defines it as the
// current isolate, which libNativeScript.so does export.
v8::Isolate* v8::internal::Internals::GetCurrentIsolate() {
  return v8::Isolate::GetCurrent();
}
