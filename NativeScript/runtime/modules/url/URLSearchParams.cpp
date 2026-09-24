#include "URLSearchParams.h"

#include <string>

#include "native_api_util.h"

using namespace ada;
using namespace nativescript;

namespace {
bool EnsureConstructorThis(napi_env env, const char* constructorName,
                           napi_value* jsThis) {
  if (jsThis == nullptr) {
    return false;
  }

  napi_valuetype thisType = napi_undefined;
  if (*jsThis != nullptr && napi_typeof(env, *jsThis, &thisType) == napi_ok &&
      (thisType == napi_object || thisType == napi_function)) {
    return true;
  }

  napi_value global = nullptr;
  napi_value ctor = nullptr;
  napi_value prototype = nullptr;
  napi_value objectCtor = nullptr;
  napi_value setPrototypeOf = nullptr;

  if (napi_create_object(env, jsThis) != napi_ok || *jsThis == nullptr) {
    return false;
  }

  if (napi_get_global(env, &global) != napi_ok ||
      napi_get_named_property(env, global, constructorName, &ctor) != napi_ok ||
      ctor == nullptr ||
      napi_get_named_property(env, ctor, "prototype", &prototype) != napi_ok ||
      prototype == nullptr ||
      napi_get_named_property(env, global, "Object", &objectCtor) != napi_ok ||
      objectCtor == nullptr ||
      napi_get_named_property(env, objectCtor, "setPrototypeOf",
                              &setPrototypeOf) != napi_ok ||
      setPrototypeOf == nullptr) {
    return false;
  }

  napi_value argv[2] = {*jsThis, prototype};
  return napi_call_function(env, objectCtor, setPrototypeOf, 2, argv,
                            nullptr) == napi_ok;
}

void ThrowTypeError(napi_env env, const char* msg) {
  napi_throw_type_error(env, nullptr, msg);
}

napi_value js_str(napi_env env, std::string_view s) {
  napi_value v;
  napi_create_string_utf8(env, s.data(), s.length(), &v);
  return v;
}

// Brand-checked unwrap of an already-retrieved receiver. Throws "Illegal
// invocation" (TypeError) when it is not a wrapped URLSearchParams.
URLSearchParams* GetInstanceOf(napi_env env, napi_value jsThis) {
  URLSearchParams* instance = nullptr;
  if (napi_unwrap(env, jsThis, reinterpret_cast<void**>(&instance)) !=
          napi_ok ||
      instance == nullptr) {
    ThrowTypeError(env, "Illegal invocation");
    return nullptr;
  }
  return instance;
}

// Brand-checked instance retrieval: throws "Illegal invocation" (TypeError)
// when the receiver is not a wrapped URLSearchParams, matching the spec.
URLSearchParams* GetInstance(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr) !=
      napi_ok) {
    return nullptr;
  }

  URLSearchParams* instance = nullptr;
  if (napi_unwrap(env, jsThis, reinterpret_cast<void**>(&instance)) !=
          napi_ok ||
      instance == nullptr) {
    ThrowTypeError(env, "Illegal invocation");
    return nullptr;
  }

  return instance;
}

napi_value WellKnownSymbol(napi_env env, const char* name) {
  napi_value global, symbolCtor, sym;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Symbol", &symbolCtor);
  napi_get_named_property(env, symbolCtor, name, &sym);
  return sym;
}

napi_value SymbolIterator(napi_env env) {
  return WellKnownSymbol(env, "iterator");
}

// WebIDL USVString coercion: ToString (invokes user toString, throws for
// Symbol). Returns false leaving a pending exception on failure.
bool ValueToString(napi_env env, napi_value v, std::string& out) {
  napi_value str;
  if (napi_coerce_to_string(env, v, &str) != napi_ok) {
    return false;
  }
  size_t len = 0;
  if (napi_get_value_string_utf8(env, str, nullptr, 0, &len) != napi_ok) {
    return false;
  }
  std::vector<char> buf(len + 1);
  napi_get_value_string_utf8(env, str, buf.data(), len + 1, nullptr);
  out.assign(buf.data(), len);
  return true;
}

