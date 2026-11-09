#include "ArgConverter.h"
#include <cstring>
#include <vector>
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
napi_value EnsurePlainConstructorThis(napi_env env, napi_value jsThis, napi_value prototype) {
    if (!napi_util::is_null_or_undefined(env, jsThis)) {
        return jsThis;
    }

    napi_value receiver = nullptr;
    if (napi_create_object(env, &receiver) != napi_ok || receiver == nullptr) {
        return nullptr;
    }

    if (!napi_util::is_null_or_undefined(env, prototype)) {
        napi_util::setPrototypeOf(env, receiver, prototype);
    }

    return receiver;
}
}

void ArgConverter::Init(napi_env env) {
    napi_status status;
    auto cache = GetTypeLongCache(env);

    napi_value longNumberCtorFunc;
    napi_value valueOfFunc;
    napi_value toStringFunc;

    NAPI_GUARD(napi_define_class(env, "NativeScriptLongNumber", NAPI_AUTO_LENGTH, ArgConverter::NativeScriptLongFunctionCallback,nullptr, 0, nullptr, &longNumberCtorFunc)) {
        return;
    }

    napi_value longNumberPrototype = napi_util::get_prototype(env, longNumberCtorFunc);

    NAPI_GUARD(napi_create_function(env, "valueOf", strlen("valueOf"),
                         ArgConverter::NativeScriptLongValueOfFunctionCallback, nullptr,
                         &valueOfFunc)) {
        return;
    }

    NAPI_GUARD(napi_create_function(env, "toString", strlen("toString"),
                         ArgConverter::NativeScriptLongToStringFunctionCallback, nullptr,
                         &toStringFunc)) {
        return;
    }


    NAPI_GUARD(napi_set_named_property(env, longNumberPrototype, "valueOf", valueOfFunc)) {
        return;
    }
    NAPI_GUARD(napi_set_named_property(env, longNumberPrototype, "toString", toStringFunc)) {
        return;
    }

    cache->LongNumberCtorFunc = napi_util::make_ref(env, longNumberCtorFunc, 1);
    napi_value nanValue;
    NAPI_GUARD(napi_create_double(env, numeric_limits<double>::quiet_NaN(), &nanValue)) {
        return;
    }


    napi_value global;
    NAPI_GUARD(napi_get_global(env, &global)) {
        return;
    }

    napi_value numCtor;
    NAPI_GUARD(napi_get_named_property(env, global, "Number", &numCtor)) {
        return;
    }

    napi_value nanObject;
    NAPI_GUARD(napi_new_instance(env, numCtor, 1, &nanValue, &nanObject)) {
        return;
    }

    cache->NanNumberObject = napi_util::make_ref(env, nanObject, 1);
}

napi_value ArgConverter::NativeScriptLongValueOfFunctionCallback(napi_env env, napi_callback_info info) {
    try {
        napi_status status;
        napi_value result;
        NAPI_GUARD(napi_create_double(env, numeric_limits<double>::quiet_NaN(), &result)) {
            return nullptr;
        }
        return result;
    } catch (NativeScriptException &e) {
        e.ReThrowToNapi(env);
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToNapi(env);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToNapi(env);
    }
    return nullptr;
}

napi_value ArgConverter::NativeScriptLongToStringFunctionCallback(napi_env env, napi_callback_info info) {
    try {
        napi_status status;
        napi_value thisArg;
        NAPI_GUARD(napi_get_cb_info(env, info, nullptr, nullptr, &thisArg, nullptr)) {
            return nullptr;
        }

        napi_value value;
        NAPI_GUARD(napi_get_named_property(env, thisArg, "value", &value)) {
            return nullptr;
        }

        return value;
    } catch (NativeScriptException &e) {
        e.ReThrowToNapi(env);
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToNapi(env);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
       nsEx.ReThrowToNapi(env);
    }
    return nullptr;
}

