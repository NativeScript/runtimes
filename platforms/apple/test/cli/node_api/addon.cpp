#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <string>

#include <dispatch/dispatch.h>

#include "node_api.h"

namespace {

inline napi_status ThrowStatusError(napi_env env, napi_status status,
                                    const char* context) {
  if (status == napi_ok) {
    return napi_ok;
  }
  napi_throw_error(env, nullptr, context);
  return status;
}

inline napi_value GetUndefinedValue(napi_env env) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

inline void AppendMarker(const char* path, const char* marker) {
  if (path == nullptr || marker == nullptr) {
    return;
  }
  std::ofstream stream(path, std::ios::app);
  if (!stream.is_open()) {
    return;
  }
  stream << marker << "\n";
}

inline char* DupCString(const std::string& value) {
  auto* dup = static_cast<char*>(std::malloc(value.size() + 1));
  if (dup == nullptr) {
    return nullptr;
  }
  std::memcpy(dup, value.c_str(), value.size() + 1);
  return dup;
}

#define CHECK_VALUE(status_expr, context_msg)               \
  do {                                                      \
    napi_status _status = (status_expr);                    \
    if (_status != napi_ok) {                               \
      ThrowStatusError(env, _status, (context_msg));        \
      return nullptr;                                       \
    }                                                       \
  } while (0)

struct MakeCallbackContext {
  napi_env env = nullptr;
  napi_async_context async_context = nullptr;

  // Keep values alive across the async boundary.
  napi_ref recv_ref = nullptr;
  napi_ref callback_ref = nullptr;
};

void RunMakeCallbackOnWorker(MakeCallbackContext* context) {
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  napi_value recv = nullptr;
  napi_value callback = nullptr;
  napi_get_reference_value(context->env, context->recv_ref, &recv);
  napi_get_reference_value(context->env, context->callback_ref, &callback);

  napi_status status = napi_make_callback(context->env, nullptr, recv, callback,
                                          0, nullptr, nullptr);
  if (status != napi_ok) {
    fprintf(stderr, "napi_make_callback failed: %d\n", static_cast<int>(status));
  }

  napi_delete_reference(context->env, context->recv_ref);
  napi_delete_reference(context->env, context->callback_ref);
  napi_async_destroy(context->env, context->async_context);
  delete context;
}

void RunMakeCallbackOnBackgroundQueue(void* data) {
  auto* context = static_cast<MakeCallbackContext*>(data);
  RunMakeCallbackOnWorker(context);
}

napi_value MakeCallbackFromNative(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  CHECK_VALUE(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
              "Failed to read callback info");
  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "makeCallbackFromNative(callback) expects a callback");
    return nullptr;
  }

  napi_valuetype callbackType = napi_undefined;
  CHECK_VALUE(napi_typeof(env, argv[0], &callbackType),
              "Failed to validate callback");
  if (callbackType != napi_function) {
    napi_throw_type_error(env, nullptr, "Callback must be a function");
    return nullptr;
  }

  auto* context = new MakeCallbackContext();
  context->env = env;

  CHECK_VALUE(napi_async_init(env, nullptr, nullptr, &context->async_context),
              "Failed to init async context");

  napi_value recvValue = nullptr;
  CHECK_VALUE(napi_get_global(env, &recvValue),
              "Failed to get global receiver");

  napi_status status =
      napi_create_reference(env, recvValue, 1, &context->recv_ref);
  if (status != napi_ok) {
    napi_async_destroy(env, context->async_context);
    delete context;
    ThrowStatusError(env, status, "Failed to create receiver ref");
    return nullptr;
  }

  status = napi_create_reference(env, argv[0], 1, &context->callback_ref);
  if (status != napi_ok) {
    napi_delete_reference(env, context->recv_ref);
    napi_async_destroy(env, context->async_context);
    delete context;
    ThrowStatusError(env, status, "Failed to create callback ref");
    return nullptr;
  }

  dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), context,
                   RunMakeCallbackOnBackgroundQueue);

  napi_value undefined = nullptr;
  CHECK_VALUE(napi_get_undefined(env, &undefined), "Failed to get undefined");
  return undefined;
}

struct TSFNContext {
  napi_threadsafe_function tsfn = nullptr;
};

void TSFNCallJS(napi_env env, napi_value js_callback, void* context, void* data) {
  (void)context;
  int32_t value = static_cast<int32_t>(reinterpret_cast<intptr_t>(data));

  napi_value recv = nullptr;
  napi_value arg = nullptr;
  napi_value argv[1];
  napi_get_undefined(env, &recv);
  napi_create_int32(env, value, &arg);
  argv[0] = arg;
  napi_call_function(env, recv, js_callback, 1, argv, nullptr);
}

