#include "Path.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "js_native_api.h"
#include "native_api_util.h"

namespace nativescript {

namespace {

constexpr char kSep = '/';
constexpr char kDelimiter = ':';

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

bool GetStringArg(napi_env env, napi_value value, std::string& out) {
  if (IsNullOrUndefined(env, value)) {
    napi_throw_type_error(env, nullptr,
                          "The \"path\" argument must be of type string");
    return false;
  }

  if (!CoerceToString(env, value, out)) {
    napi_throw_type_error(env, nullptr,
                          "The \"path\" argument must be of type string");
    return false;
  }

  return true;
}

// Split path into segments, handling empty segments
std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> segments;
  std::string segment;
  std::istringstream stream(path);
  while (std::getline(stream, segment, kSep)) {
    segments.push_back(segment);
  }
  return segments;
}

// Normalize path: resolve . and .. , collapse multiple slashes
std::string NormalizePath(const std::string& path) {
  if (path.empty()) {
    return ".";
  }

  bool isAbsolute = !path.empty() && path[0] == kSep;
  bool trailingSlash = path.size() > 1 && path.back() == kSep;

  std::vector<std::string> segments = SplitPath(path);
  std::vector<std::string> result;

  for (const auto& segment : segments) {
    if (segment.empty() || segment == ".") {
      continue;
    }
    if (segment == "..") {
      if (!result.empty() && result.back() != "..") {
        result.pop_back();
      } else if (!isAbsolute) {
        result.push_back("..");
      }
    } else {
      result.push_back(segment);
    }
  }

  std::string normalized;
  if (isAbsolute) {
    normalized = "/";
  }

  for (size_t i = 0; i < result.size(); ++i) {
    if (i > 0) {
      normalized += kSep;
    }
    normalized += result[i];
  }

  if (normalized.empty()) {
    normalized = isAbsolute ? "/" : ".";
  } else if (trailingSlash && normalized.back() != kSep) {
    normalized += kSep;
  }

  return normalized;
}

// Join multiple path segments
std::string JoinPaths(const std::vector<std::string>& paths) {
  std::string joined;
  for (size_t i = 0; i < paths.size(); ++i) {
    const auto& path = paths[i];
    if (path.empty()) {
      continue;
    }
    if (joined.empty()) {
      joined = path;
    } else {
      if (joined.back() != kSep && path.front() != kSep) {
        joined += kSep;
      } else if (joined.back() == kSep && path.front() == kSep) {
        joined += path.substr(1);
        continue;
      }
      joined += path;
    }
  }
  return NormalizePath(joined);
}

// Resolve paths to absolute path
std::string ResolvePaths(const std::vector<std::string>& paths) {
  std::string resolved;

  // Start from the end and work backwards
  for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
    const auto& path = *it;
    if (path.empty()) {
      continue;
    }

    if (resolved.empty()) {
      resolved = path;
    } else {
      resolved = path + "/" + resolved;
    }

    // If we hit an absolute path, stop
    if (!resolved.empty() && resolved[0] == kSep) {
      break;
    }
  }

  // If still not absolute, prepend cwd
  if (resolved.empty() || resolved[0] != kSep) {
    char* cwd = getcwd(nullptr, 0);
    if (cwd) {
      if (resolved.empty()) {
        resolved = cwd;
      } else {
        resolved = std::string(cwd) + "/" + resolved;
      }
      free(cwd);
    }
  }

  return NormalizePath(resolved);
}

// Get basename of a path
std::string GetBasename(const std::string& path, const std::string& ext = "") {
  if (path.empty()) {
    return "";
  }

  // Find the last non-trailing slash
  size_t end = path.size();
  while (end > 0 && path[end - 1] == kSep) {
    --end;
  }

  if (end == 0) {
    return "";
  }

  // Find the last slash before end
  size_t start = path.rfind(kSep, end - 1);
  if (start == std::string::npos) {
    start = 0;
  } else {
    start += 1;
  }

  std::string basename = path.substr(start, end - start);

  // Remove extension if provided and matches
  if (!ext.empty() && basename.size() > ext.size()) {
    if (basename.compare(basename.size() - ext.size(), ext.size(), ext) == 0) {
      basename = basename.substr(0, basename.size() - ext.size());
    }
  }

  return basename;
}

// Get dirname of a path
std::string GetDirname(const std::string& path) {
  if (path.empty()) {
    return ".";
  }

  // Find the last non-trailing slash
  size_t end = path.size();
  while (end > 0 && path[end - 1] == kSep) {
    --end;
  }

  if (end == 0) {
    return "/";
  }

  // Find the last slash before end
  size_t pos = path.rfind(kSep, end - 1);
  if (pos == std::string::npos) {
    return ".";
  }

  if (pos == 0) {
    return "/";
  }

  // Trim trailing slashes from result
  while (pos > 0 && path[pos - 1] == kSep) {
    --pos;
  }

  if (pos == 0) {
    return "/";
  }

  return path.substr(0, pos);
}

