#ifndef NUMERICCASTS_H_
#define NUMERICCASTS_H_

#include "Engine.h"
#include "Runtime.h"
#include <string>

namespace tns {
    enum class CastType {
        None,
        Char,
        Byte,
        Short,
        Long,
        Float,
        Double
    };

    class NumericCasts {
    public:
        void CreateGlobalCastFunctions(JsRuntime &rt, JsObject &globalObject);

        inline static CastType GetCastType(JsRuntime &rt, const JsValue &object) {
            CastType ret = CastType::None;

            if (!object.isObject()) return ret;

            // The napi tree short-circuits host objects here, because reading a
            // named property off one was a napi_get_named_property that the proxy
            // could answer expensively. A cast marker is only ever set on a plain
            // object created by MarkJsObject, and the host proxy's get trap
            // forwards an unknown name straight to its target, so the read below
            // returns undefined for a host object and reaches the same answer.
            auto hidden = object.asObjectBorrowed(rt).getPropertyBorrowed(rt, s_castMarker);
            if (hidden.isNumber()) {
                ret = static_cast<CastType>(static_cast<int32_t>(hidden.getNumber()));
            }

            return ret;
        }

        inline static JsValue GetCastValue(JsRuntime &rt, const JsValue &object) {
            return object.asObjectBorrowed(rt).getProperty(rt, "value");
        }

        static void MarkAsLong(JsRuntime &rt, JsObject &object, const JsValue &value);

    private:
        static JsValue MarkAsLongCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t count);

        static JsValue MarkAsByteCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t count);

        static JsValue MarkAsShortCallback(JsRuntime &rt, const JsValue &thisVal,
                                           const JsValue *args, size_t count);

        static JsValue MarkAsCharCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t count);

        static JsValue MarkAsFloatCallback(JsRuntime &rt, const JsValue &thisVal,
                                           const JsValue *args, size_t count);

        static JsValue MarkAsDoubleCallback(JsRuntime &rt, const JsValue &thisVal,
                                            const JsValue *args, size_t count);

        static void
        MarkJsObject(JsRuntime &rt, JsObject &object, CastType castType, const JsValue &value);

        static const char *s_castMarker;
    };
}

#endif /* NUMERICCASTS_H_ */
