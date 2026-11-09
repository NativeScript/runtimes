#include "v8-module-loader.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "runtime/apple/RuntimeConfig.h"

typedef napi_value (*napi_module_init)(napi_env env, napi_value exports);

namespace nativescript {
extern std::unordered_map<std::string, napi_module_init> napiModuleRegistry;
}

namespace v8impl {

namespace {

// Cache for package.json "type" field lookups
thread_local std::unordered_map<std::string, bool> g_packageTypeCache;

using ModuleRegistry =
    std::unordered_map<std::string, v8::Global<v8::Module>>;
thread_local std::unordered_map<v8::Isolate*, ModuleRegistry>
    g_moduleRegistries;

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

// Check if path has .cjs extension (explicitly CommonJS)
bool IsCJSModule(const std::string& path) {
  return path.size() >= 4 && path.compare(path.size() - 4, 4, ".cjs") == 0;
}

// Find nearest package.json by walking up from directory
std::string FindPackageJson(const std::filesystem::path& startDir) {
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
  auto cacheIt = g_packageTypeCache.find(packageJsonPath);
  if (cacheIt != g_packageTypeCache.end()) {
    return cacheIt->second;
  }

  bool isModule = false;

  std::ifstream file(packageJsonPath);
  if (file.is_open()) {
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    // Simple JSON parsing for "type": "module"
    // Look for "type" followed by : and "module"
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

  g_packageTypeCache[packageJsonPath] = isModule;
  return isModule;
}

// Determine if a .js file should be treated as ESM based on nearest
// package.json
bool ShouldTreatAsESModule(const std::string& path) {
  std::filesystem::path filePath(path);
  std::string packageJson = FindPackageJson(filePath.parent_path());

  if (!packageJson.empty()) {
    return IsPackageTypeModule(packageJson);
  }

  return false;  // Default to CommonJS
}

}  // namespace

void RegisterESModule(v8::Isolate* isolate, const std::string& moduleId,
                      v8::Local<v8::Module> module) {
  auto& registered = g_moduleRegistries[isolate][moduleId];
  registered.Reset();
  registered.Reset(isolate, module);
}

std::string NormalizeModulePath(const std::filesystem::path& path) {
  std::error_code ec;
  auto absolutePath = std::filesystem::absolute(path, ec);
  if (ec) {
    ec.clear();
    absolutePath = path;
  }

  auto normalizedPath = absolutePath.lexically_normal();
  auto canonicalPath = std::filesystem::weakly_canonical(normalizedPath, ec);
  if (!ec) {
    return canonicalPath.string();
  }

  return normalizedPath.string();
}

std::string GetModulePathFromRegistry(v8::Isolate* isolate,
                                      v8::Local<v8::Module> module) {
  auto registry = g_moduleRegistries.find(isolate);
  if (registry == g_moduleRegistries.end()) {
    return "";
  }
  for (auto& kv : registry->second) {
    v8::Local<v8::Module> registered = kv.second.Get(isolate);
    if (registered == module) {
      return kv.first;
    }
  }
  return "";
}

std::string ModulePathToURL(const std::string& modulePath) {
  if (modulePath.rfind("file://", 0) == 0) {
    return modulePath;
  }
  if (modulePath.rfind("nativescript:", 0) == 0) {
    return modulePath;
  }
  if (!modulePath.empty() && modulePath[0] == '/') {
    return "file://" + modulePath;
  }
  return "file:///" + modulePath;
}

bool IsNodeBuiltinSpecifier(const std::string& specifier) {
  static const std::unordered_set<std::string> kBuiltins = {
      "url", "node:url", "fs", "node:fs", "fs/promises", "node:fs/promises",
      "path", "node:path", "vm", "node:vm", "web", "node:web",
      "stream/web", "node:stream/web"};
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

bool IsESModule(const std::string& path) {
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
    return ShouldTreatAsESModule(path);
  }

  return false;
}

bool IsJSONModule(const std::string& path) {
  return path.size() >= 5 && path.compare(path.size() - 5, 5, ".json") == 0;
}

std::string ReadFileContent(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open file: " + path);
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return StripShebang(buffer.str());
}

v8::Local<v8::String> WrapModuleContent(v8::Isolate* isolate,
                                        const std::string& path) {
  std::string sourceText = ReadFileContent(path);

  if (IsJSONModule(path)) {
    std::string wrappedJsonModule = "export default " + sourceText + ";";
    return v8::String::NewFromUtf8(isolate, wrappedJsonModule.c_str())
        .ToLocalChecked();
  }

  if (IsESModule(path)) {
    // For ES modules, return source as-is to preserve import/export syntax
    return v8::String::NewFromUtf8(isolate, sourceText.c_str())
        .ToLocalChecked();
  }

  // For CommonJS modules, wrap in factory function
  std::string wrappedSource =
      "(function (exports, require, module, __filename, __dirname) { " +
      sourceText + "\n});";

  return v8::String::NewFromUtf8(isolate, wrappedSource.c_str())
      .ToLocalChecked();
}

std::string ResolveESModulePath(v8::Isolate* isolate,
                                const std::string& baseDir,
                                const std::string& moduleName) {
  std::string moduleNameCopy = moduleName;

  // Keep "~" alias behavior aligned with CommonJS resolution.
  if (!moduleNameCopy.empty() && moduleNameCopy[0] == '~') {
    moduleNameCopy = RuntimeConfig.ApplicationPath + moduleNameCopy.substr(1);
  }

  std::filesystem::path baseDirPath(baseDir);
  std::filesystem::path moduleNamePath(moduleNameCopy);
  std::filesystem::path fullPath = baseDirPath / moduleNamePath;

  // Check if file exists as-is
  if (std::filesystem::exists(fullPath) &&
      std::filesystem::is_regular_file(fullPath)) {
    return NormalizeModulePath(fullPath);
  }

  // Try with .mjs extension first (ES modules have priority)
  std::filesystem::path mjsPath = fullPath.string() + ".mjs";
  if (std::filesystem::exists(mjsPath) &&
      std::filesystem::is_regular_file(mjsPath)) {
    return NormalizeModulePath(mjsPath);
  }

  // Try with .js extension
  std::filesystem::path jsPath = fullPath.string() + ".js";
  if (std::filesystem::exists(jsPath) &&
      std::filesystem::is_regular_file(jsPath)) {
    return NormalizeModulePath(jsPath);
  }

  // Try with .cjs extension (explicit CommonJS)
  std::filesystem::path cjsPath = fullPath.string() + ".cjs";
  if (std::filesystem::exists(cjsPath) &&
      std::filesystem::is_regular_file(cjsPath)) {
    return NormalizeModulePath(cjsPath);
  }

  // Try directory with index.mjs
  if (std::filesystem::exists(fullPath) &&
      std::filesystem::is_directory(fullPath)) {
    std::filesystem::path indexMjs = fullPath / "index.mjs";
    if (std::filesystem::exists(indexMjs) &&
        std::filesystem::is_regular_file(indexMjs)) {
      return NormalizeModulePath(indexMjs);
    }

    // Try directory with index.js
    std::filesystem::path indexJs = fullPath / "index.js";
    if (std::filesystem::exists(indexJs) &&
        std::filesystem::is_regular_file(indexJs)) {
      return NormalizeModulePath(indexJs);
    }

    // Try directory with index.cjs
    std::filesystem::path indexCjs = fullPath / "index.cjs";
    if (std::filesystem::exists(indexCjs) &&
        std::filesystem::is_regular_file(indexCjs)) {
      return NormalizeModulePath(indexCjs);
    }
  }

  throw std::runtime_error("Module not found: " + moduleName);
}

v8::MaybeLocal<v8::Module> CompileESModule(v8::Isolate* isolate,
                                           const std::string& path) {
  const std::string absPath = NormalizeModulePath(path);
  auto& moduleRegistry = g_moduleRegistries[isolate];

  // Check if already compiled
  auto it = moduleRegistry.find(absPath);
  if (it != moduleRegistry.end()) {
    v8::Local<v8::Module> existing = it->second.Get(isolate);
    return v8::MaybeLocal<v8::Module>(existing);
  }

  // Prepare URL & source - use the absolute path consistently
  v8::Local<v8::String> sourceText = WrapModuleContent(isolate, absPath);

#if V8_MAJOR_VERSION >= 14
  v8::ScriptOrigin origin(
      v8::String::NewFromUtf8(isolate, absPath.c_str()).ToLocalChecked(), 0, 0,
      false, -1, v8::Local<v8::Value>(), false, false,
      true  // is_module
  );
#else
  v8::ScriptOrigin origin(
      isolate,
      v8::String::NewFromUtf8(isolate, absPath.c_str()).ToLocalChecked(), 0, 0,
      false, -1, v8::Local<v8::Value>(), false, false,
      true  // is_module
  );
#endif

  v8::ScriptCompiler::Source source(sourceText, origin);

  // Compile ES module
  v8::Local<v8::Module> module;
  v8::MaybeLocal<v8::Module> maybeMod = v8::ScriptCompiler::CompileModule(
      isolate, &source, v8::ScriptCompiler::kNoCompileOptions);

  if (!maybeMod.ToLocal(&module)) {
    // Compilation failed - return empty MaybeLocal, let V8 handle the
    // JavaScript exception
    return v8::MaybeLocal<v8::Module>();
  }

  // Register in global registry with absolute path
  RegisterESModule(isolate, absPath, module);

  return v8::MaybeLocal<v8::Module>(module);
}

v8::MaybeLocal<v8::Module> CompileVirtualESModule(v8::Isolate* isolate,
                                                  const std::string& moduleId,
                                                  const std::string& source) {
  auto& moduleRegistry = g_moduleRegistries[isolate];
  auto it = moduleRegistry.find(moduleId);
  if (it != moduleRegistry.end()) {
    v8::Local<v8::Module> existing = it->second.Get(isolate);
    return v8::MaybeLocal<v8::Module>(existing);
  }

  v8::Local<v8::String> sourceText =
      v8::String::NewFromUtf8(isolate, source.c_str()).ToLocalChecked();
#if V8_MAJOR_VERSION >= 14
  v8::ScriptOrigin origin(
      v8::String::NewFromUtf8(isolate, moduleId.c_str()).ToLocalChecked(), 0, 0,
      false, -1, v8::Local<v8::Value>(), false, false, true);
#else
  v8::ScriptOrigin origin(
      isolate,
      v8::String::NewFromUtf8(isolate, moduleId.c_str()).ToLocalChecked(), 0, 0,
      false, -1, v8::Local<v8::Value>(), false, false, true);
#endif

  v8::ScriptCompiler::Source scriptSource(sourceText, origin);
  v8::Local<v8::Module> module;
  v8::MaybeLocal<v8::Module> maybeMod = v8::ScriptCompiler::CompileModule(
      isolate, &scriptSource, v8::ScriptCompiler::kNoCompileOptions);

  if (!maybeMod.ToLocal(&module)) {
    return v8::MaybeLocal<v8::Module>();
  }

  RegisterESModule(isolate, moduleId, module);
  return v8::MaybeLocal<v8::Module>(module);
}

v8::Local<v8::Value> LoadESModule(v8::Isolate* isolate,
                                  const std::string& path) {
  auto context = isolate->GetCurrentContext();

  const std::string absPath = NormalizeModulePath(path);

  // First, compile the module and all its dependencies
  v8::MaybeLocal<v8::Module> maybeModule = CompileESModule(isolate, absPath);
  v8::Local<v8::Module> module;
  if (!maybeModule.ToLocal(&module)) {
    // Compilation failed - throw exception
    throw std::runtime_error("Cannot compile ES module: " + absPath);
  }

  // Instantiate (link) - this will recursively resolve dependencies
  v8::TryCatch tcLink(isolate);
  bool linked = module->InstantiateModule(context, &ResolveModuleCallback)
                    .FromMaybe(false);

  if (!linked) {
    if (tcLink.HasCaught()) {
      v8::String::Utf8Value error(isolate, tcLink.Exception());
      throw std::runtime_error("Cannot instantiate module " + absPath + ": " +
                               std::string(*error));
    } else {
      throw std::runtime_error("Cannot instantiate module " + absPath);
    }
  }

  // Evaluate
  v8::Local<v8::Value> result;
  v8::TryCatch tcEval(isolate);
  if (!module->Evaluate(context).ToLocal(&result)) {
    if (tcEval.HasCaught()) {
      v8::String::Utf8Value error(isolate, tcEval.Exception());
      throw std::runtime_error("Cannot evaluate module " + absPath + ": " +
                               std::string(*error));
    } else {
      throw std::runtime_error("Cannot evaluate module " + absPath);
    }
  }

  // Handle top-level await (if result is a Promise)
  if (result->IsPromise()) {
    v8::Local<v8::Promise> promise = result.As<v8::Promise>();

    // Process microtasks to allow Promise resolution
    int maxAttempts = 100;
    int attempts = 0;

    while (attempts < maxAttempts) {
      isolate->PerformMicrotaskCheckpoint();

      v8::Promise::PromiseState state = promise->State();

      if (state != v8::Promise::kPending) {
        if (state == v8::Promise::kRejected) {
          v8::Local<v8::Value> reason = promise->Result();
          isolate->ThrowException(reason);
          throw std::runtime_error("Module evaluation promise rejected");
        }
        break;
      }

      attempts++;
      usleep(100);  // 0.1ms delay
    }
  }

  // Return the namespace
  return module->GetModuleNamespace();
}

v8::MaybeLocal<v8::Module> ResolveModuleCallback(
    v8::Local<v8::Context> context, v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_assertions,
    v8::Local<v8::Module> referrer) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();