enum IterKind { ITER_KEYS = 0, ITER_VALUES = 1, ITER_ENTRIES = 2 };

// Per-iterator state: tracks the current index and what to yield. Freed by the
// iterator object's finalizer.
struct IterState {
  uint32_t idx;
  int kind;
};

// Internal, non-enumerable keys the iterator carries:
//  - ITER_SOURCE_KEY: a strong reference to the source URLSearchParams (kept
//    alive for `next`); released by ordinary property teardown.
//  - ITER_BRAND_KEY: a napi_external wrapping this iterator's IterState*, used
//    by `next` for the receiver brand check.
// Holding these as properties (not napi_refs) means the iterator's finalizer
// never has to delete a reference — illegal during the GC sweep on every
// engine — so it can stay a plain C++ delete (see MakeIterator).
static constexpr const char* ITER_SOURCE_KEY = "__nsSource";
static constexpr const char* ITER_BRAND_KEY = "__nsBrand";

// ES IteratorClose: best-effort call to the iterator's return() on abrupt
// completion (so a generator's `finally` runs), preserving any pending
// exception.
void IteratorClose(napi_env env, napi_value iterObj) {
  napi_value savedEx = nullptr;
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  if (pending) {
    napi_get_and_clear_last_exception(env, &savedEx);
  }
  napi_value returnFn;
  if (napi_get_named_property(env, iterObj, "return", &returnFn) == napi_ok &&
      napi_util::is_of_type(env, returnFn, napi_function)) {
    napi_value res;
    napi_call_function(env, iterObj, returnFn, 0, nullptr, &res);
    bool p2 = false;
    napi_is_exception_pending(env, &p2);
    if (p2) {
      napi_value e;
      napi_get_and_clear_last_exception(env, &e);
    }
  }
  if (savedEx != nullptr) {
    napi_throw(env, savedEx);
  }
}

// The `next` function of a live URLSearchParams iterator.
napi_value IteratorNext(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr) !=
      napi_ok) {
    ThrowTypeError(env, "Illegal invocation");
    return nullptr;
  }

  // Brand check *and* state lookup in one step: the iterator state is read out
  // of the receiver's own external property. A foreign receiver (next.call({}))
  // carries no such property and is rejected, which is what the spec requires.
  //
  // The state is deliberately NOT taken from this callback's `data`, even
  // though MakeIterator passes it there too: that would require the pointer to
  // survive a round trip through two different Node-API mechanisms, and PrimJS
  // does not hand back the same address, so every next() call failed the brand
  // check and threw.
  napi_value brandVal = nullptr;
  void* brand = nullptr;
  if (napi_get_named_property(env, jsThis, ITER_BRAND_KEY, &brandVal) !=
          napi_ok ||
      napi_util::is_null_or_undefined(env, brandVal) ||
      napi_get_value_external(env, brandVal, &brand) != napi_ok ||
      brand == nullptr) {
    ThrowTypeError(env, "Illegal invocation");
    return nullptr;
  }
  auto* st = static_cast<IterState*>(brand);

  napi_value result;
  napi_create_object(env, &result);

  napi_value srcObj;
  URLSearchParams* self = nullptr;
  if (napi_get_named_property(env, jsThis, ITER_SOURCE_KEY, &srcObj) !=
          napi_ok ||
      napi_unwrap(env, srcObj, reinterpret_cast<void**>(&self)) != napi_ok ||
      self == nullptr) {
    napi_set_named_property(env, result, "value", napi_util::undefined(env));
    napi_set_named_property(env, result, "done", napi_util::get_true(env));
    return result;
  }

  auto* p = self->GetURLSearchParams();
  if (st->idx >= p->size()) {
    napi_set_named_property(env, result, "value", napi_util::undefined(env));
    napi_set_named_property(env, result, "done", napi_util::get_true(env));
    return result;
  }

  auto pair = (*p)[st->idx++];  // live, duplicate-key correct
  napi_value value;
  if (st->kind == ITER_KEYS) {
    value = js_str(env, pair.first);
  } else if (st->kind == ITER_VALUES) {
    value = js_str(env, pair.second);
  } else {
    napi_create_array_with_length(env, 2, &value);
    napi_set_element(env, value, 0, js_str(env, pair.first));
    napi_set_element(env, value, 1, js_str(env, pair.second));
  }
  napi_set_named_property(env, result, "value", value);
  napi_set_named_property(env, result, "done", napi_util::get_false(env));
  return result;
}

