#include "FS.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "js_native_api.h"
#include "native_api_util.h"

namespace nativescript {

namespace {

namespace fs = std::filesystem;

std::string ToLower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool CoerceToString(napi_env env, napi_value value, std::string& out) {
  napi_value coerced;
  if (napi_coerce_to_string(env, value, &coerced) != napi_ok) {
    return false;
  }

  size_t length = 0;
  if (napi_get_value_string_utf8(env, coerced, nullptr, 0, &length) !=
      napi_ok) {
    return false;
  }

  std::vector<char> buffer(length + 1);
  if (napi_get_value_string_utf8(env, coerced, buffer.data(), buffer.size(),
                                 nullptr) != napi_ok) {
    return false;
  }

  out.assign(buffer.data(), length);
  return true;
}

bool IsNullOrUndefined(napi_env env, napi_value value) {
  if (value == nullptr) {
    return true;
  }

  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok) {
    return false;
  }

  return type == napi_null || type == napi_undefined;
}

std::string ReadEncodingOption(napi_env env, napi_value options) {
  if (IsNullOrUndefined(env, options)) {
    return "";
  }

  napi_valuetype type;
  napi_typeof(env, options, &type);

  if (type == napi_string) {
    std::string value;
    if (CoerceToString(env, options, value)) {
      return ToLower(value);
    }
    return "";
  }

  if (type != napi_object) {
    return "";
  }

  bool hasEncoding = false;
  if (napi_has_named_property(env, options, "encoding", &hasEncoding) !=
          napi_ok ||
      !hasEncoding) {
    return "";
  }

  napi_value encodingValue;
  if (napi_get_named_property(env, options, "encoding", &encodingValue) !=
      napi_ok) {
    return "";
  }

  if (IsNullOrUndefined(env, encodingValue)) {
    return "";
  }

  std::string encoding;
  if (!CoerceToString(env, encodingValue, encoding)) {
    return "";
  }

  return ToLower(encoding);
}

bool ReadBooleanOption(napi_env env, napi_value options, const char* name,
                       bool defaultValue) {
  if (IsNullOrUndefined(env, options)) {
    return defaultValue;
  }

  napi_valuetype type;
  napi_typeof(env, options, &type);
  if (type != napi_object) {
    return defaultValue;
  }

  bool hasProp = false;
  if (napi_has_named_property(env, options, name, &hasProp) != napi_ok ||
      !hasProp) {
    return defaultValue;
  }

  napi_value propValue;
  if (napi_get_named_property(env, options, name, &propValue) != napi_ok) {
    return defaultValue;
  }

  napi_value boolValue;
  if (napi_coerce_to_bool(env, propValue, &boolValue) != napi_ok) {
    return defaultValue;
  }

  bool result = defaultValue;
  napi_get_value_bool(env, boolValue, &result);
  return result;
}

std::string GetErrnoName(int err) {
  switch (err) {
    case EACCES:
      return "EACCES";
    case EEXIST:
      return "EEXIST";
    case EINVAL:
      return "EINVAL";
    case EIO:
      return "EIO";
    case EISDIR:
      return "EISDIR";
    case ENOENT:
      return "ENOENT";
    case ENOTDIR:
      return "ENOTDIR";
    case ENOTEMPTY:
      return "ENOTEMPTY";
    case EPERM:
      return "EPERM";
    default:
      return "ERR_FS";
  }
}

void ThrowFsError(napi_env env, const std::string& syscall,
                  const std::string& path, const std::string& code,
                  const std::string& details) {
  std::string message = code + ": " + syscall + " '" + path + "'";
  if (!details.empty()) {
    message += ", " + details;
  }

  napi_value errorMessage = napi_util::to_js_string(env, message);
  napi_value errorObj;
  napi_create_error(env, nullptr, errorMessage, &errorObj);

  napi_set_named_property(env, errorObj, "code",
                          napi_util::to_js_string(env, code));
  napi_set_named_property(env, errorObj, "path",
                          napi_util::to_js_string(env, path));
  napi_set_named_property(env, errorObj, "syscall",
                          napi_util::to_js_string(env, syscall));

  napi_throw(env, errorObj);
}

void ThrowFsError(napi_env env, const std::string& syscall,
                  const std::string& path, const std::error_code& ec) {
  std::string code = GetErrnoName(ec.value());
  ThrowFsError(env, syscall, path, code, ec.message());
}

void ThrowInvalidEncoding(napi_env env, const std::string& encoding) {
  std::string message = "Unsupported encoding: " + encoding;
  napi_throw_type_error(env, nullptr, message.c_str());
}

bool GetPathArg(napi_env env, napi_value value, std::string& pathOut) {
  if (!CoerceToString(env, value, pathOut)) {
    napi_throw_type_error(env, nullptr, "Path must be coercible to string");
    return false;
  }

  if (pathOut.empty()) {
    napi_throw_type_error(env, nullptr, "Path cannot be empty");
    return false;
  }

  return true;
}

std::string BytesToHex(const std::vector<uint8_t>& bytes) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string result;
  result.resize(bytes.size() * 2);

