#include "JsArgConverter.h"
#include "ObjectManager.h"
#include "JniSignatureParser.h"
#include "JsArgToArrayConverter.h"
#include "ArgConverter.h"
#include "NumericCasts.h"
#include "MetadataNode.h"
#include "NativeScriptException.h"
#include <cstdlib>

using namespace std;
using namespace tns;

namespace {
    // The napi tree reads the element type from napi_get_typedarray_info. There is
    // no engine:: equivalent -- typed arrays are not part of the engine contract --
    // so the element type is taken from the view's own constructor name, which is
    // the same information by another route and needs no per-engine support.
    tns::BufferCastType GetBufferCastType(JsRuntime &rt, const JsValue &view) {
        auto ctor = view.asObjectBorrowed(rt).getProperty(rt, "constructor");
        if (!ctor.isObject()) return tns::BufferCastType::Byte;
        auto name = ctor.asObjectBorrowed(rt).getProperty(rt, "name");
        if (!name.isString()) return tns::BufferCastType::Byte;
        std::string n = name.asString(rt).utf8(rt);

        if (n == "Int16Array" || n == "Uint16Array") return tns::BufferCastType::Short;
        if (n == "Int32Array" || n == "Uint32Array") return tns::BufferCastType::Int;
        if (n == "Float32Array") return tns::BufferCastType::Float;
        if (n == "Float64Array") return tns::BufferCastType::Double;
        if (n == "BigInt64Array" || n == "BigUint64Array") return tns::BufferCastType::Long;
        return tns::BufferCastType::Byte;
    }
}

JsArgConverter::JsArgConverter(JsRuntime &rt, const JsValue &caller, const JsValue *args, size_t argc,
                               const std::string &methodSignature, MetadataEntry *entry, JNIEnv *jniEnv,
                               ObjectManager *objectManager)
        : m_rt(&rt), m_jniEnv(jniEnv), m_objectManager(objectManager), m_isValid(true),
          m_error(Error()) {
    int providedArgumentsLength = argc;
    m_argsLen = 1 + providedArgumentsLength;

    if (m_argsLen > 0) {
        if ((entry != nullptr) && (entry->getIsResolved())) {
            if (entry->parsedSig.empty()) {
                JniSignatureParser parser(methodSignature);
                entry->parsedSig = parser.Parse();
            }
            m_tokens = &entry->parsedSig;
        } else {
            JniSignatureParser parser(methodSignature);
            m_ownedTokens = parser.Parse();
            m_tokens = &m_ownedTokens;
        }

        m_isValid = ConvertArg(rt, caller, 0);

        if (!m_isValid) {
            throw NativeScriptException("Error while converting argument!");
        }

        for (size_t i = 0; i < providedArgumentsLength; i++) {
            m_isValid = ConvertArg(rt, args[i], i + 1);

            if (!m_isValid) {
                break;
            }
        }
    }
}

JsArgConverter::JsArgConverter(JsRuntime &rt, const JsValue *args, size_t argc,
                               bool hasImplementationObject, const std::string &methodSignature,
                               MetadataEntry *entry, JNIEnv *jniEnv, ObjectManager *objectManager)
        : m_rt(&rt), m_jniEnv(jniEnv), m_objectManager(objectManager), m_isValid(true),
          m_error(Error()) {
    m_argsLen = !hasImplementationObject ? argc : argc - 1;

    if (m_argsLen > 0) {
        if ((entry != nullptr) && (entry->getIsResolved())) {
            if (entry->parsedSig.empty()) {
                JniSignatureParser parser(methodSignature);
                entry->parsedSig = parser.Parse();
            }
            m_tokens = &entry->parsedSig;
        } else {
            JniSignatureParser parser(methodSignature);
            m_ownedTokens = parser.Parse();
            m_tokens = &m_ownedTokens;
        }

        for (size_t i = 0; i < m_argsLen; i++) {
            m_isValid = ConvertArg(rt, args[i], i);

            if (!m_isValid) {
                break;
            }
        }
    }
}

JsArgConverter::JsArgConverter(JsRuntime &rt, const JsValue *args, size_t argc,
                               const std::string &methodSignature)
        : m_rt(&rt), m_isValid(true), m_error(Error()) {
    m_argsLen = argc;

    JniSignatureParser parser(methodSignature);
    m_ownedTokens = parser.Parse();
    m_tokens = &m_ownedTokens;

    for (size_t i = 0; i < m_argsLen; i++) {
        m_isValid = ConvertArg(rt, args[i], i);

        if (!m_isValid) {
            break;
        }
    }
}

