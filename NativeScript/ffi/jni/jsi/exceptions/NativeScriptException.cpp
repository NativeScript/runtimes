#include "Util.h"
#include "NativeScriptException.h"
#include "ArgConverter.h"
#include "NativeScriptAssert.h"
#include "Runtime.h"
#include "ObjectManager.h"
#include <sstream>

using namespace std;
using namespace tns;

NativeScriptException::NativeScriptException(JEnv& env)
    : m_javascriptException(nullptr) {
    jthrowable  thrw = env.ExceptionOccurred();
    m_javaException = JniLocalRef(thrw);
    env.ExceptionClear();
    DEBUG_WRITE("%s, %s", GetExceptionMessage(env, m_javaException).c_str(), GetExceptionStackTrace(env, m_javaException).c_str());
}

NativeScriptException::NativeScriptException(const string& message)
    : m_javascriptException(nullptr), m_javaException(JniLocalRef()), m_message(message) {

    DEBUG_WRITE("%s", m_message.c_str());
}

NativeScriptException::NativeScriptException(const string& message, const string& stackTrace)
    : m_javascriptException(nullptr), m_javaException(JniLocalRef()), m_message(message), m_stackTrace(stackTrace) {

    DEBUG_WRITE("%s, %s ", m_message.c_str(), m_stackTrace.c_str());
}

NativeScriptException::NativeScriptException(JsRuntime& rt, const JsValue& error, const string& message)
    : m_javaException(JniLocalRef()) {
    m_javascriptException = std::make_shared<JsValue>(rt, error);
    m_message = GetErrorMessage(rt, error, message);
    m_stackTrace = GetErrorStackTrace(rt, error);
    m_fullMessage = GetFullMessage(rt, error, m_message);
}

NativeScriptException::NativeScriptException(JsRuntime& rt, const JsError& error,
                                             const string& message)
    : m_javaException(JniLocalRef()) {
    if (error.value() != nullptr) {
        const JsValue& value = *error.value();
        m_javascriptException = std::make_shared<JsValue>(rt, value);
        m_message = GetErrorMessage(rt, value, message);
        m_stackTrace = GetErrorStackTrace(rt, value);
        m_fullMessage = GetFullMessage(rt, value, m_message);
        return;
    }
    // Keep any "<Name>Error: " tag at the FRONT of the composed message, so
    // js_util::create_error can rebuild that constructor when this is rethrown.
    // Hermes reports a compile failure as a native exception rather than a JS
    // throw, and jsi/hermes tags it "SyntaxError: ..." for exactly this reason;
    // burying the tag mid-message would turn it back into a plain Error, and
    // the Require specs read e.name.
    std::string what = error.what();
    std::string tag;
    for (const char* name : {"SyntaxError", "TypeError", "RangeError",
                             "ReferenceError", "EvalError", "URIError"}) {
        const std::string prefix = std::string(name) + ": ";
        if (what.rfind(prefix, 0) == 0) {
            tag = prefix;
            what = what.substr(prefix.size());
            break;
        }
    }
    m_message = tag + (message.empty() ? what : message + "\n" + what);
    m_stackTrace = error.stack();
    m_fullMessage = m_message;
}

void NativeScriptException::ReThrowToJs(JsRuntime& rt) {
    // Fallback message used if the rich error object cannot be materialized —
    // ReThrowToJs must always throw, otherwise the failing Java call silently
    // appears to succeed to JS.
    const std::string& fallback = !m_fullMessage.empty() ? m_fullMessage
                                  : !m_message.empty() ? m_message
                                  : std::string("Unknown native error.");

    JsValue errObj;

    if (m_javascriptException != nullptr) {
        errObj = *m_javascriptException;
        if (errObj.isObject()) {
            JsObject errorObject = errObj.asObject(rt);
            if (!m_fullMessage.empty()) {
                errorObject.setProperty(rt, "fullMessage",
                                        ArgConverter::convertToJsString(rt, m_fullMessage));
            } else if (!m_message.empty()) {
                errorObject.setProperty(rt, "fullMessage",
                                        ArgConverter::convertToJsString(rt, m_message));
            }
        }
    } else if (!m_fullMessage.empty()) {
        errObj = js_util::create_error(rt, m_fullMessage);
    } else if (!m_message.empty()) {
        errObj = js_util::create_error(rt, m_message);
    } else if (!m_javaException.IsNull()) {
        errObj = WrapJavaToJsException(rt);
    } else {
        errObj = js_util::create_error(rt, "No javascript exception or message provided.");
    }

    throw JsError(rt, fallback, errObj, m_stackTrace);
}

