#ifndef NATIVESCRIPTEXCEPTION_H_
#define NATIVESCRIPTEXCEPTION_H_

#include <exception>

#include "Engine.h"
#include "JEnv.h"
#include "JniLocalRef.h"
#include "ObjectManager.h"

namespace tns {
// Derives from std::exception, which the napi tree's copy does not need to.
// There, a native error was reported by calling napi_throw and returning; here
// a C++ throw IS the mechanism, so a NativeScriptException that escapes a host
// callback unwinds through the engine's C++ frames. The engine trampolines
// catch JSError and std::exception; without this base an escapee matches
// neither and terminates the process instead of surfacing as a JS error.
class NativeScriptException : public std::exception {
    public:
        /*
         * Generates a NativeScriptException with java error from environment
         */
        NativeScriptException(JEnv& env);

        /*
         * Generates a NativeScriptException with given message
         */
        NativeScriptException(const std::string& message);

        /*
         * Generates a NativeScriptException with given message and stackTrace
         */
        NativeScriptException(const std::string& message, const std::string& stackTrace);

        /*
         * Generates a NativeScriptException with javascript error from the runtime and a prepend message if any
         */
        NativeScriptException(JsRuntime& rt, const JsValue& error, const std::string& message = "");

        /*
         * Generates a NativeScriptException from a caught engine::JSError. This is
         * how a JS throw reaches native code here, where the napi tree read a
         * pending exception with napi_get_and_clear_last_exception; the JSError
         * carries the thrown value when the engine had one, and only its message
         * when it did not.
         */
        NativeScriptException(JsRuntime& rt, const JsError& error, const std::string& message = "");

        // The napi counterpart (ReThrowToNapi) sets a pending exception and
        // returns, so callers followed it with `return nullptr`. engine:: signals
        // JS errors by throwing, and the engine's host-function wrapper converts a
        // JSError back into a JS throw carrying the original value -- so this does
        // not return, and any statement after a call to it is unreachable.
        [[noreturn]] void ReThrowToJs(JsRuntime& rt);
        void ReThrowToJava(JsRuntime* rt);

        // The stored message, for logging uncaught native exceptions.
        const char* what() const noexcept override { return m_message.c_str(); }

        static void Init();

        /*
         * This handler is attached to the runtime to handle uncaught javascript exceptions.
         */
        static void OnUncaughtError(JsRuntime& rt, const JsValue& error);

        /*
         * Calls the global "__onUncaughtError" or "__onDiscardedError" if such is provided
         */
        static void CallJsFuncWithErr(JsRuntime& rt, const JsValue& errObj, bool isDiscarded);

    private:
        /*
         * Try to get native exception or NativeScriptException from js object
         */
        JniLocalRef TryGetJavaThrowableObject(JEnv& env, JsRuntime& rt, const JsValue& jsObj);

        /*
         * Gets java exception message from jthrowable
         */
        std::string GetExceptionMessage(JEnv& env, jthrowable exception);

        /*
         * Gets java exception stack trace from jthrowable
         */
        std::string GetExceptionStackTrace(JEnv& env, jthrowable exception);

        /*
         * Gets the member m_javaException, wraps it and creates a javascript error object from it
         */
        JsValue WrapJavaToJsException(JsRuntime& rt);

        /*
         * Gets all the information from a java exception and puts it in a javascript error object
         */
        JsValue GetJavaExceptionFromEnv(JsRuntime& rt, const JniLocalRef& exc, JEnv& jenv);

        /*
         * Gets all the information from a js message and an js error object and puts it in a string
         */
        static std::string GetErrorMessage(JsRuntime& rt, const JsValue& error, const std::string& prependMessage = "");

        /*
         * Generates string stack trace from js StackTrace
         */
        static std::string GetErrorStackTrace(JsRuntime& rt, const JsValue& stackTrace);

        /*
         *	Adds a prepend message to the normal message process
         */
        std::string GetFullMessage(JsRuntime& rt, const JsValue& error, const std::string& jsExceptionMessage);

        // A napi_ref in the napi tree. An owned engine::Value is already the
        // persistent handle a reference was, so the refcount goes away; shared_ptr
        // keeps the exception copyable, which `catch (NativeScriptException& e)`
        // followed by a rethrow relies on.
        std::shared_ptr<JsValue> m_javascriptException;
        JniLocalRef m_javaException;
        std::string m_message;
        std::string m_stackTrace;
        std::string m_fullMessage;

        static jclass RUNTIME_CLASS;
        static jclass THROWABLE_CLASS;
        static jclass NATIVESCRIPTEXCEPTION_CLASS;
        static jmethodID NATIVESCRIPTEXCEPTION_JSVALUE_CTOR_ID;
        static jmethodID NATIVESCRIPTEXCEPTION_THROWABLE_CTOR_ID;
        static jmethodID NATIVESCRIPTEXCEPTION_GET_MESSAGE_METHOD_ID;
        static jmethodID NATIVESCRIPTEXCEPTION_GET_STACK_TRACE_AS_STRING_METHOD_ID;

        static void PrintErrorMessage(const std::string& errorMessage);
};
}

#endif /* NATIVESCRIPTEXCEPTION_H_ */
