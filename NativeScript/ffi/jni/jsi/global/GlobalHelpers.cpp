#include "GlobalHelpers.h"
#include "ArgConverter.h"
#include "CallbackHandlers.h"
#include "Constants.h"
#include "JEnv.h"
#include "NativeScriptException.h"
#include <sstream>
#include "robin_hood.h"
#include "Util.h"
#include <regex>

using namespace std;
using namespace tns;

// Keyed by JsRuntime::identity(); &rt is not stable across callbacks.
static robin_hood::unordered_map<const void *, JsFunction> rtToPersistentSmartJSONStringify =
        robin_hood::unordered_map<const void *, JsFunction>();

static JsFunction *GetSmartJSONStringifyFunction(JsRuntime &rt) {
    auto it = rtToPersistentSmartJSONStringify.find(rt.identity());
    if (it != rtToPersistentSmartJSONStringify.end()) {
        return &it->second;
    }

    const char * smartStringifyFunctionScript = R"(
    (function () {
    function smartStringify(object, handleCirculars) {
        if (!handleCirculars) {
            return JSON.stringify(object, null, 2);
        }

        const seen = [];
        var replacer = function (key, value) {
            if (value != null && typeof value == "object") {
                if (seen.indexOf(value) >= 0) {
                    if (key) {
                        return "[Circular]";
                    }
                    return;
                }
                seen.push(value);
            }
            return value;
        };
        return JSON.stringify(object, replacer, 2);
    }
    return smartStringify;
})();
)";

    JsValue result;
    try {
        result = rt.evaluateJavaScript(
                std::make_shared<engine::StringBuffer>(smartStringifyFunctionScript),
                "<json_helper>");
    } catch (JsError &) {
        return nullptr;
    }

    if (!result.isObject()) {
        return nullptr;
    }

    auto object = result.asObject(rt);
    if (!object.isFunction(rt)) {
        return nullptr;
    }

    auto emplaced = rtToPersistentSmartJSONStringify.emplace(rt.identity(), object.asFunction(rt));
    return &emplaced.first->second;
}



std::string tns::JsonStringifyObject(JsRuntime &rt, const JsValue &value,
                                     bool handleCircularReferences) {
    if (value.isUndefined()) {
        return "";
    }

    JsFunction *smartJSONStringifyFunction = GetSmartJSONStringifyFunction(rt);
    std::string result;
    if (smartJSONStringifyFunction != nullptr) {
        const JsValue args[] = {JsValue(rt, value), JsValue(handleCircularReferences)};
        try {
            JsValue resultValue = smartJSONStringifyFunction->call(rt, args, (size_t) 2);
            result = ArgConverter::ConvertToString(rt, resultValue);
        } catch (JsError &e) {
            if (e.value() != nullptr) {
                throw NativeScriptException(rt, *e.value(), "Error converting object to json");
            }
            throw NativeScriptException("Error converting object to json");
        }
    }

    return result;
}

JsValue tns::JsonParseString(JsRuntime &rt, const std::string &value) {
    auto global = rt.global();
    auto jsonValue = global.getProperty(rt, "JSON");
    if (!jsonValue.isObject()) {
        return js_util::undefined();
    }
    auto json = jsonValue.asObject(rt);
    auto parse = json.getPropertyAsFunction(rt, "parse");

    const JsValue args[] = {ArgConverter::convertToJsString(rt, value)};
    try {
        return parse.callWithThis(rt, json, args, (size_t) 1);
    } catch (JsError &e) {
        if (e.value() != nullptr) {
            throw NativeScriptException(rt, *e.value(), "Error converting json string to object");
        }
        throw NativeScriptException("Error converting json string to object");
    }
}

