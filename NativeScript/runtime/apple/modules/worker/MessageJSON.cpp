//  Message.cpp
//  NativeScript
//
//  Created by Eduardo Speroni on 11/22/23.
//  Copyright © 2023 Progress. All rights reserved.

#ifndef TARGET_ENGINE_V8

#include "MessageJSON.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "js_native_api.h"
#include "jsr_common.h"
#include "runtime/apple/NativeScriptException.h"

#ifdef TARGET_ENGINE_QUICKJS
#include "quickjs.h"
#include "quicks-runtime.h"
#endif

namespace nativescript {
namespace {

enum class ValueTag : uint8_t {
  Undefined = 0,
  Null = 1,
  Boolean = 2,
  Number = 3,
  String = 4,
  BigInt = 5,
  Reference = 6,
  Array = 7,
  Object = 8,
  Date = 9,
  ArrayBuffer = 10,
  TypedArray = 11,
  DataView = 12,
  Map = 13,
  Set = 14,
  RegExp = 15,
  Error = 16,
  SharedArrayBuffer = 17,
};

void Check(napi_env env, napi_status status, const std::string& message) {
  if (status == napi_ok) {
    return;
  }

  bool pendingException = false;
  napi_is_exception_pending(env, &pendingException);
  if (pendingException) {
    napi_value exception = nullptr;
    napi_get_and_clear_last_exception(env, &exception);
    if (exception != nullptr) {
      throw NativeScriptException(env, exception, message);
    }
  }

  throw NativeScriptException(env, message, "DataCloneError");
}

void ThrowDataCloneError(napi_env env, const std::string& message) {
  throw NativeScriptException(env, message, "DataCloneError");
}

napi_value Global(napi_env env) {
  napi_value global = nullptr;
  Check(env, napi_get_global(env, &global), "Unable to read global object");
  return global;
}

napi_value Undefined(napi_env env) {
  napi_value value = nullptr;
  Check(env, napi_get_undefined(env, &value), "Unable to create undefined");
  return value;
}

napi_value Boolean(napi_env env, bool value) {
  napi_value result = nullptr;
  Check(env, napi_get_boolean(env, value, &result), "Unable to create boolean");
  return result;
}

napi_value String(napi_env env, const std::string& value) {
  napi_value result = nullptr;
  Check(env,
        napi_create_string_utf8(env, value.data(), value.size(), &result),
        "Unable to create string");
  return result;
}

std::string ToString(napi_env env, napi_value value) {
  size_t length = 0;
  Check(env, napi_get_value_string_utf8(env, value, nullptr, 0, &length),
        "Unable to read string length");

  std::vector<char> buffer(length + 1);
  size_t written = 0;
  Check(env,
        napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(),
                                   &written),
        "Unable to read string");
  return std::string(buffer.data(), written);
}

napi_value GetNamed(napi_env env, napi_value object, const char* name) {
  napi_value result = nullptr;
  Check(env, napi_get_named_property(env, object, name, &result),
        std::string("Unable to read property ") + name);
  return result;
}

bool IsFunction(napi_env env, napi_value value) {
  napi_valuetype type = napi_undefined;
  Check(env, napi_typeof(env, value, &type), "Unable to inspect value type");
  return type == napi_function;
}

napi_value Call(napi_env env, napi_value receiver, napi_value function,
                const std::vector<napi_value>& args,
                const std::string& message) {
  napi_value result = nullptr;
  Check(env,
        napi_call_function(env, receiver, function, args.size(),
                           args.empty() ? nullptr : args.data(), &result),
        message);
  return result;
}

napi_value Construct(napi_env env, const char* constructorName,
                     const std::vector<napi_value>& args) {
  napi_value ctor = GetNamed(env, Global(env), constructorName);
  if (!IsFunction(env, ctor)) {
    ThrowDataCloneError(env,
                        std::string("Missing constructor ") + constructorName);
  }

  napi_value result = nullptr;
  Check(env,
        napi_new_instance(env, ctor, args.size(),
                          args.empty() ? nullptr : args.data(), &result),
        std::string("Unable to construct ") + constructorName);
  return result;
}

std::mutex g_helperRefsMutex;
std::unordered_map<napi_env, napi_ref> g_helperRefs;

void CleanupStructuredCloneHelper(void* data) {
  napi_env env = static_cast<napi_env>(data);
  napi_ref ref = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_helperRefsMutex);
    auto it = g_helperRefs.find(env);
    if (it == g_helperRefs.end()) {
      return;
    }
    ref = it->second;
    g_helperRefs.erase(it);
  }
  napi_delete_reference(env, ref);
}