  // Convert specifier to std::string
  v8::String::Utf8Value specUtf8(isolate, specifier);
  std::string spec = *specUtf8 ? *specUtf8 : "";

  if (spec.empty()) {
    return v8::MaybeLocal<v8::Module>();
  }

  if (IsNodeBuiltinSpecifier(spec)) {
    const auto builtinName = NormalizeNodeBuiltinSpecifier(spec);
    const auto source = GetBuiltinESModuleSource(spec);
    if (source.empty()) {
      std::string errorMsg = "Unsupported builtin module '" + spec + "'";
      isolate->ThrowException(v8::Exception::Error(
          v8::String::NewFromUtf8(isolate, errorMsg.c_str()).ToLocalChecked()));
      return v8::MaybeLocal<v8::Module>();
    }

    const std::string moduleId = "nativescript:node_builtin/" + builtinName;
    return CompileVirtualESModule(isolate, moduleId, source);
  }

  const auto registeredSource = GetRegisteredNapiESModuleSource(spec);
  if (!registeredSource.empty()) {
    const auto normalized = NormalizeRegisteredNapiModuleSpecifier(spec);
    const std::string moduleId = "nativescript:napi_module/" + normalized;
    return CompileVirtualESModule(isolate, moduleId, registeredSource);
  }

  // Find referrer path
  std::string referrerPath = GetModulePathFromRegistry(isolate, referrer);

