#ifndef COERCION_H_
#define COERCION_H_

#include "js_native_api.h"

namespace tns {
namespace coerce {

// Each getter tries the strict Node-API getter first, so a value of the expected type costs what it
// always did. Only when that fails is the value coerced the way the classic V8 runtime does
// (ToNumber, ToInt32, ToBoolean, ToString). If coercion fails too, the failing status is returned
// and any exception the coercion raised is cleared, so the caller can log it and carry on.

inline napi_status Recover(napi_env env, napi_status status) {
    if (status == napi_pending_exception) {
        napi_value ignored;
        napi_get_and_clear_last_exception(env, &ignored);
    }
    return status;
}

inline napi_status GetDouble(napi_env env, napi_value value, double *out) {
    if (napi_get_value_double(env, value, out) == napi_ok) return napi_ok;
    napi_value number;
    napi_status status = napi_coerce_to_number(env, value, &number);
    if (status != napi_ok) return Recover(env, status);
    return napi_get_value_double(env, number, out);
}

inline napi_status GetInt32(napi_env env, napi_value value, int32_t *out) {
    if (napi_get_value_int32(env, value, out) == napi_ok) return napi_ok;
    napi_value number;
    napi_status status = napi_coerce_to_number(env, value, &number);
    if (status != napi_ok) return Recover(env, status);
    return napi_get_value_int32(env, number, out);
}

inline napi_status GetInt64(napi_env env, napi_value value, int64_t *out) {
    if (napi_get_value_int64(env, value, out) == napi_ok) return napi_ok;
    napi_value number;
    napi_status status = napi_coerce_to_number(env, value, &number);
    if (status != napi_ok) return Recover(env, status);
    return napi_get_value_int64(env, number, out);
}

inline napi_status GetBool(napi_env env, napi_value value, bool *out) {
    if (napi_get_value_bool(env, value, out) == napi_ok) return napi_ok;
    napi_value boolean;
    napi_status status = napi_coerce_to_bool(env, value, &boolean);
    if (status != napi_ok) return Recover(env, status);
    return napi_get_value_bool(env, boolean, out);
}

inline napi_status ToString(napi_env env, napi_value value, napi_value *out) {
    return Recover(env, napi_coerce_to_string(env, value, out));
}

}  // namespace coerce
}  // namespace tns

#endif /* COERCION_H_ */