bool JsArgConverter::ConvertArg(JsRuntime &rt, const JsValue &arg, int index) {
    bool success = false;

    char buff[1024];
    buff[0] = '\0';

    const auto &typeSignature = (*m_tokens)[index];

    // Record only the failing index up front (cheap). The default diagnostic
    // string is built lazily in GetError() from m_tokens[index], so the common
    // success path pays no per-argument string allocation.
    m_error.index = index;

    if (arg.isObject()) {
        if (js_util::is_array(rt, arg)) {
            success = typeSignature[0] == '[';

            if (success) {
                success = ConvertJavaScriptArray(rt, arg, index);
            }

            if (!success) {
                snprintf(buff, sizeof(buff), "Cannot convert array to %s at index %d",
                         typeSignature.c_str(), index);
            }
        } else {
            auto objectManager = m_objectManager != nullptr
                                 ? m_objectManager
                                 : Runtime::GetRuntime(rt)->GetObjectManager();

            bool isHostObject = objectManager->IsHostObject(arg);

            CastType castType = isHostObject ? CastType::None
                                             : NumericCasts::GetCastType(rt, arg);

            JsValue castValue;
            if (castType != CastType::None) {
                castValue = NumericCasts::GetCastValue(rt, arg);
            }

            JniLocalRef obj;

            JEnv jEnv = GetJEnv();

            switch (castType) {
                case CastType::Char:
                    if (castValue.isString()) {
                        string value = ArgConverter::ConvertToString(rt, castValue);
                        m_args[index].c = (jchar) value[0];
                        success = true;
                    }
                    break;

                case CastType::Byte:
                    if (castValue.isString()) {
                        string strValue = ArgConverter::ConvertToString(rt, castValue);
                        int byteArg = atoi(strValue.c_str());
                        jbyte value = (jbyte) byteArg;
                        success = ConvertFromCastFunctionObject(value, index);
                    } else if (castValue.isNumber()) {
                        jbyte value = (jbyte) js_util::get_int32(castValue);
                        success = ConvertFromCastFunctionObject(value, index);
                    }

                    break;

                case CastType::Short:
                    if (castValue.isString()) {
                        string strValue = ArgConverter::ConvertToString(rt, castValue);
                        int shortArg = atoi(strValue.c_str());
                        jshort value = (jshort) shortArg;
                        success = ConvertFromCastFunctionObject(value, index);
                    } else if (castValue.isNumber()) {
                        jshort value = (jshort) js_util::get_int32(castValue);
                        success = ConvertFromCastFunctionObject(value, index);
                    }
                    break;

                case CastType::Long:
                    if (castValue.isString()) {
                        string strValue = ArgConverter::ConvertToString(rt, castValue);
                        jlong value = (jlong) atoll(strValue.c_str());
                        success = ConvertFromCastFunctionObject(value, index);
                    } else if (castValue.isNumber()) {
                        jlong value = (jlong) js_util::get_number(castValue);
                        success = ConvertFromCastFunctionObject(value, index);
                    }
                    break;

                case CastType::Float:
                    if (castValue.isNumber()) {
                        jfloat value = (jfloat) js_util::get_number(castValue);
                        success = ConvertFromCastFunctionObject(value, index);
                    }
                    break;

                case CastType::Double:
                    if (castValue.isNumber()) {
                        jdouble value = (jdouble) js_util::get_number(castValue);
                        success = ConvertFromCastFunctionObject(value, index);
                    }
                    break;

                case CastType::None: {
                    obj = objectManager->GetJavaObjectByJsObject(arg);

                    if (obj.IsNull()) {
                        bool isArrayBuffer = arg.asObjectBorrowed(rt).isArrayBuffer(rt);
                        bool isTypedArray = false;
                        bool isDataView = false;

                        if (!isArrayBuffer) {
                            isTypedArray = js_util::is_typedarray(rt, arg);
                            if (!isTypedArray) {
                                isDataView = js_util::is_dataview(rt, arg);
                            }
                        }

                        if (isArrayBuffer || isDataView || isTypedArray) {
                            obj = JsArgConverter::GetByteBuffer(rt, arg, isArrayBuffer,
                                                                isTypedArray, isDataView);
                        }
                    }

                    if (!isHostObject) {
                        if (MetadataNode::GetNullNode(rt, arg) != nullptr) {
                            SetConvertedObject(index, nullptr);
                            success = true;
                            break;
                        }
                    }

                    success = !obj.IsNull();

                    if (success) {
                        SetConvertedObject(index, obj.Move(), obj.IsGlobal());
                    } else {
                        if (js_util::is_number_object(rt, arg)) {
                            success = ConvertJavaScriptNumber(rt, arg, index, true);
                            break;
                        } else if (js_util::is_string_object(rt, arg)) {
                            JsValue stringValue = js_util::valueOf(rt, arg);
                            success = ConvertJavaScriptString(rt, stringValue, index);
                            break;
                        } else if (js_util::is_boolean_object(rt, arg)) {
                            JsValue boolValue = js_util::valueOf(rt, arg);
                            success = ConvertJavaScriptBoolean(rt, boolValue, index);
                            break;
                        }

                        if (!success) {
                            snprintf(buff, sizeof(buff), "Cannot convert object to %s at index %d",
                                     typeSignature.c_str(), index);
                        }
                    }
                    break;
                }

                default:
                    throw NativeScriptException("Unsupported cast type");
            }
        }
    } else if (arg.isNumber()) {
        success = ConvertJavaScriptNumber(rt, arg, index, false);

        if (!success) {
            snprintf(buff, sizeof(buff), "Cannot convert number to %s at index %d",
                     typeSignature.c_str(), index);
        }
    } else if (arg.isBool()) {
        success = ConvertJavaScriptBoolean(rt, arg, index);

        if (!success) {
            snprintf(buff, sizeof(buff), "Cannot convert boolean to %s at index %d",
                     typeSignature.c_str(), index);
        }
    } else if (arg.isString()) {
        success = ConvertJavaScriptString(rt, arg, index);

        if (!success) {
            snprintf(buff, sizeof(buff), "Cannot convert string to %s at index %d",
                     typeSignature.c_str(), index);
        }
    } else if (arg.isUndefined() || arg.isNull()) {
        SetConvertedObject(index, nullptr);
        success = true;
    } else {
        SetConvertedObject(index, nullptr);
        success = false;
    }

    if (!success) {
        m_error.index = index;
        // Keep the seeded default when no specific message was formatted (buff
        // untouched), avoiding a garbage/empty message.
        if (buff[0] != '\0') {
            m_error.msg = string(buff);
        }
    }

    return success;
}


