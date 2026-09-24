#include "ModuleInternal.h"

#include <dlfcn.h>
#include <libgen.h>
#include <sys/stat.h>
#include <utime.h>

#include <cassert>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "runtime/apple/NativeScriptException.h"
#include "native_api_util.h"
#include "runtime/apple/RuntimeConfig.h"
#include "runtime/apple/Util.h"
#include "runtime/apple/modules/node/Node.h"
#include "runtime/apple/modules/web/Web.h"

#ifdef TARGET_ENGINE_V8
// vendor/v8 is on the include path for V8 builds (see NativeScript/CMakeLists).
#include "v8-module-loader.h"
#elif defined(TARGET_ENGINE_QUICKJS)
#include "quickjs.h"
#include "quicks-runtime.h"
#endif

typedef napi_value (*napi_module_init)(napi_env env, napi_value exports);

namespace nativescript {
extern std::unordered_map<std::string, napi_module_init> napiModuleRegistry;
}

using namespace nativescript;
using namespace std;

namespace {

// Cache for package.json \"type\" field lookups
thread_local std::unordered_map<std::string, bool> g_modulePackageTypeCache;

// Strip shebang line from source code (e.g., #!/usr/bin/env node)
std::string StripShebang(const std::string& source) {
  if (source.size() >= 2 && source[0] == '#' && source[1] == '!') {
    size_t lineEnd = source.find('\n');
    if (lineEnd != std::string::npos) {
      return source.substr(lineEnd + 1);
    }
    return "";  // Entire file is just a shebang
  }
  return source;
}

#if defined(TARGET_ENGINE_HERMES) || defined(TARGET_ENGINE_JSC)
std::string RewriteCommonJSDynamicImportsForFallbackEngines(
    const std::string& source) {
  static const std::regex kDynamicImportPattern(
      R"((^|[^A-Za-z0-9_$\.])import\s*\()",
      std::regex::ECMAScript | std::regex::multiline);
  return std::regex_replace(source, kDynamicImportPattern,
                            "$1__dynamicImport(");
}

std::string TrimFallbackESMToken(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string> SplitFallbackESMList(const std::string& value) {
  std::vector<std::string> parts;
  std::stringstream stream(value);
  std::string part;
  while (std::getline(stream, part, ',')) {
    part = TrimFallbackESMToken(part);
    if (!part.empty()) {
      parts.push_back(part);
    }
  }
  return parts;
}

std::string EscapeFallbackESMSpecifier(const std::string& specifier) {
  std::string escaped;
  escaped.reserve(specifier.size());
  for (char c : specifier) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\'':
        escaped += "\\'";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}

std::string FallbackESMRequireExpression(const std::string& specifier) {
  return "require('" + EscapeFallbackESMSpecifier(specifier) + "')";
}

std::string RewriteFallbackESMImportBindings(const std::string& bindings) {
  std::string result;
  for (const auto& part : SplitFallbackESMList(bindings)) {
    static const std::regex kAliasPattern(
        R"(^([A-Za-z_$][A-Za-z0-9_$]*)\s+as\s+([A-Za-z_$][A-Za-z0-9_$]*)$)");
    std::smatch alias;
    if (std::regex_match(part, alias, kAliasPattern)) {
      result += (result.empty() ? "" : ", ");
      result += alias[1].str() + ": " + alias[2].str();
    } else {
      result += (result.empty() ? "" : ", ");
      result += part;
    }
  }
  return result;
}

std::string FallbackESMExportAssignments(const std::string& exports,
                                         const std::string& sourceObject = "") {
  std::string result;
  for (const auto& part : SplitFallbackESMList(exports)) {
    static const std::regex kAliasPattern(
        R"(^([A-Za-z_$][A-Za-z0-9_$]*)\s+as\s+([A-Za-z_$][A-Za-z0-9_$]*)$)");
    std::smatch alias;
    std::string local = part;
    std::string exported = part;
    if (std::regex_match(part, alias, kAliasPattern)) {
      local = alias[1].str();
      exported = alias[2].str();
    }

    if (!result.empty()) {
      result += "\n";
    }
    result += "exports." + exported + " = ";
    result += sourceObject.empty() ? local : sourceObject + "." + local;
    result += ";";
  }
  return result;
}

template <typename Callback>
std::string RegexReplaceWithFallbackESMCallback(const std::string& source,
                                                const std::regex& pattern,
                                                Callback callback) {
  std::string result;
  std::sregex_iterator it(source.begin(), source.end(), pattern);
  std::sregex_iterator end;
  size_t last = 0;
  for (; it != end; ++it) {
    const std::smatch& match = *it;
    result.append(source, last, match.position() - last);
    result += callback(match);
    last = match.position() + match.length();
  }
  result.append(source, last, std::string::npos);
  return result;
}

std::string TransformESModuleForFallbackEngines(const std::string& source) {
  std::string result = source;
  int tempIndex = 0;

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]*,[ \t]*\*[ \t]+as[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [&](const std::smatch& match) {
        std::string module = "__esm_import_" + std::to_string(tempIndex++);
        return "const " + module + " = " +
               FallbackESMRequireExpression(match[3].str()) + ";\nconst " +
               match[1].str() + " = " + module + ".default;\nconst " +
               match[2].str() + " = " + module + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]*,[ \t]*\{([^}]*)\}[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [&](const std::smatch& match) {
        std::string module = "__esm_import_" + std::to_string(tempIndex++);
        return "const " + module + " = " +
               FallbackESMRequireExpression(match[3].str()) + ";\nconst " +
               match[1].str() + " = " + module + ".default;\nconst {" +
               RewriteFallbackESMImportBindings(match[2].str()) + "} = " +
               module + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+\{([^}]*)\}[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "const {" + RewriteFallbackESMImportBindings(match[1].str()) +
               "} = " + FallbackESMRequireExpression(match[2].str()) + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+\*[ \t]+as[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "const " + match[1].str() + " = " +
               FallbackESMRequireExpression(match[2].str()) + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "const " + match[1].str() + " = " +
               FallbackESMRequireExpression(match[2].str()) + ".default;";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(R"(^[ \t]*import[ \t]+['"]([^'"]+)['"][ \t]*;?)",
                 std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return FallbackESMRequireExpression(match[1].str()) + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*export[ \t]+\*[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "Object.assign(exports, " +
               FallbackESMRequireExpression(match[1].str()) + ");";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*export[ \t]+\{([^}]*)\}[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [&](const std::smatch& match) {
        std::string module = "__esm_export_" + std::to_string(tempIndex++);
        return "const " + module + " = " +
               FallbackESMRequireExpression(match[2].str()) + ";\n" +
               FallbackESMExportAssignments(match[1].str(), module);
      });

  result = std::regex_replace(
      result,
      std::regex(R"(^[ \t]*export[ \t]+default[ \t]+function[ \t]*)",
                 std::regex::ECMAScript | std::regex::multiline),
      "exports.default = function ");

  result = std::regex_replace(
      result,
      std::regex(R"(^[ \t]*export[ \t]+default[ \t]+class[ \t]*)",
                 std::regex::ECMAScript | std::regex::multiline),
      "exports.default = class ");

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*export[ \t]+function[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]*\()",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "exports." + match[1].str() + " = function " + match[1].str() +
               "(";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*export[ \t]+class[ \t]+([A-Za-z_$][A-Za-z0-9_$]*))",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "exports." + match[1].str() + " = class " + match[1].str();
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*export[ \t]+(const|let|var)[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]*=[ \t]*([^;\r\n]*);?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return match[1].str() + " " + match[2].str() + " = " +
               match[3].str() + ";\nexports." + match[2].str() + " = " +
               match[2].str() + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(R"(^[ \t]*export[ \t]*\{([^}]*)\}[ \t]*;)",
                 std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return FallbackESMExportAssignments(match[1].str());
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(R"(^[ \t]*export[ \t]+default[ \t]+([^;\r\n]*);?)",
                 std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "exports.default = " + match[1].str() + ";";
      });

  return RewriteCommonJSDynamicImportsForFallbackEngines(result);
}
#endif

// Check if path has .cjs extension (explicitly CommonJS)
bool IsCJSModule(const std::string& path) {
  return path.size() >= 4 && path.compare(path.size() - 4, 4, ".cjs") == 0;
}

// Find nearest package.json by walking up from directory
std::string FindNearestPackageJson(const std::filesystem::path& startDir) {
  std::filesystem::path current = startDir;

  while (!current.empty() && current != current.root_path()) {
    std::filesystem::path packagePath = current / "package.json";
    std::error_code ec;
    if (std::filesystem::exists(packagePath, ec) && !ec) {
      return packagePath.string();
    }
    current = current.parent_path();
  }

  return "";
}

// Check if package.json has "type": "module"
bool IsPackageTypeModule(const std::string& packageJsonPath) {
  auto cacheIt = g_modulePackageTypeCache.find(packageJsonPath);
  if (cacheIt != g_modulePackageTypeCache.end()) {
    return cacheIt->second;
  }

  bool isModule = false;

  std::ifstream file(packageJsonPath);
  if (file.is_open()) {
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    // Simple JSON parsing for "type": "module"
    size_t typePos = content.find("\"type\"");
    if (typePos != std::string::npos) {
      size_t colonPos = content.find(':', typePos + 6);
      if (colonPos != std::string::npos) {
        size_t valueStart = content.find('"', colonPos + 1);
        if (valueStart != std::string::npos) {
          size_t valueEnd = content.find('"', valueStart + 1);
          if (valueEnd != std::string::npos) {
            std::string typeValue =
                content.substr(valueStart + 1, valueEnd - valueStart - 1);
            isModule = (typeValue == "module");
          }
        }
      }
    }
  }

  g_modulePackageTypeCache[packageJsonPath] = isModule;
  return isModule;
}

// Determine if a .js file should be treated as ESM based on nearest
// package.json
bool ShouldTreatJsAsESModule(const std::string& path) {
  std::filesystem::path filePath(path);
  std::string packageJson = FindNearestPackageJson(filePath.parent_path());

  if (!packageJson.empty()) {
    return IsPackageTypeModule(packageJson);
  }

  return false;  // Default to CommonJS
}

bool PathExistsWithExactCase(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return false;
  }

  std::filesystem::path normalized = path.lexically_normal();
  std::filesystem::path current;

  for (const auto& component : normalized) {
    if (component == normalized.root_name() ||
        component == normalized.root_directory()) {
      current /= component;
      continue;
    }

    if (component == ".") {
      continue;
    }

    if (component == "..") {
      current = current.parent_path();
      continue;
    }

    std::filesystem::path parent = current.empty() ? "." : current;
    bool matched = false;
    std::error_code iterEc;
    for (std::filesystem::directory_iterator it(parent, iterEc), end;
         !iterEc && it != end; it.increment(iterEc)) {
      if (it->path().filename() == component) {
        matched = true;
        break;
      }
    }

    if (iterEc) {
      // On device the sandbox forbids enumerating everything above the app
      // container (/private/var/...), so case verification is only possible
      // for listable levels; exists() already vouched for the full path.
      current /= component;
      continue;
    }
    if (!matched) {
      return false;
    }

    current /= component;
  }

  return true;
}

bool IsRegularFileWithExactCase(const std::filesystem::path& path) {
  std::error_code ec;
  return PathExistsWithExactCase(path) &&
         std::filesystem::is_regular_file(path, ec) && !ec;
}

bool IsDirectoryWithExactCase(const std::filesystem::path& path) {
  std::error_code ec;
  return PathExistsWithExactCase(path) &&
         std::filesystem::is_directory(path, ec) && !ec;
}

napi_value LoadRegisteredNapiModule(napi_env env,
                                    const std::string& moduleName) {
  const auto loadByName = [&](const std::string& name) -> napi_value {
    auto it = nativescript::napiModuleRegistry.find(name);
    if (it == nativescript::napiModuleRegistry.end() || it->second == nullptr) {
      return nullptr;
    }

    napi_value moduleObj;
    napi_create_object(env, &moduleObj);

    napi_value exports;
    napi_create_object(env, &exports);

    napi_value moduleExports = it->second(env, exports);

    bool hasPendingException = false;
    napi_is_exception_pending(env, &hasPendingException);
    if (hasPendingException) {
      napi_value exception;
      napi_get_and_clear_last_exception(env, &exception);
      throw NativeScriptException(
          env, exception, "Error initializing native module '" + name + "'");
    }

    if (moduleExports == nullptr) {
      moduleExports = exports;
    }

    napi_set_named_property(env, moduleObj, "exports", moduleExports);
    return moduleObj;
  };

  napi_value moduleObj = loadByName(moduleName);
  if (moduleObj != nullptr) {
    return moduleObj;
  }

  if (moduleName.rfind("node:", 0) == 0) {
    return loadByName(moduleName.substr(5));
  }

  return nullptr;
}

std::string ModulePathToURL(const std::string& modulePath) {
  if (modulePath.rfind("file://", 0) == 0) {
    return modulePath;
  }

  if (modulePath.rfind("nativescript:", 0) == 0 ||
      modulePath.rfind("node:", 0) == 0) {
    return "nativescript:" + modulePath;
  }

  if (!modulePath.empty() && modulePath[0] == '/') {
    return "file://" + modulePath;
  }

  return "nativescript:" + modulePath;
}

bool IsNodeBuiltinSpecifier(const std::string& specifier) {
  static const std::unordered_set<std::string> kBuiltins = {
      "url",         "node:url",         "fs",   "node:fs",
      "fs/promises", "node:fs/promises", "path", "node:path",
      "vm",          "node:vm",          "web",  "node:web",
      "stream/web",  "node:stream/web"};
  return kBuiltins.contains(specifier);
}

std::string NormalizeNodeBuiltinSpecifier(const std::string& specifier) {
  if (specifier.rfind("node:", 0) == 0) {
    return specifier.substr(5);
  }
  return specifier;
}

std::string EscapeForSingleQuotedJsString(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());

  for (char c : value) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\'':
        escaped += "\\'";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      default:
        escaped += c;
        break;
    }
  }

  return escaped;
}

std::string NormalizeRegisteredNapiModuleSpecifier(
    const std::string& specifier) {
  auto it = nativescript::napiModuleRegistry.find(specifier);
  if (it != nativescript::napiModuleRegistry.end() && it->second != nullptr) {
    return specifier;
  }

  if (specifier.rfind("node:", 0) == 0) {
    std::string withoutPrefix = specifier.substr(5);
    it = nativescript::napiModuleRegistry.find(withoutPrefix);
    if (it != nativescript::napiModuleRegistry.end() && it->second != nullptr) {
      return withoutPrefix;
    }
  }

  return "";
}

std::string GetRegisteredNapiESModuleSource(const std::string& specifier) {
  std::string normalized = NormalizeRegisteredNapiModuleSpecifier(specifier);
  if (normalized.empty()) {
    return "";
  }

  std::string escapedSpecifier = EscapeForSingleQuotedJsString(normalized);
  return R"(
const __load = (name) => {
  if (typeof globalThis.require === "function") {
    return globalThis.require(name);
  }
  if (typeof globalThis.__nativeRequire === "function") {
    const dir = typeof globalThis.__approot === "string" ? `${globalThis.__approot}/app` : "";
    return globalThis.__nativeRequire(name, dir);
  }
  throw new Error(`Cannot load native module '${name}'`);
};
const __nativeModule = __load(')" +
         escapedSpecifier + R"(');
export default __nativeModule;
)";
}

std::string GetBuiltinESModuleSource(const std::string& specifier) {
  const auto builtinName = NormalizeNodeBuiltinSpecifier(specifier);

  if (builtinName == "url") {
    return R"(
const __toURL = (input) => input instanceof URL ? input : new URL(String(input));
export const URL = globalThis.URL;
export const URLSearchParams = globalThis.URLSearchParams;
export function pathToFileURL(path) {
  const value = String(path);
  return new URL(value.startsWith("/") ? `file://${value}` : `file:///${value}`);
}
export function fileURLToPath(value) {
  const u = __toURL(value);
  if (u.protocol !== "file:") {
    throw new TypeError("The URL must be of scheme file:");
  }
  return decodeURIComponent(u.pathname);
}
export default { URL, URLSearchParams, pathToFileURL, fileURLToPath };
)";
  }

