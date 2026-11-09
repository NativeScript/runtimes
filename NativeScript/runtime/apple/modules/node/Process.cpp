#include "Process.h"

#include <unistd.h>

#include <cmath>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sys/resource.h>
#include <string>
#include <vector>

#include "js_native_api.h"
#include "jsr.h"
#include "native_api_util.h"
#include "runtime/apple/Runtime.h"
#include "runtime/apple/RuntimeConfig.h"

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#endif

#if defined(TARGET_ENGINE_V8)
#include "v8-api.h"
#endif

#if !defined(_WIN32)
extern char** environ;
#endif

namespace nativescript {

namespace {

namespace fs = std::filesystem;

constexpr char kNodeCompatVersion[] = "v20.0.0";
constexpr char kNodeCompatVersionPlain[] = "20.0.0";
constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;

const std::chrono::steady_clock::time_point kProcessStartTime =
    std::chrono::steady_clock::now();

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

bool GetUint64FromValue(napi_env env, napi_value value, uint64_t& out) {
  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok) {
    return false;
  }

  if (type == napi_bigint) {
    bool lossless = false;
    if (napi_get_value_bigint_uint64(env, value, &out, &lossless) != napi_ok) {
      return false;
    }
    return lossless;
  }

  napi_value number;
  if (napi_coerce_to_number(env, value, &number) != napi_ok) {
    return false;
  }

  double doubleValue = 0;
  if (napi_get_value_double(env, number, &doubleValue) != napi_ok) {
    return false;
  }

  if (doubleValue < 0) {
    return false;
  }

  out = static_cast<uint64_t>(doubleValue);
  return true;
}

const char* GetPlatform() {
#if defined(_WIN32)
  return "win32";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

const char* GetArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x64";
#elif defined(__i386__) || defined(_M_IX86)
  return "ia32";
#else
  return "unknown";
#endif
}

std::string GetExecPath() {
  if (!RuntimeConfig.ApplicationPath.empty()) {
    return RuntimeConfig.ApplicationPath;
  }

  if (!RuntimeConfig.BaseDir.empty()) {
    return RuntimeConfig.BaseDir;
  }

  return "/";
}

napi_value CreateEnvObject(napi_env env) {
  napi_value envObj;
  napi_create_object(env, &envObj);

#if !defined(_WIN32)
  for (char** current = ::environ; current != nullptr && *current != nullptr;
       ++current) {
    std::string entry(*current);
    size_t separatorIndex = entry.find('=');
    if (separatorIndex == std::string::npos || separatorIndex == 0) {
      continue;
    }

    std::string key = entry.substr(0, separatorIndex);
    std::string value = entry.substr(separatorIndex + 1);
    napi_set_named_property(env, envObj, key.c_str(),
                            napi_util::to_js_string(env, value));
  }
#endif

  return envObj;
}

napi_value CreateVersionsObject(napi_env env) {
  napi_value versions;
  napi_create_object(env, &versions);

  napi_set_named_property(env, versions, "node",
                          napi_util::to_js_string(env, kNodeCompatVersionPlain));

#if defined(NAPI_VERSION)
  napi_set_named_property(env, versions, "napi",
                          napi_util::to_js_string(env,
                                                  std::to_string(NAPI_VERSION)));
#else
  napi_set_named_property(env, versions, "napi",
                          napi_util::to_js_string(env, "8"));
#endif

#if defined(TARGET_ENGINE_V8)
  napi_set_named_property(env, versions, "engine",
                          napi_util::to_js_string(env, "v8"));
#elif defined(TARGET_ENGINE_HERMES)
  napi_set_named_property(env, versions, "engine",
                          napi_util::to_js_string(env, "hermes"));
#elif defined(TARGET_ENGINE_QUICKJS)
  napi_set_named_property(env, versions, "engine",
                          napi_util::to_js_string(env, "quickjs"));
#elif defined(TARGET_ENGINE_JSC)
  napi_set_named_property(env, versions, "engine",
                          napi_util::to_js_string(env, "jsc"));
#endif

  napi_set_named_property(env, versions, "nativescript",
                          napi_util::to_js_string(env, "ios-next"));

  return versions;
}

uint64_t GetMonotonicNanoseconds() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

uint64_t GetResidentSetSizeBytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    return static_cast<uint64_t>(info.resident_size);
  }
