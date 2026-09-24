#ifndef METHODCACHE_H_
#define METHODCACHE_H_

#include <string>
#include <map>
#include "JEnv.h"
#include "MetadataEntry.h"
#include "ArgsWrapper.h"
#include "NativeScriptAssert.h"
#include "MetadataReader.h"
#include "Runtime.h"
#include "MetadataNode.h"
#include "NumericCasts.h"
#include "NativeScriptException.h"
#include "JsArgToArrayConverter.h"
#include "Util.h"

namespace tns {
/*
 * MethodCache: class dealing with method/constructor resolution.
 */
class MethodCache {
    public:
        /*
         * CacheMethodInfo: struct holding resolved methods/constructor resolution
         */
        struct CacheMethodInfo {
            CacheMethodInfo()
                :
                retType(MethodReturnType::Unknown), mid(nullptr), clazz(nullptr), isStatic(false) {
            }
            std::string signature;
            std::string returnType;
            MethodReturnType retType;
            jmethodID mid;
            jclass clazz;
            bool isStatic;
        };

        static void Init();

    inline static MethodCache::CacheMethodInfo ResolveMethodSignature(JsRuntime &rt, const string &className, const string &methodName, size_t argc, const JsValue* argv, bool isStatic)
    {
        CacheMethodInfo method_info;

        auto encoded_method_signature = EncodeSignature(rt, className, methodName, argc, argv, isStatic);
        auto it = s_method_ctor_signature_cache.find(encoded_method_signature);

        if (it == s_method_ctor_signature_cache.end())
        {
            auto signature = ResolveJavaMethod(rt, argc, argv, className, methodName);

            DEBUG_WRITE("ResolveMethodSignature %s='%s'", encoded_method_signature.c_str(), signature.c_str());

            if (!signature.empty())
            {
                JEnv jEnv;
                auto clazz = jEnv.FindClass(className);
                assert(clazz != nullptr);
                method_info.clazz = clazz;
                method_info.signature = signature;
                method_info.returnType = MetadataReader::ParseReturnType(method_info.signature);
                method_info.retType = MetadataReader::GetReturnType(method_info.returnType);
                method_info.isStatic = isStatic;
                method_info.mid = isStatic
                                  ? jEnv.GetStaticMethodID(clazz, methodName, signature)
                                  : jEnv.GetMethodID(clazz, methodName, signature);

                s_method_ctor_signature_cache.emplace(encoded_method_signature, method_info);
            }
        }
        else
        {
            method_info = (*it).second;
        }

        return method_info;
    }

    inline static MethodCache::CacheMethodInfo ResolveConstructorSignature(JsRuntime &rt, const ArgsWrapper &argWrapper, const string &fullClassName, jclass javaClass, bool isInterface)
    {
        CacheMethodInfo constructor_info;

        auto encoded_ctor_signature = EncodeSignature(rt, fullClassName, "<init>", argWrapper.argc, argWrapper.argv, false);
        auto it = s_method_ctor_signature_cache.find(encoded_ctor_signature);

        if (it == s_method_ctor_signature_cache.end())
        {
            auto signature = ResolveConstructor(rt, argWrapper.argc, argWrapper.argv, javaClass, isInterface);

            DEBUG_WRITE("ResolveConstructorSignature %s='%s'", encoded_ctor_signature.c_str(), signature.c_str());

            if (!signature.empty())
            {
                JEnv jEnv;
                constructor_info.clazz = javaClass;
                constructor_info.signature = signature;
                constructor_info.mid = jEnv.GetMethodID(javaClass, "<init>", signature);

                s_method_ctor_signature_cache.emplace(encoded_ctor_signature, constructor_info);
            }
        }
        else
        {
            constructor_info = (*it).second;
        }

        return constructor_info;
    }

private:
        MethodCache() {
        }

    // Encoded signature <className>.S/I.<methodName>.<argsCount>.<arg1class>.<...>
    inline static string EncodeSignature(JsRuntime &rt, const string &className, const string &methodName, size_t argc, const JsValue* argv, bool isStatic)
    {
        string sig(className);
        sig.append(".");
        if (isStatic)
        {
            sig.append("S.");
        }
        else
        {
            sig.append("I.");
        }
        sig.append(methodName);
        sig.append(".");

        stringstream s;
        s << argc;
        sig.append(s.str());

        for (size_t i = 0; i < argc; i++)
        {
            sig.append(".");
            sig.append(GetType(rt, argv[i]));
        }

        return sig;
    }

