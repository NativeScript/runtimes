/*
 * ArgConverter.h
 *
 *  Created on: Jan 29, 2014
 *      Author: slavchev
 */

#ifndef ARGCONVERTER_H_
#define ARGCONVERTER_H_

#include "Runtime.h"
#include "NativeScriptAssert.h"
#include "JEnv.h"
#include <string>
#include <map>

namespace tns {

    class ArgConverter {
    public:
        static void Init(JsRuntime &rt);

        static void ConvertJavaArgsToJsArgs(JsRuntime &rt, jobjectArray args, size_t length,
                                            JsValue *arr);

        static JsValue ConvertFromJavaLong(JsRuntime &rt, jlong value);

        static int64_t ConvertToJavaLong(JsRuntime &rt, const JsValue &value);

        static JsValue jstringToJsString(JsRuntime &rt, jstring value) {
            if (value == nullptr) return js_util::null();

            JEnv jenv;
            auto chars = jenv.GetStringUTFChars(value, JNI_FALSE);
            auto length = jenv.GetStringUTFLength(value);
            auto jsString = convertToJsString(rt, chars, length);
            jenv.ReleaseStringUTFChars(value, chars);

            return jsString;
        }

        static std::string jstringToString(jstring value) {
            if (value == nullptr) {
                return {};
            }

            JEnv jenv;

            jboolean f = JNI_FALSE;
            auto chars = jenv.GetStringUTFChars(value, &f);
            std::string s(chars);
            jenv.ReleaseStringUTFChars(value, chars);

            return s;
        }

        inline static std::string ConvertToString(JsRuntime &rt, const JsValue &s) {
            if (!s.isString()) {
                return {};
            } else {
                return js_util::get_string_value(rt, s);
            }
        }

        static std::u16string ConvertToUtf16String(JsRuntime &rt, const JsValue &s);

        inline static jstring ConvertToJavaString(JsRuntime &rt, const JsValue &jsValue) {
            JEnv jenv;
            return jenv.NewStringUTF(js_util::get_string_value(rt, jsValue).c_str());
        }

        // engine::String is UTF-8 only, so a UTF-16 payload (every jchar/jstring
        // that reaches here) is transcoded on the way in rather than handed to a
        // napi_create_string_utf16 equivalent.
        static JsValue convertToJsString(JsRuntime &rt, const jchar *data, int length);

        inline static JsValue convertToJsString(JsRuntime &rt, const std::string &s) {
            return JsValue::createStringFromUtf8(rt, s.data(), s.size());
        }

        // Every Java string that reaches JS goes through here, and the value is
        // handed straight back to the engine by the callback that produced it.
        // Value::createStringFromUtf8 is the non-owning creation: on V8 it costs
        // neither the make_shared nor the global handle that
        // String::createFromUtf8 does. See jsi/v8/V8Runtime.h for the contract.
        inline static JsValue convertToJsString(JsRuntime &rt, const char *data, int length) {
            return JsValue::createStringFromUtf8(rt, data, (size_t) length);
        }

        inline static JsValue
        ConvertToJsUTF16String(JsRuntime &rt, const std::u16string &utf16string) {
            return convertToJsString(rt, reinterpret_cast<const jchar *>(utf16string.data()),
                                     (int) utf16string.length());
        }

        static void onDisposeRuntime(JsRuntime &rt);

    private:

        // TODO: plamen5kov: rewrite logic for java long number operations in javascript (java long -> javascript number operations check)
        static const long long JS_LONG_LIMIT = ((long long) 1) << 53;

        struct TypeLongOperationsCache {
            JsFunction LongNumberCtorFunc;
            JsObject NanNumberObject;
        };

        static TypeLongOperationsCache *GetTypeLongCache(JsRuntime &rt);

        inline static jstring ObjectToString(jobject object) {
            return (jstring) object;
        }

        inline static JsValue jcharToJsString(JsRuntime &rt, jchar value) {
            return convertToJsString(rt, &value, 1);
        }

        static JsValue NativeScriptLongFunctionCallback(JsRuntime &rt, const JsValue &jsThis,
                                                        const JsValue *argv, size_t argc);

        static JsValue NativeScriptLongValueOfFunctionCallback(JsRuntime &rt,
                                                               const JsValue &jsThis,
                                                               const JsValue *argv, size_t argc);

        static JsValue NativeScriptLongToStringFunctionCallback(JsRuntime &rt,
                                                                const JsValue &jsThis,
                                                                const JsValue *argv, size_t argc);

        /*
         * "s_type_long_operations_cache" used to keep function
         * dealing with operations concerning java long -> javascript number.
         */
        // Keyed by JsRuntime::identity(); &rt is not stable across callbacks.
        static robin_hood::unordered_map<const void *, TypeLongOperationsCache *> s_type_long_operations_cache;
    };
}

#endif /* ARGCONVERTER_H_ */