// Get extension of a path
std::string GetExtname(const std::string& path) {
  std::string basename = GetBasename(path);
  if (basename.empty()) {
    return "";
  }

  // Find the last dot
  size_t dotPos = basename.rfind('.');

  // No dot, or dot at the start (hidden file), or at the end
  if (dotPos == std::string::npos || dotPos == 0) {
    return "";
  }

  return basename.substr(dotPos);
}

// Parse path into components
struct ParsedPath {
  std::string root;
  std::string dir;
  std::string base;
  std::string ext;
  std::string name;
};

ParsedPath ParsePath(const std::string& path) {
  ParsedPath result;

  if (path.empty()) {
    return result;
  }

  // Determine root
  if (path[0] == kSep) {
    result.root = "/";
  }

  result.base = GetBasename(path);
  result.dir = GetDirname(path);
  result.ext = GetExtname(path);

  if (!result.ext.empty() && result.base.size() > result.ext.size()) {
    result.name = result.base.substr(0, result.base.size() - result.ext.size());
  } else {
    result.name = result.base;
  }

  // Handle edge case where path is just "/"
  if (path == "/") {
    result.dir = "/";
    result.base = "";
    result.name = "";
  }

  return result;
}

// Format parsed path back to string
std::string FormatPath(const std::string& dir, const std::string& root,
                       const std::string& base, const std::string& name,
                       const std::string& ext) {
  // If base is provided, use it; otherwise construct from name + ext
  std::string finalBase = base;
  if (finalBase.empty() && (!name.empty() || !ext.empty())) {
    finalBase = name + ext;
  }

  // If dir is provided, join with base
  if (!dir.empty()) {
    if (dir.back() == kSep) {
      return dir + finalBase;
    }
    return dir + kSep + finalBase;
  }

  // Otherwise use root + base
  return root + finalBase;
}

// Get relative path from 'from' to 'to'
std::string GetRelativePath(const std::string& from, const std::string& to) {
  std::string resolvedFrom = ResolvePaths({from});
  std::string resolvedTo = ResolvePaths({to});

  if (resolvedFrom == resolvedTo) {
    return "";
  }

  // Split into segments
  std::vector<std::string> fromParts = SplitPath(resolvedFrom);
  std::vector<std::string> toParts = SplitPath(resolvedTo);

  // Remove empty segments
  fromParts.erase(
      std::remove_if(fromParts.begin(), fromParts.end(),
                     [](const std::string& s) { return s.empty(); }),
      fromParts.end());
  toParts.erase(std::remove_if(toParts.begin(), toParts.end(),
                               [](const std::string& s) { return s.empty(); }),
                toParts.end());

  // Find common prefix length
  size_t commonLength = 0;
  size_t minLength = std::min(fromParts.size(), toParts.size());
  for (size_t i = 0; i < minLength; ++i) {
    if (fromParts[i] == toParts[i]) {
      ++commonLength;
    } else {
      break;
    }
  }

  // Build relative path
  std::string relative;

  // Add ".." for each remaining segment in 'from'
  for (size_t i = commonLength; i < fromParts.size(); ++i) {
    if (!relative.empty()) {
      relative += kSep;
    }
    relative += "..";
  }

  // Add remaining segments from 'to'
  for (size_t i = commonLength; i < toParts.size(); ++i) {
    if (!relative.empty()) {
      relative += kSep;
    }
    relative += toParts[i];
  }

  return relative.empty() ? "." : relative;
}

}  // namespace

napi_value Path::CreateModule(napi_env env) {
  napi_value moduleObj;
  napi_value exports;
  napi_create_object(env, &moduleObj);
  napi_create_object(env, &exports);

  // Methods
  napi_util::napi_set_function(env, exports, "basename", Basename);
  napi_util::napi_set_function(env, exports, "dirname", Dirname);
  napi_util::napi_set_function(env, exports, "extname", Extname);
  napi_util::napi_set_function(env, exports, "isAbsolute", IsAbsolute);
  napi_util::napi_set_function(env, exports, "join", Join);
  napi_util::napi_set_function(env, exports, "normalize", Normalize);
  napi_util::napi_set_function(env, exports, "parse", Parse);
  napi_util::napi_set_function(env, exports, "format", Format);
  napi_util::napi_set_function(env, exports, "relative", Relative);
  napi_util::napi_set_function(env, exports, "resolve", Resolve);
  napi_util::napi_set_function(env, exports, "toNamespacedPath",
                               ToNamespacedPath);

  // Properties
  napi_value sep;
  napi_create_string_utf8(env, "/", 1, &sep);
  napi_set_named_property(env, exports, "sep", sep);

  napi_value delimiter;
  napi_create_string_utf8(env, ":", 1, &delimiter);
  napi_set_named_property(env, exports, "delimiter", delimiter);

  // posix is self-reference (since we're on POSIX)
  napi_set_named_property(env, exports, "posix", exports);

  // win32 - provide empty object (not supported on POSIX)
  napi_value win32;
  napi_create_object(env, &win32);
  napi_set_named_property(env, exports, "win32", win32);

  napi_set_named_property(env, moduleObj, "exports", exports);
  return moduleObj;
}

