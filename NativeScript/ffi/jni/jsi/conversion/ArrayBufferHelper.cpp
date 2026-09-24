#include "ArrayBufferHelper.h"
#include "ArgConverter.h"
#include "NativeScriptException.h"
#include <cstring>
#include <sstream>

using namespace tns;

namespace {
    // engine::ArrayBuffer takes ownership of a MutableBuffer instead of the napi
    // tree's (data, length, finalize) triple, so the two backing stores below are
    // what the two napi_create_external_arraybuffer calls become: one that only
    // points at the direct buffer Java already owns, and one that owns its copy.
    class BorrowedBuffer : public engine::MutableBuffer {
    public:
        BorrowedBuffer(uint8_t *data, size_t size) : m_data(data), m_size(size) {}

        size_t size() const override { return m_size; }

        uint8_t *data() override { return m_data; }

    private:
        uint8_t *m_data;
        size_t m_size;
    };

    class OwnedBuffer : public engine::MutableBuffer {
    public:
        explicit OwnedBuffer(size_t size) : m_data(new uint8_t[size]), m_size(size) {}

        ~OwnedBuffer() override { delete[] m_data; }

        size_t size() const override { return m_size; }

        uint8_t *data() override { return m_data; }

    private:
        uint8_t *m_data;
        size_t m_size;
    };
}

ArrayBufferHelper::ArrayBufferHelper()
        : m_objectManager(nullptr), m_ByteBufferClass(nullptr), m_isDirectMethodID(nullptr),
          m_remainingMethodID(nullptr), m_getMethodID(nullptr) {
}

void ArrayBufferHelper::CreateConvertFunctions(JsRuntime &rt, JsObject &global,
                                               ObjectManager *objectManager) {
    m_objectManager = objectManager;

    auto arrBufferCtor = global.getProperty(rt, "ArrayBuffer");
    if (!arrBufferCtor.isObject()) {
        return;
    }

    auto ctorObject = arrBufferCtor.asObject(rt);
    js_util::set_function(rt, ctorObject, "from",
                          [this](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                 size_t argc) -> JsValue {
                              try {
                                  return CreateFromCallbackImpl(rt, args, argc);
                              } catch (NativeScriptException &e) {
                                  e.ReThrowToJs(rt);
                              } catch (std::exception &e) {
                                  std::stringstream ss;
                                  ss << "Error: c++ exception: " << e.what() << std::endl;
                                  NativeScriptException(ss.str()).ReThrowToJs(rt);
                              } catch (...) {
                                  NativeScriptException(std::string("Error: c++ exception!"))
                                          .ReThrowToJs(rt);
                              }
                          });
}

JsValue ArrayBufferHelper::CreateFromCallbackImpl(JsRuntime &rt, const JsValue *args, size_t argc) {
    if (argc != 1) {
        throw NativeScriptException("Wrong number of arguments (1 expected)");
    }

    const JsValue &argObj = args[0];

    if (!argObj.isObject()) {
        throw NativeScriptException("Wrong type of argument (object expected)");
    }

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

    if (m_isDirectMethodID == nullptr) {
        m_isDirectMethodID = jEnv.GetMethodID(m_ByteBufferClass, "isDirect", "()Z");
        assert(m_isDirectMethodID != nullptr);
    }

    auto ret = jEnv.CallBooleanMethod(obj, m_isDirectMethodID);

    auto isDirectBuffer = ret == JNI_TRUE;

    std::shared_ptr<engine::MutableBuffer> buffer;

    if (isDirectBuffer) {
        auto data = jEnv.GetDirectBufferAddress(obj);
        auto size = jEnv.GetDirectBufferCapacity(obj);

        buffer = std::make_shared<BorrowedBuffer>(static_cast<uint8_t *>(data), (size_t) size);
    } else {
        if (m_remainingMethodID == nullptr) {
            m_remainingMethodID = jEnv.GetMethodID(m_ByteBufferClass, "remaining", "()I");
            assert(m_remainingMethodID != nullptr);
        }

        int bufferRemainingSize = jEnv.CallIntMethod(obj, m_remainingMethodID);

        if (m_getMethodID == nullptr) {
            m_getMethodID = jEnv.GetMethodID(m_ByteBufferClass, "get",
                                             "([BII)Ljava/nio/ByteBuffer;");
            assert(m_getMethodID != nullptr);
        }

        jbyteArray byteArray = jEnv.NewByteArray(bufferRemainingSize);
        jEnv.CallObjectMethod(obj, m_getMethodID, byteArray, 0, bufferRemainingSize);

        auto byteArrayElements = jEnv.GetByteArrayElements(byteArray, 0);

        auto owned = std::make_shared<OwnedBuffer>((size_t) bufferRemainingSize);
        memcpy(owned->data(), byteArrayElements, bufferRemainingSize);
        buffer = owned;

        jEnv.ReleaseByteArrayElements(byteArray, byteArrayElements, 0);
    }

    engine::ArrayBuffer arrayBuffer(rt, std::move(buffer));
    arrayBuffer.setProperty(rt, "nativeObject", argObj);

    return JsValue(rt, arrayBuffer);
}
