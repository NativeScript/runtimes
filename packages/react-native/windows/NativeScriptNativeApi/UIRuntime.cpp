#include "pch.h"

#include "UIRuntime.h"

#include "NodeApiHost.h"
#include "resource.h"

#include <shlobj.h>

#include <chrono>
#include <fstream>
#include <string_view>

#include <winrt/Windows.System.Threading.h>

namespace NativeScriptRN {

namespace {

// The slice of Node-API and Hermes' JS runtime API (js_runtime_api.h) this file uses, resolved
// from hermes.dll at run time.
typedef struct napi_env__ *napi_env;
typedef struct napi_value__ *napi_value;
typedef int napi_status;
typedef struct jsr_runtime_s *jsr_runtime;
typedef struct jsr_config_s *jsr_config;
typedef struct jsr_napi_env_scope_s *jsr_napi_env_scope;
constexpr napi_status napi_ok = 0;

struct Api {
  napi_status(__cdecl *jsr_create_config)(jsr_config *);
  napi_status(__cdecl *jsr_delete_config)(jsr_config);
  napi_status(__cdecl *jsr_config_set_explicit_microtasks)(jsr_config, bool);
  napi_status(__cdecl *jsr_create_runtime)(jsr_config, jsr_runtime *);
  napi_status(__cdecl *jsr_runtime_get_node_api_env)(jsr_runtime, napi_env *);
  napi_status(__cdecl *jsr_open_napi_env_scope)(napi_env, jsr_napi_env_scope *);
  napi_status(__cdecl *jsr_run_script)(napi_env, napi_value, const char *, napi_value *);
  napi_status(__cdecl *jsr_drain_microtasks)(napi_env, int32_t, bool *);
  napi_status(__cdecl *napi_create_string_utf8)(napi_env, const char *, size_t, napi_value *);
  napi_status(__cdecl *napi_create_object)(napi_env, napi_value *);
  napi_status(__cdecl *napi_get_global)(napi_env, napi_value *);
  napi_status(__cdecl *napi_get_named_property)(napi_env, napi_value, const char *, napi_value *);
  napi_status(__cdecl *napi_set_named_property)(napi_env, napi_value, const char *, napi_value);
  napi_status(__cdecl *napi_call_function)(napi_env, napi_value, napi_value, size_t, const napi_value *, napi_value *);
  napi_status(__cdecl *napi_get_value_string_utf8)(napi_env, napi_value, char *, size_t, size_t *);
  napi_status(__cdecl *napi_coerce_to_string)(napi_env, napi_value, napi_value *);
  napi_status(__cdecl *napi_is_exception_pending)(napi_env, bool *);
  napi_status(__cdecl *napi_get_and_clear_last_exception)(napi_env, napi_value *);

  HMODULE hermes{};

