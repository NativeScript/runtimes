#include "Engine.h"

#include <mutex>
#include <unordered_map>

using namespace tns;
using namespace tns::js_util;

namespace {

std::mutex g_builtinsMutex;
// Keyed by JsRuntime::identity(): a host callback receives a freshly
// constructed Runtime wrapper, so &rt is not stable and keying on it would
// build (and leak) a whole Builtins set per callback.
std::unordered_map<const void*, std::unique_ptr<Builtins>> g_builtins;

// `a === b` and `a instanceof b` have no engine:: entry point and no builtin to
// borrow, so they come from two one-line scripts evaluated once per runtime.
const char* const kStrictEqualsSource = "(function(a,b){return a===b;})";
const char* const kInstanceOfSource = "(function(o,c){return o instanceof c;})";

JsFunction evaluateFunction(JsRuntime& rt, const char* source,
                            const char* sourceURL) {
  JsValue result = rt.evaluateJavaScript(
      std::make_shared<engine::StringBuffer>(std::string(source)), sourceURL);
  return result.asObject(rt).asFunction(rt);
}

std::unique_ptr<Builtins> createBuiltins(JsRuntime& rt) {
  auto builtins = std::make_unique<Builtins>();
  JsObject global = rt.global();

  builtins->objectCtor = global.getPropertyAsObject(rt, "Object");
  builtins->defineProperty =
      builtins->objectCtor.getPropertyAsFunction(rt, "defineProperty");
  builtins->getPrototypeOf =
      builtins->objectCtor.getPropertyAsFunction(rt, "getPrototypeOf");
  builtins->setPrototypeOf =
      builtins->objectCtor.getPropertyAsFunction(rt, "setPrototypeOf");
  builtins->objectCreate = builtins->objectCtor.getPropertyAsFunction(rt, "create");
  builtins->objectKeys = builtins->objectCtor.getPropertyAsFunction(rt, "keys");
  builtins->hasOwnProperty = builtins->objectCtor.getPropertyAsObject(rt, "prototype")
                                 .getPropertyAsFunction(rt, "hasOwnProperty");

  JsObject reflect = global.getPropertyAsObject(rt, "Reflect");
  builtins->deleteProperty = reflect.getPropertyAsFunction(rt, "deleteProperty");

  builtins->numberCtor = global.getPropertyAsObject(rt, "Number");
  builtins->isInteger = builtins->numberCtor.getPropertyAsFunction(rt, "isInteger");

  builtins->stringCtor = global.getPropertyAsObject(rt, "String");
  builtins->booleanCtor = global.getPropertyAsObject(rt, "Boolean");
  builtins->dateCtor = global.getPropertyAsObject(rt, "Date");
  builtins->dataViewCtor = global.getPropertyAsObject(rt, "DataView");

  JsObject arrayCtor = global.getPropertyAsObject(rt, "Array");
  builtins->isArray = arrayCtor.getPropertyAsFunction(rt, "isArray");

  JsObject arrayBufferCtor = global.getPropertyAsObject(rt, "ArrayBuffer");
  builtins->isView = arrayBufferCtor.getPropertyAsFunction(rt, "isView");

  builtins->errorCtor = global.getPropertyAsFunction(rt, "Error");
  builtins->stringCoerce = global.getPropertyAsFunction(rt, "String");

  builtins->strictEquals =
      evaluateFunction(rt, kStrictEqualsSource, "<ns-strict-equals>");
  builtins->instanceOf =
      evaluateFunction(rt, kInstanceOfSource, "<ns-instance-of>");

  return builtins;
}

bool callPredicate(JsRuntime& rt, const JsFunction& fn, const JsValue& lhs,
                   const JsValue& rhs) {
  const JsValue args[] = {lhs, rhs};
  JsValue result = fn.call(rt, args, static_cast<size_t>(2));
  return result.isBool() && result.getBool();
}

}  // namespace

Builtins& Builtins::of(JsRuntime& rt) {
  std::lock_guard<std::mutex> lock(g_builtinsMutex);
  auto it = g_builtins.find(rt.identity());
  if (it != g_builtins.end()) return *it->second;
  auto inserted = g_builtins.emplace(rt.identity(), createBuiltins(rt));
  return *inserted.first->second;
}

void Builtins::dispose(JsRuntime& rt) {
  std::lock_guard<std::mutex> lock(g_builtinsMutex);
  g_builtins.erase(rt.identity());
}

