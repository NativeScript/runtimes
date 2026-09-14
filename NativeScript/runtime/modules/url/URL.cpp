#include "URL.h"

#include "URLSearchParams.h"
#include "native_api_util.h"

using namespace ada;
using namespace nativescript;

namespace {
struct CoercedUtf8String {
  std::vector<char> storage;
  size_t length = 0;

  std::string_view view() const { return {storage.data(), length}; }
};

bool CoerceToUtf8String(napi_env env, napi_value value,
                        CoercedUtf8String& out) {
  NS_NAPI_PREAMBLE
  napi_value coerced;
  NAPI_GUARD(napi_coerce_to_string(env, value, &coerced)) { return false; }

  NAPI_GUARD(
      napi_get_value_string_utf8(env, coerced, nullptr, 0, &out.length)) {
    return false;
  }

  out.storage.resize(out.length + 1);
  NAPI_GUARD(napi_get_value_string_utf8(env, coerced, out.storage.data(),
                                        out.storage.size(), nullptr)) {
    return false;
  }

  return true;
}

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

URL* GetInstance(napi_env env, napi_callback_info info) {
  NS_NAPI_PREAMBLE
  napi_value jsThis;
  void* data;
  NAPI_GUARD(napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, &data)) {
    return nullptr;
  }

  URL* instance;
  NAPI_GUARD(napi_unwrap(env, jsThis, reinterpret_cast<void**>(&instance))) {
    return nullptr;
  }

  return instance;
}

template <typename Getter>
napi_value GetUrlProperty(napi_env env, napi_callback_info info,
                          Getter getter) {
  NS_NAPI_PREAMBLE
  URL* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  auto value = (instance->GetURL()->*getter)();

  napi_value result;
  NAPI_GUARD(
      napi_create_string_utf8(env, value.data(), value.length(), &result)) {
    return nullptr;
  }

  return result;
}

template <typename Setter>
napi_value SetUrlProperty(napi_env env, napi_callback_info info,
                          Setter setter) {
  NAPI_CALLBACK_BEGIN(1)

  URL* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  if (argc < 1) return nullptr;

  CoercedUtf8String value;
  if (!CoerceToUtf8String(env, argv[0], value)) return nullptr;

  (instance->GetURL()->*setter)(value.view());
  return napi_util::get_true(env);
}
}  // namespace

URL::URL(url_aggregator url) : url_(std::move(url)) {}

url_aggregator* URL::GetURL() { return &url_; }

napi_value URL::GetHash(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_hash);
}

napi_value URL::GetHost(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_host);
}

napi_value URL::GetHostName(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_hostname);
}

napi_value URL::GetHref(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_href);
}

napi_value URL::GetOrigin(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_origin);
}

napi_value URL::GetPassword(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_password);
}

napi_value URL::GetPathName(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_pathname);
}

napi_value URL::GetPort(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_port);
}

napi_value URL::GetProtocol(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_protocol);
}

napi_value URL::GetSearch(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_search);
}

napi_value URL::GetSearchParams(napi_env env, napi_callback_info info) {
  NS_NAPI_PREAMBLE
  URL* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  auto search = instance->GetURL()->get_search();

  // Remove the leading '?' if present
  std::string_view search_string = search;
  if (!search_string.empty() && search_string[0] == '?') {
    search_string = search_string.substr(1);
  }

  // Create URLSearchParams from the search string
  url_search_params params(search_string);
  return URLSearchParams::Create(env, std::move(params), instance->GetURL());
}

napi_value URL::GetUserName(napi_env env, napi_callback_info info) {
  return GetUrlProperty(env, info, &url_aggregator::get_username);
}

napi_value URL::SetHash(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_hash);
}

napi_value URL::SetHost(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_host);
}

napi_value URL::SetHostName(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_hostname);
}

napi_value URL::SetHref(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_href);
}

napi_value URL::SetPassword(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_password);
}

napi_value URL::SetPathName(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_pathname);
}

napi_value URL::SetPort(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_port);
}

napi_value URL::SetProtocol(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_protocol);
}

napi_value URL::SetSearch(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_search);
}

napi_value URL::SetUserName(napi_env env, napi_callback_info info) {
  return SetUrlProperty(env, info, &url_aggregator::set_username);
}

// Add toString method
napi_value URL::ToString(napi_env env, napi_callback_info info) {
  NS_NAPI_PREAMBLE
  URL* instance = GetInstance(env, info);
  if (!instance) return nullptr;

  auto value = instance->GetURL()->get_href();

  napi_value result;
  NAPI_GUARD(
      napi_create_string_utf8(env, value.data(), value.length(), &result)) {
    return nullptr;
  }

  return result;
}