  if (builtinName == "fs") {
    return R"(
const __load = (name) => {
  if (typeof globalThis.require === "function") {
    return globalThis.require(name);
  }
  if (typeof globalThis.__nativeRequire === "function") {
    const dir = typeof globalThis.__approot === "string" ? `${globalThis.__approot}/app` : "";
    return globalThis.__nativeRequire(name, dir);
  }
  throw new Error(`Cannot load builtin module '${name}'`);
};
const __fs = __load("node:fs");
export const readFileSync = __fs.readFileSync;
export const writeFileSync = __fs.writeFileSync;
export const existsSync = __fs.existsSync;
export const mkdirSync = __fs.mkdirSync;
export const readdirSync = __fs.readdirSync;
export const statSync = __fs.statSync;
export const lstatSync = __fs.lstatSync;
export const unlinkSync = __fs.unlinkSync;
export const rmSync = __fs.rmSync;
export const readFile = __fs.readFile;
export const writeFile = __fs.writeFile;
export const constants = __fs.constants;
export const promises = __fs.promises;
export default __fs;
)";
  }

  if (builtinName == "fs/promises") {
    return R"(
const __load = (name) => {
  if (typeof globalThis.require === "function") {
    return globalThis.require(name);
  }
  if (typeof globalThis.__nativeRequire === "function") {
    const dir = typeof globalThis.__approot === "string" ? `${globalThis.__approot}/app` : "";
    return globalThis.__nativeRequire(name, dir);
  }
  throw new Error(`Cannot load builtin module '${name}'`);
};
const __fsp = __load("node:fs").promises;
export const readFile = __fsp.readFile;
export const writeFile = __fsp.writeFile;
export const mkdir = __fsp.mkdir;
export const readdir = __fsp.readdir;
export const stat = __fsp.stat;
export const lstat = __fsp.lstat;
export const unlink = __fsp.unlink;
export const rm = __fsp.rm;
export default __fsp;
)";
  }

  if (builtinName == "path") {
    return R"(
const __load = (name) => {
  if (typeof globalThis.require === "function") {
    return globalThis.require(name);
  }
  if (typeof globalThis.__nativeRequire === "function") {
    const dir = typeof globalThis.__approot === "string" ? `${globalThis.__approot}/app` : "";
    return globalThis.__nativeRequire(name, dir);
  }
  throw new Error(`Cannot load builtin module '${name}'`);
};
const __path = __load("node:path");
export const basename = __path.basename;
export const dirname = __path.dirname;
export const extname = __path.extname;
export const isAbsolute = __path.isAbsolute;
export const join = __path.join;
export const normalize = __path.normalize;
export const parse = __path.parse;
export const format = __path.format;
export const relative = __path.relative;
export const resolve = __path.resolve;
export const toNamespacedPath = __path.toNamespacedPath;
export const sep = __path.sep;
export const delimiter = __path.delimiter;
export const posix = __path.posix;
export const win32 = __path.win32;
export default __path;
)";
  }

  if (builtinName == "vm") {
    return R"(
const __load = (name) => {
  if (typeof globalThis.require === "function") {
    return globalThis.require(name);
  }
  if (typeof globalThis.__nativeRequire === "function") {
    const dir = typeof globalThis.__approot === "string" ? `${globalThis.__approot}/app` : "";
    return globalThis.__nativeRequire(name, dir);
  }
  throw new Error(`Cannot load builtin module '${name}'`);
};
const __vm = __load("node:vm");
export const Script = __vm.Script;
export const Module = __vm.Module;
export const SourceTextModule = __vm.SourceTextModule;
export const SyntheticModule = __vm.SyntheticModule;
export const compileFunction = __vm.compileFunction;
export const constants = __vm.constants;
export const createContext = __vm.createContext;
export const isContext = __vm.isContext;
export const measureMemory = __vm.measureMemory;
export const runInContext = __vm.runInContext;
export const runInNewContext = __vm.runInNewContext;
export const runInThisContext = __vm.runInThisContext;
export default __vm;
)";
  }

  if (builtinName == "web") {
    return R"(
const __load = (name) => {
  if (typeof globalThis.require === "function") {
    return globalThis.require(name);
  }
  if (typeof globalThis.__nativeRequire === "function") {
    const dir = typeof globalThis.__approot === "string" ? `${globalThis.__approot}/app` : "";
    return globalThis.__nativeRequire(name, dir);
  }
  throw new Error(`Cannot load builtin module '${name}'`);
};
const __web = __load("web");
export const fetch = __web.fetch;
export const Headers = __web.Headers;
export const Request = __web.Request;
export const Response = __web.Response;
export const WebSocket = __web.WebSocket;
export const ReadableStream = __web.ReadableStream;
export const WritableStream = __web.WritableStream;
export const TransformStream = __web.TransformStream;
export default __web;
)";
  }

  if (builtinName == "stream/web") {
    return R"(
const __load = (name) => {
  if (typeof globalThis.require === "function") {
    return globalThis.require(name);
  }
  if (typeof globalThis.__nativeRequire === "function") {
    const dir = typeof globalThis.__approot === "string" ? `${globalThis.__approot}/app` : "";
    return globalThis.__nativeRequire(name, dir);
  }
  throw new Error(`Cannot load builtin module '${name}'`);
};
const __streamWeb = __load("stream/web");
export const ReadableStream = __streamWeb.ReadableStream;
export const ReadableStreamDefaultReader = __streamWeb.ReadableStreamDefaultReader;
export const WritableStream = __streamWeb.WritableStream;
export const TransformStream = __streamWeb.TransformStream;
export const ByteLengthQueuingStrategy = __streamWeb.ByteLengthQueuingStrategy;
export const CountQueuingStrategy = __streamWeb.CountQueuingStrategy;
export default __streamWeb;
)";
  }

  return "";
}
}  // namespace