  if (referrerPath.empty()) {
    // Check if this is a relative import that needs a referrer context
    bool specIsRelative = !spec.empty() && spec[0] == '.';

    if (specIsRelative) {
      std::string errorMsg = "Cannot resolve relative module '" + spec +
                             "': referrer module not found in registry";
      isolate->ThrowException(v8::Exception::Error(
          v8::String::NewFromUtf8(isolate, errorMsg.c_str()).ToLocalChecked()));
      return v8::MaybeLocal<v8::Module>();
    } else {
      char cwd[1024];
      if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        referrerPath =
            std::string(cwd) + "/dummy.mjs";  // Create a dummy referrer path
      } else {
        std::string errorMsg =
            "Cannot resolve module '" + spec +
            "': no referrer and cannot get current directory";
        isolate->ThrowException(v8::Exception::Error(
            v8::String::NewFromUtf8(isolate, errorMsg.c_str())
                .ToLocalChecked()));
        return v8::MaybeLocal<v8::Module>();
      }
    }
  }

  // Compute base directory - ensure it's absolute
  std::filesystem::path referrerFilePath = NormalizeModulePath(referrerPath);
  std::string baseDir = referrerFilePath.parent_path().string();

  // Resolve the module path
  std::string absPath;
  try {
    absPath = ResolveESModulePath(isolate, baseDir, spec);
  } catch (const std::exception& e) {
    std::string errorMsg = "Cannot resolve module '" + spec + "': " + e.what();
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8(isolate, errorMsg.c_str()).ToLocalChecked()));
    return v8::MaybeLocal<v8::Module>();
  }

  v8::MaybeLocal<v8::Module> maybeModule = CompileESModule(isolate, absPath);
  if (maybeModule.IsEmpty()) {
    // Compilation failed - throw an exception if none exists
    std::string errorMsg = "Failed to compile module: " + absPath;
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8(isolate, errorMsg.c_str()).ToLocalChecked()));
    return v8::MaybeLocal<v8::Module>();
  }

  return maybeModule;
}

