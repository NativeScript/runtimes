//
// Created by Ammar Ahmed on 03/12/2024.
//

#include "WeakRef.h"

using namespace tns;

WeakRef::WeakRef(JsRuntime &rt, const JsValue &value)
        : ref_(std::make_unique<JsValue>(rt, value)) {
}

WeakRef::~WeakRef() = default;

void WeakRef::Init(JsRuntime &rt) {
    JsObject global = rt.global();

    JsValue wr = global.getProperty(rt, "WeakRef");
    if (js_util::is_null_or_undefined(wr)) {
        JsFunction cons = JsFunction::createFromHostConstructor(
                rt, JsPropNameID::forAscii(rt, "WeakRef"), 1, WeakRef::New);

        JsValue prototype = js_util::get_prototype(rt, JsValue(rt, cons));
        if (prototype.isObject()) {
            JsObject prototypeObject = prototype.asObject(rt);
            js_util::set_function(rt, prototypeObject, "get", WeakRef::Deref);
            js_util::set_function(rt, prototypeObject, "deref", WeakRef::Deref);
        }

        global.setProperty(rt, "WeakRef", cons);
    }
}

JsValue WeakRef::New(JsRuntime &rt, const JsValue &jsThis, const JsValue *argv, size_t argc) {
    // The napi version rejects a plain call by checking new.target. engine:: has
    // no new.target, and a host constructor that is called without `new` is
    // handed an undefined receiver, so that is the test here.
    if (js_util::is_null_or_undefined(jsThis)) {
        throw JsError(rt, "WeakRef must be called as a constructor");
    }

    if (argc != 1) {
        throw JsError(rt, "WeakRef constructor must be called with one argument");
    }

    jsThis.asObjectBorrowed(rt).setNativeState<WeakRef>(
            rt, std::make_shared<WeakRef>(rt, argv[0]));

    return jsThis;
}

JsValue WeakRef::Deref(JsRuntime &rt, const JsValue &jsThis, const JsValue *argv, size_t argc) {
    if (!jsThis.isObject()) {
        return js_util::undefined();
    }

    auto obj = jsThis.asObjectBorrowed(rt).getNativeState<WeakRef>(rt);
    if (obj == nullptr || obj->ref_ == nullptr) {
        return js_util::undefined();
    }

    return *obj->ref_;
}