ModuleInternal::ModuleInternal()
    : m_env(nullptr),
      m_requireFunction(nullptr),
      m_requireFactoryFunction(nullptr) {}

void ModuleInternal::DeInit() {
  // Clear the package.json type cache
  g_modulePackageTypeCache.clear();

  if (m_env != nullptr) {
    if (m_requireFunction != nullptr) {
      napi_delete_reference(m_env, m_requireFunction);
      m_requireFunction = nullptr;
    }
    if (m_requireFactoryFunction != nullptr) {
      napi_delete_reference(m_env, m_requireFactoryFunction);
      m_requireFactoryFunction = nullptr;
    }
  }

  for (const auto& pair : this->m_requireCache) {
    if (m_env != nullptr) {
      napi_delete_reference(m_env, pair.second);
    }
  }
  this->m_requireCache.clear();
  m_env = nullptr;
}

void ModuleInternal::Init(napi_env env, const std::string& baseDir) {
  napi_status status;

  m_env = env;

#ifdef TARGET_ENGINE_V8
  // Bootstrap V8 ES module hooks early so dynamic import() from CommonJS
  // settles even before the first explicit .mjs module load.
  napi_value bootstrapSource;
  if (napi_create_string_utf8(env, "export default 0;", NAPI_AUTO_LENGTH,
                              &bootstrapSource) == napi_ok) {
    const std::string bootstrapModulePath =
        RuntimeConfig.ApplicationPath + "/app/.nativescript-esm-bootstrap.mjs";
    napi_value bootstrapNamespace;
    napi_status bootstrapStatus = napi_run_script_as_module(
        env, bootstrapSource, bootstrapModulePath.c_str(), &bootstrapNamespace);
    if (bootstrapStatus != napi_ok) {
      bool pendingException = false;
      napi_is_exception_pending(env, &pendingException);
      if (pendingException) {
        napi_value ignored;
        napi_get_and_clear_last_exception(env, &ignored);
      }
    }
  }
#endif

  const char* requireFactoryScript = R"(
    (function () {
        return function require_factory(requireInternal, dirName) {
		return function require(modulePath) {
            if(typeof global.__requireOverride !== "undefined") {
				var result = global.__requireOverride(modulePath, dirName);
				if(result) {
					return result;
				}
			}
			return requireInternal(modulePath, dirName);
		}
	}
})();
)";

  napi_value source;
  napi_create_string_utf8(env, requireFactoryScript, NAPI_AUTO_LENGTH, &source);

  napi_value global;
  napi_get_global(env, &global);

  napi_value globalEnv;
  napi_create_external(env, env, nullptr, nullptr, &globalEnv);
  napi_set_named_property(env, global, "__globalEnv", globalEnv);

  napi_value result;
  status = napi_run_script(env, source, &result);
  assert(status == napi_ok);

  m_requireFactoryFunction = napi_util::make_ref(m_env, result);

  napi_value requireFunction = napi_util::napi_set_function(
      env, global, "__nativeRequire", RequireCallback, this);
  m_requireFunction = napi_util::make_ref(m_env, requireFunction);

  napi_value globalRequire = GetRequireFunction(
      env, baseDir.empty() ? RuntimeConfig.ApplicationPath : baseDir);
  status = napi_set_named_property(env, global, "require", globalRequire);
  assert(status == napi_ok);

