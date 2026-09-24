#include "NativeScriptException.h"
#include "JSONObjectHelper.h"
#include "ArgConverter.h"
#include <sstream>
#include <string>
#include <cassert>

using namespace tns;

void JSONObjectHelper::RegisterFromFunction(JsRuntime& rt, const JsValue& value) {
    if (!value.isObject()) {
        return;
    }

    auto object = value.asObjectBorrowed(rt);

    if (object.hasProperty(rt, "from")) {
        return;
    }

    JsValue from = CreateFromFunction(rt);
    object.setProperty(rt, "from", from);
}


JsValue JSONObjectHelper::CreateFromFunction(JsRuntime& rt) {
    static const char* source = R"((() => function from(data) {
            if (!data) throw new Error("Expected one parameter");
            let store;
            switch (typeof data) {
                case "string":
                case "boolean":
                case "number": {
                    return data;
                }
                case "object": {
                    if (!data) {
                        return null;
                    }

                    if (data instanceof Date) {
                        return data.toJSON();
                    }

                    if (Array.isArray(data)) {
                        store = new org.json.JSONArray();
                        data.forEach((item) => store.put(from(item)));
                        return store;
                    }

                    store = new org.json.JSONObject();
                    Object.keys(data).forEach((key) => store.put(key, from(data[key])));
                    return store;
                }
                default:
                    return null;
            }
        })();)";

    try {
        return rt.evaluateJavaScript(std::make_shared<engine::StringBuffer>(source),
                                     "<from_function>");
    } catch (JsError &) {
        return js_util::undefined();
    }
}