void NativeScriptException::ReThrowToJava(JsRuntime* rt) {
    if (rt) {
        JSScope scope(*rt);
    }
    jthrowable ex = nullptr;
    JEnv jEnv;

    if (!m_javaException.IsNull()) {
        // Static lookup avoids needing the runtime/ObjectManager here, which may
        // be unavailable while an exception is being rethrown to Java.
        std::string excClassName = ObjectManager::GetClassName((jobject)m_javaException);

        if (excClassName == "com/tns/NativeScriptException") {
            ex = m_javaException;
        } else {
            JniLocalRef msg(jEnv.NewStringUTF("Java Error!"));
            JniLocalRef stack(jEnv.NewStringUTF(""));
            ex = static_cast<jthrowable>(jEnv.NewObject(NATIVESCRIPTEXCEPTION_CLASS, NATIVESCRIPTEXCEPTION_THROWABLE_CTOR_ID, (jstring)msg, (jstring)stack, (jobject)m_javaException));
        }
    } else if (m_javascriptException != nullptr && rt != nullptr) {
        JsValue errObj = *m_javascriptException;
        if (errObj.isObject()) {
            auto exObj = TryGetJavaThrowableObject(jEnv, *rt, errObj);
            ex = (jthrowable)exObj.Move();
        }

        JniLocalRef msg(jEnv.NewStringUTF(m_message.c_str()));
        JniLocalRef stackTrace(jEnv.NewStringUTF(m_stackTrace.c_str()));

        if (ex == nullptr) {
            // The napi tree hands Java the napi_ref itself; here Java gets its own
            // owned handle, released by WrapJavaToJsException when it comes back.
            // Ownership is then unambiguous: this exception object keeps its
            // shared_ptr and Java keeps the copy it was given.
            auto* javaOwnedValue = new JsValue(*rt, errObj);
            ex = static_cast<jthrowable>(jEnv.NewObject(NATIVESCRIPTEXCEPTION_CLASS, NATIVESCRIPTEXCEPTION_JSVALUE_CTOR_ID, (jstring)msg, (jstring)stackTrace, reinterpret_cast<jlong>(javaOwnedValue)));
        } else {
            auto excClassName = ObjectManager::GetClassName(ex);
            if (excClassName != "com/tns/NativeScriptException") {
                ex = static_cast<jthrowable>(jEnv.NewObject(NATIVESCRIPTEXCEPTION_CLASS, NATIVESCRIPTEXCEPTION_THROWABLE_CTOR_ID, (jstring)msg, (jstring)stackTrace, ex));
            }
        }
    } else if (!m_message.empty()) {
        JniLocalRef msg(jEnv.NewStringUTF(m_message.c_str()));
        JniLocalRef stackTrace(jEnv.NewStringUTF(m_stackTrace.c_str()));
        ex = static_cast<jthrowable>(jEnv.NewObject(NATIVESCRIPTEXCEPTION_CLASS, NATIVESCRIPTEXCEPTION_JSVALUE_CTOR_ID, (jstring)msg, (jstring)stackTrace, (jlong)0));
    } else {
        JniLocalRef msg(jEnv.NewStringUTF("No java exception or message provided."));
         ex = static_cast<jthrowable>(jEnv.NewObject(NATIVESCRIPTEXCEPTION_CLASS, NATIVESCRIPTEXCEPTION_JSVALUE_CTOR_ID, (jstring)msg, (jstring)nullptr, (jlong)0));
    }
    jEnv.Throw(ex);
}

void NativeScriptException::Init() {
    JEnv jenv;

    RUNTIME_CLASS = jenv.FindClass("com/tns/Runtime");
    assert(RUNTIME_CLASS != nullptr);

    THROWABLE_CLASS = jenv.FindClass("java/lang/Throwable");
    assert(THROWABLE_CLASS != nullptr);

    NATIVESCRIPTEXCEPTION_CLASS = jenv.FindClass("com/tns/NativeScriptException");
    assert(NATIVESCRIPTEXCEPTION_CLASS != nullptr);

    NATIVESCRIPTEXCEPTION_JSVALUE_CTOR_ID = jenv.GetMethodID(NATIVESCRIPTEXCEPTION_CLASS, "<init>", "(Ljava/lang/String;Ljava/lang/String;J)V");
    assert(NATIVESCRIPTEXCEPTION_JSVALUE_CTOR_ID != nullptr);

    NATIVESCRIPTEXCEPTION_THROWABLE_CTOR_ID = jenv.GetMethodID(NATIVESCRIPTEXCEPTION_CLASS, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)V");
    assert(NATIVESCRIPTEXCEPTION_THROWABLE_CTOR_ID != nullptr);

    NATIVESCRIPTEXCEPTION_GET_STACK_TRACE_AS_STRING_METHOD_ID = jenv.GetStaticMethodID(NATIVESCRIPTEXCEPTION_CLASS, "getStackTraceAsString", "(Ljava/lang/Throwable;)Ljava/lang/String;");
    assert(NATIVESCRIPTEXCEPTION_GET_STACK_TRACE_AS_STRING_METHOD_ID != nullptr);

    NATIVESCRIPTEXCEPTION_GET_MESSAGE_METHOD_ID = jenv.GetStaticMethodID(NATIVESCRIPTEXCEPTION_CLASS, "getMessage", "(Ljava/lang/Throwable;)Ljava/lang/String;");
    assert(NATIVESCRIPTEXCEPTION_GET_MESSAGE_METHOD_ID != nullptr);
}