napi_value GetStructuredCloneHelper(napi_env env) {
  napi_ref cachedRef = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_helperRefsMutex);
    auto it = g_helperRefs.find(env);
    if (it != g_helperRefs.end()) {
      cachedRef = it->second;
    }
  }
  if (cachedRef != nullptr) {
    napi_value helper = nullptr;
    Check(env, napi_get_reference_value(env, cachedRef, &helper),
          "Unable to read structured clone helper");
    return helper;
  }

  static const char* kHelperSource = R"(
    (function () {
      const hasCtor = (name) => typeof globalThis[name] === 'function';
      return {
        isMap(value) {
          return hasCtor('Map') && value instanceof Map;
        },
        isSet(value) {
          return hasCtor('Set') && value instanceof Set;
        },
        isRegExp(value) {
          return hasCtor('RegExp') && value instanceof RegExp;
        },
        isSharedArrayBuffer(value) {
          return hasCtor('SharedArrayBuffer') && value instanceof SharedArrayBuffer;
        },
        mapEntries(value) {
          return Array.from(value.entries());
        },
        setValues(value) {
          return Array.from(value.values());
        },
        regexpInfo(value) {
          return { source: value.source, flags: value.flags };
        },
        errorInfo(value) {
          return { name: value.name, message: value.message, stack: value.stack };
        },
        bigintToString(value) {
          return value.toString();
        },
        makeBigInt(value) {
          return BigInt(value);
        },
        ownEnumerableKeys(value) {
          return Object.keys(value);
        },
        makeMap() {
          return new Map();
        },
        mapSet(map, key, value) {
          map.set(key, value);
          return map;
        },
        makeSet() {
          return new Set();
        },
        setAdd(set, value) {
          set.add(value);
          return set;
        },
        makeRegExp(source, flags) {
          return new RegExp(source, flags);
        },
        makeError(name, message, stack) {
          const Ctor = typeof globalThis[name] === 'function' ? globalThis[name] : Error;
          const error = new Ctor(message);
          try { error.name = name; } catch (_) {}
          if (stack !== undefined) {
            try { error.stack = stack; } catch (_) {}
          }
          return error;
        },
      };
    })();
  )";

  napi_value source = nullptr;
  Check(env,
        napi_create_string_utf8(env, kHelperSource, NAPI_AUTO_LENGTH, &source),
        "Unable to create structured clone helper source");

  napi_value helper = nullptr;
  Check(env, napi_run_script(env, source, &helper),
        "Unable to install structured clone helper");

  napi_ref ref = nullptr;
  Check(env, napi_create_reference(env, helper, 1, &ref),
        "Unable to retain structured clone helper");
  napi_ref retainedRef = ref;
  bool inserted = false;
  {
    std::lock_guard<std::mutex> lock(g_helperRefsMutex);
    auto entry = g_helperRefs.emplace(env, ref);
    inserted = entry.second;
    retainedRef = entry.first->second;
  }
  if (!inserted) {
    napi_delete_reference(env, ref);
    napi_value cachedHelper = nullptr;
    Check(env, napi_get_reference_value(env, retainedRef, &cachedHelper),
          "Unable to read structured clone helper");
    return cachedHelper;
  }
  napi_status cleanupStatus =
      js_add_env_cleanup_hook(env, CleanupStructuredCloneHelper, env);
  if (cleanupStatus != napi_ok) {
    CleanupStructuredCloneHelper(env);
    Check(env, cleanupStatus,
          "Unable to register structured clone helper cleanup");
  }
  return helper;
}

napi_value CallHelper(napi_env env, const char* name,
                      const std::vector<napi_value>& args) {
  napi_value helper = GetStructuredCloneHelper(env);
  napi_value function = GetNamed(env, helper, name);
  return Call(env, helper, function, args,
              std::string("Unable to call structured clone helper ") + name);
}

bool CallHelperBool(napi_env env, const char* name, napi_value value) {
  napi_value result = CallHelper(env, name, {value});
  bool boolResult = false;
  Check(env, napi_get_value_bool(env, result, &boolResult),
        std::string("Structured clone helper did not return boolean: ") +
            name);
  return boolResult;
}

class BufferWriter {
 public:
  void WriteU8(uint8_t value) { bytes_.push_back(value); }

