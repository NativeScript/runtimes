#ifndef NS_FFI_JNI_JSI_ENGINE_H
#define NS_FFI_JNI_JSI_ENGINE_H

// The jsi tree's counterpart to js_native_api.h + native_api_util.h: it picks
// the nativescript::engine backend for the engine being built and supplies the
// handful of JS operations the engine layer deliberately does not expose
// (defineProperty, instanceof, prototype access, delete, strict equality).
//
// Those are absent from engine:: because they are not primitives on any of the
// five engines in the same way -- they are language operations reachable from
// the global object. Rather than push five implementations down, they are
// resolved once per runtime from the JS builtins and cached (see Builtins).

#if defined(TARGET_ENGINE_V8)
#include "jsi/v8/V8Runtime.h"
#elif defined(TARGET_ENGINE_JSC)
#include "jsi/jsc/JSCRuntime.h"
#elif defined(TARGET_ENGINE_QUICKJS)
#include "jsi/quickjs/QuickJSRuntime.h"
#elif defined(TARGET_ENGINE_HERMES)
#include "jsi/hermes/HermesRuntime.h"
#else
#error "The jsi JNI bridge needs a TARGET_ENGINE_* definition."
#endif

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace engine = ::nativescript::engine;

namespace tns {

using JsRuntime = engine::Runtime;
using JsValue = engine::Value;
using JsObject = engine::Object;
using JsFunction = engine::Function;
using JsArray = engine::Array;
using JsString = engine::String;
using JsPropNameID = engine::PropNameID;
using JsError = engine::JSError;

namespace js_util {

// Resolved once per runtime. Every entry is an owned engine handle, so they
// must be released before the runtime is torn down -- QuickJS asserts an empty
// gc object list in JS_FreeRuntime and aborts otherwise. dispose() is called
// from the runtime's teardown path.
struct Builtins {
  JsFunction defineProperty;
  JsFunction getPrototypeOf;
  JsFunction setPrototypeOf;
  JsFunction objectCreate;
  JsFunction objectKeys;
  JsFunction deleteProperty;
  JsFunction hasOwnProperty;
  JsFunction isInteger;
  JsFunction isArray;
  JsFunction isView;
  JsFunction strictEquals;
  JsFunction instanceOf;
  JsObject numberCtor;
  JsObject stringCtor;
  JsObject booleanCtor;
  JsObject dateCtor;
  JsObject dataViewCtor;
  JsObject objectCtor;
  JsFunction errorCtor;
  JsFunction stringCoerce;

  static Builtins& of(JsRuntime& rt);
  static void dispose(JsRuntime& rt);
};

inline JsObject global(JsRuntime& rt) { return rt.global(); }

inline bool is_undefined(const JsValue& value) { return value.isUndefined(); }

inline bool is_null(const JsValue& value) { return value.isNull(); }

inline bool is_null_or_undefined(const JsValue& value) {
  return value.isUndefined() || value.isNull();
}

inline bool is_object(const JsValue& value) { return value.isObject(); }

inline JsValue undefined() { return JsValue::undefined(); }

inline JsValue null() { return JsValue::null(); }

inline bool get_bool(const JsValue& value) {
  return value.isBool() ? value.getBool() : false;
}

inline double get_number(const JsValue& value) {
  return value.isNumber() ? value.getNumber() : 0;
}

inline int32_t get_int32(const JsValue& value) {
  return static_cast<int32_t>(get_number(value));
}

// asString(rt).utf8(rt) built an owning engine::String -- a heap allocation and
// a persistent engine handle -- for a value that was read and dropped in the
// same expression. Value::utf8 reads the handle in place. See the declaration
// in jsi/v8/V8Runtime.h.
inline std::string get_string_value(JsRuntime& rt, const JsValue& value) {
  return value.utf8(rt);
}

inline JsValue to_js_string(JsRuntime& rt, const std::string& value) {
  return JsValue::createStringFromUtf8(rt, value.data(), value.size());
}

inline JsValue to_js_string(JsRuntime& rt, const char* value) {
  const char* text = value != nullptr ? value : "";
  return JsValue::createStringFromUtf8(rt, text, std::strlen(text));
}

inline JsValue get_property(JsRuntime& rt, const JsValue& object,
                            const char* propertyName) {
  if (!object.isObject()) return undefined();
  return object.asObjectBorrowed(rt).getProperty(rt, propertyName);
}

inline bool has_property(JsRuntime& rt, const JsValue& object,
                         const char* propertyName) {
  if (!object.isObject()) return false;
  return object.asObjectBorrowed(rt).hasProperty(rt, propertyName);
}

// Object::hasProperty walks the prototype chain; napi_has_own_property does
// not, and the callers that used it are distinguishing an own member from an
// inherited one.
bool has_own_property(JsRuntime& rt, const JsObject& object, const char* propertyName);

inline JsValue get_prototype(JsRuntime& rt, const JsValue& object) {
  return get_property(rt, object, "prototype");
}

inline void set_prototype(JsRuntime& rt, JsObject& object,
                          const JsValue& prototype) {
  object.setProperty(rt, "prototype", prototype);
}

JsValue getPrototypeOf(JsRuntime& rt, const JsValue& object);
void setPrototypeOf(JsRuntime& rt, const JsValue& object,
                    const JsValue& prototype);

// napi_util::get__proto__ reads the "__proto__" accessor; going through
// Object.getPrototypeOf instead gives the same answer for every object the
// runtime handles and does not depend on Object.prototype being intact.
inline JsValue get__proto__(JsRuntime& rt, const JsValue& object) {
  return getPrototypeOf(rt, object);
}

bool strict_equal(JsRuntime& rt, const JsValue& lhs, const JsValue& rhs);
bool instance_of(JsRuntime& rt, const JsValue& value, const JsValue& ctor);
bool delete_property(JsRuntime& rt, const JsValue& object, const JsValue& key);

void define_property_value(JsRuntime& rt, const JsObject& object,
                           const char* propertyName, const JsValue& value,
                           bool enumerable = true, bool configurable = true,
                           bool writable = true);

void define_property_get_set(JsRuntime& rt, const JsObject& object,
                             const char* propertyName,
                             const JsFunction* getter,
                             const JsFunction* setter,
                             bool enumerable = true, bool configurable = true);

JsValue valueOf(JsRuntime& rt, const JsValue& value);

bool is_number_object(JsRuntime& rt, const JsValue& value);
bool is_string_object(JsRuntime& rt, const JsValue& value);
bool is_boolean_object(JsRuntime& rt, const JsValue& value);
bool is_date(JsRuntime& rt, const JsValue& value);
bool is_dataview(JsRuntime& rt, const JsValue& value);
bool is_typedarray(JsRuntime& rt, const JsValue& value);
bool is_array(JsRuntime& rt, const JsValue& value);
bool is_float(JsRuntime& rt, const JsValue& value);
bool is_error(JsRuntime& rt, const JsValue& value);

std::string coerce_to_string(JsRuntime& rt, const JsValue& value);

JsValue create_error(JsRuntime& rt, const std::string& message,
                     const char* code = nullptr);

JsValue object_create_from(JsRuntime& rt, const JsValue& prototype);

JsFunction set_function(JsRuntime& rt, JsObject& object, const char* name,
                        engine::HostFunctionType callback);

void inherits(JsRuntime& rt, const JsObject& ctor, const JsObject& superCtor);

}  // namespace js_util
}  // namespace tns

#endif  // NS_FFI_JNI_JSI_ENGINE_H
