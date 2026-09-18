#include "ArgConverter.h"
#include "ObjectManager.h"
#include "Util.h"
#include "NativeScriptException.h"
#include "NumericCasts.h"
#include "NativeScriptAssert.h"
#include <sstream>
#ifdef USE_MIMALLOC
#include "mimalloc.h"
#endif


using namespace std;
using namespace tns;

namespace {
// The napi version reads new.target to recover the prototype when a constructor
// callback is handed a null `this`. engine:: has no new.target, so the
// constructor's own prototype is captured when the function is created and
// passed in here instead; for a `new Ctor()` call the two are the same object.
JsValue EnsurePlainConstructorThis(JsRuntime &rt, const JsValue &jsThis,
                                   const JsValue &prototype) {
    if (!js_util::is_null_or_undefined(jsThis)) {
        return jsThis;
    }

    JsObject receiver(rt);

    if (!js_util::is_null_or_undefined(prototype)) {
        js_util::setPrototypeOf(rt, JsValue(rt, receiver), prototype);
    }

    return JsValue(rt, receiver);
}
}

void ArgConverter::Init(JsRuntime &rt) {
    auto cache = GetTypeLongCache(rt);

    JsFunction longNumberCtorFunc = JsFunction::createFromHostConstructor(
            rt, JsPropNameID::forAscii(rt, "NativeScriptLongNumber"), 0,
            [](JsRuntime &rt, const JsValue &jsThis, const JsValue *argv, size_t argc) {
                return ArgConverter::NativeScriptLongFunctionCallback(rt, jsThis, argv, argc);
            });

    JsValue longNumberPrototypeValue = js_util::get_prototype(rt, JsValue(rt, longNumberCtorFunc));
    if (!longNumberPrototypeValue.isObject()) {
        return;
    }
    JsObject longNumberPrototype = longNumberPrototypeValue.asObject(rt);

    js_util::set_function(rt, longNumberPrototype, "valueOf",
                          ArgConverter::NativeScriptLongValueOfFunctionCallback);
    js_util::set_function(rt, longNumberPrototype, "toString",
                          ArgConverter::NativeScriptLongToStringFunctionCallback);

    cache->LongNumberCtorFunc = longNumberCtorFunc;

    JsValue nanValue(numeric_limits<double>::quiet_NaN());

    JsObject global = rt.global();
    JsFunction numCtor = global.getPropertyAsFunction(rt, "Number");

    const JsValue args[] = {nanValue};
    JsValue nanObject = numCtor.callAsConstructor(rt, args, static_cast<size_t>(1));
    if (!nanObject.isObject()) {
        return;
    }

    cache->NanNumberObject = nanObject.asObject(rt);
}

JsValue ArgConverter::NativeScriptLongValueOfFunctionCallback(JsRuntime &rt,
                                                              const JsValue &jsThis,
                                                              const JsValue *argv, size_t argc) {
    try {
        return JsValue(numeric_limits<double>::quiet_NaN());
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }
}

JsValue ArgConverter::NativeScriptLongToStringFunctionCallback(JsRuntime &rt,
                                                               const JsValue &jsThis,
                                                               const JsValue *argv, size_t argc) {
    try {
        return js_util::get_property(rt, jsThis, "value");
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }
}

JsValue ArgConverter::NativeScriptLongFunctionCallback(JsRuntime &rt, const JsValue &jsThis,
                                                       const JsValue *argv, size_t argc) {
    try {
        auto cache = GetTypeLongCache(rt);
        JsValue receiverValue = EnsurePlainConstructorThis(
                rt, jsThis, js_util::get_prototype(rt, JsValue(rt, cache->LongNumberCtorFunc)));
        if (!receiverValue.isObject()) {
            return js_util::undefined();
        }
        JsObject receiver = receiverValue.asObject(rt);

        receiver.setProperty(rt, "javaLong", true);

        NumericCasts::MarkAsLong(rt, receiver, argc > 0 ? argv[0] : js_util::undefined());

        receiver.setProperty(rt, "prototype", JsValue(rt, cache->NanNumberObject));
        return JsValue(rt, receiver);

    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }
}