  void WriteU32(uint32_t value) {
    for (int i = 0; i < 4; i++) {
      WriteU8(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
  }

  void WriteU64(uint64_t value) {
    for (int i = 0; i < 8; i++) {
      WriteU8(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
  }

  void WriteDouble(double value) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double size mismatch");
    std::memcpy(&bits, &value, sizeof(value));
    WriteU64(bits);
  }

  void WriteBytes(const void* data, size_t size) {
    const auto* begin = static_cast<const uint8_t*>(data);
    bytes_.insert(bytes_.end(), begin, begin + size);
  }

  void WriteString(const std::string& value) {
    WriteU32(static_cast<uint32_t>(value.size()));
    WriteBytes(value.data(), value.size());
  }

  MallocedBuffer<char> Release() {
    MallocedBuffer<char> result;
    result.size = bytes_.size();
    result.data = static_cast<char*>(std::malloc(bytes_.size()));
    if (!bytes_.empty()) {
      std::memcpy(result.data, bytes_.data(), bytes_.size());
    }
    return result;
  }

 private:
  std::vector<uint8_t> bytes_;
};

class BufferReader {
 public:
  BufferReader(const char* data, size_t size)
      : current_(reinterpret_cast<const uint8_t*>(data)),
        end_(reinterpret_cast<const uint8_t*>(data) + size) {}

  uint8_t ReadU8() {
    Ensure(1);
    return *current_++;
  }

  uint32_t ReadU32() {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
      value |= static_cast<uint32_t>(ReadU8()) << (i * 8);
    }
    return value;
  }

  uint64_t ReadU64() {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
      value |= static_cast<uint64_t>(ReadU8()) << (i * 8);
    }
    return value;
  }

  double ReadDouble() {
    uint64_t bits = ReadU64();
    double value = 0;
    static_assert(sizeof(bits) == sizeof(value), "double size mismatch");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  std::vector<uint8_t> ReadBytes(size_t size) {
    Ensure(size);
    std::vector<uint8_t> result(current_, current_ + size);
    current_ += size;
    return result;
  }

  std::string ReadString() {
    uint32_t size = ReadU32();
    Ensure(size);
    std::string result(reinterpret_cast<const char*>(current_), size);
    current_ += size;
    return result;
  }

 private:
  void Ensure(size_t size) {
    if (static_cast<size_t>(end_ - current_) < size) {
      throw NativeScriptException("Malformed worker message.");
    }
  }

  const uint8_t* current_;
  const uint8_t* end_;
};

#ifdef TARGET_ENGINE_QUICKJS
JSValue QuickJSValueFromNapi(napi_value value);
#endif

class Serializer {
 public:
  Serializer(napi_env env, BufferWriter& writer,
             std::vector<uint8_t*>* quickjsSharedArrayBuffers = nullptr)
      : env_(env),
        writer_(writer),
        quickjs_shared_array_buffers_(quickjsSharedArrayBuffers) {}

  void Write(napi_value value) {
    napi_valuetype type = napi_undefined;
    Check(env_, napi_typeof(env_, value, &type), "Unable to inspect value");

    switch (type) {
      case napi_undefined:
        WriteTag(ValueTag::Undefined);
        return;
      case napi_null:
        WriteTag(ValueTag::Null);
        return;
      case napi_boolean:
        WriteBoolean(value);
        return;
      case napi_number:
        WriteNumber(value);
        return;
      case napi_string:
        WriteStringValue(value);
        return;
      case napi_bigint:
        WriteBigInt(value);
        return;
      case napi_symbol:
      case napi_function:
      case napi_external:
        ThrowDataCloneError(env_, "Value cannot be cloned.");
        return;
      case napi_object:
        WriteObjectLike(value);
        return;
    }
  }

 private:
  void WriteTag(ValueTag tag) { writer_.WriteU8(static_cast<uint8_t>(tag)); }

  void WriteBoolean(napi_value value) {
    bool boolValue = false;
    Check(env_, napi_get_value_bool(env_, value, &boolValue),
          "Unable to read boolean");
    WriteTag(ValueTag::Boolean);
    writer_.WriteU8(boolValue ? 1 : 0);
  }

  void WriteNumber(napi_value value) {
    double number = 0;
    Check(env_, napi_get_value_double(env_, value, &number),
          "Unable to read number");
    WriteTag(ValueTag::Number);
    writer_.WriteDouble(number);
  }

  void WriteStringValue(napi_value value) {
    WriteTag(ValueTag::String);
    writer_.WriteString(ToString(env_, value));
  }

  void WriteBigInt(napi_value value) {
    napi_value text = CallHelper(env_, "bigintToString", {value});
    WriteTag(ValueTag::BigInt);
    writer_.WriteString(ToString(env_, text));
  }

  bool WriteReferenceIfSeen(napi_value value, uint32_t* idOut) {
    for (uint32_t i = 0; i < seen_.size(); i++) {
      bool equal = false;
      Check(env_, napi_strict_equals(env_, seen_[i], value, &equal),
            "Unable to compare object identity");
      if (equal) {
        WriteTag(ValueTag::Reference);
        writer_.WriteU32(i);
        return true;
      }
    }

    *idOut = static_cast<uint32_t>(seen_.size());
    seen_.push_back(value);
    return false;
  }

  void WriteObjectHeader(ValueTag tag, uint32_t id) {
    WriteTag(tag);
    writer_.WriteU32(id);
  }

  void WriteObjectLike(napi_value value) {
    bool isDate = false;
    Check(env_, napi_is_date(env_, value, &isDate), "Unable to inspect Date");
    if (isDate) {
      WriteDate(value);
      return;
    }

    bool isArrayBuffer = false;
    Check(env_, napi_is_arraybuffer(env_, value, &isArrayBuffer),
          "Unable to inspect ArrayBuffer");
    if (isArrayBuffer) {
#ifdef TARGET_ENGINE_QUICKJS
      if (CallHelperBool(env_, "isSharedArrayBuffer", value)) {
        WriteSharedArrayBuffer(value);
        return;
      }
#else
      if (CallHelperBool(env_, "isSharedArrayBuffer", value)) {
        ThrowDataCloneError(
            env_, "SharedArrayBuffer cloning is not supported by this engine.");
      }
#endif
      WriteArrayBuffer(value);
      return;
    }

    bool isDataView = false;
    Check(env_, napi_is_dataview(env_, value, &isDataView),
          "Unable to inspect DataView");
    if (isDataView) {
      WriteDataView(value);
      return;
    }

    bool isTypedArray = false;
    Check(env_, napi_is_typedarray(env_, value, &isTypedArray),
          "Unable to inspect TypedArray");
    if (isTypedArray) {
      WriteTypedArray(value);
      return;
    }

    bool isArray = false;
    Check(env_, napi_is_array(env_, value, &isArray), "Unable to inspect Array");
    if (isArray) {
      WriteArray(value);
      return;
    }

    if (CallHelperBool(env_, "isMap", value)) {
      WriteMap(value);
      return;
    }

    if (CallHelperBool(env_, "isSet", value)) {
      WriteSet(value);
      return;
    }

    if (CallHelperBool(env_, "isRegExp", value)) {
      WriteRegExp(value);
      return;
    }

    bool isError = false;
    Check(env_, napi_is_error(env_, value, &isError), "Unable to inspect Error");
    if (isError) {
      WriteError(value);
      return;
    }

    WritePlainObject(value);
  }

  void WriteDate(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    double time = 0;
    Check(env_, napi_get_date_value(env_, value, &time), "Unable to read Date");
    WriteObjectHeader(ValueTag::Date, id);
    writer_.WriteDouble(time);
  }

  void WriteArrayBuffer(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    void* data = nullptr;
    size_t byteLength = 0;
    Check(env_, napi_get_arraybuffer_info(env_, value, &data, &byteLength),
          "Unable to read ArrayBuffer");
    WriteObjectHeader(ValueTag::ArrayBuffer, id);
    writer_.WriteU32(static_cast<uint32_t>(byteLength));
    writer_.WriteBytes(data, byteLength);
  }

#ifdef TARGET_ENGINE_QUICKJS
  void WriteSharedArrayBuffer(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    if (quickjs_shared_array_buffers_ == nullptr) {
      ThrowDataCloneError(env_, "SharedArrayBuffer cannot be retained.");
    }

    JSContext* context = qjs_get_context(env_);
    if (context == nullptr) {
      throw NativeScriptException("QuickJS context is not available.");
    }

    size_t byteLength = 0;
    JSSABTab sabTab = {nullptr, 0};
    uint8_t* bytes = JS_WriteObject2(
        context, &byteLength, QuickJSValueFromNapi(value),
        JS_WRITE_OBJ_REFERENCE | JS_WRITE_OBJ_SAB, &sabTab);
    if (bytes == nullptr) {
      JSValue exception = JS_GetException(context);
      napi_value error = nullptr;
      qjs_create_scoped_value(env_, exception, &error);
      throw NativeScriptException(env_, error,
                                  "Unable to serialize SharedArrayBuffer");
    }

    WriteObjectHeader(ValueTag::SharedArrayBuffer, id);
    writer_.WriteU32(static_cast<uint32_t>(byteLength));
    writer_.WriteBytes(bytes, byteLength);
    js_free(context, bytes);

    if (sabTab.tab != nullptr) {
      for (size_t i = 0; i < sabTab.len; i++) {
        qjs_shared_array_buffer_data_retain(sabTab.tab[i]);
        quickjs_shared_array_buffers_->push_back(sabTab.tab[i]);
      }
      js_free(context, sabTab.tab);
    }
  }
#endif

  void WriteTypedArray(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    napi_typedarray_type type = napi_int8_array;
    size_t length = 0;
    napi_value arrayBuffer = nullptr;
    size_t byteOffset = 0;
    Check(env_,
          napi_get_typedarray_info(env_, value, &type, &length, nullptr,
                                   &arrayBuffer, &byteOffset),
          "Unable to read TypedArray");
    WriteObjectHeader(ValueTag::TypedArray, id);
    writer_.WriteU32(static_cast<uint32_t>(type));
    writer_.WriteU32(static_cast<uint32_t>(length));
    writer_.WriteU32(static_cast<uint32_t>(byteOffset));
    Write(arrayBuffer);
  }

  void WriteDataView(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    size_t byteLength = 0;
    napi_value arrayBuffer = nullptr;
    size_t byteOffset = 0;
    Check(env_,
          napi_get_dataview_info(env_, value, &byteLength, nullptr,
                                 &arrayBuffer, &byteOffset),
          "Unable to read DataView");
    WriteObjectHeader(ValueTag::DataView, id);
    writer_.WriteU32(static_cast<uint32_t>(byteLength));
    writer_.WriteU32(static_cast<uint32_t>(byteOffset));
    Write(arrayBuffer);
  }

  void WriteArray(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    uint32_t length = 0;
    Check(env_, napi_get_array_length(env_, value, &length),
          "Unable to read Array length");
    WriteObjectHeader(ValueTag::Array, id);
    writer_.WriteU32(length);
    for (uint32_t i = 0; i < length; i++) {
      napi_value element = nullptr;
      Check(env_, napi_get_element(env_, value, i, &element),
            "Unable to read Array element");
      Write(element);
    }
  }

  void WriteMap(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    napi_value entries = CallHelper(env_, "mapEntries", {value});
    uint32_t length = 0;
    Check(env_, napi_get_array_length(env_, entries, &length),
          "Unable to read Map entries");
    WriteObjectHeader(ValueTag::Map, id);
    writer_.WriteU32(length);
    for (uint32_t i = 0; i < length; i++) {
      napi_value pair = nullptr;
      napi_value key = nullptr;
      napi_value itemValue = nullptr;
      Check(env_, napi_get_element(env_, entries, i, &pair),
            "Unable to read Map pair");
      Check(env_, napi_get_element(env_, pair, 0, &key),
            "Unable to read Map key");
      Check(env_, napi_get_element(env_, pair, 1, &itemValue),
            "Unable to read Map value");
      Write(key);
      Write(itemValue);
    }
  }

  void WriteSet(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    napi_value values = CallHelper(env_, "setValues", {value});
    uint32_t length = 0;
    Check(env_, napi_get_array_length(env_, values, &length),
          "Unable to read Set values");
    WriteObjectHeader(ValueTag::Set, id);
    writer_.WriteU32(length);
    for (uint32_t i = 0; i < length; i++) {
      napi_value itemValue = nullptr;
      Check(env_, napi_get_element(env_, values, i, &itemValue),
            "Unable to read Set value");
      Write(itemValue);
    }
  }

  void WriteRegExp(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    napi_value info = CallHelper(env_, "regexpInfo", {value});
    WriteObjectHeader(ValueTag::RegExp, id);
    writer_.WriteString(ToString(env_, GetNamed(env_, info, "source")));
    writer_.WriteString(ToString(env_, GetNamed(env_, info, "flags")));
  }

  void WriteError(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    napi_value info = CallHelper(env_, "errorInfo", {value});
    WriteObjectHeader(ValueTag::Error, id);
    writer_.WriteString(ToString(env_, GetNamed(env_, info, "name")));
    writer_.WriteString(ToString(env_, GetNamed(env_, info, "message")));

    napi_value stack = GetNamed(env_, info, "stack");
    napi_valuetype stackType = napi_undefined;
    Check(env_, napi_typeof(env_, stack, &stackType), "Unable to inspect stack");
    writer_.WriteU8(stackType == napi_string ? 1 : 0);
    if (stackType == napi_string) {
      writer_.WriteString(ToString(env_, stack));
    }
  }

  void WritePlainObject(napi_value value) {
    uint32_t id = 0;
    if (WriteReferenceIfSeen(value, &id)) {
      return;
    }

    napi_value keys = CallHelper(env_, "ownEnumerableKeys", {value});
    uint32_t length = 0;
    Check(env_, napi_get_array_length(env_, keys, &length),
          "Unable to read object keys");
    WriteObjectHeader(ValueTag::Object, id);
    writer_.WriteU32(length);
    for (uint32_t i = 0; i < length; i++) {
      napi_value key = nullptr;
      napi_value itemValue = nullptr;
      Check(env_, napi_get_element(env_, keys, i, &key),
            "Unable to read object key");
      Check(env_, napi_get_property(env_, value, key, &itemValue),
            "Unable to read object property");
      writer_.WriteString(ToString(env_, key));
      Write(itemValue);
    }
  }

  napi_env env_;
  BufferWriter& writer_;
  std::vector<napi_value> seen_;
  std::vector<uint8_t*>* quickjs_shared_array_buffers_;
};

class Deserializer {
 public:
  Deserializer(napi_env env, BufferReader& reader)
      : env_(env), reader_(reader) {}

  napi_value Read() {
    ValueTag tag = static_cast<ValueTag>(reader_.ReadU8());
    switch (tag) {
      case ValueTag::Undefined:
        return Undefined(env_);
      case ValueTag::Null:
        return Null();
      case ValueTag::Boolean:
        return Boolean(env_, reader_.ReadU8() != 0);
      case ValueTag::Number:
        return Number();
      case ValueTag::String:
        return String(env_, reader_.ReadString());
      case ValueTag::BigInt:
        return CallHelper(env_, "makeBigInt", {String(env_, reader_.ReadString())});
      case ValueTag::Reference:
        return Reference();
      case ValueTag::Array:
        return Array();
      case ValueTag::Object:
        return Object();
      case ValueTag::Date:
        return Date();
      case ValueTag::ArrayBuffer:
        return ArrayBuffer();
      case ValueTag::TypedArray:
        return TypedArray();
      case ValueTag::DataView:
        return DataView();
      case ValueTag::Map:
        return Map();
      case ValueTag::Set:
        return Set();
      case ValueTag::RegExp:
        return RegExp();
      case ValueTag::Error:
        return Error();
      case ValueTag::SharedArrayBuffer:
#ifdef TARGET_ENGINE_QUICKJS
        return SharedArrayBuffer();
#else
        throw NativeScriptException(
            "SharedArrayBuffer worker messages are not supported by this engine.");
#endif
    }

    throw NativeScriptException("Malformed worker message.");
  }

 private:
  napi_value Null() {
    napi_value value = nullptr;
    Check(env_, napi_get_null(env_, &value), "Unable to create null");
    return value;
  }

  napi_value Number() {
    napi_value value = nullptr;
    Check(env_, napi_create_double(env_, reader_.ReadDouble(), &value),
          "Unable to create number");
    return value;
  }

  napi_value Reference() {
    uint32_t id = reader_.ReadU32();
    if (id >= refs_.size() || refs_[id] == nullptr) {
      throw NativeScriptException("Malformed worker message reference.");
    }
    return refs_[id];
  }

  void StoreRef(uint32_t id, napi_value value) {
    if (id >= refs_.size()) {
      refs_.resize(id + 1, nullptr);
    }
    refs_[id] = value;
  }

  napi_value Array() {
    uint32_t id = reader_.ReadU32();
    uint32_t length = reader_.ReadU32();
    napi_value array = nullptr;
    Check(env_, napi_create_array_with_length(env_, length, &array),
          "Unable to create Array");
    StoreRef(id, array);
    for (uint32_t i = 0; i < length; i++) {
      napi_value element = Read();
      Check(env_, napi_set_element(env_, array, i, element),
            "Unable to set Array element");
    }
    return array;
  }

  napi_value Object() {
    uint32_t id = reader_.ReadU32();
    uint32_t length = reader_.ReadU32();
    napi_value object = nullptr;
    Check(env_, napi_create_object(env_, &object), "Unable to create Object");
    StoreRef(id, object);
    for (uint32_t i = 0; i < length; i++) {
      std::string key = reader_.ReadString();
      napi_value value = Read();
      Check(env_, napi_set_named_property(env_, object, key.c_str(), value),
            "Unable to set object property");
    }
    return object;
  }

  napi_value Date() {
    uint32_t id = reader_.ReadU32();
    napi_value date = nullptr;
    Check(env_, napi_create_date(env_, reader_.ReadDouble(), &date),
          "Unable to create Date");
    StoreRef(id, date);
    return date;
  }

  napi_value ArrayBuffer() {
    uint32_t id = reader_.ReadU32();
    uint32_t byteLength = reader_.ReadU32();
    std::vector<uint8_t> bytes = reader_.ReadBytes(byteLength);
    void* data = nullptr;
    napi_value arrayBuffer = nullptr;
    Check(env_, napi_create_arraybuffer(env_, byteLength, &data, &arrayBuffer),
          "Unable to create ArrayBuffer");
    if (byteLength != 0) {
      std::memcpy(data, bytes.data(), byteLength);
    }
    StoreRef(id, arrayBuffer);
    return arrayBuffer;
  }

#ifdef TARGET_ENGINE_QUICKJS
  napi_value SharedArrayBuffer() {
    uint32_t id = reader_.ReadU32();
    uint32_t byteLength = reader_.ReadU32();
    std::vector<uint8_t> bytes = reader_.ReadBytes(byteLength);

    JSContext* context = qjs_get_context(env_);
    if (context == nullptr) {
      throw NativeScriptException("QuickJS context is not available.");
    }

    JSValue value =
        JS_ReadObject(context, bytes.data(), bytes.size(),
                      JS_READ_OBJ_REFERENCE | JS_READ_OBJ_SAB);
    if (JS_IsException(value)) {
      JSValue exception = JS_GetException(context);
      napi_value error = nullptr;
      qjs_create_scoped_value(env_, exception, &error);
      throw NativeScriptException(env_, error,
                                  "Unable to deserialize SharedArrayBuffer");
    }

    napi_value result = nullptr;
    Check(env_, qjs_create_scoped_value(env_, value, &result),
          "Unable to create SharedArrayBuffer value");
    StoreRef(id, result);
    return result;
  }
#endif

  napi_value TypedArray() {
    uint32_t id = reader_.ReadU32();
    auto type = static_cast<napi_typedarray_type>(reader_.ReadU32());
    uint32_t length = reader_.ReadU32();
    uint32_t byteOffset = reader_.ReadU32();
    napi_value arrayBuffer = Read();
    napi_value typedArray = nullptr;
    Check(env_,
          napi_create_typedarray(env_, type, length, arrayBuffer, byteOffset,
                                 &typedArray),
          "Unable to create TypedArray");
    StoreRef(id, typedArray);
    return typedArray;
  }

  napi_value DataView() {
    uint32_t id = reader_.ReadU32();
    uint32_t byteLength = reader_.ReadU32();
    uint32_t byteOffset = reader_.ReadU32();
    napi_value arrayBuffer = Read();
    napi_value dataView = nullptr;
    Check(env_,
          napi_create_dataview(env_, byteLength, arrayBuffer, byteOffset,
                               &dataView),
          "Unable to create DataView");
    StoreRef(id, dataView);
    return dataView;
  }

  napi_value Map() {
    uint32_t id = reader_.ReadU32();
    uint32_t length = reader_.ReadU32();
    napi_value map = CallHelper(env_, "makeMap", {});
    StoreRef(id, map);
    for (uint32_t i = 0; i < length; i++) {
      napi_value key = Read();
      napi_value value = Read();
      CallHelper(env_, "mapSet", {map, key, value});
    }
    return map;
  }

  napi_value Set() {
    uint32_t id = reader_.ReadU32();
    uint32_t length = reader_.ReadU32();
    napi_value set = CallHelper(env_, "makeSet", {});
    StoreRef(id, set);
    for (uint32_t i = 0; i < length; i++) {
      napi_value value = Read();
      CallHelper(env_, "setAdd", {set, value});
    }
    return set;
  }

  napi_value RegExp() {
    uint32_t id = reader_.ReadU32();
    napi_value regexp = CallHelper(env_, "makeRegExp",
                                   {String(env_, reader_.ReadString()),
                                    String(env_, reader_.ReadString())});
    StoreRef(id, regexp);
    return regexp;
  }

  napi_value Error() {
    uint32_t id = reader_.ReadU32();
    std::string name = reader_.ReadString();
    std::string message = reader_.ReadString();
    napi_value stack = Undefined(env_);
    if (reader_.ReadU8() != 0) {
      stack = String(env_, reader_.ReadString());
    }
    napi_value error =
        CallHelper(env_, "makeError", {String(env_, name), String(env_, message), stack});
    StoreRef(id, error);
    return error;
  }

  napi_env env_;
  BufferReader& reader_;
  std::vector<napi_value> refs_;
};

#ifdef TARGET_ENGINE_QUICKJS
JSValue QuickJSValueFromNapi(napi_value value) {
  return *reinterpret_cast<JSValue*>(value);
}

MallocedBuffer<char> CopyToMallocedBuffer(const uint8_t* data, size_t size) {
  MallocedBuffer<char> result;
  result.size = size;
  result.data = static_cast<char*>(std::malloc(size));
  if (size != 0) {
    std::memcpy(result.data, data, size);
  }
  return result;
}

void ClearQuickJSException(JSContext* context) {
  JSValue exception = JS_GetException(context);
  JS_FreeValue(context, exception);
}
#endif

}  // namespace

namespace worker {

bool Message::Serialize(napi_env env, napi_value input) {
#ifdef TARGET_ENGINE_QUICKJS
  ReleaseQuickJSSharedArrayBuffers();

  JSContext* context = qjs_get_context(env);
  if (context != nullptr) {
    size_t byteLength = 0;
    JSSABTab sabTab = {nullptr, 0};
    uint8_t* bytes = JS_WriteObject2(
        context, &byteLength, QuickJSValueFromNapi(input),
        JS_WRITE_OBJ_REFERENCE | JS_WRITE_OBJ_SAB, &sabTab);
    if (bytes != nullptr) {
      main_message_buf_ = CopyToMallocedBuffer(bytes, byteLength);
      js_free(context, bytes);
      if (sabTab.tab != nullptr && sabTab.len != 0) {
        quickjs_shared_array_buffers_.assign(sabTab.tab,
                                             sabTab.tab + sabTab.len);
        for (uint8_t* sharedArrayBuffer : quickjs_shared_array_buffers_) {
          qjs_shared_array_buffer_data_retain(sharedArrayBuffer);
        }
      } else {
        quickjs_shared_array_buffers_.clear();
      }
      if (sabTab.tab != nullptr) {
        js_free(context, sabTab.tab);
      }
      is_quickjs_native_message_ = true;
      return true;
    }

    ClearQuickJSException(context);
  }

  is_quickjs_native_message_ = false;
  quickjs_shared_array_buffers_.clear();
#endif

  BufferWriter writer;
#ifdef TARGET_ENGINE_QUICKJS
  Serializer serializer(env, writer, &quickjs_shared_array_buffers_);
#else
  Serializer serializer(env, writer);
#endif
  serializer.Write(input);
  main_message_buf_ = writer.Release();
  return true;
}

napi_value Message::Deserialize(napi_env env) {
#ifdef TARGET_ENGINE_QUICKJS
  if (is_quickjs_native_message_) {
    JSContext* context = qjs_get_context(env);
    if (context == nullptr) {
      throw NativeScriptException("QuickJS context is not available.");
    }

    JSValue value = JS_ReadObject(
        context, reinterpret_cast<const uint8_t*>(main_message_buf_.data),
        main_message_buf_.size, JS_READ_OBJ_REFERENCE | JS_READ_OBJ_SAB);
    if (JS_IsException(value)) {
      JSValue exception = JS_GetException(context);
      napi_value error = nullptr;
      qjs_create_scoped_value(env, exception, &error);
      throw NativeScriptException(env, error, "Unable to deserialize worker message");
    }

    napi_value result = nullptr;
    Check(env, qjs_create_scoped_value(env, value, &result),
          "Unable to create worker message value");
    ReleaseQuickJSSharedArrayBuffers();
    return result;
  }
#endif

  BufferReader reader(main_message_buf_.data, main_message_buf_.size);
  Deserializer deserializer(env, reader);
  napi_value result = deserializer.Read();
#ifdef TARGET_ENGINE_QUICKJS
  ReleaseQuickJSSharedArrayBuffers();
#endif
  return result;
}

Message::Message(MallocedBuffer<char>&& payload)
    : main_message_buf_(std::move(payload)) {}

Message::Message(Message&& other) noexcept
    : main_message_buf_(std::move(other.main_message_buf_)) {
#ifdef TARGET_ENGINE_QUICKJS
  is_quickjs_native_message_ = other.is_quickjs_native_message_;
  quickjs_shared_array_buffers_ =
      std::move(other.quickjs_shared_array_buffers_);
  other.is_quickjs_native_message_ = false;
  other.quickjs_shared_array_buffers_.clear();
#endif
}

Message& Message::operator=(Message&& other) noexcept {
  if (this == &other) {
    return *this;
  }

#ifdef TARGET_ENGINE_QUICKJS
  ReleaseQuickJSSharedArrayBuffers();
#endif
  main_message_buf_ = std::move(other.main_message_buf_);
#ifdef TARGET_ENGINE_QUICKJS
  is_quickjs_native_message_ = other.is_quickjs_native_message_;
  quickjs_shared_array_buffers_ =
      std::move(other.quickjs_shared_array_buffers_);
  other.is_quickjs_native_message_ = false;
  other.quickjs_shared_array_buffers_.clear();
#endif
  return *this;
}

Message::~Message() {
#ifdef TARGET_ENGINE_QUICKJS
  ReleaseQuickJSSharedArrayBuffers();
#endif
}

#ifdef TARGET_ENGINE_QUICKJS
void Message::ReleaseQuickJSSharedArrayBuffers() {
  for (uint8_t* sharedArrayBuffer : quickjs_shared_array_buffers_) {
    qjs_shared_array_buffer_data_release(sharedArrayBuffer);
  }
  quickjs_shared_array_buffers_.clear();
}
#endif

};  // namespace worker
};  // namespace nativescript

#endif  // TARGET_ENGINE_V8