napi_value Path::Basename(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "The \"path\" argument must be of type string");
    return nullptr;
  }

  std::string path;
  if (!GetStringArg(env, argv[0], path)) {
    return nullptr;
  }

  std::string ext;
  if (argc > 1 && !IsNullOrUndefined(env, argv[1])) {
    if (!CoerceToString(env, argv[1], ext)) {
      napi_throw_type_error(env, nullptr,
                            "The \"ext\" argument must be of type string");
      return nullptr;
    }
  }

  std::string result = GetBasename(path, ext);
  return napi_util::to_js_string(env, result);
}

napi_value Path::Dirname(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string path;
  if (!GetStringArg(env, argv[0], path)) {
    return nullptr;
  }

  std::string result = GetDirname(path);
  return napi_util::to_js_string(env, result);
}

napi_value Path::Extname(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string path;
  if (!GetStringArg(env, argv[0], path)) {
    return nullptr;
  }

  std::string result = GetExtname(path);
  return napi_util::to_js_string(env, result);
}

napi_value Path::IsAbsolute(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string path;
  if (!GetStringArg(env, argv[0], path)) {
    return nullptr;
  }

  bool isAbsolute = !path.empty() && path[0] == kSep;

  napi_value result;
  napi_get_boolean(env, isAbsolute, &result);
  return result;
}

napi_value Path::Join(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc == 0) {
    return napi_util::to_js_string(env, ".");
  }

  std::vector<std::string> paths;
  for (size_t i = 0; i < argc; ++i) {
    std::string path;
    if (!GetStringArg(env, argv[i], path)) {
      return nullptr;
    }
    paths.push_back(path);
  }

  std::string result = JoinPaths(paths);
  return napi_util::to_js_string(env, result);
}

napi_value Path::Normalize(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string path;
  if (!GetStringArg(env, argv[0], path)) {
    return nullptr;
  }

  std::string result = NormalizePath(path);
  return napi_util::to_js_string(env, result);
}

napi_value Path::Parse(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string path;
  if (!GetStringArg(env, argv[0], path)) {
    return nullptr;
  }

  ParsedPath parsed = ParsePath(path);

  napi_value result;
  napi_create_object(env, &result);

  napi_set_named_property(env, result, "root",
                          napi_util::to_js_string(env, parsed.root));
  napi_set_named_property(env, result, "dir",
                          napi_util::to_js_string(env, parsed.dir));
  napi_set_named_property(env, result, "base",
                          napi_util::to_js_string(env, parsed.base));
  napi_set_named_property(env, result, "ext",
                          napi_util::to_js_string(env, parsed.ext));
  napi_set_named_property(env, result, "name",
                          napi_util::to_js_string(env, parsed.name));

  return result;
}

napi_value Path::Format(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  if (IsNullOrUndefined(env, argv[0])) {
    napi_throw_type_error(
        env, nullptr,
        "The \"pathObject\" argument must be of type object. Received null");
    return nullptr;
  }

  napi_valuetype type;
  napi_typeof(env, argv[0], &type);
  if (type != napi_object) {
    napi_throw_type_error(env, nullptr,
                          "The \"pathObject\" argument must be of type object");
    return nullptr;
  }

  napi_value pathObject = argv[0];

  auto getStringProperty = [&](const char* name) -> std::string {
    napi_value value;
    if (napi_get_named_property(env, pathObject, name, &value) != napi_ok) {
      return "";
    }
    if (IsNullOrUndefined(env, value)) {
      return "";
    }
    std::string str;
    CoerceToString(env, value, str);
    return str;
  };

  std::string dir = getStringProperty("dir");
  std::string root = getStringProperty("root");
  std::string base = getStringProperty("base");
  std::string name = getStringProperty("name");
  std::string ext = getStringProperty("ext");

  std::string result = FormatPath(dir, root, base, name, ext);
  return napi_util::to_js_string(env, result);
}

napi_value Path::Relative(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  std::string from;
  std::string to;

  if (!GetStringArg(env, argv[0], from) || !GetStringArg(env, argv[1], to)) {
    return nullptr;
  }

  std::string result = GetRelativePath(from, to);
  return napi_util::to_js_string(env, result);
}

napi_value Path::Resolve(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  std::vector<std::string> paths;
  for (size_t i = 0; i < argc; ++i) {
    std::string path;
    if (!GetStringArg(env, argv[i], path)) {
      return nullptr;
    }
    paths.push_back(path);
  }

  std::string result = ResolvePaths(paths);
  return napi_util::to_js_string(env, result);
}

napi_value Path::ToNamespacedPath(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  // On POSIX, this simply returns the path unchanged
  if (IsNullOrUndefined(env, argv[0])) {
    return argv[0];
  }

  napi_valuetype type;
  napi_typeof(env, argv[0], &type);
  if (type != napi_string) {
    // Return the value as-is if not a string (Node.js behavior)
    return argv[0];
  }

  return argv[0];
}

}  // namespace nativescript