  size_t out = 0;
  for (auto b : bytes) {
    result[out++] = kHexDigits[(b >> 4) & 0x0F];
    result[out++] = kHexDigits[b & 0x0F];
  }

  return result;
}

std::string BytesToBase64(const std::vector<uint8_t>& bytes) {
  static constexpr char kBase64Chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string result;
  result.reserve(((bytes.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 2 < bytes.size()) {
    uint32_t n = (static_cast<uint32_t>(bytes[i]) << 16) |
                 (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                 static_cast<uint32_t>(bytes[i + 2]);
    result.push_back(kBase64Chars[(n >> 18) & 63]);
    result.push_back(kBase64Chars[(n >> 12) & 63]);
    result.push_back(kBase64Chars[(n >> 6) & 63]);
    result.push_back(kBase64Chars[n & 63]);
    i += 3;
  }

  if (i < bytes.size()) {
    uint32_t n = static_cast<uint32_t>(bytes[i]) << 16;
    result.push_back(kBase64Chars[(n >> 18) & 63]);
    if (i + 1 < bytes.size()) {
      n |= static_cast<uint32_t>(bytes[i + 1]) << 8;
      result.push_back(kBase64Chars[(n >> 12) & 63]);
      result.push_back(kBase64Chars[(n >> 6) & 63]);
      result.push_back('=');
    } else {
      result.push_back(kBase64Chars[(n >> 12) & 63]);
      result.push_back('=');
      result.push_back('=');
    }
  }

  return result;
}

bool HexToBytes(const std::string& input, std::vector<uint8_t>& bytesOut) {
  auto hexToNibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };

  if (input.size() % 2 != 0) {
    return false;
  }

  bytesOut.clear();
  bytesOut.reserve(input.size() / 2);

  for (size_t i = 0; i < input.size(); i += 2) {
    int hi = hexToNibble(input[i]);
    int lo = hexToNibble(input[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    bytesOut.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  return true;
}

bool Base64ToBytes(const std::string& input, std::vector<uint8_t>& bytesOut) {
  static std::array<int8_t, 256> lut = [] {
    std::array<int8_t, 256> map{};
    map.fill(-1);
    for (int i = 0; i < 26; i++) {
      map[static_cast<uint8_t>('A' + i)] = i;
      map[static_cast<uint8_t>('a' + i)] = i + 26;
    }
    for (int i = 0; i < 10; i++) {
      map[static_cast<uint8_t>('0' + i)] = i + 52;
    }
    map[static_cast<uint8_t>('+')] = 62;
    map[static_cast<uint8_t>('/')] = 63;
    return map;
  }();

  std::string normalized;
  normalized.reserve(input.size());
  for (char c : input) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      normalized.push_back(c);
    }
  }

  if (normalized.size() % 4 != 0) {
    return false;
  }

  bytesOut.clear();
  bytesOut.reserve((normalized.size() / 4) * 3);

  for (size_t i = 0; i < normalized.size(); i += 4) {
    char c0 = normalized[i];
    char c1 = normalized[i + 1];
    char c2 = normalized[i + 2];
    char c3 = normalized[i + 3];

    if (c0 == '=' || c1 == '=') {
      return false;
    }

    int8_t v0 = lut[static_cast<uint8_t>(c0)];
    int8_t v1 = lut[static_cast<uint8_t>(c1)];
    if (v0 < 0 || v1 < 0) {
      return false;
    }

    uint32_t n =
        (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12);

    if (c2 == '=') {
      bytesOut.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
      if (c3 != '=') {
        return false;
      }
      continue;
    }

    int8_t v2 = lut[static_cast<uint8_t>(c2)];
    if (v2 < 0) {
      return false;
    }
    n |= static_cast<uint32_t>(v2) << 6;

    if (c3 == '=') {
      bytesOut.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
      bytesOut.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
      continue;
    }

    int8_t v3 = lut[static_cast<uint8_t>(c3)];
    if (v3 < 0) {
      return false;
    }

    n |= static_cast<uint32_t>(v3);
    bytesOut.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    bytesOut.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    bytesOut.push_back(static_cast<uint8_t>(n & 0xFF));
  }

  return true;
}

napi_value BytesToJsValue(napi_env env, const std::vector<uint8_t>& bytes,
                          const std::string& encoding) {
  std::string normalized = ToLower(encoding);

  if (normalized.empty()) {
    void* data = nullptr;
    napi_value arrayBuffer;

    if (bytes.empty()) {
      napi_create_arraybuffer(env, 0, &data, &arrayBuffer);
      return arrayBuffer;
    }

    uint8_t* storage = static_cast<uint8_t*>(malloc(bytes.size()));
    memcpy(storage, bytes.data(), bytes.size());
    napi_create_external_arraybuffer(
        env, storage, bytes.size(),
        [](napi_env, void* ptr, void*) { free(ptr); }, nullptr, &arrayBuffer);
    return arrayBuffer;
  }

  if (normalized == "utf8" || normalized == "utf-8") {
    return napi_util::to_js_string(env,
                                   std::string(bytes.begin(), bytes.end()));
  }

  if (normalized == "ascii" || normalized == "latin1" ||
      normalized == "binary") {
    std::string utf8;
    utf8.reserve(bytes.size() * 2);
    for (auto b : bytes) {
      if (b < 0x80) {
        utf8.push_back(static_cast<char>(b));
      } else {
        utf8.push_back(static_cast<char>(0xC0 | (b >> 6)));
        utf8.push_back(static_cast<char>(0x80 | (b & 0x3F)));
      }
    }
    return napi_util::to_js_string(env, utf8);
  }

  if (normalized == "hex") {
    return napi_util::to_js_string(env, BytesToHex(bytes));
  }

  if (normalized == "base64") {
    return napi_util::to_js_string(env, BytesToBase64(bytes));
  }

  ThrowInvalidEncoding(env, encoding);
  return nullptr;
}

bool JsValueToBytes(napi_env env, napi_value value, const std::string& encoding,
                    std::vector<uint8_t>& bytesOut) {
  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok) {
    return false;
  }

  bool isArrayBuffer = false;
  napi_is_arraybuffer(env, value, &isArrayBuffer);
  if (isArrayBuffer) {
    void* data = nullptr;
    size_t length = 0;
    napi_get_arraybuffer_info(env, value, &data, &length);
    bytesOut.assign(static_cast<uint8_t*>(data),
                    static_cast<uint8_t*>(data) + length);
    return true;
  }

  bool isTypedArray = false;
  napi_is_typedarray(env, value, &isTypedArray);
  if (isTypedArray) {
    napi_typedarray_type typedArrayType;
    size_t length = 0;
    void* data = nullptr;
    napi_value buffer;
    size_t byteOffset = 0;
    napi_get_typedarray_info(env, value, &typedArrayType, &length, &data,
                             &buffer, &byteOffset);
    size_t elementSize = 1;
    switch (typedArrayType) {
      case napi_int16_array:
      case napi_uint16_array:
        elementSize = 2;
        break;
      case napi_int32_array:
      case napi_uint32_array:
      case napi_float32_array:
        elementSize = 4;
        break;
      case napi_float64_array:
      case napi_bigint64_array:
      case napi_biguint64_array:
        elementSize = 8;
        break;
      default:
        elementSize = 1;
        break;
    }

    const uint8_t* ptr = static_cast<uint8_t*>(data);
    bytesOut.assign(ptr, ptr + (length * elementSize));
    return true;
  }

  std::string text;
  if (!CoerceToString(env, value, text)) {
    return false;
  }

  std::string normalized = ToLower(encoding);
  if (normalized.empty() || normalized == "utf8" || normalized == "utf-8" ||
      normalized == "ascii" || normalized == "latin1" ||
      normalized == "binary") {
    bytesOut.assign(text.begin(), text.end());
    return true;
  }

  if (normalized == "hex") {
    if (!HexToBytes(text, bytesOut)) {
      napi_throw_type_error(env, nullptr, "Invalid hex string");
      return false;
    }
    return true;
  }

  if (normalized == "base64") {
    if (!Base64ToBytes(text, bytesOut)) {
      napi_throw_type_error(env, nullptr, "Invalid base64 string");
      return false;
    }
    return true;
  }

  ThrowInvalidEncoding(env, encoding);
  return false;
}

bool ReadFileToBytes(const std::string& path, std::vector<uint8_t>& bytesOut,
                     std::error_code& ec) {
  ec.clear();

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    ec = std::error_code(errno, std::generic_category());
    return false;
  }

  input.seekg(0, std::ios::end);
  std::streamsize size = input.tellg();
  input.seekg(0, std::ios::beg);

  if (size < 0) {
    ec = std::make_error_code(std::errc::io_error);
    return false;
  }

  bytesOut.resize(static_cast<size_t>(size));
  if (size > 0 &&
      !input.read(reinterpret_cast<char*>(bytesOut.data()), size).good()) {
    ec = std::error_code(errno, std::generic_category());
    return false;
  }

  return true;
}

bool WriteBytesToFile(const std::string& path,
                      const std::vector<uint8_t>& bytes, std::error_code& ec) {
  ec.clear();

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    ec = std::error_code(errno, std::generic_category());
    return false;
  }

  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  if (!output.good()) {
    ec = std::error_code(errno, std::generic_category());
    return false;
  }

  return true;
}