void ArgConverter::ConvertJavaArgsToJsArgs(JsRuntime &rt, jobjectArray args, size_t argc,
                                           JsValue *arr) {
    JEnv jenv;

    auto runtime = Runtime::GetRuntime(rt);
    auto objectManager = runtime->GetObjectManager();

    int jArrayIndex = 0;
    for (int i = 0; i < argc; i++) {
        JniLocalRef argTypeIDObj(jenv.GetObjectArrayElement(args, jArrayIndex++));
        JniLocalRef arg(jenv.GetObjectArrayElement(args, jArrayIndex++));
        JniLocalRef argJavaClassPath(jenv.GetObjectArrayElement(args, jArrayIndex++));

        Type argTypeID = (Type) JType::IntValue(jenv, argTypeIDObj);

        JsValue jsArg;
        switch (argTypeID) {
            case Type::Boolean:
                jsArg = JsValue(JType::BooleanValue(jenv, arg) == JNI_TRUE);
                break;
            case Type::Char:
                jsArg = jcharToJsString(rt, JType::CharValue(jenv, arg));
                break;
            case Type::Byte:
                jsArg = JsValue((double) JType::ByteValue(jenv, arg));
                break;
            case Type::Short:
                jsArg = JsValue((double) JType::ShortValue(jenv, arg));
                break;
            case Type::Int:
                jsArg = JsValue((double) JType::IntValue(jenv, arg));
                break;
            case Type::Long:
                jsArg = JsValue((double) JType::LongValue(jenv, arg));
                break;
            case Type::Float:
                jsArg = JsValue((double) JType::FloatValue(jenv, arg));
                break;
            case Type::Double:
                jsArg = JsValue((double) JType::DoubleValue(jenv, arg));
                break;
            case Type::String:
                jsArg = jstringToJsString(rt, (jstring) arg);
                break;
            case Type::JsObject: {
                jint javaObjectID = JType::IntValue(jenv, arg);
                jsArg = objectManager->GetJsObjectByJavaObject(javaObjectID);

                if (js_util::is_null_or_undefined(jsArg)) {
                    string argClassName = jstringToString(ObjectToString(argJavaClassPath));
                    argClassName = Util::ConvertFromCanonicalToJniName(argClassName);
                    jsArg = objectManager->CreateJSWrapper(javaObjectID, argClassName);
                }
                break;
            }
            case Type::Null:
                jsArg = js_util::null();
                break;
        }

        arr[i] = jsArg;
    }

}

JsValue ArgConverter::ConvertFromJavaLong(JsRuntime &rt, jlong value) {
    long long longValue = value;

    if ((-JS_LONG_LIMIT < longValue) && (longValue < JS_LONG_LIMIT)) {
        return JsValue((double) longValue);
    }

    auto cache = GetTypeLongCache(rt);
    char strNumber[24];
    sprintf(strNumber, "%lld", longValue);
    const JsValue args[] = {convertToJsString(rt, std::string(strNumber))};

    return cache->LongNumberCtorFunc.callAsConstructor(rt, args, static_cast<size_t>(1));
}

int64_t ArgConverter::ConvertToJavaLong(JsRuntime &rt, const JsValue &value) {
    JsValue valueProp = js_util::get_property(rt, value, "value");
    if (!valueProp.isString()) {
        return 0;
    }

    string num = js_util::get_string_value(rt, valueProp);

    int64_t longValue = atoll(num.c_str());

    return longValue;
}

ArgConverter::TypeLongOperationsCache *ArgConverter::GetTypeLongCache(JsRuntime &rt) {
    TypeLongOperationsCache *cache;
    auto itFound = s_type_long_operations_cache.find(rt.identity());
    if (itFound == s_type_long_operations_cache.end()) {
        cache = new TypeLongOperationsCache;
        s_type_long_operations_cache.emplace(rt.identity(), cache);
    } else {
        cache = itFound->second;
    }

    return cache;
}

JsValue ArgConverter::convertToJsString(JsRuntime &rt, const jchar *data, int length) {
    if (data == nullptr || length <= 0) {
        return convertToJsString(rt, std::string());
    }

    static_assert(sizeof(jchar) == sizeof(char16_t));
    return JsValue(rt, JsString::createFromUtf16(rt, reinterpret_cast<const char16_t *>(data),
                                                 static_cast<size_t>(length)));
}

u16string ArgConverter::ConvertToUtf16String(JsRuntime &rt, const JsValue &s) {
    if (!s.isString()) {
        return {};
    } else {
        auto utf16str = Util::ConvertFromUtf8ToUtf16(js_util::get_string_value(rt, s));

        return utf16str;
    }
}

void ArgConverter::onDisposeRuntime(JsRuntime &rt) {
    auto itFound = s_type_long_operations_cache.find(rt.identity());
    if (itFound != s_type_long_operations_cache.end()) {
        delete itFound->second;
        s_type_long_operations_cache.erase(itFound);
    }
}

robin_hood::unordered_map<const void *, ArgConverter::TypeLongOperationsCache *> ArgConverter::s_type_long_operations_cache;