v8::MaybeLocal<v8::Promise> ImportModuleDynamicallyCallback(
    v8::Local<v8::Context> context, v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name, v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_assertions) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::EscapableHandleScope scope(isolate);

  // Create Promise resolver
  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context).ToLocalChecked();

  try {
    // Use the static resolver to locate/compile the module
    v8::Local<v8::Module> refMod;
    v8::MaybeLocal<v8::Module> maybeModule =
        ResolveModuleCallback(context, specifier, import_assertions, refMod);

    v8::Local<v8::Module> module;
    if (!maybeModule.ToLocal(&module)) {
      resolver
          ->Reject(context,
                   v8::Exception::Error(v8::String::NewFromUtf8(
                                            isolate, "Failed to resolve module")
                                            .ToLocalChecked()))
          .Check();
      isolate->PerformMicrotaskCheckpoint();
      return scope.Escape(resolver->GetPromise());
    }

    // If not yet instantiated/evaluated, do it now
    if (module->GetStatus() == v8::Module::kUninstantiated) {
      if (!module->InstantiateModule(context, &ResolveModuleCallback)
               .FromMaybe(false)) {
        resolver
            ->Reject(context, v8::Exception::Error(
                                  v8::String::NewFromUtf8(
                                      isolate, "Failed to instantiate module")
                                      .ToLocalChecked()))
            .Check();
        isolate->PerformMicrotaskCheckpoint();
        return scope.Escape(resolver->GetPromise());
      }
    }

    if (module->GetStatus() != v8::Module::kEvaluated) {
      if (module->Evaluate(context).IsEmpty()) {
        resolver
            ->Reject(context, v8::Exception::Error(
                                  v8::String::NewFromUtf8(
                                      isolate, "Failed to evaluate module")
                                      .ToLocalChecked()))
            .Check();
        isolate->PerformMicrotaskCheckpoint();
        return scope.Escape(resolver->GetPromise());
      }
    }

    resolver->Resolve(context, module->GetModuleNamespace()).Check();
    isolate->PerformMicrotaskCheckpoint();

  } catch (const std::exception& e) {
    resolver
        ->Reject(
            context,
            v8::Exception::Error(
                v8::String::NewFromUtf8(isolate, e.what()).ToLocalChecked()))
        .Check();
    isolate->PerformMicrotaskCheckpoint();
  }

  return scope.Escape(resolver->GetPromise());
}

