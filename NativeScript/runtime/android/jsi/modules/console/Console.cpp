//
// Created by pkanev on 12/8/2017.
//

#include <assert.h>
#include <android/log.h>
#include <cstdlib>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <GlobalHelpers.h>
#include <NativeScriptException.h>

#include "ArgConverter.h"
#include "Console.h"
#include "JEnv.h"

using namespace tns;
using namespace std;


const char *Console::LOG_TAG = "JS";
std::map<const void *, std::map<std::string, double>> Console::s_rtToConsoleTimersMap;
int Console::m_maxLogcatObjectSize;

namespace {

// napi_coerce_to_string has no engine:: equivalent, and hand-rolling it would
// have to reproduce JS number formatting. `String(value)` is that coercion,
// exactly, on every engine -- and it is also what the napi version had to do
// separately for Symbols, which throw under implicit coercion but stringify
// fine through String().
std::string coerceToString(engine::Runtime &rt, const engine::Value &value) {
    // Delegates to the one implementation in js_util rather than repeating it.
    //
    // The local copy this replaces built a NON-const `engine::Value args[1]` and
    // called `.call(rt, args, 1)`. Binding a non-const array to the
    // `const Value (&)[N]` overload needs a qualification conversion, while the
    // variadic `Args&&...` overload matches exactly -- so the variadic won and
    // made a two-argument JS call passing the decayed array (converted to
    // `bool`) and the count. Every console line came out as "true".
    // js_util's version uses a const array and an explicit size_t, which is why
    // it was never affected.
    return js_util::coerce_to_string(rt, value);
}

// The napi version used napi_is_error. There is no such predicate here, so an
// error is recognised the way console output actually cares about: it carries a
// string `stack`.
bool looksLikeError(engine::Runtime &rt, const engine::Object &object) {
    return object.getProperty(rt, "stack").isString();
}

std::string transformJSObject(engine::Runtime &rt, const engine::Object &object) {
    engine::Value toStringFunc = object.getProperty(rt, "toString");

    if (toStringFunc.isObject() && toStringFunc.asObjectBorrowed(rt).isFunction(rt)) {
        engine::Value result =
                toStringFunc.asObject(rt).asFunction(rt).callWithThis(rt, object);
        auto value = result.isString() ? result.asString(rt).utf8(rt) : coerceToString(rt, result);

        if (looksLikeError(rt, object)) {
            auto stack_value = object.getProperty(rt, "stack").asString(rt).utf8(rt);
            if (!stack_value.empty() && value.find(stack_value) == std::string::npos) {
                value += "\n" + stack_value;
            }
        }

        auto hasCustomToStringImplementation = value.find("[object Object]") == std::string::npos;
        if (hasCustomToStringImplementation) return value;
    }
    // If no custom toString method, stringify the object
    return JsonStringifyObject(rt, engine::Value(rt, object), false);
}

std::string buildStringFromArg(engine::Runtime &rt, const engine::Value &val) {
    if (val.isObject()) {
        engine::Object object = val.asObjectBorrowed(rt);
        if (object.isFunction(rt)) {
            return coerceToString(rt, val);
        }
        if (object.isArray(rt)) {
            return JsonStringifyObject(rt, engine::Value(rt, val), false);
        }
        return transformJSObject(rt, object);
    }
    return coerceToString(rt, val);
}

std::string buildLogString(engine::Runtime &rt, const engine::Value *args, size_t count,
                           size_t startingIndex = 0) {
    std::stringstream ss;

    if (count) {
        for (size_t i = startingIndex; i < count; i++) {
            // separate args with a space
            if (i != 0) {
                ss << " ";
            }

            std::string argString = buildStringFromArg(rt, args[i]);
            ss << argString;
        }
    } else {
        ss << std::endl;
    }

    return ss.str();
}

engine::Value assertCallback(engine::Runtime &rt, const engine::Value &,
                             const engine::Value *args, size_t count) {
    try {
        bool expressionPasses = false;

        if (count > 0) {
            const engine::Value &condition = args[0];
            if (condition.isBool()) {
                expressionPasses = condition.getBool();
            } else if (condition.isNumber()) {
                expressionPasses = condition.getNumber() != 0;
            } else if (condition.isString()) {
                expressionPasses = !condition.asString(rt).utf8(rt).empty();
            } else {
                expressionPasses = !condition.isUndefined() && !condition.isNull();
            }
        }

        if (!expressionPasses) {
            std::stringstream assertionError;
            assertionError << "Assertion failed: ";

            if (count > 1) {
                assertionError << buildLogString(rt, args, count, 1);
            } else {
                assertionError << "console.assert";
            }

            std::string log = assertionError.str();
            Console::sendToADBLogcat(log, ANDROID_LOG_ERROR);
        }
    }
    catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    }
    catch (std::exception e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    }
    catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }
    return engine::Value::undefined();
}