void RunTSFNWorker(TSFNContext* context) {
  for (intptr_t i = 1; i <= 3; i++) {
    napi_call_threadsafe_function(context->tsfn, reinterpret_cast<void*>(i),
                                  napi_tsfn_blocking);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  napi_release_threadsafe_function(context->tsfn, napi_tsfn_release);
  delete context;
}

void RunTSFNWorkerOnBackgroundQueue(void* data) {
  auto* context = static_cast<TSFNContext*>(data);
  RunTSFNWorker(context);
}

napi_value StartThreadsafeDemo(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  CHECK_VALUE(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
              "Failed to read callback info");
  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "startThreadsafeDemo(onValue) expects a callback");
    return nullptr;
  }

  napi_valuetype onValueType = napi_undefined;
  CHECK_VALUE(napi_typeof(env, argv[0], &onValueType),
              "Failed to validate onValue callback");
  if (onValueType != napi_function) {
    napi_throw_type_error(env, nullptr, "Callback must be a function");
    return nullptr;
  }

  auto* context = new TSFNContext();

  napi_value workName = nullptr;
  CHECK_VALUE(napi_create_string_utf8(env, "tsfn-demo", NAPI_AUTO_LENGTH,
                                      &workName),
              "Failed to create work name");
  CHECK_VALUE(napi_create_threadsafe_function(
                  env, argv[0], nullptr, workName, 0, 1, nullptr, nullptr,
                  nullptr, TSFNCallJS, &context->tsfn),
              "Failed to create threadsafe function");

  dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), context,
                   RunTSFNWorkerOnBackgroundQueue);

  napi_value undefined = nullptr;
  CHECK_VALUE(napi_get_undefined(env, &undefined), "Failed to get undefined");
  return undefined;
}

void EnvCleanupHook(void* arg) { (void)arg; }

void AsyncCleanupHook(napi_async_cleanup_hook_handle handle, void* data) {
  (void)data;
  napi_remove_async_cleanup_hook(handle);
}

void EnvTeardownMarkerHook(void* arg) {
  auto* markerPath = static_cast<char*>(arg);
  AppendMarker(markerPath, "env_cleanup");
  std::free(markerPath);
}

void AsyncTeardownMarkerHook(napi_async_cleanup_hook_handle handle, void* data) {
  auto* markerPath = static_cast<char*>(data);
  AppendMarker(markerPath, "async_cleanup");
  napi_remove_async_cleanup_hook(handle);
  std::free(markerPath);
}

napi_value GetNodeVersion(napi_env env, napi_callback_info info) {
  (void)info;

  const napi_node_version* version = nullptr;
  CHECK_VALUE(napi_get_node_version(env, &version),
              "Failed to get node version");
  if (version == nullptr) {
    napi_throw_error(env, nullptr, "Node version pointer is null");
    return nullptr;
  }

  napi_value result = nullptr;
  CHECK_VALUE(napi_create_object(env, &result), "Failed to create result");

  napi_value major = nullptr;
  napi_value minor = nullptr;
  napi_value patch = nullptr;
  napi_value release = nullptr;

  CHECK_VALUE(napi_create_uint32(env, version->major, &major),
              "Failed to set major");
  CHECK_VALUE(napi_create_uint32(env, version->minor, &minor),
              "Failed to set minor");
  CHECK_VALUE(napi_create_uint32(env, version->patch, &patch),
              "Failed to set patch");
  CHECK_VALUE(napi_create_string_utf8(env, version->release, NAPI_AUTO_LENGTH,
                                      &release),
              "Failed to set release");

  CHECK_VALUE(napi_set_named_property(env, result, "major", major),
              "Failed to set major property");
  CHECK_VALUE(napi_set_named_property(env, result, "minor", minor),
              "Failed to set minor property");
  CHECK_VALUE(napi_set_named_property(env, result, "patch", patch),
              "Failed to set patch property");
  CHECK_VALUE(napi_set_named_property(env, result, "release", release),
              "Failed to set release property");

  return result;
}