  // hermes.dll: the one react-native-windows loaded, else one beside the app or this module.
  bool Load(std::wstring const &moduleDir, std::string &error) {
    hermes = GetModuleHandleW(L"hermes.dll");
    if (!hermes) hermes = LoadLibraryW(L"hermes.dll");
    if (!hermes) hermes = LoadLibraryW((moduleDir + L"\\hermes.dll").c_str());
    if (!hermes) {
      error = "hermes.dll isn't loaded. NativeScript on Windows needs an app that runs Hermes (the react-native-windows default).";
      return false;
    }
    bool ok = true;
    auto get = [&](auto &fn, const char *name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(hermes, name));
      if (!fn) {
        ok = false;
        error = std::string("hermes.dll has no ") + name + ": it is older than NativeScript needs.";
      }
    };
    get(jsr_create_config, "jsr_create_config");
    get(jsr_delete_config, "jsr_delete_config");
    get(jsr_config_set_explicit_microtasks, "jsr_config_set_explicit_microtasks");
    get(jsr_create_runtime, "jsr_create_runtime");
    get(jsr_runtime_get_node_api_env, "jsr_runtime_get_node_api_env");
    get(jsr_open_napi_env_scope, "jsr_open_napi_env_scope");
    get(jsr_run_script, "jsr_run_script");
    get(jsr_drain_microtasks, "jsr_drain_microtasks");
    get(napi_create_string_utf8, "napi_create_string_utf8");
    get(napi_create_object, "napi_create_object");
    get(napi_get_global, "napi_get_global");
    get(napi_get_named_property, "napi_get_named_property");
    get(napi_set_named_property, "napi_set_named_property");
    get(napi_call_function, "napi_call_function");
    get(napi_get_value_string_utf8, "napi_get_value_string_utf8");
    get(napi_coerce_to_string, "napi_coerce_to_string");
    get(napi_is_exception_pending, "napi_is_exception_pending");
    get(napi_get_and_clear_last_exception, "napi_get_and_clear_last_exception");
    return ok;
  }
};

std::string Narrow(std::wstring const &text) {
  return winrt::to_string(text);
}

std::wstring ModulePath(HMODULE module) {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length < path.size()) {
      path.resize(length);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

std::wstring Directory(std::wstring const &path) {
  return path.substr(0, path.find_last_of(L"\\/"));
}

// An embedded resource (NativeScriptNativeApi.rc) as bytes.
std::string_view Resource(HMODULE module, int id) {
  HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!info) return {};
  HGLOBAL data = LoadResource(module, info);
  if (!data) return {};
  return {static_cast<const char *>(LockResource(data)), SizeofResource(module, info)};
}

// Windows loads a DLL only from a file, so the embedded addon is written out once per build of it:
// %LOCALAPPDATA%\nativescript-react-native\<hash of its bytes>\nswinrt.node. A packaged app's
// writes there land in its own package data.
std::wstring ExtractAddon(std::string_view bytes, std::string &error) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : bytes) hash = (hash ^ c) * 1099511628211ull;
  wchar_t name[17];
  swprintf_s(name, L"%016llx", static_cast<unsigned long long>(hash));

  wchar_t base[MAX_PATH];
  DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  std::wstring dir = (length && length < MAX_PATH ? std::wstring(base, length) : Directory(ModulePath(nullptr))) +
      L"\\nativescript-react-native\\" + name;
  std::wstring path = dir + L"\\nswinrt.node";
  WIN32_FILE_ATTRIBUTE_DATA existing{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &existing) && existing.nFileSizeHigh == 0 &&
      existing.nFileSizeLow == bytes.size()) {
    return path;
  }
  SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
  // Through a temporary file, so another process never loads a half-written one.
  std::wstring temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  {
    std::ofstream file(temp, std::ios::binary);
    file.write(bytes.data(), bytes.size());
    if (!file) {
      error = "Couldn't write " + Narrow(temp) + ".";
      return {};
    }
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) &&
      GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    error = "Couldn't write " + Narrow(path) + " (error " + std::to_string(GetLastError()) + ").";
    DeleteFileW(temp.c_str());
    return {};
  }
  DeleteFileW(temp.c_str());
  return path;
}

// The JS the runtime runs once NativeScript is attached: globals as in the desktop hosts, a
// console that buffers for React Native's own, and the worklet runner. Results are JSON strings
// collected by id, so promises can settle later.
constexpr const char *kPrelude = R"JS(
(function () {
  var ns = globalThis.__nsWinRT;
  globalThis.Windows = ns.Windows;
  globalThis.XamlWindow = ns.XamlWindow;
  globalThis.NSWinRT = Object.assign(globalThis.NSWinRT || {}, { toPromise: ns.toPromise, interop: ns.interop });
  var logs = [];
  var format = function (value) {
    if (typeof value === 'string') return value;
    try { var json = JSON.stringify(value); return json === undefined ? String(value) : json; } catch (e) { return String(value); }
  };
  globalThis.console = {};
  ['log', 'info', 'warn', 'error', 'debug'].forEach(function (level) {
    globalThis.console[level] = function () {
      logs.push([level, Array.prototype.map.call(arguments, format).join(' ')]);
    };
  });
  var results = {};
  var finish = function (id, ok, value) {
    var out = { ok: ok, logs: logs.splice(0) };
    if (ok) out.value = value === undefined ? null : value;
    else out.error = value;
    try { results[id] = JSON.stringify(out); }
    catch (e) { results[id] = JSON.stringify({ ok: false, error: 'The worklet returned a value that is not JSON: ' + e.message, logs: out.logs }); }
  };
  var describe = function (e) { return e && e.stack ? String(e.stack) : e && e.message ? String(e.message) : String(e); };
  globalThis.__nsRunWorklet = function (id, code, payload) {
    try {
      var p = JSON.parse(payload);
      var closure = p.closure || {};
      // Names a worklet captured that don't exist on React Native's side (Windows, XamlWindow,
      // NSWinRT) are this runtime's globals.
      (p.globals || []).forEach(function (name) { closure[name] = globalThis[name]; });
      var fn = (0, eval)('(' + code + ')');
      var result = fn.apply({ __closure: closure }, p.args || []);
      if (result && typeof result.then === 'function') {
        result.then(function (v) { finish(id, true, v); }, function (e) { finish(id, false, describe(e)); });
      } else {
        finish(id, true, result);
      }
    } catch (e) {
      finish(id, false, describe(e));
    }
  };
  globalThis.__nsTakeResult = function (id) {
    var r = results[id];
    if (r === undefined) return '';
    delete results[id];
    return r;
  };
})();
)JS";

} // namespace