// ON UNCAUGHT EXCEPTION
void NativeScriptException::OnUncaughtError(JsRuntime& rt, const JsValue& error) {
    string errorMessage = GetErrorMessage(rt, error);
    string stackTrace = GetErrorStackTrace(rt, error);

    NativeScriptException e(errorMessage, stackTrace);
    e.ReThrowToJava(&rt);
}

void NativeScriptException::CallJsFuncWithErr(JsRuntime& rt, const JsValue& errObj, bool isDiscarded) {
    JsObject global = rt.global();

    JsValue handler = isDiscarded ? global.getProperty(rt, "__onDiscardedError")
                                  : global.getProperty(rt, "__onUncaughtError");

    if (handler.isObject() && handler.asObjectBorrowed(rt).isFunction(rt)) {
        const JsValue args[] = {errObj};
        handler.asObjectBorrowed(rt).asFunction(rt).callWithThis(rt, global, args, 1);
    }
}

JsValue NativeScriptException::WrapJavaToJsException(JsRuntime& rt) {
    JsValue errObj;

    JEnv jenv;

    string excClassName = ObjectManager::GetClassName((jobject)m_javaException);
    if (excClassName == "com/tns/NativeScriptException") {
        jfieldID fieldID = jenv.GetFieldID(jenv.GetObjectClass(m_javaException), "jsValueAddress", "J");
        jlong addr = jenv.GetLongField(m_javaException, fieldID);

        if (addr != 0) {
            auto pv = reinterpret_cast<JsValue*>(addr);
            errObj = *pv;
            delete pv;
        } else {
            errObj = GetJavaExceptionFromEnv(rt, m_javaException, jenv);
        }
    } else {
        errObj = GetJavaExceptionFromEnv(rt, m_javaException, jenv);
    }

    return errObj;
}

JsValue NativeScriptException::GetJavaExceptionFromEnv(JsRuntime& rt, const JniLocalRef& exc, JEnv& jenv) {
    auto errMsg = GetExceptionMessage(jenv, exc);
    auto stackTrace = GetExceptionStackTrace(jenv, exc);
    DEBUG_WRITE("Error during java interop errorMessage: %s\n stackTrace:\n %s", errMsg.c_str(), stackTrace.c_str());

    auto objectManager = Runtime::GetRuntime(rt)->GetObjectManager();

    JsValue errObj = js_util::create_error(rt, errMsg, "0");
    if (!errObj.isObject()) {
        return errObj;
    }
    JsObject errorObject = errObj.asObject(rt);

    jint javaObjectID = objectManager->GetOrCreateObjectId((jobject)exc);
    auto nativeExceptionObject = objectManager->GetJsObjectByJavaObject(javaObjectID);

    if (js_util::is_null_or_undefined(nativeExceptionObject)) {
        string className = objectManager->GetClassName((jobject)exc);
        nativeExceptionObject = objectManager->CreateJSWrapper(javaObjectID, className);
    }

    errorObject.setProperty(rt, "nativeException", nativeExceptionObject);

    string jsStackTraceMessage = GetErrorStackTrace(rt, errObj);
    errorObject.setProperty(rt, "stack", ArgConverter::convertToJsString(rt, jsStackTraceMessage));
    errorObject.setProperty(rt, "stackTrace",
                            ArgConverter::convertToJsString(rt, jsStackTraceMessage + stackTrace));

    return errObj;
}

string NativeScriptException::GetFullMessage(JsRuntime& rt, const JsValue& error, const string& jsExceptionMessage) {
    if (!js_util::is_error(rt, error)) {
        return jsExceptionMessage;
    }

    stringstream ss;
    ss << jsExceptionMessage;

    string stackTraceMessage = GetErrorStackTrace(rt, error);

    ss << endl << "StackTrace: " << endl << stackTraceMessage << endl;

    string loggedMessage = ss.str();

    PrintErrorMessage(loggedMessage);

    return loggedMessage;
}