#if defined(TARGET_ENGINE_QUICKJS)
  InitQuickJSESModuleLoader(env);
#endif
}

napi_value ModuleInternal::GetRequireFunction(napi_env env,
                                              const std::string& dirName) {
  napi_value requireFunc;

  auto itFound = m_requireCache.find(dirName);

  if (itFound != m_requireCache.end()) {
    requireFunc = napi_util::get_ref_value(env, itFound->second);
  } else {
    napi_value requireFuncFactory =
        napi_util::get_ref_value(env, m_requireFactoryFunction);

    napi_value requireInternalFunc =
        napi_util::get_ref_value(env, m_requireFunction);

    napi_value args[2];
    args[0] = requireInternalFunc;
    napi_create_string_utf8(env, dirName.c_str(), NAPI_AUTO_LENGTH, &args[1]);

    napi_value thiz;
    napi_create_object(env, &thiz);

    napi_value result;
    napi_status status =
        napi_call_function(env, thiz, requireFuncFactory, 2, args, &result);
    assert(status == napi_ok && result != nullptr);

    bool isFunction = napi_util::is_of_type(env, result, napi_function);
    assert(isFunction);

    requireFunc = result;

    napi_ref poFunc = napi_util::make_ref(env, requireFunc);
    m_requireCache.emplace(dirName, poFunc);
  }

  return requireFunc;
}

napi_value ModuleInternal::RequireCallback(napi_env env,
                                           napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(0)
  try {
    auto thiz = static_cast<ModuleInternal*>(data);
    return thiz->RequireCallbackImpl(env, info);
  } catch (NativeScriptException& e) {
    e.ReThrowToJS(env);
  } catch (std::exception& e) {
    stringstream ss;
    ss << "Error: C++ Exception: " << e.what() << endl;
    NativeScriptException nsEx(ss.str());
    nsEx.ReThrowToJS(env);
  } catch (...) {
    NativeScriptException nsEx(std::string("Error: c++ exception!"));
    nsEx.ReThrowToJS(env);
  }

  return nullptr;
}

napi_value ModuleInternal::Require(napi_env env, const std::string& moduleName,
                                   const std::string& callingModuleDirName) {
  auto isData = false;

  napi_value moduleObj =
      LoadImpl(env, moduleName, callingModuleDirName, isData);

  if (isData) {
    assert(!napi_util::is_null_or_undefined(env, moduleObj));
    return moduleObj;
  } else {
    // Check if this is an ES module by looking for __esModule property
    bool hasEsModuleProp;
    napi_status status =
        napi_has_named_property(env, moduleObj, "__esModule", &hasEsModuleProp);

    bool isEsModule = false;
    if (status == napi_ok && hasEsModuleProp) {
      napi_value esModuleFlag;
      napi_get_named_property(env, moduleObj, "__esModule", &esModuleFlag);
      napi_get_value_bool(env, esModuleFlag, &isEsModule);
    }

    if (isEsModule) {
      // For ES modules, return the module namespace directly
      return moduleObj;
    } else {
      // For CommonJS modules, return the exports
      napi_value exports;
      napi_get_named_property(env, moduleObj, "exports", &exports);
      assert(!napi_util::is_null_or_undefined(env, exports));
      return exports;
    }
  }
}

napi_value ModuleInternal::RequireCallbackImpl(napi_env env,
                                               napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc != 2) {
    throw NativeScriptException(
        string("require should be called with two parameters"));
  }
  if (!napi_util::is_of_type(env, argv[0], napi_string)) {
    throw NativeScriptException(
        string("require's first parameter should be string"));
  }
  if (!napi_util::is_of_type(env, argv[1], napi_string)) {
    throw NativeScriptException(
        string("require's second parameter should be string"));
  }

  string moduleName = napi_util::get_cxx_string(env, argv[0]);
  string callingModuleDirName = napi_util::get_cxx_string(env, argv[1]);

  try {
    return Require(env, moduleName, callingModuleDirName);
  } catch (NativeScriptException& e) {
    e.ReThrowToJS(env);
    return nullptr;
  }
}

napi_value ModuleInternal::RequireNativeCallback(napi_env env,
                                                 napi_callback_info info) {
  void* data;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  auto cb = reinterpret_cast<napi_register_module_v*>(data);
  napi_value exports;
  napi_create_object(env, &exports);
  return cb(env, exports);
}

napi_status ModuleInternal::Load(napi_env env, const std::string& path) {
  napi_value global;
  napi_get_global(env, &global);

  napi_value require;
  napi_get_named_property(env, global, "require", &require);

  napi_value args[1];
  napi_create_string_utf8(env, path.c_str(), path.size(), &args[0]);

  napi_value result;
  napi_status status =
      napi_call_function(env, global, require, 1, args, &result);
  return status;
}

void ModuleInternal::LoadWorker(napi_env env, const string& path) {
  Load(env, path);
  bool hasPendingException;
  napi_is_exception_pending(env, &hasPendingException);

  if (hasPendingException) {
    napi_value error;
    napi_get_and_clear_last_exception(env, &error);
    // TODO
    // CallbackHandlers::CallWorkerScopeOnErrorHandle(env, error);
  }
}

void ModuleInternal::CheckFileExists(napi_env env, const std::string& path,
                                     const std::string& baseDir) {
  struct stat buffer;
  if (stat(path.c_str(), &buffer) != 0) {
    std::string errMsg = "Module not found: " + path;
    throw NativeScriptException(errMsg);
  }
  if (baseDir != "") {
    std::string fullPath = baseDir + "/" + path;
    if (stat(fullPath.c_str(), &buffer) != 0) {
      std::string errMsg = "Module not found: " + fullPath;
      throw NativeScriptException(errMsg);
    }
  }
}

napi_value ModuleInternal::LoadInternalModule(napi_env env,
                                              const std::string& moduleName) {
  auto nodeModule = Node::LoadInternalModule(env, moduleName);
  if (nodeModule != nullptr) {
    return nodeModule;
  }

#ifdef __APPLE__
  auto webModule = Web::LoadInternalModule(env, moduleName);
  if (webModule != nullptr) {
    return webModule;
  }
#endif

  auto napiModule = LoadRegisteredNapiModule(env, moduleName);
  if (napiModule != nullptr) {
    return napiModule;
  }

  if (moduleName == "url" || moduleName == "node:url") {
    napi_value moduleObj;
    napi_create_object(env, &moduleObj);
    napi_value url;
    napi_value exports;
    napi_create_object(env, &exports);
    napi_get_named_property(env, napi_util::global(env), "URL", &url);
    napi_set_named_property(env, exports, "URL", url);
    napi_set_named_property(env, moduleObj, "exports", exports);
    napi_util::napi_set_function(
        env, exports, "pathToFileURL",
        [](napi_env env, napi_callback_info info) -> napi_value {
          return napi_util::to_js_string(env, "file://");
        });
    return moduleObj;
  }
  return nullptr;
}