// Constructor
napi_value URL::New(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  if (!EnsureConstructorThis(env, "URL", &jsThis)) {
    napi_throw_error(env, nullptr, "Failed to initialize URL instance");
    return nullptr;
  }

  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "URL constructor requires at least 1 argument");
    return nullptr;
  }

  CoercedUtf8String url_input;
  if (!CoerceToUtf8String(env, argv[0], url_input)) return nullptr;

  auto parse_result =
      ada::parse<ada::url_aggregator>(url_input.view(), nullptr);

  // If the input is not absolute, retry with the provided base if available.
  if (!parse_result && argc > 1) {
    CoercedUtf8String base_input;
    if (!CoerceToUtf8String(env, argv[1], base_input)) return nullptr;

    auto base_result =
        ada::parse<ada::url_aggregator>(base_input.view(), nullptr);
    if (base_result) {
      parse_result = ada::parse<ada::url_aggregator>(url_input.view(),
                                                     &base_result.value());
    }
  }

  if (!parse_result) {
    napi_throw_type_error(env, nullptr, "Invalid URL");
    return nullptr;
  }

  URL* urlImpl = new URL(std::move(parse_result.value()));
  napi_wrap(env, jsThis, urlImpl, URL::Destructor, urlImpl, nullptr);

  return jsThis;
}

// Static method
napi_value URL::CanParse(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "canParse requires at least 1 argument");
    return nullptr;
  }

  CoercedUtf8String input;
  if (!CoerceToUtf8String(env, argv[0], input)) return nullptr;

  bool result =
      static_cast<bool>(ada::parse<ada::url_aggregator>(input.view(), nullptr));

  if (!result && argc > 1) {
    CoercedUtf8String base_input;
    if (!CoerceToUtf8String(env, argv[1], base_input)) return nullptr;

    auto base_result =
        ada::parse<ada::url_aggregator>(base_input.view(), nullptr);
    if (base_result) {
      result = static_cast<bool>(
          ada::parse<ada::url_aggregator>(input.view(), &base_result.value()));
    }
  }

  napi_value returnValue;
  NAPI_GUARD(napi_get_boolean(env, result, &returnValue)) { return nullptr; }

  return returnValue;
}

void URL::Destructor(napi_env env, void* data, void* hint) {
#ifdef __HERMES__
  URL* url = static_cast<URL*>(hint);
#else
  URL* url = static_cast<URL*>(data);
#endif
  delete url;
}

void URL::Init(napi_env env, napi_value global) {
  NS_NAPI_PREAMBLE
  napi_value ctor;
  static const int instance_prop_count = 13;
  napi_property_descriptor properties[instance_prop_count] = {
      {"hash", nullptr, nullptr, GetHash, SetHash, nullptr, napi_default,
       nullptr},
      {"host", nullptr, nullptr, GetHost, SetHost, nullptr, napi_default,
       nullptr},
      {"hostname", nullptr, nullptr, GetHostName, SetHostName, nullptr,
       napi_default, nullptr},
      {"href", nullptr, nullptr, GetHref, SetHref, nullptr, napi_default,
       nullptr},
      {"origin", nullptr, nullptr, GetOrigin, nullptr, nullptr, napi_default,
       nullptr},
      {"password", nullptr, nullptr, GetPassword, SetPassword, nullptr,
       napi_default, nullptr},
      {"pathname", nullptr, nullptr, GetPathName, SetPathName, nullptr,
       napi_default, nullptr},
      {"port", nullptr, nullptr, GetPort, SetPort, nullptr, napi_default,
       nullptr},
      {"protocol", nullptr, nullptr, GetProtocol, SetProtocol, nullptr,
       napi_default, nullptr},
      {"search", nullptr, nullptr, GetSearch, SetSearch, nullptr, napi_default,
       nullptr},
      {"searchParams", nullptr, nullptr, GetSearchParams, nullptr, nullptr,
       napi_default, nullptr},
      {"username", nullptr, nullptr, GetUserName, SetUserName, nullptr,
       napi_default, nullptr},
      {"toString", nullptr, ToString, nullptr, nullptr, nullptr, napi_default,
       nullptr}};

  NAPI_GUARD(napi_define_class(env, "URL", NAPI_AUTO_LENGTH, New, nullptr,
                               instance_prop_count, properties, &ctor)) {
    return;
  }

  // Add static methods
  static const int static_prop_count = 1;
  napi_property_descriptor static_properties[static_prop_count] = {
      {"canParse", nullptr, CanParse, nullptr, nullptr, nullptr, napi_static,
       nullptr},
  };

  NAPI_GUARD(
      napi_define_properties(env, ctor, static_prop_count, static_properties)) {
    return;
  }

  NAPI_GUARD(napi_set_named_property(env, global, "URL", ctor)) { return; }
}