#endif

  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
  }

  return 0;
}

struct MemoryUsageInfo {
  uint64_t rss = 0;
  uint64_t heapTotal = 0;
  uint64_t heapUsed = 0;
  uint64_t external = 0;
  uint64_t arrayBuffers = 0;
};

MemoryUsageInfo GetMemoryUsageInfo(napi_env env) {
  MemoryUsageInfo info;
  info.rss = GetResidentSetSizeBytes();

#if defined(TARGET_ENGINE_V8)
  if (env != nullptr && env->isolate != nullptr) {
    v8::HeapStatistics stats;
    env->isolate->GetHeapStatistics(&stats);
    info.heapTotal = static_cast<uint64_t>(stats.total_heap_size());
    info.heapUsed = static_cast<uint64_t>(stats.used_heap_size());
    info.external = static_cast<uint64_t>(stats.external_memory());
    info.arrayBuffers = 0;
  }
#endif

  return info;
}

struct ProcessSignalState {
  std::mutex mutex;
  napi_env env = nullptr;
  napi_ref processRef = nullptr;
#if defined(__APPLE__) && !defined(_WIN32)
  dispatch_source_t sigintSource = nullptr;
  bool sigintInstalled = false;
#endif
};

ProcessSignalState& GetProcessSignalState() {
  static ProcessSignalState state;
  return state;
}

bool EmitSignalToProcess(const char* signalName) {
  ProcessSignalState& state = GetProcessSignalState();

  napi_env env = nullptr;
  napi_ref processRef = nullptr;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    env = state.env;
    processRef = state.processRef;
  }

  if (env == nullptr || processRef == nullptr || !Runtime::IsAlive(env)) {
    return false;
  }

  NapiScope scope(env);

  napi_value processObj;
  if (napi_get_reference_value(env, processRef, &processObj) != napi_ok ||
      napi_util::is_null_or_undefined(env, processObj)) {
    return false;
  }

  napi_value emitFn;
  if (napi_get_named_property(env, processObj, "emit", &emitFn) != napi_ok ||
      !napi_util::is_of_type(env, emitFn, napi_function)) {
    return false;
  }

  napi_value signalValue = napi_util::to_js_string(env, signalName);
  napi_value args[1] = {signalValue};
  napi_value emitResult;
  if (napi_call_function(env, processObj, emitFn, 1, args, &emitResult) !=
      napi_ok) {
    napi_value pendingException;
    napi_get_and_clear_last_exception(env, &pendingException);
    return false;
  }

  bool handled = false;
  if (napi_get_value_bool(env, emitResult, &handled) != napi_ok) {
    return false;
  }

  return handled;
}

#if defined(__APPLE__) && !defined(_WIN32)
void HandleSigintDispatch(void* context) {
  (void)context;

  const bool handled = EmitSignalToProcess("SIGINT");
  if (!handled) {
    std::fflush(nullptr);
    std::exit(130);
  }
}
#endif

