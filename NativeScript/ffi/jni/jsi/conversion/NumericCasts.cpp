#include "NumericCasts.h"
#include "NativeScriptAssert.h"
#include "Util.h"
#include "ArgConverter.h"
#include "NativeScriptException.h"
#include <sstream>

using namespace std;
using namespace tns;

void NumericCasts::CreateGlobalCastFunctions(JsRuntime &rt, JsObject &globalObject) {
    js_util::set_function(rt, globalObject, "long", MarkAsLongCallback);
    js_util::set_function(rt, globalObject, "byte", MarkAsByteCallback);
    js_util::set_function(rt, globalObject, "short", MarkAsShortCallback);
    js_util::set_function(rt, globalObject, "double", MarkAsDoubleCallback);
    js_util::set_function(rt, globalObject, "float", MarkAsFloatCallback);
    js_util::set_function(rt, globalObject, "char", MarkAsCharCallback);
}

void NumericCasts::MarkAsLong(JsRuntime &rt, JsObject &object, const JsValue &value) {
    MarkJsObject(rt, object, CastType::Long, value);
}

JsValue NumericCasts::MarkAsLongCallback(JsRuntime &rt, const JsValue &thisVal,
                                         const JsValue *args, size_t count) {
    if (count != 1) {
        throw JsError(rt, "long(x) should be called with single parameter");
    }

    if (!args[0].isString() && !args[0].isNumber()) {
        throw JsError(rt,
                      "long(x) should be called with single parameter containing a long number representation");
    }

    JsObject cast(rt);
    MarkJsObject(rt, cast, CastType::Long, args[0]);
    return JsValue(rt, cast);
}

JsValue NumericCasts::MarkAsByteCallback(JsRuntime &rt, const JsValue &thisVal,
                                         const JsValue *args, size_t count) {
    if (count != 1) {
        throw JsError(rt, "byte(x) should be called with single parameter");
    }

    if (!args[0].isString() && !args[0].isNumber() &&
        !js_util::is_number_object(rt, args[0]) && !js_util::is_string_object(rt, args[0])) {
        throw JsError(rt,
                      "byte(x) should be called with single parameter containing a byte number representation");
    }

    JsValue value = args[0].isNumber()
                    ? JsValue(rt, args[0])
                    : js_util::to_js_string(rt, js_util::coerce_to_string(rt, args[0]));

    JsObject cast(rt);
    MarkJsObject(rt, cast, CastType::Byte, value);
    return JsValue(rt, cast);
}

JsValue NumericCasts::MarkAsShortCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t count) {
    if (count != 1) {
        throw JsError(rt, "short(x) should be called with single parameter");
    }

    if (!args[0].isString() && !args[0].isNumber() &&
        !js_util::is_number_object(rt, args[0]) && !js_util::is_string_object(rt, args[0])) {
        throw JsError(rt,
                      "short(x) should be called with single parameter containing a byte number representation");
    }

    JsValue value = args[0].isNumber()
                    ? JsValue(rt, args[0])
                    : js_util::to_js_string(rt, js_util::coerce_to_string(rt, args[0]));

    JsObject cast(rt);
    MarkJsObject(rt, cast, CastType::Short, value);
    return JsValue(rt, cast);
}

JsValue NumericCasts::MarkAsCharCallback(JsRuntime &rt, const JsValue &thisVal,
                                         const JsValue *args, size_t count) {
    if (count != 1) {
        throw JsError(rt, "char(x) should be called with single parameter");
    }

    if (!args[0].isString()) {
        throw JsError(rt,
                      "char(x) should be called with single parameter containing a char representation");
    }

    if (args[0].asString(rt).utf8(rt).size() != 1) {
        throw JsError(rt,
                      "char(x) should be called with single parameter containing a single char");
    }

    JsObject cast(rt);
    MarkJsObject(rt, cast, CastType::Char, args[0]);
    return JsValue(rt, cast);
}

JsValue NumericCasts::MarkAsFloatCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t count) {
    if (count != 1) {
        throw JsError(rt, "float(x) should be called with single parameter");
    }

    if (!args[0].isNumber()) {
        throw JsError(rt,
                      "float(x) should be called with single parameter containing a float number representation");
    }

    JsObject cast(rt);
    MarkJsObject(rt, cast, CastType::Float, args[0]);
    return JsValue(rt, cast);
}

JsValue NumericCasts::MarkAsDoubleCallback(JsRuntime &rt, const JsValue &thisVal,
                                           const JsValue *args, size_t count) {
    if (count != 1) {
        throw JsError(rt, "double(x) should be called with single parameter");
    }

    if (!args[0].isNumber()) {
        throw JsError(rt,
                      "double(x) should be called with single parameter containing a double number representation");
    }

    JsObject cast(rt);
    MarkJsObject(rt, cast, CastType::Double, args[0]);
    return JsValue(rt, cast);
}

void
NumericCasts::MarkJsObject(JsRuntime &rt, JsObject &object, CastType castType,
                           const JsValue &value) {
    object.setProperty(rt, s_castMarker, JsValue(static_cast<int>(castType)));
    object.setProperty(rt, "value", value);
}

const char *NumericCasts::s_castMarker = "t::cast";