std::string ModuleInternal::ResolvePathFromPackageJson(
    napi_env env, const std::string& packageJsonPath, bool& error) {
  error = false;
  if (!IsRegularFileWithExactCase(packageJsonPath)) {
    return "";
  }

  std::ifstream packageJsonFile(packageJsonPath);
  if (!packageJsonFile.is_open()) {
    // Missing package.json is not fatal for directory resolution.
    error = false;
    return "";
  }
  std::string line;
  std::stringstream packageJsonStream;
  while (std::getline(packageJsonFile, line)) {
    packageJsonStream << line;
  }
  packageJsonFile.close();
  std::string packageJson = packageJsonStream.str();
  napi_value obj = JsonParse(env, packageJson);
  if (obj == nullptr) {
    bool hasPendingException = false;
    napi_is_exception_pending(env, &hasPendingException);
    if (hasPendingException) {
      napi_value exception;
      napi_get_and_clear_last_exception(env, &exception);
    }
    error = true;
    return "";
  }
  bool hasMain = false;
  napi_status hasMainStatus =
      napi_has_named_property(env, obj, "main", &hasMain);
  if (hasMainStatus != napi_ok || !hasMain) {
    // package.json without "main" should fall back to
    // index.js/index.mjs/index.cjs
    error = false;
    return "";
  }
  napi_value mainValue;
  napi_get_named_property(env, obj, "main", &mainValue);
  if (mainValue == nullptr) {
    error = false;
    return "";
  }

  napi_valuetype type;
  napi_typeof(env, mainValue, &type);
  if (type != napi_string) {
    error = false;
    return "";
  }

  std::string main = napi_util::get_cxx_string(env, mainValue);
  if (main.empty()) {
    error = false;
    return "";
  }

  std::filesystem::path packageJsonDir(packageJsonPath);
  std::filesystem::path packageJsonDirName =
      packageJsonDir.parent_path().string();
  std::filesystem::path mainPath = packageJsonDirName / main;

  if (IsDirectoryWithExactCase(mainPath)) {
    mainPath = mainPath / "package.json";
    if (IsRegularFileWithExactCase(mainPath)) {
      return ResolvePathFromPackageJson(env, mainPath.string(), error);
    }

    std::filesystem::path indexMjs = mainPath.parent_path() / "index.mjs";
    if (IsRegularFileWithExactCase(indexMjs)) {
      return indexMjs.string();
    }

    std::filesystem::path indexJs = mainPath.parent_path() / "index.js";
    if (IsRegularFileWithExactCase(indexJs)) {
      return indexJs.string();
    }

    std::filesystem::path indexCjs = mainPath.parent_path() / "index.cjs";
    if (IsRegularFileWithExactCase(indexCjs)) {
      return indexCjs.string();
    }

    error = false;
    return "";
  }

  if (IsRegularFileWithExactCase(mainPath)) {
    return mainPath.string();
  }

  // Support extensionless "main" entries (e.g. "bundle") by resolving to
  // modern ESM/CJS bundle outputs.
  if (!mainPath.has_extension()) {
    std::filesystem::path mjsPath = mainPath;
    mjsPath.replace_extension(".mjs");
    if (IsRegularFileWithExactCase(mjsPath)) {
      return mjsPath.string();
    }

    std::filesystem::path jsPath = mainPath;
    jsPath.replace_extension(".js");
    if (IsRegularFileWithExactCase(jsPath)) {
      return jsPath.string();
    }

    std::filesystem::path cjsPath = mainPath;
    cjsPath.replace_extension(".cjs");
    if (IsRegularFileWithExactCase(cjsPath)) {
      return cjsPath.string();
    }
  }

  // Unresolvable "main" should fall back to index.js/index.mjs/index.cjs.
  error = false;
  return "";
}

std::string ModuleInternal::ResolvePath(napi_env env,
                                        const std::string& baseDir,
                                        const std::string& moduleName) {
  std::string moduleNameCopy = moduleName;

  if (moduleName.starts_with("~")) {
    moduleNameCopy = RuntimeConfig.ApplicationPath + moduleNameCopy.substr(1);
  }

  std::filesystem::path baseDirPath(baseDir);
  std::filesystem::path moduleNamePath(moduleNameCopy);
  std::filesystem::path fullPath = baseDirPath / moduleNamePath;

  // Normalize the path to remove redundant ./ sequences
  fullPath = fullPath.lexically_normal();

  bool exists = PathExistsWithExactCase(fullPath);
  bool isDirectory = exists && IsDirectoryWithExactCase(fullPath);
  bool isNonRelativeModule =
      !moduleNameCopy.starts_with("./") && !moduleNameCopy.starts_with("../") &&
      !moduleNameCopy.starts_with("/") && !moduleNameCopy.starts_with("~");

  if (exists == true && isDirectory == true) {
    // Try .mjs first for ES modules
    std::filesystem::path mjsFile = fullPath;
    mjsFile = mjsFile.replace_extension(".mjs");
    if (IsRegularFileWithExactCase(mjsFile)) {
      return mjsFile.string();
    }

    // Then try .js for CommonJS
    std::filesystem::path jsFile = fullPath;
    jsFile = jsFile.replace_extension(".js");
    if (IsRegularFileWithExactCase(jsFile)) {
      return jsFile.string();
    }

    // Then try .cjs for explicit CommonJS
    std::filesystem::path cjsFile = fullPath;
    cjsFile = cjsFile.replace_extension(".cjs");
    if (IsRegularFileWithExactCase(cjsFile)) {
      return cjsFile.string();
    }
  }

  if (exists == false) {
    std::filesystem::path appModulesPath;
    bool hasAppModulesBase = false;

    if (isNonRelativeModule) {
      appModulesPath = std::filesystem::path(RuntimeConfig.ApplicationPath) /
                       "tns_modules" / moduleNameCopy;
      hasAppModulesBase = true;
      if (PathExistsWithExactCase(appModulesPath)) {
        fullPath = appModulesPath;
        exists = true;
        isDirectory = IsDirectoryWithExactCase(fullPath);
      } else {
        // Preserve legacy runtime behavior for extensionless non-relative
        // requires by probing app/tns_modules before caller-relative paths.
        std::filesystem::path appModulesMjs = appModulesPath.string() + ".mjs";
        if (IsRegularFileWithExactCase(appModulesMjs)) {
          return appModulesMjs.string();
        }

        std::filesystem::path appModulesJs = appModulesPath.string() + ".js";
        if (IsRegularFileWithExactCase(appModulesJs)) {
          return appModulesJs.string();
        }

        std::filesystem::path appModulesJson =
            appModulesPath.string() + ".json";
        if (IsRegularFileWithExactCase(appModulesJson)) {
          return appModulesJson.string();
        }

        std::filesystem::path appModulesCjs = appModulesPath.string() + ".cjs";
        if (IsRegularFileWithExactCase(appModulesCjs)) {
          return appModulesCjs.string();
        }
      }
    }

    // Try .mjs extension first (ES modules have priority)
    std::filesystem::path mjsBase =
        hasAppModulesBase ? appModulesPath : fullPath;
    std::filesystem::path mjsPath = mjsBase.string() + ".mjs";
    if (PathExistsWithExactCase(mjsPath)) {
      exists = true;
      isDirectory = IsDirectoryWithExactCase(mjsPath);
      if (!isDirectory) {
        return mjsPath.string();
      }
    }

    // Try .js extension by appending, preserving dotted filenames
    std::filesystem::path jsBase =
        hasAppModulesBase ? appModulesPath : fullPath;
    std::filesystem::path jsPath = jsBase.string() + ".js";
    if (PathExistsWithExactCase(jsPath)) {
      exists = true;
      isDirectory = IsDirectoryWithExactCase(jsPath);
      fullPath = jsPath;
    }

    // Try .cjs extension for explicit CommonJS
    if (!exists) {
      std::filesystem::path cjsBase =
          hasAppModulesBase ? appModulesPath : fullPath;
      std::filesystem::path cjsPath = cjsBase.string() + ".cjs";
      if (PathExistsWithExactCase(cjsPath)) {
        exists = true;
        isDirectory = IsDirectoryWithExactCase(cjsPath);
        fullPath = cjsPath;
      }
    }
  }

  if (exists == false) {
    throw NativeScriptException("The specified module does not exist: " +
                                moduleName);
  }

  if (isDirectory == false) {
    return fullPath.string();
  }

  std::filesystem::path packageJson = fullPath / "package.json";
  if (std::filesystem::is_regular_file(packageJson)) {
    bool error = false;
    std::string entry =
        this->ResolvePathFromPackageJson(env, packageJson.string(), error);
    if (error) {
      throw NativeScriptException("Unable to locate main entry in " +
                                  packageJson.string());
    }

    if (!entry.empty()) {
      fullPath = entry;
    }
  }

  exists = PathExistsWithExactCase(fullPath);
  isDirectory = exists && IsDirectoryWithExactCase(fullPath);
  if (exists == true && isDirectory == false) {
    return fullPath.string();
  }

  if (exists == false) {
    // Try index.mjs first
    std::filesystem::path indexMjs = fullPath.parent_path() / "index.mjs";
    if (IsRegularFileWithExactCase(indexMjs)) {
      return indexMjs.string();
    }

    // Then try path + ".js" (preserves dotted filenames like "file.name")
    std::filesystem::path jsCandidate = fullPath.string() + ".js";
    if (IsRegularFileWithExactCase(jsCandidate)) {
      return jsCandidate.string();
    }

    // Finally try path + ".cjs" for explicit CommonJS entry files.
    fullPath = fullPath.string() + ".cjs";
  } else {
    // Try index.mjs first in directory
    std::filesystem::path indexMjs = fullPath / "index.mjs";
    if (IsRegularFileWithExactCase(indexMjs)) {
      return indexMjs.string();
    }

    // Then try index.js
    std::filesystem::path indexJs = fullPath / "index.js";
    if (IsRegularFileWithExactCase(indexJs)) {
      fullPath = indexJs;
    } else {
      // Then try index.cjs
      std::filesystem::path indexCjs = fullPath / "index.cjs";
      if (IsRegularFileWithExactCase(indexCjs)) {
        return indexCjs.string();
      }
      fullPath /= "index.js";
    }
  }

  exists = PathExistsWithExactCase(fullPath);
  if (exists == false) {
    throw NativeScriptException("The specified module does not exist: " +
                                moduleName);
  }
  return fullPath.string();
}