void InstallProcessSignalBridge(napi_env env, napi_value processObj) {
#if defined(__APPLE__) && !defined(_WIN32)
  if (Runtime::IsWorker()) {
    return;
  }

  ProcessSignalState& state = GetProcessSignalState();
  std::lock_guard<std::mutex> lock(state.mutex);

  if (state.processRef == nullptr || state.env != env) {
    // Best effort: a runtime can only have one global process object.
    napi_ref processRef = nullptr;
    if (napi_create_reference(env, processObj, 1, &processRef) == napi_ok) {
      state.env = env;
      state.processRef = processRef;
    }
  }

  if (state.sigintInstalled) {
    return;
  }

  ::signal(SIGINT, SIG_IGN);

  dispatch_source_t source = dispatch_source_create(
      DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
  if (source == nullptr) {
    return;
  }

  dispatch_set_context(source, nullptr);
  dispatch_source_set_event_handler_f(source, HandleSigintDispatch);
  dispatch_resume(source);

  state.sigintSource = source;
  state.sigintInstalled = true;
#else
  (void)env;
  (void)processObj;
#endif
}

void InstallProcessEventEmitterShim(napi_env env, napi_value processObj) {
  const char* shimScript = R"JS(
    (function () {
      return function attachProcessEvents(process) {
        if (!process || typeof process !== "object") {
          return;
        }

        if (typeof process.on === "function") {
          return;
        }

        const events = Object.create(null);
        const toKey = (eventName) => String(eventName);
        const getList = (eventName, create) => {
          const key = toKey(eventName);
          let list = events[key];
          if (!list && create) {
            list = [];
            events[key] = list;
          }
          return list;
        };

        function on(eventName, listener) {
          if (typeof listener !== "function") {
            throw new TypeError('The "listener" argument must be of type function');
          }
          getList(eventName, true).push(listener);
          return process;
        }

        function addListener(eventName, listener) {
          return on(eventName, listener);
        }

        function once(eventName, listener) {
          if (typeof listener !== "function") {
            throw new TypeError('The "listener" argument must be of type function');
          }

          function wrapped() {
            off(eventName, wrapped);
            return listener.apply(process, arguments);
          }

          wrapped.listener = listener;
          return on(eventName, wrapped);
        }

        function off(eventName, listener) {
          if (typeof listener !== "function") {
            return process;
          }

          const list = getList(eventName, false);
          if (!list || list.length === 0) {
            return process;
          }

          for (let i = 0; i < list.length; i++) {
            const candidate = list[i];
            if (candidate === listener || candidate.listener === listener) {
              list.splice(i, 1);
              break;
            }
          }

          if (list.length === 0) {
            delete events[toKey(eventName)];
          }

          return process;
        }

        function removeListener(eventName, listener) {
          return off(eventName, listener);
        }

        function removeAllListeners(eventName) {
          if (arguments.length === 0) {
            for (const key in events) {
              if (Object.prototype.hasOwnProperty.call(events, key)) {
                delete events[key];
              }
            }
            return process;
          }

          delete events[toKey(eventName)];
          return process;
        }

        function listeners(eventName) {
          const list = getList(eventName, false);
          return list ? list.slice() : [];
        }

        function listenerCount(eventName, listener) {
          const list = getList(eventName, false);
          if (!list || list.length === 0) {
            return 0;
          }

          if (typeof listener !== "function") {
            return list.length;
          }

          let count = 0;
          for (const candidate of list) {
            if (candidate === listener || candidate.listener === listener) {
              count++;
            }
          }
          return count;
        }

        function emit(eventName) {
          const list = getList(eventName, false);
          if (!list || list.length === 0) {
            return false;
          }

          const args = Array.prototype.slice.call(arguments, 1);
          const snapshot = list.slice();
          for (const listener of snapshot) {
            listener.apply(process, args);
          }
          return true;
        }

        process.on = on;
        process.addListener = addListener;
        process.once = once;
        process.off = off;
        process.removeListener = removeListener;
        process.removeAllListeners = removeAllListeners;
        process.listeners = listeners;
        process.listenerCount = listenerCount;
        process.emit = emit;
      };
    })()
  )JS";

  napi_value script;
  napi_create_string_utf8(env, shimScript, NAPI_AUTO_LENGTH, &script);

  napi_value attachFn;
  if (napi_run_script(env, script, &attachFn) != napi_ok ||
      napi_util::is_null_or_undefined(env, attachFn)) {
    return;
  }

  napi_value global;
  napi_get_global(env, &global);
  napi_value argv[1] = {processObj};
  napi_call_function(env, global, attachFn, 1, argv, nullptr);
}

}  // namespace

void Process::Init(napi_env env, napi_value global) {
  EnsureGlobalProcess(env, global);
}

napi_value Process::CreateModule(napi_env env) {
  napi_value moduleObj;
  napi_create_object(env, &moduleObj);

  napi_value global;
  napi_get_global(env, &global);

  napi_value processObj = EnsureGlobalProcess(env, global);
  napi_set_named_property(env, moduleObj, "exports", processObj);

  return moduleObj;
}