// iterator[Symbol.iterator]() -> this (iterators are iterable).
napi_value IteratorSelf(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);
  return jsThis;
}

napi_value MakeIterator(napi_env env, napi_value jsThis, int kind) {
  auto* st = new IterState{0, kind};

  napi_value iterator;
  napi_create_object(env, &iterator);

  // Attach two non-enumerable internal properties (napi_default => not
  // enumerable, invisible to JS): the strong source reference kept alive for
  // `next`, and a napi_external wrapping the IterState* for the brand check.
  //
  // The external is the SOLE owner of st and frees it when collected. Do not
  // also register st with napi_add_finalizer on the iterator: that hands the
  // same pointer to two independent finalization paths, and on QuickJS (where
  // this runtime dispatches finalizers through a JS FinalizationRegistry) both
  // fire, double-freeing IterState. The resulting heap corruption surfaced far
  // away from here, as a SIGSEGV in an unrelated spec.
  //
  // The `next` function below also carries st as its callback data, but only as
  // a non-owning alias: it is reachable only through the iterator, which holds
  // the external, so st outlives every call made on it.
  napi_value brandVal;
  napi_create_external(
      env, st,
      [](napi_env, void* d, void*) { delete static_cast<IterState*>(d); },
      nullptr, &brandVal);
  napi_property_descriptor descs[2] = {};
  descs[0].utf8name = ITER_SOURCE_KEY;
  descs[0].value = jsThis;
  descs[0].attributes = napi_default;
  descs[1].utf8name = ITER_BRAND_KEY;
  descs[1].value = brandVal;
  descs[1].attributes = napi_default;
  napi_define_properties(env, iterator, 2, descs);

  napi_value nextFn;
  napi_create_function(env, "next", NAPI_AUTO_LENGTH, IteratorNext, st,
                       &nextFn);
  napi_set_named_property(env, iterator, "next", nextFn);

  napi_value selfFn;
  napi_create_function(env, "[Symbol.iterator]", NAPI_AUTO_LENGTH,
                       IteratorSelf, nullptr, &selfFn);
  napi_set_property(env, iterator, SymbolIterator(env), selfFn);

  // Symbol.toStringTag = "URLSearchParams Iterator" for spec-correct
  // Object.prototype.toString output.
  napi_set_property(env, iterator, WellKnownSymbol(env, "toStringTag"),
                    js_str(env, "URLSearchParams Iterator"));

  return iterator;
}

// Drives the ES iterator protocol on `iterable`, invoking fn(itemValue) for
// each yielded value. Returns false (with a pending exception) on protocol
// error or if fn returns false.
template <class F>
bool ForEachOfIterable(napi_env env, napi_value iterable, F&& fn) {
  napi_value iterMethod;
  if (napi_get_property(env, iterable, SymbolIterator(env), &iterMethod) !=
      napi_ok) {
    return false;
  }
  if (!napi_util::is_of_type(env, iterMethod, napi_function)) {
    ThrowTypeError(env, "value is not iterable");
    return false;
  }
  napi_value iterObj;
  if (napi_call_function(env, iterable, iterMethod, 0, nullptr, &iterObj) !=
      napi_ok) {
    return false;
  }
  if (!napi_util::is_object(env, iterObj)) {
    ThrowTypeError(env, "iterator result is not an object");
    return false;
  }

  napi_value nextFn;
  if (napi_get_named_property(env, iterObj, "next", &nextFn) != napi_ok ||
      !napi_util::is_of_type(env, nextFn, napi_function)) {
    ThrowTypeError(env, "iterator has no next method");
    return false;
  }

  for (;;) {
    napi_value step;
    if (napi_call_function(env, iterObj, nextFn, 0, nullptr, &step) !=
        napi_ok) {
      return false;
    }
    if (!napi_util::is_object(env, step)) {
      ThrowTypeError(env, "iterator result is not an object");
      return false;
    }
    napi_value doneVal;
    bool done = false;
    if (napi_get_named_property(env, step, "done", &doneVal) != napi_ok) {
      return false;
    }
    napi_coerce_to_bool(env, doneVal, &doneVal);
    napi_get_value_bool(env, doneVal, &done);
    if (done) return true;

    napi_value value;
    if (napi_get_named_property(env, step, "value", &value) != napi_ok) {
      return false;
    }
    if (!fn(value)) {
      IteratorClose(env, iterObj);
      return false;
    }
  }
}