void InitializeESModuleSystem(v8::Isolate* isolate) {
  // Set module resolution and dynamic import callbacks
  isolate->SetHostImportModuleDynamicallyCallback(
      ImportModuleDynamicallyCallback);
  isolate->SetHostInitializeImportMetaObjectCallback(
      [](v8::Local<v8::Context> context, v8::Local<v8::Module> module,
         v8::Local<v8::Object> meta) {
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        const std::string modulePath =
            GetModulePathFromRegistry(isolate, module);
        if (modulePath.empty()) {
          return;
        }

        const std::string moduleURL = ModulePathToURL(modulePath);
        v8::Local<v8::String> key =
            v8::String::NewFromUtf8(isolate, "url").ToLocalChecked();
        v8::Local<v8::String> value =
            v8::String::NewFromUtf8(isolate, moduleURL.c_str())
                .ToLocalChecked();
        meta->CreateDataProperty(context, key, value).Check();
      });
}

void CleanupESModuleSystem(v8::Isolate* isolate) {
  auto registry = g_moduleRegistries.find(isolate);
  if (registry != g_moduleRegistries.end()) {
    for (auto& kv : registry->second) {
      kv.second.Reset();
    }
    g_moduleRegistries.erase(registry);
  }

  // Clear the package.json type cache
  g_packageTypeCache.clear();
}

}  // namespace v8impl