napi_value Process::EnsureGlobalProcess(napi_env env, napi_value global) {
  bool hasProcess = false;
  if (napi_has_named_property(env, global, "process", &hasProcess) == napi_ok &&
      hasProcess) {
    napi_value existing;
    if (napi_get_named_property(env, global, "process", &existing) == napi_ok &&
        !napi_util::is_null_or_undefined(env, existing)) {
      return existing;
    }
  }

  napi_value processObj = CreateProcessObject(env);
  napi_set_named_property(env, global, "process", processObj);
  return processObj;
}

napi_value Process::CreateProcessObject(napi_env env) {
  napi_value processObj;
  napi_create_object(env, &processObj);

  const bool hasRuntimeArgs = !RuntimeConfig.Arguments.empty();
  const std::string argv0 =
      hasRuntimeArgs ? RuntimeConfig.Arguments.front() : GetExecPath();
  const std::string execPath = argv0;

  napi_value argv;
  if (hasRuntimeArgs) {
    napi_create_array_with_length(env, RuntimeConfig.Arguments.size(), &argv);
    for (size_t i = 0; i < RuntimeConfig.Arguments.size(); i++) {
      napi_set_element(env, argv, i,
                       napi_util::to_js_string(env, RuntimeConfig.Arguments[i]));
    }
  } else {
    napi_create_array_with_length(env, 1, &argv);
    napi_set_element(env, argv, 0, napi_util::to_js_string(env, execPath));
  }

  napi_set_named_property(env, processObj, "argv", argv);
  napi_set_named_property(env, processObj, "argv0",
                          napi_util::to_js_string(env, argv0));
  napi_set_named_property(env, processObj, "execPath",
                          napi_util::to_js_string(env, execPath));

  napi_set_named_property(env, processObj, "env", CreateEnvObject(env));
  napi_set_named_property(env, processObj, "stdin", CreateReadableStream(env, 0));
  napi_set_named_property(env, processObj, "stdout", CreateWritableStream(env, 1));
  napi_set_named_property(env, processObj, "stderr", CreateWritableStream(env, 2));

  napi_set_named_property(env, processObj, "platform",
                          napi_util::to_js_string(env, GetPlatform()));
  napi_set_named_property(env, processObj, "arch",
                          napi_util::to_js_string(env, GetArch()));

  napi_set_named_property(env, processObj, "pid",
                          napi_util::to_js_number(env,
                                                  static_cast<int32_t>(getpid())));
  napi_set_named_property(env, processObj, "ppid",
                          napi_util::to_js_number(
                              env, static_cast<int32_t>(getppid())));

  napi_set_named_property(env, processObj, "version",
                          napi_util::to_js_string(env, kNodeCompatVersion));
  napi_set_named_property(env, processObj, "versions", CreateVersionsObject(env));

  napi_value release;
  napi_create_object(env, &release);
  napi_set_named_property(env, release, "name",
                          napi_util::to_js_string(env, "node"));
  napi_set_named_property(env, processObj, "release", release);

  napi_util::napi_set_function(env, processObj, "cwd", Cwd);
  napi_util::napi_set_function(env, processObj, "chdir", Chdir);
  napi_util::napi_set_function(env, processObj, "exit", Exit);
  napi_util::napi_set_function(env, processObj, "uptime", Uptime);

  napi_value hrtime = napi_util::napi_set_function(env, processObj, "hrtime", Hrtime);
  napi_value hrtimeBigInt;
  napi_create_function(env, "bigint", NAPI_AUTO_LENGTH, HrtimeBigInt, nullptr,
                       &hrtimeBigInt);
  napi_set_named_property(env, hrtime, "bigint", hrtimeBigInt);

  napi_value memoryUsage =
      napi_util::napi_set_function(env, processObj, "memoryUsage", MemoryUsage);
  napi_value memoryUsageRss;
  napi_create_function(env, "rss", NAPI_AUTO_LENGTH, MemoryUsageRss, nullptr,
                       &memoryUsageRss);
  napi_set_named_property(env, memoryUsage, "rss", memoryUsageRss);

  InstallProcessEventEmitterShim(env, processObj);
  InstallProcessSignalBridge(env, processObj);

  return processObj;
}