// sequence<sequence<USVString>> form: [[k, v], [k, v], ...].
bool BuildFromSequence(napi_env env, napi_value iterable,
                       url_search_params& params) {
  return ForEachOfIterable(env, iterable, [&](napi_value entry) -> bool {
    if (!napi_util::is_object(env, entry)) {
      ThrowTypeError(env, "URLSearchParams init sequence entry is not iterable");
      return false;
    }
    // Collect the whole entry before validating its length: aborting early on
    // the third element would skip the "exactly two elements" TypeError.
    std::vector<std::string> pair;
    bool ok = ForEachOfIterable(env, entry, [&](napi_value item) -> bool {
      std::string s;
      if (!ValueToString(env, item, s)) {
        return false;
      }
      pair.push_back(std::move(s));
      return true;
    });
    if (!ok) {
      return false;
    }
    if (pair.size() != 2) {
      ThrowTypeError(env,
                     "URLSearchParams init sequence entry does not contain "
                     "exactly two elements");
      return false;
    }
    params.append(pair[0], pair[1]);
    return true;
  });
}

// record<USVString, USVString> form.
bool BuildFromRecord(napi_env env, napi_value object,
                     url_search_params& params) {
  napi_value keys;
  if (napi_get_all_property_names(env, object, napi_key_own_only,
                                  napi_key_enumerable,
                                  napi_key_numbers_to_strings,
                                  &keys) != napi_ok) {
    return false;
  }
  uint32_t len = 0;
  napi_get_array_length(env, keys, &len);
  for (uint32_t i = 0; i < len; i++) {
    napi_value key, value;
    napi_get_element(env, keys, i, &key);
    napi_get_property(env, object, key, &value);
    std::string k, v;
    if (!ValueToString(env, key, k) || !ValueToString(env, value, v)) {
      return false;  // e.g. Symbol key
    }
    params.append(k, v);
  }
  return true;
}
}  // namespace

URLSearchParams::URLSearchParams(url_search_params params,
                                 url_aggregator* parent)
    : params_(params), parent_(parent) {}

url_search_params* URLSearchParams::GetURLSearchParams() { return &params_; }

void URLSearchParams::Reset(url_search_params params, url_aggregator* parent) {
  params_ = std::move(params);
  parent_ = parent;
}

napi_value URLSearchParams::Create(napi_env env, ada::url_search_params params,
                                   ada::url_aggregator* parent) {
  NS_NAPI_PREAMBLE

  napi_value global;
  NAPI_GUARD(napi_get_global(env, &global)) { return nullptr; }

  napi_value constructor;
  NAPI_GUARD(
      napi_get_named_property(env, global, "URLSearchParams", &constructor)) {
    return nullptr;
  }

  napi_value result;
  NAPI_GUARD(napi_new_instance(env, constructor, 0, nullptr, &result)) {
    return nullptr;
  }

  URLSearchParams* searchParams = nullptr;
  NAPI_GUARD(
      napi_unwrap(env, result, reinterpret_cast<void**>(&searchParams))) {
    return nullptr;
  }

  searchParams->Reset(std::move(params), parent);
  return result;
}

void URLSearchParams::SyncParent() {
  if (parent_ == nullptr) {
    return;
  }

  auto search = params_.to_string();
  if (search.empty()) {
    parent_->set_search("");
  } else {
    std::string prefixed = "?" + search;
    parent_->set_search(prefixed);
  }
}