napi_value ModuleInternal::LoadImpl(napi_env env, const std::string& moduleName,
                                    const std::string& baseDir, bool& isData) {
  auto pathKind = GetModulePathKind(moduleName);
  auto cachePathKey = (pathKind == ModulePathKind::Global)
                          ? moduleName
                          : (baseDir + "*" + moduleName);

  napi_value result;

  //   DEBUG_WRITE(">>LoadImpl cachePathKey=%s", cachePathKey.c_str());

  auto it = m_loadedModules.find(cachePathKey);

  if (it == m_loadedModules.end()) {
    /**
     * Load internal modules like url, fs and statically registered
     * N-API modules directly for global requires.
     */
    napi_value moduleObj = ModuleInternal::LoadInternalModule(env, moduleName);
    if (moduleObj != nullptr) {
      auto poModuleObj = napi_util::make_ref(env, moduleObj);
      m_loadedModules.emplace(cachePathKey, ModuleCacheEntry(poModuleObj));
      return moduleObj;
    }

    std::string path;

    // Search App System libs
    std::string sys_lib("system_lib://");
    if (moduleName.rfind(sys_lib, 0) == 0) {
      auto pos = moduleName.find(sys_lib);
      path = std::string(moduleName);
      path.replace(pos, sys_lib.length(), "");
    } else if (moduleName.ends_with(".so") || moduleName.ends_with(".dylib") ||
               moduleName.ends_with(".node")) {
      path = moduleName;
    } else {
      path = ResolvePath(env, baseDir, moduleName);
    }

    auto it2 = m_loadedModules.find(path);

    if (it2 == m_loadedModules.end()) {
      if (path.ends_with(".js") || path.ends_with(".mjs") ||
          path.ends_with(".cjs") || path.ends_with(".so") ||
          path.ends_with(".dylib") || path.ends_with(".node")) {
        isData = false;
        result = LoadModule(env, path, cachePathKey);
      } else if (path.ends_with(".json")) {
        isData = true;
        result = LoadData(env, path);
      } else {
        std::filesystem::path filePath(path);

        if (std::filesystem::is_directory(filePath)) {
          std::filesystem::path packageJson = filePath / "package.json";
          if (std::filesystem::is_regular_file(packageJson)) {
            bool error = false;
            std::string packageMain =
                ResolvePathFromPackageJson(env, packageJson.string(), error);
            if (error) {
              throw NativeScriptException("Unable to locate main entry in " +
                                          packageJson.string());
            }
            if (!packageMain.empty() && packageMain != path) {
              return LoadImpl(env, packageMain, baseDir, isData);
            }
          }
        }

        std::filesystem::path fileWithIndexJs = filePath / "index.js";
        std::filesystem::path fileWithIndexMjs = filePath / "index.mjs";
        std::filesystem::path fileWithIndexCjs = filePath / "index.cjs";
        if (IsRegularFileWithExactCase(fileWithIndexMjs)) {
          return LoadImpl(env, fileWithIndexMjs.string(), baseDir, isData);
        } else if (IsRegularFileWithExactCase(fileWithIndexJs)) {
          return LoadImpl(env, fileWithIndexJs.string(), baseDir, isData);
        } else if (IsRegularFileWithExactCase(fileWithIndexCjs)) {
          return LoadImpl(env, fileWithIndexCjs.string(), baseDir, isData);
        }
        std::string errMsg = "Unsupported file extension: " + path;
        throw NativeScriptException(errMsg);
      }
    } else {
      auto& cacheEntry = it2->second;
      isData = cacheEntry.isData;
      result = napi_util::get_ref_value(env, cacheEntry.obj);
    }
  } else {
    auto& cacheEntry = it->second;
    isData = cacheEntry.isData;
    result = napi_util::get_ref_value(env, cacheEntry.obj);
  }

  return result;
}

std::string ModuleInternal::EnsureFileProtocol(const std::string& path) {
  const std::string protocol = "file://";
  if (path.compare(0, protocol.length(), protocol) != 0) {
    return protocol + path;
  }
  return path;
}

napi_value ModuleInternal::LoadModule(napi_env env,
                                      const std::string& modulePath,
                                      const std::string& moduleCacheKey) {
  napi_value result;

  napi_value context;
  napi_get_global(env, &context);

  napi_value moduleObj;
  napi_create_object(env, &moduleObj);

  napi_value exportsObj;
  napi_create_object(env, &exportsObj);

  napi_set_named_property(env, moduleObj, "exports", exportsObj);

  napi_value fullRequiredModulePath;
  napi_create_string_utf8(env, modulePath.c_str(), modulePath.size(),
                          &fullRequiredModulePath);
  napi_set_named_property(env, moduleObj, "filename", fullRequiredModulePath);

  napi_ref poModuleObj = napi_util::make_ref(env, moduleObj);
  TempModule tempModule(this, modulePath, moduleCacheKey, poModuleObj);

  napi_value moduleFunc;

  if (IsESModule(modulePath)) {
    // Handle ES modules
    napi_value esModuleResult = LoadESModule(env, modulePath);

    // Store the namespace as module.exports so the public require() result is
    // the namespace while the cache shape stays compatible with CommonJS.
    napi_set_named_property(env, moduleObj, "exports", esModuleResult);
    tempModule.SaveToCache();
    return moduleObj;
  } else if (modulePath.ends_with(".js") || modulePath.ends_with(".cjs")) {
    napi_value script = LoadScript(env, modulePath, fullRequiredModulePath);
    // DEBUG_WRITE("%s", modulePath.c_str());

    // napi_status status = js_execute_script(
    //     env, script, EnsureFileProtocol(modulePath).c_str(), &moduleFunc);
    napi_status status =
        napi_run_script_source(env, script, modulePath.c_str(), &moduleFunc);
    if (status != napi_ok) {
      bool pendingException;
      napi_is_exception_pending(env, &pendingException);
      napi_value error = nullptr;
      if (pendingException) {
        napi_get_and_clear_last_exception(env, &error);
      }
      if (error) {
        throw NativeScriptException(env, error,
                                    "Error running script " + modulePath);
      } else {
        throw NativeScriptException("Error running script " + modulePath);
      }
    }
  } else if (modulePath.ends_with(".so") || modulePath.ends_with(".dylib") ||
             modulePath.ends_with(".node")) {
    auto handle = dlopen(modulePath.c_str(), RTLD_NOW);
    if (handle == nullptr) {
      auto error = dlerror();
      std::string errMsg(error);
      throw NativeScriptException(errMsg);
    }
    auto func = dlsym(handle, "napi_register_module_v1");

    if (func == nullptr) {
      std::string errMsg("Cannot find 'napi_register_module_v1' in " +
                         modulePath);
      throw NativeScriptException(errMsg);
    }

    auto cb = reinterpret_cast<napi_register_module_v*>(func);
    napi_value exports;
    napi_create_object(env, &exports);
    napi_value result = cb(env, exports);
    napi_set_named_property(env, moduleObj, "exports", result);
    tempModule.SaveToCache();
    return moduleObj;
  } else {
    std::string errMsg = "Unsupported file extension: " + modulePath;
    throw NativeScriptException(errMsg);
  }

  napi_value fileName;
  napi_create_string_utf8(env, modulePath.c_str(), modulePath.size(),
                          &fileName);

  char pathcopy[1024];
  strcpy(pathcopy, modulePath.c_str());
  std::string strDirName(dirname(pathcopy));

  napi_value dirName;
  napi_create_string_utf8(env, strDirName.c_str(), strDirName.size(), &dirName);

  napi_value require = GetRequireFunction(env, strDirName);

  napi_value requireArgs[5] = {moduleObj, exportsObj, require, fileName,
                               dirName};

  napi_set_named_property(env, moduleObj, "require", require);
  napi_util::define_property(env, moduleObj, "id", fileName);

  napi_value thiz;
  napi_create_object(env, &thiz);

  napi_value globalExtends;
  napi_get_named_property(env, context, "__extends", &globalExtends);
  napi_set_named_property(env, thiz, "__extends", globalExtends);

  napi_value callResult;
  napi_status status =
      napi_call_function(env, thiz, moduleFunc, 5, requireArgs, &callResult);
  bool pendingException;
  napi_is_exception_pending(env, &pendingException);
  if (status != napi_ok || pendingException) {
    napi_value exception;
    napi_get_and_clear_last_exception(env, &exception);
    if (exception) {
      throw NativeScriptException(env, exception,
                                  "Error calling module function: ");
    } else {
      throw NativeScriptException("Error calling module function: " +
                                  modulePath);
    }
  }

  tempModule.SaveToCache();
  result = moduleObj;

  return result;
}