std::vector<tns::JsStacktraceFrame> tns::BuildStacktraceFrames(JsRuntime &rt, const JsValue *error,
                                                               int size) {
    std::vector<tns::JsStacktraceFrame> frames;
    JsValue stack;
    if (error != nullptr) {
        if (!error->isObject()) return frames;
        stack = error->asObjectBorrowed(rt).getProperty(rt, "stack");
    } else {
        // The napi tree branched on __HERMES__ / __PRIMJS__ to build the carrier
        // error, because napi_create_error was not usable on all of them.
        // Invoking the Error constructor works identically on every engine and
        // needs no per-engine branch.
        //
        // It must be a *call* into the constructor, not an evaluated
        // `new Error()` script: evaluating one pushes the script's own frame
        // onto the stack, so every frame index shifts by one. That is not
        // cosmetic -- MetadataNode::GetExtendLocation builds generated class
        // names out of frames[0], and it produced names like
        // "Button1_<stacktrace>_1_-59_" that no dex proxy exists for.
        JsValue err;
        try {
            err = JsValue(rt, GlobalHelpers::CreateError(rt, ""));
        } catch (JsError &) {
            return frames;
        }
        if (!err.isObject()) return frames;
        stack = err.asObject(rt).getProperty(rt, "stack");
    }

    if (js_util::is_null_or_undefined(stack)) return frames;

    string stackTrace = js_util::get_string_value(rt, stack);
    vector<string> stackLines;
    Util::SplitString(stackTrace, "\n", stackLines);

    // Source modules carry a full "file://…" URL in every frame, so this matches
    // those exactly as before (also covers JSC's "func@file://…:line:col" form).
    const regex schemeRegex(R"((file:.*):(\d+):(\d+))");
#ifdef NS_BYTECODE_ENABLED
    // Bytecode modules embed an app-relative source name (e.g. "shared/index.js")
    // at compile time — see tools/bytecode-compiler/compile-bytecode.js — because
    // the device-absolute path can't be baked in ahead of time. Those frames have
    // no scheme, so match a "(path:line:col)" (or leading-space) form and rebuild
    // the full runtime URL from the app root. A leading "(" or space anchors the
    // path so a function name is never glued on; "@" stays a valid path char so
    // scoped modules (tns_modules/@nativescript/…) survive. Bytecode engines
    // (Hermes/QuickJS/PrimJS) all emit the parenthesised V8-style frame, so the
    // "@"-delimited (JSC) form never reaches here — and JSC has no bytecode.
    const regex bareRegex(R"RE([(\s]([^\s():]+):(\d+):(\d+))RE");
#endif

#if defined(NS_JSI_HOST_RUNTIME)
    // A guest's modules come from the embedder's bundler, and its frames carry
    // no "file:" scheme -- Metro serves "http://10.0.2.2:8081/index.bundle?..."
    // in development and an "index.android.bundle" path in release. Without a
    // match there are no frames at all, and MetadataNode::GetExtendLocation then
    // has no call site to name an unnamed extend() after, so every one of them
    // collapses onto the same generated class.
    //
    // Anchored the same way as the bytecode form, and the scheme pattern above
    // still runs first, so nothing changes for a frame that does carry one.
    const regex hostRegex(R"RE([(\s@]([A-Za-z][A-Za-z\d+.-]*://[^\s()]+|[^\s()@]+):(\d+):(\d+))RE");
#endif

    int current = 0;
    for (auto &frame : stackLines) {
        smatch match;
        std::string filePath;
        if (regex_search(frame, match, schemeRegex)) {
            filePath = match[1].str();
        }
#ifdef NS_BYTECODE_ENABLED
        else if (regex_search(frame, match, bareRegex)) {
            filePath = "file://" + Constants::APP_ROOT_FOLDER_PATH + match[1].str();
        }
#endif
#if defined(NS_JSI_HOST_RUNTIME)
        else if (regex_search(frame, match, hostRegex)) {
            filePath = match[1].str();
        }
#endif
        else {
            continue;
        }
        current++;
        frames.emplace_back(stoi(match[2].str()),
                            stoi(match[3].str()),
                            filePath,
                            frame);
        if (current == size) break;
    }
    return frames;
}

JsObject tns::GlobalHelpers::CreateError(JsRuntime &rt, const std::string &message) {
    auto errorCtor = rt.global().getPropertyAsFunction(rt, "Error");
    const JsValue args[] = {js_util::to_js_string(rt, message)};
    return errorCtor.callAsConstructor(rt, args, (size_t) 1).asObject(rt);
}

void tns::GlobalHelpers::onDisposeRuntime(JsRuntime &rt) {
    rtToPersistentSmartJSONStringify.erase(rt.identity());
}