napi_value TriggerFatalException(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  CHECK_VALUE(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
              "Failed to read callback info");

  napi_value message = nullptr;
  if (argc >= 1) {
    napi_valuetype type = napi_undefined;
    CHECK_VALUE(napi_typeof(env, argv[0], &type),
                "Failed to inspect error message type");
    if (type == napi_string) {
      message = argv[0];
    }
  }
  if (message == nullptr) {
    CHECK_VALUE(napi_create_string_utf8(env, "fatal exception",
                                        NAPI_AUTO_LENGTH, &message),
                "Failed to build fallback exception message");
  }

  napi_value error = nullptr;
  CHECK_VALUE(napi_create_error(env, nullptr, message, &error),
              "Failed to create error object");
  CHECK_VALUE(napi_fatal_exception(env, error), "Failed to raise exception");
  return nullptr;
}

napi_value TestEnvCleanupHooks(napi_env env, napi_callback_info info) {
  (void)info;
  void* token = reinterpret_cast<void*>(0xC1EA);
  CHECK_VALUE(napi_add_env_cleanup_hook(env, EnvCleanupHook, token),
              "Failed to add env cleanup hook");
  CHECK_VALUE(napi_remove_env_cleanup_hook(env, EnvCleanupHook, token),
              "Failed to remove env cleanup hook");
  return GetUndefinedValue(env);
}

napi_value TestAsyncCleanupHooks(napi_env env, napi_callback_info info) {
  (void)info;

  napi_async_cleanup_hook_handle handle = nullptr;
  CHECK_VALUE(napi_add_async_cleanup_hook(env, AsyncCleanupHook, nullptr, &handle),
              "Failed to add async cleanup hook");
  if (handle == nullptr) {
    napi_throw_error(env, nullptr, "Expected async cleanup hook handle");
    return nullptr;
  }
  CHECK_VALUE(napi_remove_async_cleanup_hook(handle),
              "Failed to remove async cleanup hook");
  return GetUndefinedValue(env);
}

napi_value RegisterTeardownHooks(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  CHECK_VALUE(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr),
              "Failed to read callback info");
  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "registerTeardownHooks(path) expects a file path");
    return nullptr;
  }

  napi_valuetype argType = napi_undefined;
  CHECK_VALUE(napi_typeof(env, argv[0], &argType),
              "Failed to validate path argument");
  if (argType != napi_string) {
    napi_throw_type_error(env, nullptr, "Path must be a string");
    return nullptr;
  }

  size_t pathLength = 0;
  CHECK_VALUE(napi_get_value_string_utf8(env, argv[0], nullptr, 0, &pathLength),
              "Failed to get path length");
  std::string markerPath(pathLength + 1, '\0');
  CHECK_VALUE(napi_get_value_string_utf8(env, argv[0], markerPath.data(),
                                         markerPath.size(), &pathLength),
              "Failed to read path");
  markerPath.resize(pathLength);

  char* envPath = DupCString(markerPath);
  char* asyncPath = DupCString(markerPath);
  if (envPath == nullptr || asyncPath == nullptr) {
    std::free(envPath);
    std::free(asyncPath);
    napi_throw_error(env, nullptr, "Failed to allocate marker path");
    return nullptr;
  }

  CHECK_VALUE(napi_add_env_cleanup_hook(env, EnvTeardownMarkerHook, envPath),
              "Failed to register env teardown hook");

  napi_async_cleanup_hook_handle handle = nullptr;
  napi_status asyncStatus = napi_add_async_cleanup_hook(
      env, AsyncTeardownMarkerHook, asyncPath, &handle);
  if (asyncStatus != napi_ok) {
    napi_remove_env_cleanup_hook(env, EnvTeardownMarkerHook, envPath);
    std::free(envPath);
    std::free(asyncPath);
    ThrowStatusError(env, asyncStatus, "Failed to register async teardown hook");
    return nullptr;
  }

  return GetUndefinedValue(env);
}

}  // namespace

NAPI_MODULE_INIT() {
  napi_property_descriptor properties[] = {
      {"makeCallbackFromNative", nullptr, MakeCallbackFromNative, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"startThreadsafeDemo", nullptr, StartThreadsafeDemo, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"getNodeVersion", nullptr, GetNodeVersion, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"triggerFatalException", nullptr, TriggerFatalException, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"testEnvCleanupHooks", nullptr, TestEnvCleanupHooks, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"testAsyncCleanupHooks", nullptr, TestAsyncCleanupHooks, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"registerTeardownHooks", nullptr, RegisterTeardownHooks, nullptr, nullptr,
       nullptr, napi_default, nullptr},
  };

  napi_status status =
      napi_define_properties(env, exports,
                             sizeof(properties) / sizeof(properties[0]),
                             properties);
  if (status != napi_ok) {
    return nullptr;
  }

  return exports;
}