napi_value URLSearchParams::New(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  if (!EnsureConstructorThis(env, "URLSearchParams", &jsThis)) {
    napi_throw_error(env, nullptr,
                     "Failed to initialize URLSearchParams instance");
    return nullptr;
  }

  url_search_params params;

  // Per spec the init argument is one of:
  //   USVString                      - query string
  //   sequence<sequence<USVString>>  - [[k, v], ...]   (has Symbol.iterator)
  //   record<USVString, USVString>   - plain object     (no Symbol.iterator)
  // Only undefined means "no init"; null and other primitives are coerced to a
  // USVString, so new URLSearchParams(null) parses as the string "null".
  if (argc > 0 && !napi_util::is_undefined(env, argv[0])) {
    if (napi_util::is_of_type(env, argv[0], napi_string)) {
      std::string init;
      if (!ValueToString(env, argv[0], init)) {
        return nullptr;
      }
      params = url_search_params(init);
    } else if (napi_util::is_object(env, argv[0])) {
      napi_value iterMethod;
      NAPI_GUARD(
          napi_get_property(env, argv[0], SymbolIterator(env), &iterMethod)) {
        return nullptr;
      }
      if (napi_util::is_null_or_undefined(env, iterMethod)) {
        if (!BuildFromRecord(env, argv[0], params)) {
          return nullptr;
        }
      } else if (napi_util::is_of_type(env, iterMethod, napi_function)) {
        if (!BuildFromSequence(env, argv[0], params)) {
          return nullptr;
        }
      } else {
        ThrowTypeError(
            env, "URLSearchParams init Symbol.iterator is not a function");
        return nullptr;
      }
    } else {
      // number / boolean / bigint / null / symbol -> USVString
      std::string init;
      if (!ValueToString(env, argv[0], init)) {
        return nullptr;
      }
      params = url_search_params(init);
    }
  }

  URLSearchParams* searchParams = new URLSearchParams(std::move(params));
  NAPI_GUARD(napi_wrap(env, jsThis, searchParams, URLSearchParams::Destructor,
                       searchParams, nullptr)) {
    delete searchParams;
    return nullptr;
  }

  return jsThis;
}

void URLSearchParams::Destructor(napi_env env, void* data, void* hint) {
#ifdef __HERMES__
  URLSearchParams* searchParams = static_cast<URLSearchParams*>(hint);
#else
  URLSearchParams* searchParams = static_cast<URLSearchParams*>(data);
#endif
  delete searchParams;
}

// Instance methods
napi_value URLSearchParams::Append(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  if (argc < 2) {
    ThrowTypeError(env, "URLSearchParams.append requires 2 arguments");
    return nullptr;
  }

  std::string key, value;
  if (!ValueToString(env, argv[0], key) ||
      !ValueToString(env, argv[1], value)) {
    return nullptr;
  }

  instance->GetURLSearchParams()->append(key, value);
  instance->SyncParent();
  return nullptr;
}

napi_value URLSearchParams::Has(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  if (argc < 1) return napi_util::get_false(env);

  std::string key;
  if (!ValueToString(env, argv[0], key)) {
    return nullptr;
  }

  // The 2-argument form matches on name *and* value; an explicit undefined
  // second argument is treated as omitted.
  bool has;
  if (argc > 1 && !napi_util::is_undefined(env, argv[1])) {
    std::string value;
    if (!ValueToString(env, argv[1], value)) {
      return nullptr;
    }
    has = instance->GetURLSearchParams()->has(key, value);
  } else {
    has = instance->GetURLSearchParams()->has(key);
  }

  napi_value result;
  napi_get_boolean(env, has, &result);
  return result;
}

napi_value URLSearchParams::Get(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  if (argc < 1) return napi_util::null(env);

  std::string key;
  if (!ValueToString(env, argv[0], key)) {
    return nullptr;
  }

  auto value = instance->GetURLSearchParams()->get(key);
  if (!value.has_value()) {
    // Per spec, a missing name returns null (not undefined).
    return napi_util::null(env);
  }

  return js_str(env, value.value());
}