void JsArgConverter::SetConvertedObject(int index, jobject obj, bool isGlobal) {
    m_args[index].l = obj;
    if ((obj != nullptr) && !isGlobal) {
        m_args_refs[m_args_refs_size++] = index;
    }
}

bool JsArgConverter::ConvertJavaScriptNumber(JsRuntime &rt, const JsValue &jsValue, int index,
                                             bool isNumberObject) {
    bool success = true;

    jvalue value = {0};

    const auto &typeSignature = (*m_tokens)[index];

    const char typePrefix = typeSignature[0];

    double number = isNumberObject ? js_util::get_number(js_util::valueOf(rt, jsValue))
                                   : js_util::get_number(jsValue);

    switch (typePrefix) {
        case 'B': // byte
            value.b = (jbyte) (int32_t) number;
            break;
        case 'S': // short
            value.s = (jshort) (int32_t) number;
            break;
        case 'I': // int
            value.i = (jint) (int32_t) number;
            break;
        case 'J': // long
            value.j = (jlong) (int64_t) number;
            break;
        case 'F': // float
            value.f = (jfloat) number;
            break;
        case 'D': // double
            value.d = (jdouble) number;
            break;
        default:
            success = false;
            break;
    }

    if (success) {
        m_args[index] = value;
    }

    return success;
}

bool JsArgConverter::ConvertJavaScriptBoolean(JsRuntime &rt, const JsValue &jsValue, int index) {
    bool success;

    const auto &typeSignature = (*m_tokens)[index];

    if (typeSignature == "Z") {
        m_args[index].z = js_util::get_bool(jsValue) ? JNI_TRUE : JNI_FALSE;
        success = true;
    } else {
        success = false;
    }

    return success;
}

bool JsArgConverter::ConvertJavaScriptString(JsRuntime &rt, const JsValue &jsValue, int index) {
    jstring stringObject = ArgConverter::ConvertToJavaString(rt, jsValue);
    SetConvertedObject(index, stringObject);
    return true;
}