JniLocalRef NativeScriptException::TryGetJavaThrowableObject(JEnv& env, JsRuntime& rt, const JsValue& jsObj) {
    JniLocalRef javaThrowableObject;

    auto objectManager = Runtime::GetRuntime(rt)->GetObjectManager();

    auto javaObj = objectManager->GetJavaObjectByJsObject(jsObj);
    JniLocalRef objClass;

    if (!javaObj.IsNull()) {
        objClass = JniLocalRef(env.GetObjectClass(javaObj));
    } else {
        JsValue nativeEx = js_util::get_property(rt, jsObj, "nativeException");
        if (js_util::is_object(nativeEx)) {
            javaObj = objectManager->GetJavaObjectByJsObject(nativeEx);
            objClass = JniLocalRef(env.GetObjectClass(javaObj));
        }
    }

    auto isThrowable = !objClass.IsNull() ? env.IsAssignableFrom(objClass, THROWABLE_CLASS) : JNI_FALSE;

    if (isThrowable == JNI_TRUE) {
        javaThrowableObject = JniLocalRef(env.NewLocalRef(javaObj));
    }

    return javaThrowableObject;
}

void NativeScriptException::PrintErrorMessage(const string& errorMessage) {
    stringstream ss(errorMessage);
    string line;
    while (getline(ss, line, '\n')) {
        DEBUG_WRITE("%s", line.c_str());
    }
}

string NativeScriptException::GetErrorMessage(JsRuntime& rt, const JsValue& error, const string& prependMessage) {
    if (!js_util::is_error(rt, error)) {
        return js_util::coerce_to_string(rt, error);
    }

    JsValue message = js_util::get_property(rt, error, "message");

    string mes = ArgConverter::ConvertToString(rt, message);

    stringstream ss;

    if (!prependMessage.empty()) {
        ss << prependMessage << endl;
    }

    string errMessage;
    bool hasFullErrorMessage = false;
    JsValue fullMessage = js_util::get_property(rt, error, "fullMessage");
    if (fullMessage.isString()) {
        hasFullErrorMessage = true;
        errMessage = ArgConverter::ConvertToString(rt, fullMessage);
        ss << errMessage;
    }

    if (!mes.empty()) {
        if (hasFullErrorMessage) {
            ss << endl;
        }
        ss << mes;
    }

    return ss.str();
}

string NativeScriptException::GetErrorStackTrace(JsRuntime& rt, const JsValue& error) {
    stringstream ss;

    if (!js_util::is_error(rt, error)) return "";

    JsValue stack = js_util::get_property(rt, error, "stack");

    string stackStr = ArgConverter::ConvertToString(rt, stack);
    ss << stackStr;

    return ss.str();
}

string NativeScriptException::GetExceptionMessage(JEnv& env, jthrowable exception) {
    string errMsg;
    JniLocalRef msg(env.CallStaticObjectMethod(NATIVESCRIPTEXCEPTION_CLASS, NATIVESCRIPTEXCEPTION_GET_MESSAGE_METHOD_ID, exception));

    const char* msgStr = env.GetStringUTFChars(msg, nullptr);

    errMsg.append(msgStr);

    env.ReleaseStringUTFChars(msg, msgStr);

    return errMsg;
}

string NativeScriptException::GetExceptionStackTrace(JEnv& env, jthrowable exception) {
    string errStackTrace;
    JniLocalRef msg(env.CallStaticObjectMethod(NATIVESCRIPTEXCEPTION_CLASS, NATIVESCRIPTEXCEPTION_GET_STACK_TRACE_AS_STRING_METHOD_ID, exception));

    const char* msgStr = env.GetStringUTFChars(msg, nullptr);

    errStackTrace.append(msgStr);

    env.ReleaseStringUTFChars(msg, msgStr);

    return errStackTrace;
}

jclass NativeScriptException::RUNTIME_CLASS = nullptr;
jclass NativeScriptException::THROWABLE_CLASS = nullptr;
jclass NativeScriptException::NATIVESCRIPTEXCEPTION_CLASS = nullptr;
jmethodID NativeScriptException::NATIVESCRIPTEXCEPTION_JSVALUE_CTOR_ID = nullptr;
jmethodID NativeScriptException::NATIVESCRIPTEXCEPTION_THROWABLE_CTOR_ID = nullptr;
jmethodID NativeScriptException::NATIVESCRIPTEXCEPTION_GET_MESSAGE_METHOD_ID = nullptr;
jmethodID NativeScriptException::NATIVESCRIPTEXCEPTION_GET_STACK_TRACE_AS_STRING_METHOD_ID = nullptr;