// log/info/warn/error differ only in prefix and logcat priority; the napi
// version repeats the same body four times because each is a separate
// napi_callback.
engine::Value logWithPrefix(engine::Runtime &rt, const engine::Value *args, size_t count,
                            const char *prefix, android_LogPriority priority) {
    try {
        std::string log = prefix;
        log += buildLogString(rt, args, count);

        Console::sendToADBLogcat(log, priority);
    }
    catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    }
    catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    }
    catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }
    return engine::Value::undefined();
}

engine::Value dirCallback(engine::Runtime &rt, const engine::Value &, const engine::Value *args,
                          size_t count) {
    try {
        std::stringstream ss;

        if (count > 0) {
            const engine::Value &arg = args[0];
            if (arg.isObject()) {
                ss << "==== object dump start ====" << std::endl;

                engine::Object object = arg.asObjectBorrowed(rt);
                engine::Array propNames = object.getPropertyNames(rt);
                size_t propertiesLen = propNames.size(rt);

                for (size_t i = 0; i < propertiesLen; i++) {
                    engine::Value propertyName = propNames.getValueAtIndex(rt, i);
                    engine::Value propertyValue = object.getProperty(rt, propertyName);

                    ss << coerceToString(rt, propertyName);

                    if (propertyValue.isObject() &&
                        propertyValue.asObjectBorrowed(rt).isFunction(rt)) {
                        ss << "()";
                    } else if (propertyValue.isObject() &&
                               propertyValue.asObjectBorrowed(rt).isArray(rt)) {
                        std::string jsonStringifiedArray = buildStringFromArg(rt, propertyValue);
                        ss << ": " << jsonStringifiedArray;
                    } else if (propertyValue.isObject()) {
                        std::string jsonStringifiedObject =
                                transformJSObject(rt, propertyValue.asObjectBorrowed(rt));
                        // if object prints out as the error string for circular references, replace with #CR instead for brevity
                        if (jsonStringifiedObject.find("circular structure") != std::string::npos) {
                            jsonStringifiedObject = "#CR";
                        }
                        ss << ": " << jsonStringifiedObject;
                    } else {
                        ss << ": \"" << coerceToString(rt, propertyValue) << "\"";
                    }

                    ss << std::endl;
                }

                ss << "==== object dump end ====" << std::endl;
            } else {
                std::string logString = buildLogString(rt, args, count);
                ss << logString;
            }
        } else {
            ss << std::endl;
        }

        std::string log = ss.str();

        Console::sendToADBLogcat(log, ANDROID_LOG_INFO);
    }
    catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    }
    catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    }
    catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }

    return engine::Value::undefined();
}

engine::Value traceCallback(engine::Runtime &rt, const engine::Value &, const engine::Value *args,
                            size_t count) {
    try {
        std::stringstream ss;

        std::string logString = buildLogString(rt, args, count);

        if (logString.compare("\n") == 0) {
            ss << "Trace";
        } else {
            ss << "Trace: " << logString;
        }

        ss << std::endl;

        // Create an error object to get the stack trace
        engine::Object error = GlobalHelpers::CreateError(rt, "Trace");
        engine::Value stack = error.getProperty(rt, "stack");

        ss << (stack.isString() ? stack.asString(rt).utf8(rt) : std::string()) << std::endl;

        std::string log = ss.str();
        __android_log_write(ANDROID_LOG_ERROR, "JS", log.c_str());
    }
    catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    }
    catch (std::exception e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    }
    catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }

    return engine::Value::undefined();
}

}  // namespace

