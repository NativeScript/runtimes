#ifndef COERCION_H_
#define COERCION_H_

#include <cmath>
#include <cstdint>
#include <limits>

#include "Engine.h"

namespace tns {
namespace coerce {

// Each conversion returns a value that is already the right type as it always did. Only when it is
// not is the value coerced the way the classic V8 runtime does (ToNumber, ToInt32, ToBoolean). A
// coercion that throws, such as a Symbol, yields 0 / false and leaves no exception behind.

inline double CoerceToDouble(JsRuntime &rt, const JsValue &value) {
    try {
        JsValue ctor = rt.global().getProperty(rt, "Number");
        JsFunction fn = ctor.asObjectBorrowed(rt).asFunction(rt);
        JsValue argument(rt, value);
        JsValue result = fn.callWithThis(rt, rt.global(), &argument, 1);
        return result.isNumber() ? result.getNumber() : std::numeric_limits<double>::quiet_NaN();
    } catch (JsError &) {
        return 0;
    }
}

inline bool CoerceToBool(JsRuntime &rt, const JsValue &value) {
    try {
        JsValue ctor = rt.global().getProperty(rt, "Boolean");
        JsFunction fn = ctor.asObjectBorrowed(rt).asFunction(rt);
        JsValue argument(rt, value);
        JsValue result = fn.callWithThis(rt, rt.global(), &argument, 1);
        return result.isBool() && result.getBool();
    } catch (JsError &) {
        return false;
    }
}

inline double ToDouble(JsRuntime &rt, const JsValue &value) {
    return value.isNumber() ? value.getNumber() : CoerceToDouble(rt, value);
}

inline int32_t Int32FromDouble(double d) {
    if (d >= -2147483648.0 && d <= 2147483647.0) return static_cast<int32_t>(d);
    if (!std::isfinite(d)) return 0;
    double wrapped = std::fmod(std::trunc(d), 4294967296.0);
    if (wrapped < 0) wrapped += 4294967296.0;
    return static_cast<int32_t>(static_cast<uint32_t>(wrapped));
}

inline int32_t ToInt32(JsRuntime &rt, const JsValue &value) {
    return Int32FromDouble(ToDouble(rt, value));
}

inline int64_t ToInt64(JsRuntime &rt, const JsValue &value) {
    double d = ToDouble(rt, value);
    if (std::isnan(d)) return 0;
    if (d >= 9223372036854775807.0) return std::numeric_limits<int64_t>::max();
    if (d <= -9223372036854775808.0) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(d);
}

inline bool ToBool(JsRuntime &rt, const JsValue &value) {
    return value.isBool() ? value.getBool() : CoerceToBool(rt, value);
}

}  // namespace coerce
}  // namespace tns

#endif /* COERCION_H_ */
