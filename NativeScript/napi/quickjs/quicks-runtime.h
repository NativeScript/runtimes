//
// Created by Ammar Ahmed on 09/02/2025.
//

#ifndef TEST_APP_QUICKS_RUNTIME_H
#define TEST_APP_QUICKS_RUNTIME_H
#include "js_native_api.h"
#include "quickjs.h"

EXTERN_C_START

NAPI_EXTERN napi_status NAPI_CDECL qjs_create_runtime(napi_runtime* runtime);

NAPI_EXTERN napi_status NAPI_CDECL qjs_create_napi_env(napi_env* env,
                                                       napi_runtime runtime);

NAPI_EXTERN napi_status NAPI_CDECL qjs_free_napi_env(napi_env env);

NAPI_EXTERN napi_status NAPI_CDECL qjs_free_runtime(napi_runtime runtime);

NAPI_EXTERN napi_status NAPI_CDECL qjs_execute_script(napi_env env,
                                                      napi_value script,
                                                      const char* file,
                                                      napi_value* result);

NAPI_EXTERN napi_status NAPI_CDECL
qjs_runtime_before_gc_callback(napi_env env, napi_finalize cb, void* data);

NAPI_EXTERN napi_status NAPI_CDECL
qjs_runtime_after_gc_callback(napi_env env, napi_finalize cb, void* data);

NAPI_EXTERN napi_status NAPI_CDECL qjs_execute_pending_jobs(napi_env env);

NAPI_EXTERN napi_status NAPI_CDECL qjs_update_stack_top(napi_env env);

NAPI_EXTERN JSContext* NAPI_CDECL qjs_get_context(napi_env env);

NAPI_EXTERN JSRuntime* NAPI_CDECL qjs_get_runtime(napi_env env);

NAPI_EXTERN void NAPI_CDECL
qjs_shared_array_buffer_data_retain(uint8_t* data);

NAPI_EXTERN void NAPI_CDECL
qjs_shared_array_buffer_data_release(uint8_t* data);

NAPI_EXTERN napi_status NAPI_CDECL qjs_create_scoped_value(napi_env env,
                                                           JSValue value,
                                                           napi_value* result);

EXTERN_C_END

#endif  // TEST_APP_QUICKS_RUNTIME_H
