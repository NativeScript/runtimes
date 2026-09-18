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
        static void Init(napi_env env);

        static void ConvertJavaArgsToJsArgs(napi_env env, jobjectArray args, size_t length, napi_value* arr);

        static napi_value ConvertFromJavaLong(napi_env env, jlong value);

        static int64_t ConvertToJavaLong(napi_env env, napi_value value);

        static napi_value jstringToJsString(napi_env env, jstring value) {
            if (value == nullptr) return napi_util::null(env);

            JNIEnv *jni = JEnv();
            jsize length = jni->GetStringLength(value);
            const jchar *chars = jni->GetStringChars(value, nullptr);
            if (chars == nullptr) return napi_util::null(env);
            napi_value jsString = convertToJsString(env, chars, length);
            jni->ReleaseStringChars(value, chars);
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

        inline static std::string ConvertToString(napi_env env, napi_value s) {
            if (s == nullptr) {
                return {};
            } else {
                return napi_util::get_string_value(env, s);
            }
        }

        static std::u16string ConvertToUtf16String(napi_env env, napi_value s);

        inline static jstring ConvertToJavaString(napi_env env, napi_value jsValue) {
            size_t length = 0;
            if (napi_get_value_string_utf16(env, jsValue, nullptr, 0, &length) != napi_ok) return nullptr;
            char16_t stack[256];
            std::u16string heap;
            char16_t *buffer = stack;
            if (length + 1 > sizeof stack / sizeof stack[0]) {
                heap.resize(length + 1);
                buffer = heap.data();
            }
            size_t copied = 0;
            if (napi_get_value_string_utf16(env, jsValue, buffer, length + 1, &copied) != napi_ok) return nullptr;
            JEnv jenv;
            return jenv.NewString(reinterpret_cast<const jchar *>(buffer), static_cast<jsize>(copied));
        }

        inline static napi_value convertToJsString(napi_env env, const jchar *data, int length) {
            napi_value result;
            napi_create_string_utf16(env, reinterpret_cast<const char16_t *>(data), length,
                                     &result);
            return result;
        }

        inline static napi_value convertToJsString(napi_env env, const std::string &s) {
            napi_value result;
            napi_create_string_utf8(env, s.c_str(), s.length(), &result);
            return result;
        }

        inline static napi_value convertToJsString(napi_env env, const char *data, int length) {
            napi_value result;
            napi_create_string_utf8(env, data, length, &result);
            return result;
        }

        inline static napi_value
        ConvertToJsUTF16String(napi_env env, const std::u16string &utf16string) {
            napi_value result;
            napi_create_string_utf16(env, reinterpret_cast<const char16_t *>(utf16string.data()),
                                     utf16string.length(), &result);
            return result;
        }

        static void onDisposeEnv(napi_env env);

    private:

        // TODO: plamen5kov: rewrite logic for java long number operations in javascript (java long -> javascript number operations check)
        static const long long JS_LONG_LIMIT = ((long long) 1) << 53;

        struct TypeLongOperationsCache {
            napi_ref LongNumberCtorFunc;
            napi_ref NanNumberObject;
        };

        static TypeLongOperationsCache *GetTypeLongCache(napi_env env);

        inline static jstring ObjectToString(jobject object) {
            return (jstring) object;
        }

        inline static napi_value jcharToJsString(napi_env env, jchar value) {
            auto v8String = convertToJsString(env, &value, 1);
            return v8String;
        }

        static napi_value NativeScriptLongFunctionCallback(napi_env env, napi_callback_info info);

        static napi_value
        NativeScriptLongValueOfFunctionCallback(napi_env env, napi_callback_info info);

        static napi_value
        NativeScriptLongToStringFunctionCallback(napi_env env, napi_callback_info info);

        /*
         * "s_type_long_operations_cache" used to keep function
         * dealing with operations concerning java long -> javascript number.
         */
        static robin_hood::unordered_map<napi_env, TypeLongOperationsCache *> s_type_long_operations_cache;
    };
}

#endif /* ARGCONVERTER_H_ */