napi_value ModuleInternal::LoadScript(napi_env env, const std::string& path,
                                      napi_value fullRequiredModulePath) {
  napi_value scriptText = ModuleInternal::WrapModuleContent(env, path);
  return scriptText;
}

napi_value ModuleInternal::LoadData(napi_env env, const std::string& path) {
  std::string jsonData;
  std::ifstream jsonFile(path);
  if (!jsonFile.is_open()) {
    throw NativeScriptException("Unable to open JSON file: " + path);
  }

  std::stringstream buffer;
  buffer << jsonFile.rdbuf();
  jsonData = buffer.str();
  napi_value json = JsonParse(env, jsonData);

  if (!napi_util::is_of_type(env, json, napi_object)) {
    bool pendingException;
    napi_is_exception_pending(env, &pendingException);
    if (pendingException) {
      napi_value error;
      napi_get_and_clear_last_exception(env, &error);
      throw NativeScriptException(env, error,
                                  "JSON is not valid, file=" + path);
    } else {
      throw NativeScriptException("JSON is not valid, file=" + path);
    }
  }

  napi_ref poObj = napi_util::make_ref(env, json);
  m_loadedModules.emplace(path, ModuleCacheEntry(poObj, true /* isData */));
  return json;
}

// ES Module support functions
bool ModuleInternal::IsESModule(const std::string& path) {
  // .mjs is always ESM
  if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".mjs") == 0) {
    return true;
  }

  // .cjs is always CommonJS
  if (IsCJSModule(path)) {
    return false;
  }

  // .js files: check package.json "type" field
  if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".js") == 0) {
    return ShouldTreatJsAsESModule(path);
  }

  return false;
}