struct UIRuntime::Impl : std::enable_shared_from_this<UIRuntime::Impl> {
  Post post;
  HMODULE resources{};
  Api api{};
  napi_env env{};
  bool started{false};
  std::string startError;
  uint64_t nextId{1};

  std::string Str(napi_value value) {
    napi_value text;
    if (api.napi_coerce_to_string(env, value, &text) != napi_ok) return "";
    size_t length = 0;
    api.napi_get_value_string_utf8(env, text, nullptr, 0, &length);
    std::string out(length, '\0');
    api.napi_get_value_string_utf8(env, text, out.data(), length + 1, &length);
    return out;
  }

  // The pending exception's stack (or message), cleared; "" if none.
  std::string TakeException() {
    bool pending = false;
    api.napi_is_exception_pending(env, &pending);
    if (!pending) return "";
    napi_value error, stack;
    api.napi_get_and_clear_last_exception(env, &error);
    if (api.napi_get_named_property(env, error, "stack", &stack) == napi_ok) {
      auto text = Str(stack);
      if (!text.empty() && text != "undefined") return text;
    }
    return Str(error);
  }

  bool RunScript(std::string const &source, const char *name, std::string &error) {
    napi_value script, result;
    api.napi_create_string_utf8(env, source.c_str(), source.size(), &script);
    if (api.jsr_run_script(env, script, name, &result) != napi_ok) {
      error = std::string(name) + ": " + TakeException();
      return false;
    }
    Drain();
    return true;
  }

  void Drain() {
    bool more = true;
    for (int i = 0; i < 16 && more; i++) {
      if (api.jsr_drain_microtasks(env, -1, &more) != napi_ok) break;
    }
  }

  napi_value GlobalFunction(const char *name) {
    napi_value global, fn;
    api.napi_get_global(env, &global);
    api.napi_get_named_property(env, global, name, &fn);
    return fn;
  }

  std::string Start() {
    if (started) return "";
    if (!startError.empty()) return startError;
    startError = StartOnce();
    started = startError.empty();
    return startError;
  }

  std::string StartOnce() {
    std::string error;
    if (!api.Load(Directory(ModulePath(resources)), error)) return error;

    jsr_config config{};
    jsr_runtime runtime{};
    api.jsr_create_config(&config);
    // Promise reactions run when this module drains them (Drain), after every script and task;
    // without explicit microtasks this Hermes never runs them for an embedder.
    api.jsr_config_set_explicit_microtasks(config, true);
    if (api.jsr_create_runtime(config, &runtime) != napi_ok) return "Hermes couldn't create a runtime.";
    api.jsr_delete_config(config);
    api.jsr_runtime_get_node_api_env(runtime, &env);
    // The scope stays open for the runtime's life: it lives on this thread only.
    jsr_napi_env_scope scope{};
    api.jsr_open_napi_env_scope(env, &scope);

    // Node's half of Node-API (threadsafe functions, cleanup hooks, buffers) comes from this module,
    // the engine's from hermes.dll: the addon's napi-sys reads NAPI_HOST_MODULES to find them.
    std::weak_ptr<Impl> weak = shared_from_this();
    auto onThread = [weak](std::function<void()> work) {
      if (auto self = weak.lock()) {
        self->post([weak, work = std::move(work)]() {
          work();
          if (auto again = weak.lock()) again->Drain();
        });
      }
    };
    if (!NodeApiHost::Init(api.hermes, onThread)) return "hermes.dll lacks Node-API functions NativeScript needs.";
    SetEnvironmentVariableW(L"NAPI_HOST_MODULES", NodeApiHost::HostModules(api.hermes).c_str());

    auto addonBytes = Resource(resources, IDR_NSWINRT_NODE);
    auto helpers = std::string(Resource(resources, IDR_NSWINRT_JS));
    if (addonBytes.empty() || helpers.empty()) {
      return "NativeScriptNativeApi.dll has no embedded @nativescript/windows-napi: rebuild it with the addon installed.";
    }
    auto addonPath = ExtractAddon(addonBytes, error);
    if (addonPath.empty()) return error;
    HMODULE addon = LoadLibraryW(addonPath.c_str());
    if (!addon) {
      return "Couldn't load " + Narrow(addonPath) + " (error " + std::to_string(GetLastError()) +
          "), @nativescript/windows-napi's addon.";
    }
    using Register = napi_value(__cdecl *)(napi_env, napi_value);
    auto registerModule = reinterpret_cast<Register>(GetProcAddress(addon, "napi_register_module_v1"));
    if (!registerModule) return Narrow(addonPath) + " is not a Node-API addon.";

    napi_value exports, global;
    api.napi_create_object(env, &exports);
    napi_value native = registerModule(env, exports);
    auto pending = TakeException();
    if (!pending.empty()) return "@nativescript/windows-napi failed to start: " + pending;
    api.napi_get_global(env, &global);
    api.napi_set_named_property(env, global, "__nsNative", native ? native : exports);

    // nswinrt.js is CommonJS whose only require is './index.js', the native exports.
    auto wrapped = "globalThis.__nsWinRT = (function () { var module = { exports: {} };"
                   " (function (module, exports, require) {\n" +
        helpers +
        "\n})(module, module.exports, function () { return globalThis.__nsNative; });"
        " return module.exports; })();";
    if (!RunScript(wrapped, "nswinrt.js", error)) return error;
    if (!RunScript(kPrelude, "nativescript-react-native-windows.js", error)) return error;
    return "";
  }