napi_value URLSearchParams::Delete(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  if (argc < 1) {
    ThrowTypeError(env, "URLSearchParams.delete requires 1 argument");
    return nullptr;
  }

  std::string key;
  if (!ValueToString(env, argv[0], key)) {
    return nullptr;
  }

  // The 2-argument form removes only pairs matching name *and* value.
  if (argc > 1 && !napi_util::is_undefined(env, argv[1])) {
    std::string value;
    if (!ValueToString(env, argv[1], value)) {
      return nullptr;
    }
    instance->GetURLSearchParams()->remove(key, value);
  } else {
    instance->GetURLSearchParams()->remove(key);
  }
  instance->SyncParent();
  return nullptr;
}

napi_value URLSearchParams::GetAll(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  if (argc < 1) {
    ThrowTypeError(env, "URLSearchParams.getAll requires 1 argument");
    return nullptr;
  }

  std::string key;
  if (!ValueToString(env, argv[0], key)) {
    return nullptr;
  }

  auto values = instance->GetURLSearchParams()->get_all(key);

  napi_value result;
  napi_create_array_with_length(env, values.size(), &result);
  // napi_set_element takes a uint32_t index; iterate as one to avoid narrowing.
  for (uint32_t i = 0; i < values.size(); i++) {
    napi_set_element(env, result, i, js_str(env, values[i]));
  }
  return result;
}

napi_value URLSearchParams::Set(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  if (argc < 2) {
    ThrowTypeError(env, "URLSearchParams.set requires 2 arguments");
    return nullptr;
  }

  std::string key, value;
  if (!ValueToString(env, argv[0], key) ||
      !ValueToString(env, argv[1], value)) {
    return nullptr;
  }

  instance->GetURLSearchParams()->set(key, value);
  instance->SyncParent();
  return nullptr;
}

napi_value URLSearchParams::GetSize(napi_env env, napi_callback_info info) {
  NS_NAPI_PREAMBLE
  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  auto size = instance->GetURLSearchParams()->size();

  napi_value result;
  NAPI_GUARD(napi_create_int32(env, static_cast<int32_t>(size), &result)) {
    return nullptr;
  }

  return result;
}

napi_value URLSearchParams::Sort(napi_env env, napi_callback_info info) {
  NS_NAPI_PREAMBLE
  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  instance->GetURLSearchParams()->sort();
  instance->SyncParent();
  return nullptr;
}

napi_value URLSearchParams::ToString(napi_env env, napi_callback_info info) {
  NS_NAPI_PREAMBLE
  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  auto value = instance->GetURLSearchParams()->to_string();

  napi_value result;
  NAPI_GUARD(
      napi_create_string_utf8(env, value.data(), value.length(), &result)) {
    return nullptr;
  }

  return result;
}

napi_value URLSearchParams::Keys(napi_env env, napi_callback_info info) {
  // Read the callback info once: PrimJS does not return the receiver from a
  // second napi_get_cb_info call for the same napi_callback_info, so calling it
  // again inside GetInstance made the unwrap fail and every iterator accessor
  // throw "Illegal invocation".
  napi_value jsThis;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr) !=
      napi_ok) {
    return nullptr;
  }
  if (!GetInstanceOf(env, jsThis)) return nullptr;
  return MakeIterator(env, jsThis, ITER_KEYS);
}

napi_value URLSearchParams::Values(napi_env env, napi_callback_info info) {
  // Read the callback info once: PrimJS does not return the receiver from a
  // second napi_get_cb_info call for the same napi_callback_info, so calling it
  // again inside GetInstance made the unwrap fail and every iterator accessor
  // throw "Illegal invocation".
  napi_value jsThis;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr) !=
      napi_ok) {
    return nullptr;
  }
  if (!GetInstanceOf(env, jsThis)) return nullptr;
  return MakeIterator(env, jsThis, ITER_VALUES);
}