napi_value BuildStatObject(napi_env env, const fs::file_status& status,
                           const fs::path& path) {
  napi_value statObj;
  napi_create_object(env, &statObj);

  std::error_code ec;
  uintmax_t size = fs::is_regular_file(status) ? fs::file_size(path, ec) : 0;
  if (ec) {
    size = 0;
  }

  long long mtimeMs = 0;
  auto writeTime = fs::last_write_time(path, ec);
  if (!ec) {
    auto systemTime =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            writeTime - fs::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    mtimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                  systemTime.time_since_epoch())
                  .count();
  }

  napi_set_named_property(
      env, statObj, "size",
      napi_util::to_js_number(env, static_cast<double>(size)));
  napi_set_named_property(
      env, statObj, "mtimeMs",
      napi_util::to_js_number(env, static_cast<double>(mtimeMs)));
  napi_set_named_property(
      env, statObj, "ctimeMs",
      napi_util::to_js_number(env, static_cast<double>(mtimeMs)));
  napi_set_named_property(
      env, statObj, "atimeMs",
      napi_util::to_js_number(env, static_cast<double>(mtimeMs)));
  napi_set_named_property(
      env, statObj, "birthtimeMs",
      napi_util::to_js_number(env, static_cast<double>(mtimeMs)));

  napi_set_named_property(env, statObj, "_isFile",
                          fs::is_regular_file(status)
                              ? napi_util::get_true(env)
                              : napi_util::get_false(env));
  napi_set_named_property(env, statObj, "_isDirectory",
                          fs::is_directory(status) ? napi_util::get_true(env)
                                                   : napi_util::get_false(env));
  napi_set_named_property(env, statObj, "_isSymbolicLink",
                          fs::is_symlink(status) ? napi_util::get_true(env)
                                                 : napi_util::get_false(env));

  auto boolAccessor = [](napi_env env, napi_callback_info info) -> napi_value {
    NAPI_CALLBACK_BEGIN(0)
    const char* prop = static_cast<const char*>(data);
    bool hasProp = false;
    napi_has_named_property(env, jsThis, prop, &hasProp);
    if (!hasProp) {
      return napi_util::get_false(env);
    }

    napi_value value;
    napi_get_named_property(env, jsThis, prop, &value);
    napi_value result;
    napi_coerce_to_bool(env, value, &result);
    return result;
  };

  napi_util::napi_set_function(env, statObj, "isFile", boolAccessor,
                               (void*)"_isFile");
  napi_util::napi_set_function(env, statObj, "isDirectory", boolAccessor,
                               (void*)"_isDirectory");
  napi_util::napi_set_function(env, statObj, "isSymbolicLink", boolAccessor,
                               (void*)"_isSymbolicLink");

  return statObj;
}

