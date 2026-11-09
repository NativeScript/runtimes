
#include <CoreFoundation/CFRunLoop.h>
#include <memory>
#include <mutex>
#include "runtime/apple/NativeScriptException.h"
#include "jsr_common.h"
#include "native_api_util.h"
#include "robin_hood.h"

namespace nativescript {

inline std::mutex smartJSONStringifyMutex;
inline robin_hood::unordered_map<napi_env, napi_ref> envToPersistentSmartJSONStringify =
    robin_hood::unordered_map<napi_env, napi_ref>();

inline void CleanupSmartJSONStringify(void* data) {
  napi_env env = static_cast<napi_env>(data);
  napi_ref ref = nullptr;
  {
    std::lock_guard<std::mutex> lock(smartJSONStringifyMutex);
    auto it = envToPersistentSmartJSONStringify.find(env);
    if (it == envToPersistentSmartJSONStringify.end()) {
      return;
    }
    ref = it->second;
    envToPersistentSmartJSONStringify.erase(it);
  }
  napi_delete_reference(env, ref);
}

inline napi_value GetSmartJSONStringifyFunction(napi_env env) {
  napi_ref cachedRef = nullptr;
  {
    std::lock_guard<std::mutex> lock(smartJSONStringifyMutex);
    auto it = envToPersistentSmartJSONStringify.find(env);
    if (it != envToPersistentSmartJSONStringify.end()) {
      cachedRef = it->second;
    }
  }
  if (cachedRef != nullptr) {
    napi_value smartStringifyFunction;
    napi_get_reference_value(env, cachedRef, &smartStringifyFunction);
    return smartStringifyFunction;
  }

  const char* smartStringifyFunctionScript = R"(
    (function () {
    function smartStringify(object, handleCirculars) {
        if (!handleCirculars) {
            return JSON.stringify(object, null, 2);
        }

        const seen = [];
        var replacer = function (key, value) {
            if (value != null && typeof value == "object") {
                if (seen.indexOf(value) >= 0) {
                    if (key) {
                        return "[Circular]";
                    }
                    return;
                }
                seen.push(value);
            }
            return value;
        };
        return JSON.stringify(object, replacer, 2);
    }
    return smartStringify;
})();
)";

  napi_value source;
  napi_create_string_utf8(env, smartStringifyFunctionScript, strlen(smartStringifyFunctionScript),
                          &source);

  napi_value global;
  napi_get_global(env, &global);

  napi_value result;
  napi_status status = js_execute_script(env, source, "<json_helper>", &result);
  if (status != napi_ok) {
    return nullptr;
  }

  if (!napi_util::is_of_type(env, result, napi_function)) {
    return nullptr;
  }

  napi_ref smartStringifyPersistentFunction;
  napi_create_reference(env, result, 1, &smartStringifyPersistentFunction);

  napi_ref retainedRef = smartStringifyPersistentFunction;
  bool inserted = false;
  {
    std::lock_guard<std::mutex> lock(smartJSONStringifyMutex);
    auto entry = envToPersistentSmartJSONStringify.emplace(
        env, smartStringifyPersistentFunction);
    inserted = entry.second;
    retainedRef = entry.first->second;
  }
  if (!inserted) {
    napi_delete_reference(env, smartStringifyPersistentFunction);
    napi_value cachedResult;
    napi_get_reference_value(env, retainedRef, &cachedResult);
    return cachedResult;
  }
  if (js_add_env_cleanup_hook(env, CleanupSmartJSONStringify, env) != napi_ok) {
    CleanupSmartJSONStringify(env);
    return nullptr;
  }

  return result;
}

inline std::string JsonStringifyObject(napi_env env, napi_value value,
                                       bool handleCircularReferences) {
  if (value == nullptr) {
    return "";
  }

  napi_value smartJSONStringifyFunction = GetSmartJSONStringifyFunction(env);
  std::string result;
  if (smartJSONStringifyFunction != nullptr) {
    napi_value resultValue;
    napi_value args[2];
    args[0] = value;
    args[1] = handleCircularReferences ? napi_util::get_true(env) : napi_util::get_false(env);
    napi_status status = napi_call_function(env, napi_util::global(env), smartJSONStringifyFunction,
                                            2, args, &resultValue);
    if (status != napi_ok) {
      napi_value exception;
      napi_get_and_clear_last_exception(env, &exception);
      if (!napi_util::is_null_or_undefined(env, exception)) {
        throw NativeScriptException(env, exception, "Error converting object to json");
      } else {
        throw NativeScriptException("Error converting object to json");
      }
    }
    result = napi_util::get_cxx_string(env, resultValue);
  }

  return result;
}

inline napi_value JsonParse(napi_env env, const std::string& json) {
  napi_value result = nullptr;
  napi_value parseFunction;
  napi_value global;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "JSON", &parseFunction);
  napi_get_named_property(env, parseFunction, "parse", &parseFunction);
  napi_value args[1];
  napi_create_string_utf8(env, json.c_str(), json.size(), &args[0]);
  napi_status status =
      napi_call_function(env, global, parseFunction, 1, args, &result);
  if (status != napi_ok) {
    return nullptr;
  }
  return result;
}

struct LockAndCV {
  std::mutex m;
  std::condition_variable cv;
};

inline void ExecuteOnRunLoop(CFRunLoopRef queue, std::function<void()> func, bool async) {
  if (!async) {
    bool __block finished = false;
    auto state = std::make_shared<LockAndCV>();
    std::unique_lock<std::mutex> lock(state->m);
    CFRunLoopPerformBlock(queue, kCFRunLoopCommonModes, ^(void) {
      func();
      {
        std::lock_guard<std::mutex> lk(state->m);
        finished = true;
      }
      state->cv.notify_all();
    });
    CFRunLoopWakeUp(queue);
    while (!finished) {
      state->cv.wait(lock);
    }
  } else {
    CFRunLoopPerformBlock(queue, kCFRunLoopCommonModes, ^(void) {
      func();
    });
    CFRunLoopWakeUp(queue);
  }
}

}  // namespace nativescript