napi_value URLSearchParams::Entries(napi_env env, napi_callback_info info) {
  // Read the callback info once: PrimJS does not return the receiver from a
  // second napi_get_cb_info call for the same napi_callback_info, so calling it
  // again inside GetInstance made the unwrap fail and every iterator accessor
  // throw "Illegal invocation".
  napi_value jsThis;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr) !=
      napi_ok) {
    return nullptr;
  }
  if (!GetInstanceOf(env, jsThis)) return nullptr;
  return MakeIterator(env, jsThis, ITER_ENTRIES);
}

napi_value URLSearchParams::ForEach(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  URLSearchParams* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  if (argc < 1 || !napi_util::is_of_type(env, argv[0], napi_function)) {
    ThrowTypeError(env,
                   "URLSearchParams.forEach requires a callback function");
    return nullptr;
  }

  napi_value callback = argv[0];
  napi_value thisArg = argc >= 2 ? argv[1] : nullptr;

  napi_value global;
  napi_get_global(env, &global);

  // Use get_entries() so duplicate keys (e.g. ?a=1&a=2) each yield their own
  // value; get(key) would return the first value for every occurrence.
  auto entries = instance->GetURLSearchParams()->get_entries();
  while (entries.has_next()) {
    if (auto entry = entries.next()) {
      auto& [key, value] = entry.value();
      // Per spec, forEach callback receives (value, key, searchParams).
      napi_value args[3] = {js_str(env, value), js_str(env, key), jsThis};
      napi_value result;
      if (napi_call_function(env, thisArg ? thisArg : global, callback, 3,
                             args, &result) != napi_ok) {
        // If the callback throws, stop iteration.
        return nullptr;
      }
    }
  }

  return nullptr;
}

void URLSearchParams::Init(napi_env env, napi_value global) {
  NS_NAPI_PREAMBLE
  napi_value ctor;
  static const int prop_count = 13;
  napi_property_descriptor properties[prop_count] = {
      {"append", nullptr, Append, nullptr, nullptr, nullptr,
       napi_default_jsproperty, nullptr},
      {"delete", nullptr, Delete, nullptr, nullptr, nullptr,
       napi_default_jsproperty, nullptr},
      {"entries", nullptr, Entries, nullptr, nullptr, nullptr,
       napi_default_jsproperty, nullptr},
      {"forEach", nullptr, ForEach, nullptr, nullptr, nullptr,
       napi_default_jsproperty, nullptr},
      {"get", nullptr, Get, nullptr, nullptr, nullptr, napi_default_jsproperty,
       nullptr},
      {"getAll", nullptr, GetAll, nullptr, nullptr, nullptr,
       napi_default_jsproperty, nullptr},
      {"has", nullptr, Has, nullptr, nullptr, nullptr, napi_default_jsproperty,
       nullptr},
      {"keys", nullptr, Keys, nullptr, nullptr, nullptr,
       napi_default_jsproperty, nullptr},
      {"set", nullptr, Set, nullptr, nullptr, nullptr, napi_default_jsproperty,
       nullptr},
      {"size", nullptr, nullptr, GetSize, nullptr, nullptr,
       napi_default_jsproperty, nullptr},
      {"sort", nullptr, Sort, nullptr, nullptr, nullptr,
       napi_default_jsproperty, nullptr},
      {"toString", nullptr, ToString, nullptr, nullptr, nullptr,
       napi_default_jsproperty, nullptr},
      {"values", nullptr, Values, nullptr, nullptr, nullptr,
       napi_default_jsproperty, nullptr}};

  NAPI_GUARD(napi_define_class(env, "URLSearchParams", NAPI_AUTO_LENGTH, New,
                               nullptr, prop_count, properties, &ctor)) {
    return;
  }

  // Per spec, URLSearchParams.prototype[Symbol.iterator] === prototype.entries,
  // so `for (const [k, v] of params)` works.
  napi_value proto, entriesFn;
  if (napi_get_named_property(env, ctor, "prototype", &proto) == napi_ok &&
      napi_get_named_property(env, proto, "entries", &entriesFn) == napi_ok) {
    napi_set_property(env, proto, SymbolIterator(env), entriesFn);
  }

  NAPI_GUARD(napi_set_named_property(env, global, "URLSearchParams", ctor)) {
    return;
  }
}