napi_value BuildDirentObject(napi_env env, const fs::directory_entry& entry) {
  napi_value direntObj;
  napi_create_object(env, &direntObj);

  auto name = entry.path().filename().string();
  napi_set_named_property(env, direntObj, "name",
                          napi_util::to_js_string(env, name));

  std::error_code ec;
  auto status = entry.symlink_status(ec);
  bool isFile = !ec && fs::is_regular_file(status);
  bool isDir = !ec && fs::is_directory(status);
  bool isSymlink = !ec && fs::is_symlink(status);

  napi_set_named_property(
      env, direntObj, "_isFile",
      isFile ? napi_util::get_true(env) : napi_util::get_false(env));
  napi_set_named_property(
      env, direntObj, "_isDirectory",
      isDir ? napi_util::get_true(env) : napi_util::get_false(env));
  napi_set_named_property(
      env, direntObj, "_isSymbolicLink",
      isSymlink ? napi_util::get_true(env) : napi_util::get_false(env));

  auto boolAccessor = [](napi_env env, napi_callback_info info) -> napi_value {
    NAPI_CALLBACK_BEGIN(0)
    const char* prop = static_cast<const char*>(data);
    bool hasProp = false;
    napi_has_named_property(env, jsThis, prop, &hasProp);
    if (!hasProp) {
      return napi_util::get_false(env);
    }

    napi_value value;
    napi_get_named_property(env, jsThis, prop, &value);
    napi_value result;
    napi_coerce_to_bool(env, value, &result);
    return result;
  };

  napi_util::napi_set_function(env, direntObj, "isFile", boolAccessor,
                               (void*)"_isFile");
  napi_util::napi_set_function(env, direntObj, "isDirectory", boolAccessor,
                               (void*)"_isDirectory");
  napi_util::napi_set_function(env, direntObj, "isSymbolicLink", boolAccessor,
                               (void*)"_isSymbolicLink");

  return direntObj;
}

