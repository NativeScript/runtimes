#include "ArrayBufferHelper.h"
#include "ArgConverter.h"
#include "NativeScriptException.h"
#include <sstream>

using namespace tns;

ArrayBufferHelper::ArrayBufferHelper()
        : m_objectManager(nullptr), m_ByteBufferClass(nullptr),
          m_remainingMethodID(nullptr), m_getMethodID(nullptr) {
}

void ArrayBufferHelper::CreateConvertFunctions(napi_env env, napi_value global, ObjectManager* objectManager) {
    napi_status status;
    m_objectManager = objectManager;
    napi_value fromFunc;
    NAPI_GUARD(napi_create_function(env, "from", NAPI_AUTO_LENGTH, CreateFromCallbackStatic, this, &fromFunc)) {
        return;
    }

    napi_value arrBufferCtorFunc;
    NAPI_GUARD(napi_get_named_property(env, global, "ArrayBuffer", &arrBufferCtorFunc)) {
        return;
    }
    NAPI_GUARD(napi_set_named_property(env, arrBufferCtorFunc, "from", fromFunc)) {}
}

napi_value ArrayBufferHelper::CreateFromCallbackStatic(napi_env env, napi_callback_info info) {
    NAPI_CALLBACK_BEGIN(1)
    try {
        auto thiz = reinterpret_cast<ArrayBufferHelper*>(data);
        return thiz->CreateFromCallbackImpl(env, argc, argv);
    } catch (NativeScriptException& e) {
        e.ReThrowToNapi(env);
    } catch (std::exception& e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToNapi(env);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToNapi(env);
    }

    return nullptr;
}

napi_value ArrayBufferHelper::CreateFromCallbackImpl(napi_env env, size_t argc, napi_value* args) {
    napi_status status;
    if (argc != 1) {
        throw NativeScriptException("Wrong number of arguments (1 expected)");
    }

    napi_value arg = args[0];

    bool isObject = napi_util::is_object(env, arg);

    if (!isObject) {
        throw NativeScriptException("Wrong type of argument (object expected)");
    }

    auto argObj = arg;

    auto obj = m_objectManager->GetJavaObjectByJsObject(argObj);

    if (obj.IsNull()) {
        throw NativeScriptException("Wrong type of argument (object expected)");
    }

    JEnv jEnv;

    if (m_ByteBufferClass == nullptr) {
        m_ByteBufferClass = jEnv.FindClass("java/nio/ByteBuffer");
        assert(m_ByteBufferClass != nullptr);
    }

    auto isByteBuffer = jEnv.IsInstanceOf(obj, m_ByteBufferClass);

    if (!isByteBuffer) {
        throw NativeScriptException("Wrong type of argument (ByteBuffer expected)");
    }

    napi_value arrayBuffer;

    auto data = jEnv.GetDirectBufferAddress(obj);

    if (data != nullptr) {
        auto size = jEnv.GetDirectBufferCapacity(obj);

        void* externalData = data;
        NAPI_GUARD(napi_create_external_arraybuffer(env, externalData, size, nullptr, nullptr, &arrayBuffer)) {
            return nullptr;
        }
    } else {
        if (m_remainingMethodID == nullptr) {
            m_remainingMethodID = jEnv.GetMethodID(m_ByteBufferClass, "remaining", "()I");
            assert(m_remainingMethodID != nullptr);
        }

        int bufferRemainingSize = jEnv.CallIntMethod(obj, m_remainingMethodID);

        if (m_getMethodID == nullptr) {
            m_getMethodID = jEnv.GetMethodID(m_ByteBufferClass, "get", "([BII)Ljava/nio/ByteBuffer;");
            assert(m_getMethodID != nullptr);
        }

        jbyteArray byteArray = jEnv.NewByteArray(bufferRemainingSize);
        jEnv.CallObjectMethod(obj, m_getMethodID, byteArray, 0, bufferRemainingSize);

        auto byteArrayElements = jEnv.GetByteArrayElements(byteArray, 0);

        jbyte* copy = new jbyte[bufferRemainingSize];
        memcpy(copy, byteArrayElements, bufferRemainingSize);

        NAPI_GUARD(napi_create_external_arraybuffer(env, copy, bufferRemainingSize, [](napi_env env, void* finalize_data, void* finalize_hint) {
            delete[] static_cast<jbyte*>(finalize_data);
        }, nullptr, &arrayBuffer)) {}

        jEnv.ReleaseByteArrayElements(byteArray, byteArrayElements, 0);
    }

    NAPI_GUARD(napi_set_named_property(env, arrayBuffer, "nativeObject", argObj)) {}

    return arrayBuffer;
}