    inline static string GetType(JsRuntime &rt, const JsValue &value)
    {
        string type = "";

        if (value.isObject())
        {
            MetadataNode *nullNode = MetadataNode::GetNullNode(rt, value);
            if (nullNode != nullptr)
            {
                type = nullNode->GetName();

                DEBUG_WRITE("Parameter of type %s with NULL value is passed to the method.", type.c_str());
                return type;
            }
        }

        if (value.isString()) {
            type = "string";
        } else if (value.isNull()) {
            type = "null";
        } else if (value.isUndefined()) {
            type = "undefined";
        } else if (value.isNumber()) {
            type = "number";
        } else if (value.isBool()) {
            type = "bool";
        } else if (value.isObject()) {
            // engine:: has one object kind, so the napi typeof ladder (which
            // separated napi_function from napi_object before reaching the
            // is_array/is_typedarray probes) collapses into this ordering.
            if (js_util::is_array(rt, value)) {
                type = "array";
            } else if (js_util::is_typedarray(rt, value)) {
                type = "typedarray";
            } else if (js_util::is_dataview(rt, value)) {
                type = "view";
            } else if (js_util::is_date(rt, value)) {
                type = "date";
            } else {
                type = "object";
            }
        }

        // Handle special cases for typed arrays
        if (type == "typedarray")
        {
            auto ctor = value.asObjectBorrowed(rt).getProperty(rt, "constructor");
            string name;
            if (ctor.isObject()) {
                auto ctorName = ctor.asObjectBorrowed(rt).getProperty(rt, "name");
                if (ctorName.isString()) name = ctorName.asString(rt).utf8(rt);
            }

            if (name == "Int8Array" || name == "Uint8Array" || name == "Uint8ClampedArray") {
                type = "bytebuffer";
            } else if (name == "Int16Array" || name == "Uint16Array") {
                type = "shortbuffer";
            } else if (name == "Int32Array" || name == "Uint32Array") {
                type = "intbuffer";
            } else if (name == "BigInt64Array" || name == "BigUint64Array") {
                type = "longbuffer";
            } else if (name == "Float32Array") {
                type = "floatbuffer";
            } else if (name == "Float64Array") {
                type = "doublebuffer";
            } else {
                type = "<unknown>";
            }
        }

        // Handle special cases for numbers
        if (type == "number")
        {
            double d = js_util::get_number(value);
            int64_t i = (int64_t)d;
            bool isInteger = d == i;
            type = isInteger ? "intnumber" : "doublenumber";
        }

        // Handle special cases for objects
        if (type == "object")
        {
            auto castType = NumericCasts::GetCastType(rt, value);
            MetadataNode *node;

            switch (castType)
            {
                case CastType::Char:
                    type = "char";
                    break;
                case CastType::Byte:
                    type = "byte";
                    break;
                case CastType::Short:
                    type = "short";
                    break;
                case CastType::Long:
                    type = "long";
                    break;
                case CastType::Float:
                    type = "float";
                    break;
                case CastType::Double:
                    type = "double";
                    break;
                case CastType::None:
                    node = MetadataNode::GetNodeFromHandle(rt, value);
                    type = (node != nullptr) ? node->GetName() : "<unknown>";

                    if (type == "<unknown>") {
                        if (js_util::is_number_object(rt, value)) {
                            JsValue numValue = js_util::valueOf(rt, value);
                            if (js_util::is_float(rt, numValue)) {
                                type = "float";
                            } else {
                                type = "int";
                            }
                        } else if (js_util::is_string_object(rt, value)) {
                            type = "string";
                        } else if (js_util::is_boolean_object(rt, value)) {
                            type = "bool";
                        }
                    }

                    break;
                default:
                    throw NativeScriptException("Unsupported cast type");
            }
        }

        if (type == "undefined") {
            type = "null";
        }

        return type;
    }

    inline static string ResolveJavaMethod(JsRuntime &rt, size_t argc, const JsValue* argv, const string &className, const string &methodName)
    {
        JEnv jEnv;

        JsArgToArrayConverter argConverter(rt, argc, argv, false);

        auto canonicalClassName = Util::ConvertFromJniToCanonicalName(className);
        JniLocalRef jsClassName(jEnv.NewStringUTF(canonicalClassName.c_str()));
        JniLocalRef jsMethodName(jEnv.NewStringUTF(methodName.c_str()));

        jobjectArray arrArgs = argConverter.ToJavaArray();

        auto runtime = Runtime::GetRuntime(rt);

        jstring signature = (jstring)jEnv.CallObjectMethod(runtime->GetJavaRuntime(), RESOLVE_METHOD_OVERLOAD_METHOD_ID, (jstring)jsClassName, (jstring)jsMethodName, arrArgs);

        string resolvedSignature;

        const char *str = jEnv.GetStringUTFChars(signature, nullptr);
        resolvedSignature = string(str);
        jEnv.ReleaseStringUTFChars(signature, str);

        jEnv.DeleteLocalRef(signature);

        return resolvedSignature;
    }

    inline static string ResolveConstructor(JsRuntime &rt, size_t argc, const JsValue* argv, jclass javaClass, bool isInterface)
    {
        JEnv jEnv;
        string resolvedSignature;

        JsArgToArrayConverter argConverter(rt, argc, argv, isInterface);
        if (argConverter.IsValid())
        {
            jobjectArray javaArgs = argConverter.ToJavaArray();

            auto runtime = Runtime::GetRuntime(rt);

            jstring signature = (jstring)jEnv.CallObjectMethod(runtime->GetJavaRuntime(), RESOLVE_CONSTRUCTOR_SIGNATURE_ID, javaClass, javaArgs);

            const char *str = jEnv.GetStringUTFChars(signature, nullptr);
            resolvedSignature = string(str);
            jEnv.ReleaseStringUTFChars(signature, str);
            jEnv.DeleteLocalRef(signature);
        }
        else
        {
            JsArgToArrayConverter::Error err = argConverter.GetError();
            throw NativeScriptException(err.msg);
        }

        return resolvedSignature;
    }

    static jclass RUNTIME_CLASS;

        static jmethodID RESOLVE_METHOD_OVERLOAD_METHOD_ID;

        static jmethodID RESOLVE_CONSTRUCTOR_SIGNATURE_ID;

        /*
         * "s_method_ctor_signature_cache" holding all resolved CacheMethodInfo against an encoded_signature string.
         *  Used for caching the resolved constructor or method signature.
         * The encoded signature has template: <className>.S/I.<methodName>.<argsCount>.<arg1class>.<...>
         */
        static robin_hood::unordered_map<std::string, CacheMethodInfo> s_method_ctor_signature_cache;
};
}

#endif /* METHODCACHE_H_ */