napi_value Process::CreateWritableStream(napi_env env, int fd) {
  napi_value streamObj;
  napi_create_object(env, &streamObj);

  napi_util::napi_set_function(env, streamObj, "write", StreamWrite,
                               reinterpret_cast<void*>(static_cast<intptr_t>(fd)));
  napi_set_named_property(env, streamObj, "fd", napi_util::to_js_number(env, fd));

  napi_value isTTY;
  napi_get_boolean(env, false, &isTTY);
  napi_set_named_property(env, streamObj, "isTTY", isTTY);

  return streamObj;
}

napi_value Process::CreateReadableStream(napi_env env, int fd) {
  napi_value streamObj;
  napi_create_object(env, &streamObj);
  napi_set_named_property(env, streamObj, "fd", napi_util::to_js_number(env, fd));

  napi_value isTTY;
  napi_get_boolean(env, false, &isTTY);
  napi_set_named_property(env, streamObj, "isTTY", isTTY);

  return streamObj;
}

napi_value Process::Cwd(napi_env env, napi_callback_info info) {
  (void)info;

  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (ec) {
    if (!RuntimeConfig.ApplicationPath.empty()) {
      return napi_util::to_js_string(env, RuntimeConfig.ApplicationPath);
    }

    napi_throw_error(env, nullptr,
                     "Unable to resolve current working directory");
    return nullptr;
  }

  return napi_util::to_js_string(env, cwd.string());
}

napi_value Process::Chdir(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  std::string targetDir;
  if (napi_util::is_null_or_undefined(env, argv[0]) ||
      !CoerceToString(env, argv[0], targetDir) || targetDir.empty()) {
    napi_throw_type_error(env, nullptr,
                          "The \"directory\" argument must be a non-empty string");
    return nullptr;
  }

  std::error_code ec;
  fs::current_path(targetDir, ec);
  if (ec) {
    std::string message = "Unable to change current working directory to \"" +
                          targetDir + "\": " + ec.message();
    napi_throw_error(env, nullptr, message.c_str());
    return nullptr;
  }

  return napi_util::undefined(env);
}

napi_value Process::Uptime(napi_env env, napi_callback_info info) {
  (void)info;

  const auto elapsed = std::chrono::steady_clock::now() - kProcessStartTime;
  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();

  return napi_util::to_js_number(env, seconds);
}

napi_value Process::Hrtime(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc > 1) {
    napi_throw_type_error(env, nullptr, "hrtime() accepts at most one argument");
    return nullptr;
  }

  uint64_t nowNanoseconds = GetMonotonicNanoseconds();
  uint64_t diffNanoseconds = nowNanoseconds;

  if (argc == 1 && !napi_util::is_null_or_undefined(env, argv[0])) {
    bool isArray = false;
    if (napi_is_array(env, argv[0], &isArray) != napi_ok || !isArray) {
      napi_throw_type_error(env, nullptr,
                            "The \"time\" argument must be an array");
      return nullptr;
    }

    uint32_t length = 0;
    napi_get_array_length(env, argv[0], &length);
    if (length != 2) {
      napi_throw_type_error(
          env, nullptr,
          "The \"time\" argument must be an array with two elements");
      return nullptr;
    }

    napi_value secValue;
    napi_value nanoValue;
    napi_get_element(env, argv[0], 0, &secValue);
    napi_get_element(env, argv[0], 1, &nanoValue);

    uint64_t seconds = 0;
    uint64_t nanoseconds = 0;
    if (!GetUint64FromValue(env, secValue, seconds) ||
        !GetUint64FromValue(env, nanoValue, nanoseconds)) {
      napi_throw_type_error(env, nullptr,
                            "hrtime values must be non-negative numbers");
      return nullptr;
    }

    uint64_t baseNanoseconds =
        seconds * kNanosecondsPerSecond + (nanoseconds % kNanosecondsPerSecond);

    diffNanoseconds =
        nowNanoseconds >= baseNanoseconds ? nowNanoseconds - baseNanoseconds : 0;
  }

  napi_value result;
  napi_create_array_with_length(env, 2, &result);
  napi_set_element(
      env, result, 0,
      napi_util::to_js_number(
          env, static_cast<double>(diffNanoseconds / kNanosecondsPerSecond)));
  napi_set_element(
      env, result, 1,
      napi_util::to_js_number(
          env, static_cast<double>(diffNanoseconds % kNanosecondsPerSecond)));

  return result;
}