napi_value ReadFileSyncImpl(napi_env env, size_t argc, napi_value* argv) {
  if (argc < 1) {
    napi_throw_type_error(env, nullptr, "fs.readFileSync(path[, options])");
    return nullptr;
  }

  std::string path;
  if (!GetPathArg(env, argv[0], path)) {
    return nullptr;
  }

  std::string encoding = argc > 1 ? ReadEncodingOption(env, argv[1]) : "";
  std::error_code ec;
  std::vector<uint8_t> bytes;
  if (!ReadFileToBytes(path, bytes, ec)) {
    ThrowFsError(env, "readFileSync", path, ec);
    return nullptr;
  }

  return BytesToJsValue(env, bytes, encoding);
}

napi_value WriteFileSyncImpl(napi_env env, size_t argc, napi_value* argv) {
  if (argc < 2) {
    napi_throw_type_error(env, nullptr,
                          "fs.writeFileSync(path, data[, options])");
    return nullptr;
  }

  std::string path;
  if (!GetPathArg(env, argv[0], path)) {
    return nullptr;
  }

  std::string encoding = argc > 2 ? ReadEncodingOption(env, argv[2]) : "";
  std::vector<uint8_t> bytes;
  if (!JsValueToBytes(env, argv[1], encoding, bytes)) {
    return nullptr;
  }

  std::error_code ec;
  if (!WriteBytesToFile(path, bytes, ec)) {
    ThrowFsError(env, "writeFileSync", path, ec);
    return nullptr;
  }

  return napi_util::undefined(env);
}

