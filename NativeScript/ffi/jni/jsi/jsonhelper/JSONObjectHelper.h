#ifndef JSONOBJECTHELPER_H_
#define JSONOBJECTHELPER_H_

#include "Engine.h"

namespace tns {

    class JSONObjectHelper {
    public:
        static void RegisterFromFunction(JsRuntime& rt, const JsValue& value);
    private:
        static JsValue CreateFromFunction(JsRuntime& rt);
    };

}

#endif //JSONOBJECTHELPER_H_