JsValue js_util::getPrototypeOf(JsRuntime& rt, const JsValue& object) {
  if (!object.isObject()) return undefined();
  const JsValue args[] = {object};
  return Builtins::of(rt).getPrototypeOf.call(rt, args, static_cast<size_t>(1));
}

void js_util::setPrototypeOf(JsRuntime& rt, const JsValue& object,
                             const JsValue& prototype) {
  if (!object.isObject()) return;
  const JsValue args[] = {object, prototype};
  Builtins::of(rt).setPrototypeOf.call(rt, args, static_cast<size_t>(2));
}

bool js_util::strict_equal(JsRuntime& rt, const JsValue& lhs,
                           const JsValue& rhs) {
  return callPredicate(rt, Builtins::of(rt).strictEquals, lhs, rhs);
}

bool js_util::instance_of(JsRuntime& rt, const JsValue& value,
                          const JsValue& ctor) {
  return callPredicate(rt, Builtins::of(rt).instanceOf, value, ctor);
}

bool js_util::delete_property(JsRuntime& rt, const JsValue& object,
                              const JsValue& key) {
  return callPredicate(rt, Builtins::of(rt).deleteProperty, object, key);
}

void js_util::define_property_value(JsRuntime& rt, const JsObject& object,
                                    const char* propertyName,
                                    const JsValue& value, bool enumerable,
                                    bool configurable, bool writable) {
  JsObject descriptor(rt);
  descriptor.setProperty(rt, "value", value);
  descriptor.setProperty(rt, "enumerable", enumerable);
  descriptor.setProperty(rt, "configurable", configurable);
  descriptor.setProperty(rt, "writable", writable);

  const JsValue args[] = {JsValue(rt, object), to_js_string(rt, propertyName),
                          JsValue(rt, descriptor)};
  Builtins::of(rt).defineProperty.call(rt, args, static_cast<size_t>(3));
}

void js_util::define_property_get_set(JsRuntime& rt, const JsObject& object,
                                      const char* propertyName,
                                      const JsFunction* getter,
                                      const JsFunction* setter, bool enumerable,
                                      bool configurable) {
  JsObject descriptor(rt);
  if (getter != nullptr) descriptor.setProperty(rt, "get", *getter);
  if (setter != nullptr) descriptor.setProperty(rt, "set", *setter);
  descriptor.setProperty(rt, "enumerable", enumerable);
  descriptor.setProperty(rt, "configurable", configurable);

  const JsValue args[] = {JsValue(rt, object), to_js_string(rt, propertyName),
                          JsValue(rt, descriptor)};
  Builtins::of(rt).defineProperty.call(rt, args, static_cast<size_t>(3));
}

bool js_util::has_own_property(JsRuntime& rt, const JsObject& object,
                               const char* propertyName) {
  const JsValue args[] = {to_js_string(rt, propertyName)};
  JsValue result = Builtins::of(rt).hasOwnProperty.callWithThis(
      rt, object, args, static_cast<size_t>(1));
  return result.isBool() && result.getBool();
}

JsValue js_util::valueOf(JsRuntime& rt, const JsValue& value) {
  if (!value.isObject()) return value;
  JsObject object = value.asObjectBorrowed(rt);
  JsValue fn = object.getProperty(rt, "valueOf");
  if (!fn.isObject()) return value;
  JsObject fnObject = fn.asObjectBorrowed(rt);
  if (!fnObject.isFunction(rt)) return value;
  return fnObject.asFunction(rt).callWithThis(rt, object, nullptr, 0);
}

bool js_util::is_number_object(JsRuntime& rt, const JsValue& value) {
  return instance_of(rt, value, JsValue(rt, Builtins::of(rt).numberCtor));
}

bool js_util::is_string_object(JsRuntime& rt, const JsValue& value) {
  return instance_of(rt, value, JsValue(rt, Builtins::of(rt).stringCtor));
}

bool js_util::is_boolean_object(JsRuntime& rt, const JsValue& value) {
  return instance_of(rt, value, JsValue(rt, Builtins::of(rt).booleanCtor));
}

bool js_util::is_date(JsRuntime& rt, const JsValue& value) {
  return instance_of(rt, value, JsValue(rt, Builtins::of(rt).dateCtor));
}

bool js_util::is_dataview(JsRuntime& rt, const JsValue& value) {
  return instance_of(rt, value, JsValue(rt, Builtins::of(rt).dataViewCtor));
}