napi_value ModuleInternal::LoadESModule(napi_env env, const std::string& path) {
#if defined(TARGET_ENGINE_V8) || defined(TARGET_ENGINE_QUICKJS)
  try {
    // Use canonicalized module paths so the top-level ESM module identity
    // matches dependency resolution cache keys.
    std::error_code pathError;
    auto absPathFs = std::filesystem::absolute(path, pathError);
    if (pathError) {
      pathError.clear();
      absPathFs = std::filesystem::path(path);
    }
    absPathFs = absPathFs.lexically_normal();
    auto canonicalPath =
        std::filesystem::weakly_canonical(absPathFs, pathError);
    if (!pathError) {
      absPathFs = canonicalPath;
    }
    std::string absPath = absPathFs.string();

    // Read the ES module source
    napi_value scriptContent = WrapModuleContent(env, absPath);

    // Use the new napi_run_script_as_module function
    napi_value moduleNamespace;
    napi_status status = napi_run_script_as_module(
        env, scriptContent, absPath.c_str(), &moduleNamespace);

    if (status != napi_ok) {
      bool pendingException;
      napi_is_exception_pending(env, &pendingException);
      if (pendingException) {
        napi_value error;
        napi_get_and_clear_last_exception(env, &error);
        throw NativeScriptException(env, error,
                                    "Failed to load ES module " + absPath);
      } else {
        throw NativeScriptException("Failed to load ES module " + absPath);
      }
    }

    return moduleNamespace;

  } catch (const std::exception& e) {
    throw NativeScriptException("Failed to load ES module " + path + ": " +
                                e.what());
  }
#elif defined(TARGET_ENGINE_HERMES) || defined(TARGET_ENGINE_JSC)
  try {
    std::error_code pathError;
    auto absPathFs = std::filesystem::absolute(path, pathError);
    if (pathError) {
      pathError.clear();
      absPathFs = std::filesystem::path(path);
    }
    absPathFs = absPathFs.lexically_normal();
    auto canonicalPath =
        std::filesystem::weakly_canonical(absPathFs, pathError);
    if (!pathError) {
      absPathFs = canonicalPath;
    }
    std::string absPath = absPathFs.string();

    std::ifstream file(absPath);
    if (!file.is_open()) {
      throw NativeScriptException("Unable to open file: " + absPath);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string transformed =
        TransformESModuleForFallbackEngines(StripShebang(buffer.str()));
    file.close();

    std::string wrapped;
    wrapped.reserve(transformed.length() + 1024);
    wrapped += MODULE_PROLOGUE;
    wrapped += transformed;
    wrapped += MODULE_EPILOGUE;

    napi_value scriptContent = nullptr;
    napi_create_string_utf8(env, wrapped.c_str(), wrapped.size(),
                            &scriptContent);

    napi_value moduleFunc = nullptr;
    napi_status status =
        napi_run_script_source(env, scriptContent, absPath.c_str(), &moduleFunc);
    if (status != napi_ok) {
      bool pendingException = false;
      napi_is_exception_pending(env, &pendingException);
      if (pendingException) {
        napi_value error = nullptr;
        napi_get_and_clear_last_exception(env, &error);
        throw NativeScriptException(env, error,
                                    "Failed to transform ES module " + absPath);
      }
      throw NativeScriptException("Failed to transform ES module " + absPath);
    }

    napi_value moduleObj = nullptr;
    napi_create_object(env, &moduleObj);

    napi_value exportsObj = nullptr;
    napi_create_object(env, &exportsObj);
    napi_set_named_property(env, moduleObj, "exports", exportsObj);

    napi_value esModuleMarker = nullptr;
    napi_get_boolean(env, true, &esModuleMarker);
    napi_util::define_property(env, exportsObj, "__esModule", esModuleMarker);

    napi_value fileName = nullptr;
    napi_create_string_utf8(env, absPath.c_str(), absPath.size(), &fileName);
    napi_set_named_property(env, moduleObj, "filename", fileName);

    std::string dirNameString =
        std::filesystem::path(absPath).parent_path().string();
    napi_value dirName = nullptr;
    napi_create_string_utf8(env, dirNameString.c_str(), dirNameString.size(),
                            &dirName);

    napi_value require = GetRequireFunction(env, dirNameString);
    napi_set_named_property(env, moduleObj, "require", require);
    napi_util::define_property(env, moduleObj, "id", fileName);

    napi_value thisArg = nullptr;
    napi_create_object(env, &thisArg);

    napi_value global = nullptr;
    napi_get_global(env, &global);
    napi_value globalExtends = nullptr;
    napi_get_named_property(env, global, "__extends", &globalExtends);
    napi_set_named_property(env, thisArg, "__extends", globalExtends);

    napi_value args[5] = {moduleObj, exportsObj, require, fileName, dirName};
    napi_value callResult = nullptr;
    status = napi_call_function(env, thisArg, moduleFunc, 5, args, &callResult);
    bool pendingException = false;
    napi_is_exception_pending(env, &pendingException);
    if (status != napi_ok || pendingException) {
      napi_value exception = nullptr;
      napi_get_and_clear_last_exception(env, &exception);
      if (exception != nullptr) {
        throw NativeScriptException(env, exception,
                                    "Error evaluating ES module " + absPath);
      }
      throw NativeScriptException("Error evaluating ES module " + absPath);
    }

    return exportsObj;
  } catch (const std::exception& e) {
    throw NativeScriptException("Failed to load ES module " + path + ": " +
                                e.what());
  }
#else
  throw NativeScriptException("ES Modules are not supported in this runtime.");
#endif
}

#if defined(TARGET_ENGINE_QUICKJS)
void ModuleInternal::InitQuickJSESModuleLoader(napi_env env) {
  JSRuntime* runtime = qjs_get_runtime(env);
  if (runtime == nullptr) {
    return;
  }

  JS_SetModuleLoaderFunc(
      runtime,
      [](JSContext* ctx, const char* base_name, const char* name,
         void* opaque) -> char* {
        auto* modules = static_cast<ModuleInternal*>(opaque);
        if (modules == nullptr) {
          return js_strdup(ctx, name != nullptr ? name : "");
        }

        std::string normalized = modules->NormalizeQuickJSImportSpecifier(
            base_name != nullptr ? base_name : "", name != nullptr ? name : "");
        return js_strdup(ctx, normalized.c_str());
      },
      [](JSContext* ctx, const char* module_name,
         void* opaque) -> JSModuleDef* {
        auto* modules = static_cast<ModuleInternal*>(opaque);
        if (modules == nullptr) {
          return nullptr;
        }

        return modules->LoadQuickJSImportModule(
            ctx, module_name != nullptr ? module_name : "");
      },
      this);
}

std::string ModuleInternal::NormalizeQuickJSImportSpecifier(
    const std::string& baseName, const std::string& moduleName) {
  if (moduleName.empty()) {
    return moduleName;
  }

  if (IsNodeBuiltinSpecifier(moduleName) ||
      !NormalizeRegisteredNapiModuleSpecifier(moduleName).empty()) {
    return moduleName;
  }

  if (moduleName.rfind("file://", 0) == 0) {
    return moduleName.substr(std::string("file://").size());
  }

  std::string normalizedBase = baseName;
  if (normalizedBase.rfind("file://", 0) == 0) {
    normalizedBase = normalizedBase.substr(std::string("file://").size());
  }

  std::string baseDir = RuntimeConfig.ApplicationPath;
  if (!normalizedBase.empty() && normalizedBase[0] == '/') {
    std::filesystem::path basePath(normalizedBase);
    baseDir = basePath.parent_path().string();
  } else if (!normalizedBase.empty() &&
             normalizedBase.rfind("nativescript:", 0) != 0 &&
             normalizedBase.rfind("node:", 0) != 0) {
    std::filesystem::path basePath(normalizedBase);
    baseDir = basePath.parent_path().string();
  }

  try {
    std::string resolved = ResolvePath(m_env, baseDir, moduleName);
    std::error_code ec;
    auto absolutePath = std::filesystem::absolute(resolved, ec);
    if (ec) {
      ec.clear();
      absolutePath = std::filesystem::path(resolved);
    }

    absolutePath = absolutePath.lexically_normal();
    auto canonicalPath = std::filesystem::weakly_canonical(absolutePath, ec);
    if (!ec) {
      absolutePath = canonicalPath;
    }

    return absolutePath.string();
  } catch (...) {
    return moduleName;
  }
}

JSModuleDef* ModuleInternal::LoadQuickJSImportModule(
    JSContext* ctx, const std::string& moduleName) {
  std::string source = GetBuiltinESModuleSource(moduleName);
  if (source.empty()) {
    source = GetRegisteredNapiESModuleSource(moduleName);
  }

  std::string resolvedModuleName = moduleName;
  if (source.empty()) {
    resolvedModuleName = NormalizeQuickJSImportSpecifier("", moduleName);
    if (!IsESModule(resolvedModuleName)) {
      JS_ThrowReferenceError(ctx, "could not load module '%s'",
                             moduleName.c_str());
      return nullptr;
    }

    std::ifstream file(resolvedModuleName);
    if (!file.is_open()) {
      JS_ThrowReferenceError(ctx, "could not load module '%s'",
                             resolvedModuleName.c_str());
      return nullptr;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    source = StripShebang(buffer.str());
  }

  JSValue moduleValue =
      JS_Eval(ctx, source.c_str(), source.size(), resolvedModuleName.c_str(),
              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(moduleValue)) {
    return nullptr;
  }

  JSModuleDef* module =
      static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(moduleValue));
  JSValue meta = JS_GetImportMeta(ctx, module);
  if (JS_IsException(meta)) {
    JS_FreeValue(ctx, moduleValue);
    return nullptr;
  }

  std::string moduleURL = ModulePathToURL(resolvedModuleName);
  if (JS_DefinePropertyValueStr(ctx, meta, "url",
                                JS_NewString(ctx, moduleURL.c_str()),
                                JS_PROP_C_W_E) < 0 ||
      JS_DefinePropertyValueStr(ctx, meta, "main", JS_FALSE, JS_PROP_C_W_E) <
          0) {
    JS_FreeValue(ctx, meta);
    JS_FreeValue(ctx, moduleValue);
    return nullptr;
  }

  JS_FreeValue(ctx, meta);
  JS_FreeValue(ctx, moduleValue);
  return module;
}
#endif

napi_value ModuleInternal::WrapModuleContent(napi_env env,
                                             const std::string& path) {
  std::string content;
  std::ifstream file(path);
  if (!file.is_open()) {
    throw NativeScriptException("Unable to open file: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  content = StripShebang(buffer.str());
  file.close();

  std::string result;

  if (IsESModule(path)) {
    // For ES modules, return content as-is to preserve import/export syntax
    result = content;
  } else {
#if defined(TARGET_ENGINE_HERMES) || defined(TARGET_ENGINE_JSC)
    content = RewriteCommonJSDynamicImportsForFallbackEngines(content);
#endif
    // For CommonJS modules, wrap in factory function
    result.reserve(content.length() + 1024);
    result += MODULE_PROLOGUE;
    result += content;
    result += MODULE_EPILOGUE;
  }

  napi_value wrappedContent;
  napi_create_string_utf8(env, result.c_str(), result.size(), &wrappedContent);

  return wrappedContent;
}

ModuleInternal::ModulePathKind ModuleInternal::GetModulePathKind(
    const std::string& path) {
  ModulePathKind kind;
  switch (path[0]) {
    case '.':
      kind = ModulePathKind::Relative;
      break;
    case '/':
      kind = ModulePathKind::Absolute;
      break;
    default:
      kind = ModulePathKind::Global;
      break;
  }
  return kind;
}

#if defined(TARGET_ENGINE_HERMES) || defined(TARGET_ENGINE_JSC)
const char* ModuleInternal::MODULE_PROLOGUE =
    "(function(module, exports, require, __filename, __dirname){ "
    "const __dynamicImport = (specifier) => Promise.resolve().then(() => { "
    "const __loaded = require(specifier); "
    "if (__loaded !== null && (typeof __loaded === 'object' || typeof __loaded "
    "=== 'function')) { "
    "if (__loaded.__esModule) { return __loaded; } "
    "return Object.assign({ default: __loaded }, __loaded); "
    "} "
    "return { default: __loaded }; "
    "}); ";
#else
const char* ModuleInternal::MODULE_PROLOGUE =
    "(function(module, exports, require, __filename, __dirname){ ";
#endif
const char* ModuleInternal::MODULE_EPILOGUE = "\n})";
int ModuleInternal::MODULE_PROLOGUE_LENGTH =
    static_cast<int>(std::string(ModuleInternal::MODULE_PROLOGUE).length());