  void Run(std::string const &code, std::string const &payload, Done done) {
    auto error = Start();
    if (!error.empty()) return done(Failure(error));
    auto id = std::to_string(nextId++);
    napi_value args[3], receiver{}, ignored;
    api.napi_create_string_utf8(env, id.c_str(), id.size(), &args[0]);
    api.napi_create_string_utf8(env, code.c_str(), code.size(), &args[1]);
    api.napi_create_string_utf8(env, payload.c_str(), payload.size(), &args[2]);
    api.napi_get_global(env, &receiver);
    if (api.napi_call_function(env, receiver, GlobalFunction("__nsRunWorklet"), 3, args, &ignored) != napi_ok) {
      return done(Failure(TakeException()));
    }
    Collect(id, std::move(done), std::chrono::steady_clock::now());
  }

  // A worklet that returned a promise settles later: from microtasks, or from WinRT completions
  // that arrive as messages on this thread. Poll for its result until it's there.
  void Collect(std::string id, Done done, std::chrono::steady_clock::time_point started) {
    Drain();
    napi_value idValue, result, global;
    api.napi_create_string_utf8(env, id.c_str(), id.size(), &idValue);
    api.napi_get_global(env, &global);
    if (api.napi_call_function(env, global, GlobalFunction("__nsTakeResult"), 1, &idValue, &result) != napi_ok) {
      return done(Failure(TakeException()));
    }
    auto json = Str(result);
    if (!json.empty()) return done(json);
    if (std::chrono::steady_clock::now() - started > std::chrono::minutes(2)) {
      return done(Failure("The worklet's promise didn't settle within 2 minutes."));
    }
    std::weak_ptr<Impl> weak = shared_from_this();
    auto retry = [weak, id, done, started]() {
      if (auto self = weak.lock()) self->Collect(id, done, started);
    };
    winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
        [post = post, retry](auto const &) { post(retry); }, std::chrono::milliseconds(8));
  }

  static std::string Failure(std::string const &message) {
    std::string escaped;
    for (char c : message) {
      switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            snprintf(buffer, sizeof buffer, "\\u%04x", c);
            escaped += buffer;
          } else {
            escaped += c;
          }
      }
    }
    return "{\"ok\":false,\"error\":\"" + escaped + "\",\"logs\":[]}";
  }
};

std::shared_ptr<UIRuntime> UIRuntime::Create(Post post, HMODULE resources) {
  auto runtime = std::make_shared<UIRuntime>();
  runtime->impl_ = std::make_shared<Impl>();
  runtime->impl_->post = std::move(post);
  runtime->impl_->resources = resources;
  return runtime;
}

std::string UIRuntime::Start() {
  return impl_->Start();
}

bool UIRuntime::Started() const {
  return impl_->started;
}

void UIRuntime::Run(std::string const &code, std::string const &payload, Done done) {
  impl_->Run(code, payload, std::move(done));
}

} // namespace NativeScriptRN