void Console::createConsole(engine::Runtime &rt, const int maxLogcatObjectSize,
                            const bool forceLog) {
    m_maxLogcatObjectSize = maxLogcatObjectSize;

    s_rtToConsoleTimersMap.emplace(rt.identity(), std::map<std::string, double>());

    engine::Object console(rt);
    engine::Object global = rt.global();

    engine_util::SetFunction(rt, console, "assert", assertCallback);
    engine_util::SetFunction(rt, console, "error",
                             [](engine::Runtime &runtime, const engine::Value &,
                                const engine::Value *args, size_t count) -> engine::Value {
                                 return logWithPrefix(runtime, args, count, "CONSOLE ERROR: ",
                                                      ANDROID_LOG_ERROR);
                             });
    engine_util::SetFunction(rt, console, "info",
                             [](engine::Runtime &runtime, const engine::Value &,
                                const engine::Value *args, size_t count) -> engine::Value {
                                 return logWithPrefix(runtime, args, count, "CONSOLE INFO: ",
                                                      ANDROID_LOG_INFO);
                             });
    engine_util::SetFunction(rt, console, "log",
                             [](engine::Runtime &runtime, const engine::Value &,
                                const engine::Value *args, size_t count) -> engine::Value {
                                 return logWithPrefix(runtime, args, count, "CONSOLE LOG: ",
                                                      ANDROID_LOG_INFO);
                             });
    engine_util::SetFunction(rt, console, "warn",
                             [](engine::Runtime &runtime, const engine::Value &,
                                const engine::Value *args, size_t count) -> engine::Value {
                                 return logWithPrefix(runtime, args, count, "CONSOLE WARN: ",
                                                      ANDROID_LOG_WARN);
                             });
    engine_util::SetFunction(rt, console, "dir", dirCallback);
    engine_util::SetFunction(rt, console, "trace", traceCallback);

    engine_util::SetFunction(rt, console, "time",
                             [](engine::Runtime &runtime, const engine::Value &,
                                const engine::Value *args, size_t count) -> engine::Value {
                                 std::string label = "default";
                                 if (count > 0 && args[0].isString()) {
                                     label = args[0].asString(runtime).utf8(runtime);
                                 }

                                 auto it = Console::s_rtToConsoleTimersMap.find(runtime.identity());
                                 if (it == Console::s_rtToConsoleTimersMap.end()) {
                                     return engine::Value::undefined();
                                 }

                                 auto nano = std::chrono::time_point_cast<std::chrono::microseconds>(
                                         std::chrono::system_clock::now());
                                 double timeStamp = nano.time_since_epoch().count();

                                 it->second.insert(std::make_pair(label, timeStamp));
                                 return engine::Value::undefined();
                             });

    engine_util::SetFunction(rt, console, "timeEnd",
                             [](engine::Runtime &runtime, const engine::Value &,
                                const engine::Value *args, size_t count) -> engine::Value {
                                 std::string label = "default";
                                 if (count > 0 && args[0].isString()) {
                                     label = args[0].asString(runtime).utf8(runtime);
                                 }

                                 auto it = Console::s_rtToConsoleTimersMap.find(runtime.identity());
                                 if (it == Console::s_rtToConsoleTimersMap.end()) {
                                     return engine::Value::undefined();
                                 }

                                 auto itTimersMap = it->second.find(label);
                                 if (itTimersMap == it->second.end()) {
                                     std::string warning = std::string(
                                             "No such label '" + label +
                                             "' for console.timeEnd()");

                                     __android_log_write(ANDROID_LOG_WARN, "JS", warning.c_str());

                                     return engine::Value::undefined();
                                 }

                                 auto nano = std::chrono::time_point_cast<std::chrono::microseconds>(
                                         std::chrono::system_clock::now());
                                 double endTimeStamp = nano.time_since_epoch().count();
                                 double startTimeStamp = itTimersMap->second;

                                 it->second.erase(label);

                                 double diffMicroseconds = endTimeStamp - startTimeStamp;
                                 double diffMilliseconds = diffMicroseconds / 1000.0;

                                 std::stringstream ss;
                                 ss << "CONSOLE TIME: " << label << ": " << std::fixed
                                    << std::setprecision(3) << diffMilliseconds << "ms";
                                 std::string log = ss.str();

                                 __android_log_write(ANDROID_LOG_INFO, "JS", log.c_str());
                                 return engine::Value::undefined();
                             });

    global.setProperty(rt, "console", console);
}

void Console::onDisposeRuntime(engine::Runtime &rt) {
    s_rtToConsoleTimersMap.erase(rt.identity());
}

void Console::sendToADBLogcat(const std::string &message, android_LogPriority logPriority) {
    // limit the size of the message that we send to logcat using the predefined value in package.json
    auto messageToLog = message;
    if (messageToLog.length() > m_maxLogcatObjectSize) {
        messageToLog = messageToLog.erase(m_maxLogcatObjectSize, std::string::npos);
        messageToLog = messageToLog + "...";
    }

    // split strings into chunks of 4000 characters
    // __android_log_write can't send more than 4000 to the stdout at a time
    auto messageLength = messageToLog.length();
    int maxStringLength = 4000;

    if (messageLength < maxStringLength) {
        __android_log_write(logPriority, Console::LOG_TAG, messageToLog.c_str());
    } else {
        for (int i = 0; i < messageLength; i += maxStringLength) {
            auto messagePart = messageToLog.substr(i, maxStringLength);

            __android_log_write(logPriority, Console::LOG_TAG, messagePart.c_str());
        }
    }
}
