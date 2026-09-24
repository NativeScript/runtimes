#ifndef ARRAYHELPER_H_
#define ARRAYHELPER_H_

#include "Engine.h"
#include "ObjectManager.h"
#include <string.h>

namespace tns {
class ArrayHelper {
    public:
        static void Init(JsRuntime &rt);

    private:
        ArrayHelper();

        static JsValue CreateJavaArrayCallback(JsRuntime &rt, const JsValue &thisVal,
                                               const JsValue *args, size_t argc);

        static JsValue CreateJavaArray(JsRuntime &rt, const JsValue *args, size_t argc);

        static jobject CreateArrayByClassName(const std::string& typeName, int length);

        static jclass RUNTIME_CLASS;

        static jmethodID CREATE_ARRAY_HELPER;
};
}

#endif /* ARRAYHELPER_H_ */
