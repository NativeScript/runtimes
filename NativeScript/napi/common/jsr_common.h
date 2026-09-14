//
// Created by Ammar Ahmed on 17/01/2025.
//

#ifndef TEST_APP_JSR_COMMON_H
#define TEST_APP_JSR_COMMON_H

#include "js_native_api.h"

napi_status js_create_runtime(napi_runtime* runtime);
napi_status js_create_napi_env(napi_env* env, napi_runtime runtime);
napi_status js_set_runtime_flags(const char* flags);
napi_status js_lock_env(napi_env env);
napi_status js_unlock_env(napi_env env);
napi_status js_free_napi_env(napi_env env);
napi_status js_free_runtime(napi_runtime runtime);
napi_status js_execute_script(napi_env env,
                              napi_value script,
                              const char *file,
                              napi_value *result);

napi_status js_execute_pending_jobs(napi_env env);

napi_status js_get_engine_ptr(napi_env env, int64_t *engine_ptr);
napi_status js_adjust_external_memory(napi_env env, int64_t changeInBytes, int64_t* externalMemory);
napi_status js_cache_script(napi_env env, const char *source, const char *file);
napi_status js_run_cached_script(napi_env env, const char * file, napi_value script, void* cache, napi_value *result);

napi_status js_get_runtime_version(napi_env env, napi_value* version);

// Invoked by engine-specific env teardown to execute registered node-api
// cleanup hooks for the environment before it is released.
using js_env_cleanup_hook = void (*)(void*);
napi_status js_add_env_cleanup_hook(napi_env env, js_env_cleanup_hook hook,
                                    void* data);
napi_status js_remove_env_cleanup_hook(napi_env env, js_env_cleanup_hook hook,
                                       void* data);
void js_run_env_cleanup_hooks(napi_env env);

#endif //TEST_APP_JSR_COMMON_H