napi_value Process::HrtimeBigInt(napi_env env, napi_callback_info info) {
  (void)info;

  napi_value result;
  napi_create_bigint_uint64(env, GetMonotonicNanoseconds(), &result);
  return result;
}

napi_value Process::MemoryUsage(napi_env env, napi_callback_info info) {
  (void)info;

  MemoryUsageInfo infoValue = GetMemoryUsageInfo(env);

  napi_value usage;
  napi_create_object(env, &usage);
  napi_set_named_property(env, usage, "rss",
                          napi_util::to_js_number(
                              env, static_cast<double>(infoValue.rss)));
  napi_set_named_property(env, usage, "heapTotal",
                          napi_util::to_js_number(
                              env, static_cast<double>(infoValue.heapTotal)));
  napi_set_named_property(env, usage, "heapUsed",
                          napi_util::to_js_number(
                              env, static_cast<double>(infoValue.heapUsed)));
  napi_set_named_property(env, usage, "external",
                          napi_util::to_js_number(
                              env, static_cast<double>(infoValue.external)));
  napi_set_named_property(
      env, usage, "arrayBuffers",
      napi_util::to_js_number(env, static_cast<double>(infoValue.arrayBuffers)));

  return usage;
}

napi_value Process::MemoryUsageRss(napi_env env, napi_callback_info info) {
  (void)info;
  (void)env;
  return napi_util::to_js_number(
      env, static_cast<double>(GetResidentSetSizeBytes()));
}

napi_value Process::Exit(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  int32_t exitCode = 0;
  if (argc > 0 && !napi_util::is_null_or_undefined(env, argv[0])) {
    napi_value codeAsNumber;
    if (napi_coerce_to_number(env, argv[0], &codeAsNumber) != napi_ok) {
      napi_throw_type_error(env, nullptr,
                            "The \"code\" argument must be a number");
      return nullptr;
    }

    double codeValue = 0;
    if (napi_get_value_double(env, codeAsNumber, &codeValue) != napi_ok ||
        !std::isfinite(codeValue)) {
      napi_throw_type_error(env, nullptr,
                            "The \"code\" argument must be a finite number");
      return nullptr;
    }

    exitCode = static_cast<int32_t>(codeValue);
  }

  std::fflush(nullptr);
  std::exit(exitCode);
  return nullptr;
}

napi_value Process::StreamWrite(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc < 1 || napi_util::is_null_or_undefined(env, argv[0])) {
    napi_throw_type_error(env, nullptr,
                          "The \"chunk\" argument must be a string-like value");
    return nullptr;
  }

  std::string chunk;
  if (!CoerceToString(env, argv[0], chunk)) {
    napi_throw_type_error(env, nullptr,
                          "The \"chunk\" argument must be a string-like value");
    return nullptr;
  }

  intptr_t fd = reinterpret_cast<intptr_t>(data);
  FILE* stream = (fd == 2) ? stderr : stdout;
  if (!chunk.empty()) {
    std::fwrite(chunk.data(), sizeof(char), chunk.size(), stream);
  }
  std::fflush(stream);

  napi_value callback = nullptr;
  if (argc > 1) {
    napi_valuetype secondType;
    if (napi_typeof(env, argv[1], &secondType) == napi_ok &&
        secondType == napi_function) {
      callback = argv[1];
    }
  }

  if (callback == nullptr && argc > 2) {
    napi_valuetype thirdType;
    if (napi_typeof(env, argv[2], &thirdType) == napi_ok &&
        thirdType == napi_function) {
      callback = argv[2];
    }
  }

  if (callback != nullptr) {
    napi_value global;
    napi_get_global(env, &global);
    napi_call_function(env, global, callback, 0, nullptr, nullptr);
  }

  napi_value result;
  napi_get_boolean(env, true, &result);
  return result;
}

}  // namespace nativescript
