#include "ArrayHelper.h"
#include "ArgConverter.h"
#include "NativeScriptException.h"
#include "Runtime.h"
#include <sstream>

using namespace std;
using namespace tns;

ArrayHelper::ArrayHelper() {
}

void ArrayHelper::Init(JsRuntime &rt) {
    JEnv jenv;

    RUNTIME_CLASS = jenv.FindClass("com/tns/Runtime");
    assert(RUNTIME_CLASS != nullptr);

    CREATE_ARRAY_HELPER = jenv.GetStaticMethodID(RUNTIME_CLASS, "createArrayHelper", "(Ljava/lang/String;I)Ljava/lang/Object;");
    assert(CREATE_ARRAY_HELPER != nullptr);

    auto global = rt.global();
    auto arrayConstructor = global.getProperty(rt, "Array");
    if (!arrayConstructor.isObject()) {
        return;
    }

    auto ctorObject = arrayConstructor.asObject(rt);
    js_util::set_function(rt, ctorObject, "create", CreateJavaArrayCallback);
}

JsValue ArrayHelper::CreateJavaArrayCallback(JsRuntime &rt, const JsValue &thisVal,
                                             const JsValue *args, size_t argc) {
    try {
       return CreateJavaArray(rt, args, argc);
    } catch (NativeScriptException& e) {
        e.ReThrowToJs(rt);
    } catch (JsError&) {
        throw;
    } catch (std::exception& e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }
}

JsValue ArrayHelper::CreateJavaArray(JsRuntime &rt, const JsValue *args, size_t argc) {
    if (argc != 2) {
        throw JsError(rt, "Expect two parameters.");
    }

    const JsValue &type = args[0];
    const JsValue &length = args[1];

    JniLocalRef array;

    auto runtime = Runtime::GetRuntime(rt);
    auto objectManager = runtime->GetObjectManager();

    if (type.isString()) {
        if (!length.isNumber()) {
            throw JsError(rt, "Expect integer value as a second argument.");
        }

        if (js_util::is_float(rt, length)) {
            throw JsError(rt, "Expect integer value as a second argument. It is a float");
        }

        int32_t len = js_util::get_int32(length);
        if (len < 0) {
            throw JsError(rt, "Expect non-negative integer value as a second argument.");
        }

        string typeName = ArgConverter::ConvertToString(rt, type);
        array = JniLocalRef(CreateArrayByClassName(typeName, len));
    } else if (type.isObject()) {
        if (!length.isNumber()) {
            throw JsError(rt, "Expect integer value as a second argument.");
        }

        if (js_util::is_float(rt, length)) {
            throw JsError(rt, "Expect integer value as a second argument.");
        }

        int32_t len = js_util::get_int32(length);
        if (len < 0) {
            throw JsError(rt, "Expect non-negative integer value as a second argument.");
        }

        auto classVal = type.asObjectBorrowed(rt).getProperty(rt, "class");

        if (classVal.isUndefined()) {
            throw JsError(rt, "Expect known class as a second argument.");
        }

        auto c = objectManager->GetJavaObjectByJsObject(classVal);

        JEnv jenv;
        array = jenv.NewObjectArray(len, static_cast<jclass>(c), nullptr);
    } else {
        throw JsError(rt, "Expect primitive type name or class function as a first argument");
    }

    jint javaObjectID = objectManager->GetOrCreateObjectId(array);
    return objectManager->CreateJSWrapper(javaObjectID, "" /* ignored */, array);
}

jobject ArrayHelper::CreateArrayByClassName(const string& typeName, int length) {
    JEnv jEnv;
    jobject array;

    if (typeName == "char") {
        array = jEnv.NewCharArray(length);
    } else if (typeName == "boolean") {
        array = jEnv.NewBooleanArray(length);
    } else if (typeName == "byte") {
        array = jEnv.NewByteArray(length);
    } else if (typeName == "short") {
        array = jEnv.NewShortArray(length);
    } else if (typeName == "int") {
        array = jEnv.NewIntArray(length);
    } else if (typeName == "long") {
        array = jEnv.NewLongArray(length);
    } else if (typeName == "float") {
        array = jEnv.NewFloatArray(length);
    } else if (typeName == "double") {
        array = jEnv.NewDoubleArray(length);
    } else {
        JniLocalRef s(jEnv.NewStringUTF(typeName.c_str()));
        array = jEnv.CallStaticObjectMethod(RUNTIME_CLASS, CREATE_ARRAY_HELPER, (jstring)s, length);
    }

    return array;
}

jclass ArrayHelper::RUNTIME_CLASS = nullptr;
jmethodID ArrayHelper::CREATE_ARRAY_HELPER = nullptr;