void InstallFsWrapper(napi_env env, napi_value exports) {
  const char* wrapperScript = R"JS(
    (function (exports) {
      const readFileSync = exports.readFileSync;
      const writeFileSync = exports.writeFileSync;
      const mkdirSync = exports.mkdirSync;
      const readdirSync = exports.readdirSync;
      const statSync = exports.statSync;
      const lstatSync = exports.lstatSync;
      const unlinkSync = exports.unlinkSync;
      const rmSync = exports.rmSync;

      function toAsync(fn) {
        return function (...args) {
          return Promise.resolve().then(() => fn(...args));
        };
      }

      exports.readFile = function readFile(path, options, callback) {
        if (typeof options === "function") {
          callback = options;
          options = undefined;
        }

        if (typeof callback === "function") {
          setTimeout(() => {
            try {
              callback(null, readFileSync(path, options));
            } catch (err) {
              callback(err);
            }
          }, 0);
          return;
        }

        return toAsync(readFileSync)(path, options);
      };

      exports.writeFile = function writeFile(path, data, options, callback) {
        if (typeof options === "function") {
          callback = options;
          options = undefined;
        }

        if (typeof callback === "function") {
          setTimeout(() => {
            try {
              callback(null, writeFileSync(path, data, options));
            } catch (err) {
              callback(err);
            }
          }, 0);
          return;
        }

        return toAsync(writeFileSync)(path, data, options);
      };

      exports.promises = {
        readFile: toAsync(readFileSync),
        writeFile: toAsync(writeFileSync),
        mkdir: toAsync(mkdirSync),
        readdir: toAsync(readdirSync),
        stat: toAsync(statSync),
        lstat: toAsync(lstatSync),
        unlink: toAsync(unlinkSync),
        rm: toAsync(rmSync),
      };
    })
  )JS";

  napi_value script;
  napi_create_string_utf8(env, wrapperScript, NAPI_AUTO_LENGTH, &script);

  napi_value fn;
  napi_run_script(env, script, &fn);

  napi_value global;
  napi_get_global(env, &global);

  napi_value argv[1] = {exports};
  napi_call_function(env, global, fn, 1, argv, nullptr);
}

}  // namespace

napi_value FS::CreateModule(napi_env env) {
  napi_value moduleObj;
  napi_value exports;
  napi_create_object(env, &moduleObj);
  napi_create_object(env, &exports);

  napi_util::napi_set_function(env, exports, "readFileSync", ReadFileSync);
  napi_util::napi_set_function(env, exports, "writeFileSync", WriteFileSync);
  napi_util::napi_set_function(env, exports, "existsSync", ExistsSync);
  napi_util::napi_set_function(env, exports, "mkdirSync", MkdirSync);
  napi_util::napi_set_function(env, exports, "readdirSync", ReaddirSync);
  napi_util::napi_set_function(env, exports, "statSync", StatSync);
  napi_util::napi_set_function(env, exports, "lstatSync", LstatSync);
  napi_util::napi_set_function(env, exports, "unlinkSync", UnlinkSync);
  napi_util::napi_set_function(env, exports, "rmSync", RmSync);

  napi_value constants;
  napi_create_object(env, &constants);
  napi_set_named_property(env, constants, "F_OK",
                          napi_util::to_js_number(env, 0));
  napi_set_named_property(env, constants, "R_OK",
                          napi_util::to_js_number(env, 4));
  napi_set_named_property(env, constants, "W_OK",
                          napi_util::to_js_number(env, 2));
  napi_set_named_property(env, constants, "X_OK",
                          napi_util::to_js_number(env, 1));
  napi_set_named_property(env, exports, "constants", constants);

  InstallFsWrapper(env, exports);
  napi_set_named_property(env, moduleObj, "exports", exports);

  return moduleObj;
}

napi_value FS::ReadFileSync(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()
  return ReadFileSyncImpl(env, argc, argv.data());
}

napi_value FS::WriteFileSync(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()
  return WriteFileSyncImpl(env, argc, argv.data());
}

napi_value FS::ExistsSync(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string path;
  if (!GetPathArg(env, argv[0], path)) {
    return nullptr;
  }

  std::error_code ec;
  bool exists = fs::exists(path, ec);
  if (ec) {
    exists = false;
  }

  napi_value result;
  napi_get_boolean(env, exists, &result);
  return result;
}

napi_value FS::MkdirSync(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc < 1) {
    napi_throw_type_error(env, nullptr, "fs.mkdirSync(path[, options])");
    return nullptr;
  }

  std::string path;
  if (!GetPathArg(env, argv[0], path)) {
    return nullptr;
  }

  bool recursive =
      argc > 1 ? ReadBooleanOption(env, argv[1], "recursive", false) : false;

  std::error_code ec;
  bool created = recursive ? fs::create_directories(path, ec)
                           : fs::create_directory(path, ec);
  if (ec) {
    ThrowFsError(env, "mkdirSync", path, ec);
    return nullptr;
  }

  if (!recursive && !created && fs::exists(path)) {
    ThrowFsError(env, "mkdirSync", path, "EEXIST", "file already exists");
    return nullptr;
  }

  return napi_util::undefined(env);
}