bool JsArgConverter::ConvertJavaScriptArray(JsRuntime &rt, const JsValue &jsArr, int index) {
    bool success = true;

    jarray arr = nullptr;

    auto jsArray = jsArr.asObjectBorrowed(rt).getArray(rt);
    const jsize arrLength = (jsize) jsArray.size(rt);

    const auto &arraySignature = (*m_tokens)[index];

    std::string elementType = arraySignature.substr(1);

    const char elementTypePrefix = elementType[0];

    jclass elementClass;
    std::string strippedClassName;

    JEnv jenv = GetJEnv();
    switch (elementTypePrefix) {
        case 'Z': {
            arr = jenv.NewBooleanArray(arrLength);
            std::vector<jboolean> bools(arrLength);
            for (jsize i = 0; i < arrLength; i++) {
                bools[i] = (jboolean) js_util::get_bool(jsArray.getValueAtIndexBorrowed(rt, i));
            }
            jenv.SetBooleanArrayRegion((jbooleanArray) arr, 0, arrLength, bools.data());
            break;
        }
        case 'B': {
            arr = jenv.NewByteArray(arrLength);
            std::vector<jbyte> bytes(arrLength);
            for (jsize i = 0; i < arrLength; i++) {
                bytes[i] = (jbyte) js_util::get_int32(jsArray.getValueAtIndexBorrowed(rt, i));
            }
            jenv.SetByteArrayRegion((jbyteArray) arr, 0, arrLength, bytes.data());
            break;
        }
        case 'C': {
            arr = jenv.NewCharArray(arrLength);
            std::vector<jchar> chars(arrLength);
            for (jsize i = 0; i < arrLength; i++) {
                std::string str = js_util::get_string_value(rt,
                                                            jsArray.getValueAtIndexBorrowed(rt, i));
                chars[i] = str.empty() ? (jchar) 0 : (jchar) str[0];
            }
            jenv.SetCharArrayRegion((jcharArray) arr, 0, arrLength, chars.data());
            break;
        }
        case 'S': {
            arr = jenv.NewShortArray(arrLength);
            std::vector<jshort> shorts(arrLength);
            for (jsize i = 0; i < arrLength; i++) {
                shorts[i] = (jshort) js_util::get_int32(jsArray.getValueAtIndexBorrowed(rt, i));
            }
            jenv.SetShortArrayRegion((jshortArray) arr, 0, arrLength, shorts.data());
            break;
        }
        case 'I': {
            arr = jenv.NewIntArray(arrLength);
            std::vector<jint> ints(arrLength);
            for (jsize i = 0; i < arrLength; i++) {
                ints[i] = (jint) js_util::get_int32(jsArray.getValueAtIndexBorrowed(rt, i));
            }
            jenv.SetIntArrayRegion((jintArray) arr, 0, arrLength, ints.data());
            break;
        }
        case 'J': {
            arr = jenv.NewLongArray(arrLength);
            std::vector<jlong> longs(arrLength);
            for (jsize i = 0; i < arrLength; i++) {
                longs[i] = (jlong) js_util::get_number(jsArray.getValueAtIndexBorrowed(rt, i));
            }
            jenv.SetLongArrayRegion((jlongArray) arr, 0, arrLength, longs.data());
            break;
        }
        case 'F': {
            arr = jenv.NewFloatArray(arrLength);
            std::vector<jfloat> floats(arrLength);
            for (jsize i = 0; i < arrLength; i++) {
                floats[i] = (jfloat) js_util::get_number(jsArray.getValueAtIndexBorrowed(rt, i));
            }
            jenv.SetFloatArrayRegion((jfloatArray) arr, 0, arrLength, floats.data());
            break;
        }
        case 'D': {
            arr = jenv.NewDoubleArray(arrLength);
            std::vector<jdouble> doubles(arrLength);
            for (jsize i = 0; i < arrLength; i++) {
                doubles[i] = (jdouble) js_util::get_number(jsArray.getValueAtIndexBorrowed(rt, i));
            }
            jenv.SetDoubleArrayRegion((jdoubleArray) arr, 0, arrLength, doubles.data());
            break;
        }
        case 'L':
            strippedClassName = elementType.substr(1, elementType.length() - 2);
            elementClass = jenv.FindClass(strippedClassName);
            arr = jenv.NewObjectArray(arrLength, elementClass, nullptr);
            for (jsize i = 0; i < arrLength; i++) {
                JsValue element = jsArray.getValueAtIndex(rt, i);
                JsArgToArrayConverter c(rt, element, false, (int) Type::Null,
                                        m_objectManager != nullptr
                                        ? m_objectManager
                                        : Runtime::GetRuntime(rt)->GetObjectManager());
                jobject o = c.GetConvertedArg();
                jenv.SetObjectArrayElement((jobjectArray) arr, (int) i, o);
            }
            break;
        default:
            success = false;
            break;
    }

    if (success) {
        SetConvertedObject(index, arr);
    }

    return success;
}