// napi_is_typedarray has no builtin equivalent; ArrayBuffer.isView is true for
// every typed array plus DataView, so the DataView case is subtracted.
bool js_util::is_typedarray(JsRuntime& rt, const JsValue& value) {
  const JsValue args[] = {value};
  JsValue result =
      Builtins::of(rt).isView.call(rt, args, static_cast<size_t>(1));
  if (!(result.isBool() && result.getBool())) return false;
  return !is_dataview(rt, value);
}

bool js_util::is_array(JsRuntime& rt, const JsValue& value) {
  const JsValue args[] = {value};
  JsValue result =
      Builtins::of(rt).isArray.call(rt, args, static_cast<size_t>(1));
  return result.isBool() && result.getBool();
}

bool js_util::is_float(JsRuntime& rt, const JsValue& value) {
  const JsValue args[] = {value};
  JsValue result =
      Builtins::of(rt).isInteger.call(rt, args, static_cast<size_t>(1));
  return !(result.isBool() && result.getBool());
}

JsValue js_util::object_create_from(JsRuntime& rt, const JsValue& prototype) {
  const JsValue args[] = {prototype};
  return Builtins::of(rt).objectCreate.call(rt, args, static_cast<size_t>(1));
}

JsFunction js_util::set_function(JsRuntime& rt, JsObject& object,
                                 const char* name,
                                 engine::HostFunctionType callback) {
  JsFunction fn = JsFunction::createFromHostFunction(
      rt, JsPropNameID::forAscii(rt, name), 0, std::move(callback));
  object.setProperty(rt, name, fn);
  return fn;
}

void js_util::inherits(JsRuntime& rt, const JsObject& ctor,
                       const JsObject& superCtor) {
  setPrototypeOf(rt, ctor.getProperty(rt, "prototype"),
                 superCtor.getProperty(rt, "prototype"));
  setPrototypeOf(rt, JsValue(rt, ctor), JsValue(rt, superCtor));
}

bool js_util::is_error(JsRuntime& rt, const JsValue& value) {
  return instance_of(rt, value, JsValue(rt, Builtins::of(rt).errorCtor));
}

// napi_coerce_to_string has no engine:: entry point; String(value) is the same
// abstract operation and works for every value kind, including symbols.
std::string js_util::coerce_to_string(JsRuntime& rt, const JsValue& value) {
  if (value.isString()) return value.asString(rt).utf8(rt);
  const JsValue args[] = {value};
  JsValue result =
      Builtins::of(rt).stringCoerce.call(rt, args, static_cast<size_t>(1));
  return result.isString() ? result.asString(rt).utf8(rt) : std::string();
}

JsValue js_util::create_error(JsRuntime& rt, const std::string& message,
                              const char* code) {
  // Rebuild the original error type from a "<Name>Error: " prefix.
  //
  // A JSError that carries its thrown value keeps its constructor for free, but
  // one built from a message alone would otherwise always come back as a plain
  // Error. Hermes is where this shows: it reports a *compile* failure as a
  // JSINativeException rather than a JS throw, so jsi/hermes tags the message
  // "SyntaxError: ..." precisely so the type can be restored here. Every other
  // engine raises a real SyntaxError with a value and never reaches this path.
  // The Require specs assert the type ("main started SyntaxError main ended").
  JsFunction ctor = Builtins::of(rt).errorCtor;
  std::string text = message;
  static const char* kErrorNames[] = {"SyntaxError", "TypeError",  "RangeError",
                                      "ReferenceError", "EvalError", "URIError"};
  for (const char* name : kErrorNames) {
    const std::string prefix = std::string(name) + ": ";
    if (text.rfind(prefix, 0) != 0) continue;
    JsValue candidate = rt.global().getProperty(rt, name);
    if (candidate.isObject() && candidate.asObjectBorrowed(rt).isFunction(rt)) {
      ctor = candidate.asObject(rt).asFunction(rt);
      text = text.substr(prefix.size());
    }
    break;
  }

  const JsValue args[] = {to_js_string(rt, text)};
  JsValue error = ctor.callAsConstructor(rt, args, static_cast<size_t>(1));
  if (code != nullptr && error.isObject()) {
    JsObject errorObject = error.asObject(rt);
    errorObject.setProperty(rt, "code", to_js_string(rt, code));
  }
  return error;
}