napi_value ArgConverter::NativeScriptLongFunctionCallback(napi_env env, napi_callback_info info) {
    try {
        NAPI_CALLBACK_BEGIN(1);
        napi_value newTarget;
        napi_get_new_target(env, info, &newTarget);
        napi_value receiverPrototype = !napi_util::is_null_or_undefined(env, newTarget)
                                       ? napi_util::get_prototype(env, newTarget)
                                       : nullptr;
        napi_value receiver = EnsurePlainConstructorThis(env, jsThis, receiverPrototype);
        if (receiver == nullptr) {
            return nullptr;
        }
        auto cache = GetTypeLongCache(env);
        napi_value javaLong;
        NAPI_GUARD(napi_get_boolean(env, true, &javaLong)) {
            return nullptr;
        }
        NAPI_GUARD(napi_set_named_property(env, receiver, "javaLong", javaLong)) {
            return nullptr;
        }

        NumericCasts::MarkAsLong(env, receiver, argv[0]);

        NAPI_GUARD(napi_set_named_property(env, receiver, "prototype", napi_util::get_ref_value(env, cache->NanNumberObject))) {
            return nullptr;
        }
        return receiver;

    } catch (NativeScriptException &e) {
      e.ReThrowToNapi(env);
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
          nsEx.ReThrowToNapi(env);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
     nsEx.ReThrowToNapi(env);
    }
    return nullptr;
}

int ArgConverter::GetJavaArgsLength(JNIEnv *env, const JavaCallArgs &args) {
    return args.types == nullptr ? 0 : env->GetArrayLength(args.types);
}

void ArgConverter::ConvertJavaArgsToJsArgs(napi_env env, const JavaCallArgs &args, size_t argc, napi_value* arr) {
    napi_status status;
    JEnv jenv;

    constexpr size_t kInlineArgs = 16;
    jint inlineTypes[kInlineArgs];
    jlong inlineValues[kInlineArgs];
    std::vector<jint> heapTypes;
    std::vector<jlong> heapValues;
    jint *types = inlineTypes;
    jlong *values = inlineValues;
    if (argc > kInlineArgs) {
        heapTypes.resize(argc);
        heapValues.resize(argc);
        types = heapTypes.data();
        values = heapValues.data();
    }

    JNIEnv *jni = jenv;
    jni->GetIntArrayRegion(args.types, 0, argc, types);
    jni->GetLongArrayRegion(args.values, 0, argc, values);

    auto runtime = Runtime::GetRuntime(env);
    auto objectManager = runtime->GetObjectManager();

    for (size_t i = 0; i < argc; i++) {
        jlong bits = values[i];
        napi_value jsArg;
        switch (static_cast<Type>(types[i])) {
            case Type::Boolean:
                NAPI_GUARD(napi_get_boolean(env, bits != 0, &jsArg)) {}
                break;
            case Type::Char:
                jsArg = jcharToJsString(env, static_cast<jchar>(bits));
                break;
            case Type::Byte:
            case Type::Short:
            case Type::Int:
                NAPI_GUARD(napi_create_int32(env, static_cast<int32_t>(bits), &jsArg)) {}
                break;
            case Type::Long:
                NAPI_GUARD(napi_create_int64(env, bits, &jsArg)) {}
                break;
            case Type::Float: {
                auto raw = static_cast<int32_t>(bits);
                float value;
                memcpy(&value, &raw, sizeof(value));
                NAPI_GUARD(napi_create_double(env, value, &jsArg)) {}
                break;
            }
            case Type::Double: {
                double value;
                memcpy(&value, &bits, sizeof(value));
                NAPI_GUARD(napi_create_double(env, value, &jsArg)) {}
                break;
            }
            case Type::String: {
                JniLocalRef str(jni->GetObjectArrayElement(args.refs, i));
                jsArg = jstringToJsString(env, (jstring) str);
                break;
            }
            case Type::JsObject: {
                auto javaObjectID = static_cast<jint>(bits);
                jsArg = objectManager->GetJsObjectByJavaObject(javaObjectID);

                if (napi_util::is_null_or_undefined(env, jsArg)) {
                    JniLocalRef argJavaClassPath(jni->GetObjectArrayElement(args.refs, i));
                    string argClassName = jstringToString(ObjectToString(argJavaClassPath));
                    argClassName = Util::ConvertFromCanonicalToJniName(argClassName);
                    jsArg = objectManager->CreateJSWrapper(javaObjectID, argClassName);
                }
                break;
            }
            case Type::Null:
                NAPI_GUARD(napi_get_null(env, &jsArg)) {}
                break;
        }

        arr[i] = jsArg;
    }
}