napi_value FS::ReaddirSync(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc < 1) {
    napi_throw_type_error(env, nullptr, "fs.readdirSync(path[, options])");
    return nullptr;
  }

  std::string path;
  if (!GetPathArg(env, argv[0], path)) {
    return nullptr;
  }

  std::string encoding = argc > 1 ? ReadEncodingOption(env, argv[1]) : "utf8";
  if (encoding.empty()) {
    encoding = "utf8";
  }

  bool withFileTypes =
      argc > 1 ? ReadBooleanOption(env, argv[1], "withFileTypes", false)
               : false;

  std::error_code ec;
  if (!fs::exists(path, ec)) {
    ThrowFsError(
        env, "readdirSync", path,
        ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory));
    return nullptr;
  }
  if (!fs::is_directory(path, ec)) {
    ThrowFsError(env, "readdirSync", path,
                 ec ? ec : std::make_error_code(std::errc::not_a_directory));
    return nullptr;
  }

  if (ec) {
    ThrowFsError(env, "readdirSync", path, ec);
    return nullptr;
  }

  std::vector<napi_value> entries;
  for (const auto& entry : fs::directory_iterator(path, ec)) {
    if (ec) {
      ThrowFsError(env, "readdirSync", path, ec);
      return nullptr;
    }

    if (withFileTypes) {
      entries.push_back(BuildDirentObject(env, entry));
      continue;
    }

    auto name = entry.path().filename().string();
    std::vector<uint8_t> nameBytes(name.begin(), name.end());
    entries.push_back(BytesToJsValue(env, nameBytes, encoding));
  }

  napi_value result;
  napi_create_array_with_length(env, entries.size(), &result);
  for (size_t i = 0; i < entries.size(); i++) {
    napi_set_element(env, result, i, entries[i]);
  }

  return result;
}

napi_value FS::StatSync(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string path;
  if (!GetPathArg(env, argv[0], path)) {
    return nullptr;
  }

  std::error_code ec;
  auto fileStatus = fs::status(path, ec);
  if (ec || fileStatus.type() == fs::file_type::not_found) {
    ThrowFsError(
        env, "statSync", path,
        ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory));
    return nullptr;
  }

  return BuildStatObject(env, fileStatus, path);
}

napi_value FS::LstatSync(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string path;
  if (!GetPathArg(env, argv[0], path)) {
    return nullptr;
  }

  std::error_code ec;
  auto fileStatus = fs::symlink_status(path, ec);
  if (ec || fileStatus.type() == fs::file_type::not_found) {
    ThrowFsError(
        env, "lstatSync", path,
        ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory));
    return nullptr;
  }

  return BuildStatObject(env, fileStatus, path);
}

napi_value FS::UnlinkSync(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string path;
  if (!GetPathArg(env, argv[0], path)) {
    return nullptr;
  }

  std::error_code ec;
  bool removed = fs::remove(path, ec);
  if (ec || !removed) {
    ThrowFsError(
        env, "unlinkSync", path,
        ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory));
    return nullptr;
  }

  return napi_util::undefined(env);
}

napi_value FS::RmSync(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc < 1) {
    napi_throw_type_error(env, nullptr, "fs.rmSync(path[, options])");
    return nullptr;
  }

  std::string path;
  if (!GetPathArg(env, argv[0], path)) {
    return nullptr;
  }

  bool recursive =
      argc > 1 ? ReadBooleanOption(env, argv[1], "recursive", false) : false;
  bool force =
      argc > 1 ? ReadBooleanOption(env, argv[1], "force", false) : false;

  std::error_code ec;
  bool exists = fs::exists(path, ec);
  if (ec) {
    ThrowFsError(env, "rmSync", path, ec);
    return nullptr;
  }

  if (!exists) {
    if (force) {
      return napi_util::undefined(env);
    }
    ThrowFsError(env, "rmSync", path, "ENOENT", "no such file or directory");
    return nullptr;
  }

  if (recursive) {
    fs::remove_all(path, ec);
    if (ec) {
      ThrowFsError(env, "rmSync", path, ec);
      return nullptr;
    }
    return napi_util::undefined(env);
  }

  bool removed = fs::remove(path, ec);
  if (ec || !removed) {
    ThrowFsError(
        env, "rmSync", path,
        ec ? ec : std::make_error_code(std::errc::operation_not_permitted));
    return nullptr;
  }

  return napi_util::undefined(env);
}

}  // namespace nativescript
