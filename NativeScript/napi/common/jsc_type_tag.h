#ifndef NATIVESCRIPT_NAPI_COMMON_JSC_TYPE_TAG_H
#define NATIVESCRIPT_NAPI_COMMON_JSC_TYPE_TAG_H

#include <JavaScriptCore/JavaScript.h>

#include <cstdio>
#include <cstring>

#include "js_native_api.h"

namespace nativescript::napi::jsc {

constexpr size_t kOwnerPrefixSize = 2 + sizeof(void*) * 2 + 2;
constexpr size_t kEncodedTypeTagSize = kOwnerPrefixSize + 16 + 1 + 16;

inline void FormatTypeTag(char* buffer, size_t size, JSObjectRef object,
                          const napi_type_tag* tag) {
  std::snprintf(buffer, size, "%p:%016llx:%016llx",
                static_cast<void*>(object),
                static_cast<unsigned long long>(tag->lower),
                static_cast<unsigned long long>(tag->upper));
}

inline bool ReadTypeTag(JSContextRef context, JSValueRef value, char* buffer,
                        size_t size) {
  if (!JSValueIsString(context, value)) {
    return false;
  }
  JSValueRef exception = nullptr;
  JSStringRef string = JSValueToStringCopy(context, value, &exception);
  if (exception != nullptr || string == nullptr) {
    return false;
  }
  const size_t written = JSStringGetUTF8CString(string, buffer, size);
  JSStringRelease(string);
  return written > 0;
}

inline napi_status TypeTagObject(napi_env env, napi_value value,
                                 const napi_type_tag* tag) {
  if (env == nullptr || value == nullptr || tag == nullptr) {
    return napi_invalid_arg;
  }
  JSValueRef jsValue = reinterpret_cast<JSValueRef>(value);
  if (!JSValueIsObject(env->context, jsValue)) {
    return napi_object_expected;
  }

  JSObjectRef object = reinterpret_cast<JSObjectRef>(value);
  JSValueRef exception = nullptr;
  JSValueRef existing = JSObjectGetPropertyForKey(
      env->context, object, env->type_tag_symbol, &exception);
  if (exception != nullptr) {
    env->last_exception = exception;
    return napi_pending_exception;
  }

  char ownerPrefix[kOwnerPrefixSize] = {};
  std::snprintf(ownerPrefix, sizeof(ownerPrefix), "%p:",
                static_cast<void*>(object));
  char existingTag[kEncodedTypeTagSize] = {};
  if (!JSValueIsUndefined(env->context, existing) &&
      ReadTypeTag(env->context, existing, existingTag, sizeof(existingTag)) &&
      std::strncmp(existingTag, ownerPrefix, std::strlen(ownerPrefix)) == 0) {
    return napi_invalid_arg;
  }

  char encoded[kEncodedTypeTagSize] = {};
  FormatTypeTag(encoded, sizeof(encoded), object, tag);
  JSStringRef string = JSStringCreateWithUTF8CString(encoded);
  JSValueRef tagValue = JSValueMakeString(env->context, string);
  JSStringRelease(string);
  JSObjectSetPropertyForKey(
      env->context, object, env->type_tag_symbol, tagValue,
      kJSPropertyAttributeDontEnum | kJSPropertyAttributeReadOnly |
          kJSPropertyAttributeDontDelete,
      &exception);
  if (exception != nullptr) {
    env->last_exception = exception;
    return napi_pending_exception;
  }
  return napi_ok;
}

inline napi_status CheckObjectTypeTag(napi_env env, napi_value value,
                                      const napi_type_tag* tag, bool* result) {
  if (env == nullptr || value == nullptr || tag == nullptr || result == nullptr) {
    return napi_invalid_arg;
  }
  *result = false;
  JSValueRef jsValue = reinterpret_cast<JSValueRef>(value);
  if (!JSValueIsObject(env->context, jsValue)) {
    return napi_object_expected;
  }

  JSObjectRef object = reinterpret_cast<JSObjectRef>(value);
  JSValueRef exception = nullptr;
  JSValueRef stored = JSObjectGetPropertyForKey(
      env->context, object, env->type_tag_symbol, &exception);
  if (exception != nullptr) {
    env->last_exception = exception;
    return napi_pending_exception;
  }

  char actual[kEncodedTypeTagSize] = {};
  char expected[kEncodedTypeTagSize] = {};
  FormatTypeTag(expected, sizeof(expected), object, tag);
  *result = ReadTypeTag(env->context, stored, actual, sizeof(actual)) &&
            std::strcmp(actual, expected) == 0;
  return napi_ok;
}

}  // namespace nativescript::napi::jsc

#endif  // NATIVESCRIPT_NAPI_COMMON_JSC_TYPE_TAG_H