napi_value ArgConverter::ConvertFromJavaLong(napi_env env, jlong value) {
    napi_status status;
    napi_value convertedValue;
    long long longValue = value;

    if ((-JS_LONG_LIMIT < longValue) && (longValue < JS_LONG_LIMIT)) {
        NAPI_GUARD(napi_create_double(env, longValue, &convertedValue)) {
            return nullptr;
        }
    } else {
        auto cache = GetTypeLongCache(env);
        char strNumber[24];
        sprintf(strNumber, "%lld", longValue);
        napi_value strValue;
        NAPI_GUARD(napi_create_string_utf8(env, strNumber, NAPI_AUTO_LENGTH, &strValue)) {
            return nullptr;
        }
        napi_value args[1] = {strValue};

        NAPI_GUARD(napi_new_instance(env, napi_util::get_ref_value(env, cache->LongNumberCtorFunc), 1, args,
                          &convertedValue)) {
            return nullptr;
        }
    }

    return convertedValue;
}

int64_t ArgConverter::ConvertToJavaLong(napi_env env, napi_value value) {
    napi_status status;
    napi_value valueProp;
    NAPI_GUARD(napi_get_named_property(env, value, "value", &valueProp)) {
        return 0;
    }

    size_t str_len;
    NAPI_GUARD(napi_get_value_string_utf8(env, valueProp, nullptr, 0, &str_len)) {
        return 0;
    }
    string num(str_len, '\0');
    NAPI_GUARD(napi_get_value_string_utf8(env, valueProp, &num[0], str_len + 1, &str_len)) {
        return 0;
    }

    int64_t longValue = atoll(num.c_str());

    return longValue;
}

ArgConverter::TypeLongOperationsCache *ArgConverter::GetTypeLongCache(napi_env env) {
    auto cache = s_type_long_operations_cache.Get(env);
    if (cache) return cache;
    cache = new TypeLongOperationsCache;
    s_type_long_operations_cache.Insert(env, cache);
    return cache;
}

u16string ArgConverter::ConvertToUtf16String(napi_env env, napi_value s) {
    if (s == nullptr) {
        return {};
    } else {
        napi_status status;
        size_t str_len;
        NAPI_GUARD(napi_get_value_string_utf8(env, s, nullptr, 0, &str_len)) {
            return {};
        }
        string str(str_len, '\0');
        NAPI_GUARD(napi_get_value_string_utf8(env, s, &str[0], str_len + 1, &str_len)) {
            return {};
        }
        auto utf16str = Util::ConvertFromUtf8ToUtf16(str);

        return utf16str;
    }
}

void ArgConverter::onDisposeEnv(napi_env env) {
    napi_status status;
    auto cache = s_type_long_operations_cache.Get(env);
    if (cache) {
        if (cache->LongNumberCtorFunc) {
            NAPI_GUARD(napi_delete_reference(env, cache->LongNumberCtorFunc)) {}
        }
        if (cache->NanNumberObject) {
            NAPI_GUARD(napi_delete_reference(env, cache->NanNumberObject)) {}
        }
        s_type_long_operations_cache.Remove(env);
        delete cache;
    }
}

tns::ConcurrentMap<napi_env, ArgConverter::TypeLongOperationsCache *> ArgConverter::s_type_long_operations_cache;
