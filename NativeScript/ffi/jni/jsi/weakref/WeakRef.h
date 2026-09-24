//
// Created by Ammar Ahmed on 03/12/2024.
//

#ifndef TEST_APP_WEAKREF_H
#define TEST_APP_WEAKREF_H

#include "Engine.h"

namespace tns {
    class WeakRef : public engine::HostObject {
    public:
        static void Init(JsRuntime &rt);
        static JsValue New(JsRuntime &rt, const JsValue &jsThis, const JsValue *argv, size_t argc);

        explicit WeakRef(JsRuntime &rt, const JsValue &value);
        ~WeakRef() override;

    private:
        std::unique_ptr<JsValue> ref_;

        static JsValue Deref(JsRuntime &rt, const JsValue &jsThis, const JsValue *argv,
                             size_t argc);
    };

}
#endif //TEST_APP_WEAKREF_H