template<typename T>
bool JsArgConverter::ConvertFromCastFunctionObject(T value, int index) {
    bool success = false;

    const auto &typeSignature = (*m_tokens)[index];

    const char typeSignaturePrefix = typeSignature[0];

    switch (typeSignaturePrefix) {
        case 'B':
            m_args[index].b = (jbyte) value;
            success = true;
            break;

        case 'S':
            m_args[index].s = (jshort) value;
            success = true;
            break;

        case 'I':
            m_args[index].i = (jint) value;
            success = true;
            break;

        case 'J':
            m_args[index].j = (jlong) value;
            success = true;
            break;

        case 'F':
            m_args[index].f = (jfloat) value;
            success = true;
            break;

        case 'D':
            m_args[index].d = (jdouble) value;
            success = true;
            break;

        default:
            success = false;
            break;
    }

    return success;
}

int JsArgConverter::Length() const {
    return m_argsLen;
}

bool JsArgConverter::IsValid() const {
    return m_isValid;
}

jvalue *JsArgConverter::ToArgs() {
    return m_args;
}

JsArgConverter::Error JsArgConverter::GetError() const {
    Error e = m_error;
    // Build the default diagnostic lazily (only when an error is actually
    // queried and no specific message was already formatted on the failure path).
    if (e.index >= 0 && e.msg.empty() && m_tokens != nullptr &&
        e.index < (int) m_tokens->size()) {
        e.msg = "Cannot convert argument at index " + std::to_string(e.index) +
                " to " + (*m_tokens)[e.index];
    }
    return e;
}

JsArgConverter::~JsArgConverter() {
    if (m_argsLen > 0) {
        JEnv env = GetJEnv();
        for (int i = 0; i < m_args_refs_size; i++) {
            int index = m_args_refs[i];
            if (index != -1) {
                env.DeleteLocalRef(m_args[index].l);
            }
        }
    }
}

JniLocalRef JsArgConverter::GetByteBuffer(JsRuntime &rt, const JsValue &object, bool isArrayBuffer,
                                          bool isTypedArray, bool isDataView) {
    JEnv jEnv;

    BufferCastType bufferCastType = tns::BufferCastType::Byte;
    size_t offset = 0;
    size_t length = 0;
    uint8_t *data = nullptr;

    if (isTypedArray || isDataView) {
        // The napi tree reads the view's data pointer (which already points at
        // the view's start) and then adds byteOffset again. Going through the
        // backing ArrayBuffer instead makes `data + offset` the view start
        // exactly once; the two agree wherever byteOffset is 0.
        auto view = object.asObjectBorrowed(rt);
        auto bufferValue = view.getProperty(rt, "buffer");
        if (!bufferValue.isObject()) {
            return JniLocalRef();
        }
        auto arrayBuffer = bufferValue.asObject(rt).getArrayBuffer(rt);
        data = arrayBuffer.data(rt);
        offset = (size_t) js_util::get_number(view.getPropertyBorrowed(rt, "byteOffset"));

        if (isTypedArray) {
            length = arrayBuffer.size(rt);
            bufferCastType = GetBufferCastType(rt, object);
        } else {
            length = (size_t) js_util::get_number(view.getPropertyBorrowed(rt, "byteLength"));
        }
    } else if (isArrayBuffer) {
        auto arrayBuffer = object.asObjectBorrowed(rt).getArrayBuffer(rt);
        data = arrayBuffer.data(rt);
        length = arrayBuffer.size(rt);
    }

    jobject directBuffer;

    if (isDataView || isTypedArray) {
        directBuffer = jEnv.NewDirectByteBuffer(data + offset, length);
    } else {
        directBuffer = jEnv.NewDirectByteBuffer(data, length);
    }


    auto directBufferClazz = jEnv.GetObjectClass(directBuffer);

    auto byteOrderId = BYTE_ORDER_METHOD_ID;

    if (!BYTE_ORDER_METHOD_ID) {
        byteOrderId = jEnv.GetMethodID(directBufferClazz, "order",
                                       "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;");
        BYTE_ORDER_METHOD_ID = byteOrderId;
    }

    auto byteOrderClazz = jEnv.FindClass("java/nio/ByteOrder");

    auto byteOrderEnumId = BYTE_ORDER_ENUM_ID;

    if (!byteOrderEnumId) {
        byteOrderEnumId = jEnv.GetStaticMethodID(byteOrderClazz,
                                                 "nativeOrder",
                                                 "()Ljava/nio/ByteOrder;");
        BYTE_ORDER_ENUM_ID = byteOrderEnumId;
    }

    auto nativeByteOrder = jEnv.CallStaticObjectMethodA(byteOrderClazz,
                                                        byteOrderEnumId,
                                                        nullptr);

    directBuffer = jEnv.CallObjectMethod(directBuffer, byteOrderId,
                                         nativeByteOrder);

    jobject buffer;

    if (bufferCastType == BufferCastType::Short) {
        auto id = AS_SHORT_BUFFER;
        if (!id) {
            id = jEnv.GetMethodID(directBufferClazz, "asShortBuffer",
                                  "()Ljava/nio/ShortBuffer;");
            AS_SHORT_BUFFER = id;
        }

        buffer = jEnv.CallObjectMethodA(directBuffer, id, nullptr);
    } else if (bufferCastType == BufferCastType::Int) {
        auto id = AS_INT_BUFFER;

        if (!id) {
            id = jEnv.GetMethodID(directBufferClazz, "asIntBuffer",
                                  "()Ljava/nio/IntBuffer;");
            AS_INT_BUFFER = id;
        }
        buffer = jEnv.CallObjectMethodA(directBuffer, id, nullptr);
    } else if (bufferCastType == BufferCastType::Long) {
        auto id = AS_LONG_BUFFER;

        if (!id) {
            id = jEnv.GetMethodID(directBufferClazz, "asLongBuffer",
                                  "()Ljava/nio/LongBuffer;");
            AS_LONG_BUFFER = id;
        }

        buffer = jEnv.CallObjectMethodA(directBuffer, id, nullptr);
    } else if (bufferCastType == BufferCastType::Float) {

        auto id = AS_FLOAT_BUFFER;
        if (!id) {
            id = jEnv.GetMethodID(directBufferClazz, "asFloatBuffer",
                                  "()Ljava/nio/FloatBuffer;");
            AS_FLOAT_BUFFER = id;
        }

        buffer = jEnv.CallObjectMethodA(directBuffer, id, nullptr);
    } else if (bufferCastType == BufferCastType::Double) {

        auto id = AS_DOUBLE_BUFFER;
        if (!id) {
            id = jEnv.GetMethodID(directBufferClazz, "asDoubleBuffer",
                                  "()Ljava/nio/DoubleBuffer;");
            AS_DOUBLE_BUFFER = id;
        }
        buffer = jEnv.CallObjectMethodA(directBuffer, id, nullptr);
    } else {
        buffer = directBuffer;
    }

    buffer = jEnv.NewGlobalRef(buffer);

    ObjectManager *objectManager = Runtime::GetRuntime(rt)->GetObjectManager();

    int id = objectManager->GetOrCreateObjectId(buffer);
    auto clazz = jEnv.GetObjectClass(buffer);

    ObjectManager::MarkObject(rt, object);

    objectManager->Link(object, id, clazz);

    return objectManager->GetJavaObjectByJsObject(object);
}

jmethodID JsArgConverter::BYTE_ORDER_METHOD_ID = nullptr;
jmethodID JsArgConverter::BYTE_ORDER_ENUM_ID = nullptr;
jmethodID JsArgConverter::AS_SHORT_BUFFER = nullptr;
jmethodID JsArgConverter::AS_LONG_BUFFER = nullptr;
jmethodID JsArgConverter::AS_FLOAT_BUFFER = nullptr;
jmethodID JsArgConverter::AS_INT_BUFFER = nullptr;
jmethodID JsArgConverter::AS_DOUBLE_BUFFER = nullptr;
