#include "JsArgToArrayConverter.h"
#include <sstream>
#include "ObjectManager.h"
#include "ArgConverter.h"
#include "NumericCasts.h"
#include "NativeScriptException.h"
#include "Runtime.h"
#include "MetadataNode.h"
#include "JsArgConverter.h"

using namespace std;
using namespace tns;

JsArgToArrayConverter::JsArgToArrayConverter(JsRuntime &rt, const JsValue &arg,
                                             bool isImplementationObject, int classReturnType,
                                             ObjectManager* objectManager)
        : m_arr(nullptr), m_argsAsObject(nullptr), m_argsLen(0), m_isValid(false), m_error(Error()),
          m_return_type(classReturnType) {
    m_objectManager = objectManager;
    if (!isImplementationObject) {
        m_argsLen = 1;
        m_argsAsObject = (m_argsLen <= INLINE_CAPACITY) ? m_inlineArgs : new jobject[m_argsLen];
        memset(m_argsAsObject, 0, m_argsLen * sizeof(jobject));

        m_isValid = ConvertArg(rt, arg, 0);
    }
}

JsArgToArrayConverter::JsArgToArrayConverter(JsRuntime &rt, size_t argc, const JsValue *argv,
                                             bool hasImplementationObject)
        : m_arr(nullptr), m_argsAsObject(nullptr), m_argsLen(0), m_isValid(false), m_error(Error()),
          m_return_type(static_cast<int>(Type::Null)) {
    m_argsLen = !hasImplementationObject ? argc : argc - 2;

    bool success = true;

    if (m_argsLen > 0) {
        m_argsAsObject = (m_argsLen <= INLINE_CAPACITY) ? m_inlineArgs : new jobject[m_argsLen];
        memset(m_argsAsObject, 0, m_argsLen * sizeof(jobject));

        for (int i = 0; i < m_argsLen; i++) {
            success = ConvertArg(rt, argv[i], i);

            if (!success) {
                break;
            }
        }
    }

    m_isValid = success;
}

bool JsArgToArrayConverter::ConvertArg(JsRuntime &rt, const JsValue &arg, int index) {
    bool success = false;
    // Error text is built only on failure (avoids a per-call stringstream).
    std::string errMsg;

    // Seed a default diagnostic: the early `return false` paths below do not
    // reach the error-population tail, and the caller loop stops at the first
    // failing argument, so GetError() always carries a non-empty, indexed
    // message even on those paths.
    m_error.index = index;
    m_error.msg = "Cannot marshal JavaScript argument at index " +
                  std::to_string(index) + " to Java type.";

    JEnv jEnv;

    Type returnType = JType::getClassType(m_return_type);

    if (arg.isUndefined() || arg.isNull()) {
        SetConvertedObject(jEnv, index, nullptr);
        success = true;
    } else if (arg.isNumber()) {
        double d = js_util::get_number(arg);
        int64_t i = (int64_t) d;

        bool isWholeNumber = d == i;

        if (isWholeNumber) {
            jobject obj;

            if ((INT_MIN <= i) && (i <= INT_MAX) &&
                (returnType == Type::Int || returnType == Type::Null)) {
                obj = JType::NewInt(jEnv, (jint) i);
            } else {
                obj = JType::NewLong(jEnv, (jlong) d);
            }

            SetConvertedObject(jEnv, index, obj);
            success = true;
        } else {
            jobject obj;

            if ((FLT_MIN <= d) && (d <= FLT_MAX) &&
                (returnType == Type::Float || returnType == Type::Null)) {
                obj = JType::NewFloat(jEnv, (jfloat) d);
            } else {
                obj = JType::NewDouble(jEnv, (jdouble) d);
            }

            SetConvertedObject(jEnv, index, obj);
            success = true;
        }
    } else if (arg.isBool()) {
        auto javaObject = JType::NewBoolean(jEnv, js_util::get_bool(arg));
        SetConvertedObject(jEnv, index, javaObject);
        success = true;
    } else if (arg.isString()) {
        auto stringObject = ArgConverter::ConvertToJavaString(rt, arg);
        SetConvertedObject(jEnv, index, stringObject);
        success = true;
    } else if (arg.isObject()) {
        const JsValue &jsObj = arg;

        auto objectManager = m_objectManager != nullptr
                             ? m_objectManager
                             : Runtime::GetRuntime(rt)->GetObjectManager();

        bool isHostObject = objectManager->IsHostObject(jsObj);

        CastType castType = isHostObject ? CastType::None
                                         : NumericCasts::GetCastType(rt, jsObj);

        JsValue castValue;
        jchar charValue;
        jbyte byteValue;
        jshort shortValue;
        jlong longValue;
        jfloat floatValue;
        jdouble doubleValue;
        jobject javaObject;
        JniLocalRef obj;

        switch (castType) {
            case CastType::Char:
                castValue = NumericCasts::GetCastValue(rt, jsObj);
                charValue = '\0';
                if (!castValue.isUndefined()) {
                    string str = ArgConverter::ConvertToString(rt, castValue);
                    charValue = (jchar) str[0];
                }
                javaObject = JType::NewChar(jEnv, charValue);
                SetConvertedObject(jEnv, index, javaObject);
                success = true;
                break;

            case CastType::Byte:
                castValue = NumericCasts::GetCastValue(rt, jsObj);
                byteValue = 0;

                if (!castValue.isUndefined()) {
                    if (castValue.isString()) {
                        string value = ArgConverter::ConvertToString(rt, castValue);
                        int byteArg = atoi(value.c_str());
                        byteValue = (jbyte) byteArg;
                    } else {
                        byteValue = (jbyte) js_util::get_int32(castValue);
                    }
                }

                javaObject = JType::NewByte(jEnv, byteValue);
                SetConvertedObject(jEnv, index, javaObject);
                success = true;
                break;

            case CastType::Short:
                castValue = NumericCasts::GetCastValue(rt, jsObj);
                shortValue = 0;
                if (!castValue.isUndefined()) {
                    if (castValue.isString()) {
                        string value = ArgConverter::ConvertToString(rt, castValue);
                        int shortArg = atoi(value.c_str());
                        shortValue = (jshort) shortArg;
                    } else {
                        shortValue = (jshort) js_util::get_int32(castValue);
                    }
                }

                javaObject = JType::NewShort(jEnv, shortValue);

                SetConvertedObject(jEnv, index, javaObject);
                success = true;
                break;

            case CastType::Long:
                castValue = NumericCasts::GetCastValue(rt, jsObj);
                longValue = 0;
                if (!castValue.isUndefined()) {
                    if (castValue.isString()) {
                        auto strValue = ArgConverter::ConvertToString(rt, castValue);
                        longValue = atoll(strValue.c_str());
                    } else {
                        longValue = (jlong) js_util::get_number(castValue);
                    }
                }
                javaObject = JType::NewLong(jEnv, longValue);
                SetConvertedObject(jEnv, index, javaObject);
                success = true;
                break;

            case CastType::Float:
                castValue = NumericCasts::GetCastValue(rt, jsObj);
                floatValue = 0;
                if (!castValue.isUndefined()) {
                    floatValue = (jfloat) js_util::get_number(castValue);
                }
                javaObject = JType::NewFloat(jEnv, floatValue);
                SetConvertedObject(jEnv, index, javaObject);
                success = true;
                break;

            case CastType::Double:
                castValue = NumericCasts::GetCastValue(rt, jsObj);
                doubleValue = 0;
                if (!castValue.isUndefined()) {
                    doubleValue = (jdouble) js_util::get_number(castValue);
                }
                javaObject = JType::NewDouble(jEnv, doubleValue);
                SetConvertedObject(jEnv, index, javaObject);
                success = true;
                break;

            case CastType::None: {
                obj = objectManager->GetJavaObjectByJsObject(jsObj);

                if (obj.IsNull()) {
                    bool isArrayBuffer = jsObj.asObjectBorrowed(rt).isArrayBuffer(rt);
                    bool isTypedArray = false;
                    bool isDataView = false;

                    if (!isArrayBuffer) {
                        isTypedArray = js_util::is_typedarray(rt, jsObj);
                        if (!isTypedArray) {
                            isDataView = js_util::is_dataview(rt, jsObj);
                        }
                    }

                    if (isArrayBuffer || isDataView || isTypedArray) {
                        obj = JsArgConverter::GetByteBuffer(rt, jsObj, isArrayBuffer, isTypedArray,
                                                            isDataView);
                    }
                }

                if (!isHostObject) {
                    MetadataNode *node = MetadataNode::GetNullNode(rt, jsObj);
                    if (node != nullptr) {
                        auto type = node->GetName();
                        auto nullObjName = "com/tns/NullObject";
                        auto nullObjCtorSig = "(Ljava/lang/Class;)V";

                        jclass nullClazz = jEnv.FindClass(nullObjName);
                        jmethodID ctor = jEnv.GetMethodID(nullClazz, "<init>", nullObjCtorSig);
                        jclass clazzToNull = jEnv.FindClass(type);
                        jobject nullObjType = jEnv.NewObject(nullClazz, ctor, clazzToNull);

                        if (nullObjType != nullptr) {
                            SetConvertedObject(jEnv, index, nullObjType, false);
                        } else {
                            SetConvertedObject(jEnv, index, nullptr);
                        }

                        return true;
                    }
                }

                success = !obj.IsNull();
                if (success) {
                    SetConvertedObject(jEnv, index, obj.Move(), obj.IsGlobal());
                } else {
                    if (js_util::is_number_object(rt, arg)) {
                        JsValue numValue = js_util::valueOf(rt, arg);
                        if (js_util::is_float(rt, numValue)) {
                            javaObject = JType::NewFloat(jEnv,
                                                         (jfloat) js_util::get_number(numValue));
                        } else {
                            javaObject = JType::NewInt(jEnv, (jint) js_util::get_int32(numValue));
                        }
                        SetConvertedObject(jEnv, index, javaObject);
                        success = true;
                        break;
                    } else if (js_util::is_string_object(rt, arg)) {
                        JsValue stringValue = js_util::valueOf(rt, arg);
                        javaObject = ArgConverter::ConvertToJavaString(rt, stringValue);
                        SetConvertedObject(jEnv, index, javaObject);
                        success = true;
                        break;
                    } else if (js_util::is_boolean_object(rt, arg)) {
                        JsValue boolValue = js_util::valueOf(rt, arg);
                        javaObject = JType::NewBoolean(jEnv, js_util::get_bool(boolValue));
                        SetConvertedObject(jEnv, index, javaObject);
                        success = true;
                        break;
                    }

                    if (!success) {
                        stringstream s;
                        s << "Cannot marshal JavaScript argument "
                          << js_util::coerce_to_string(rt, jsObj) << " at index " << index
                          << " to Java type.";
                        errMsg = s.str();
                    }
                }
                break;
            }

            default:
                throw NativeScriptException("Unsupported cast type");
        }
    } else {
        errMsg = "Cannot marshal JavaScript argument at index " + std::to_string(index) +
                 " to Java type.";
        success = false;
    }

    if (!success) {
        m_error.index = index;
        // Keep the seeded default when no specific message was built.
        if (!errMsg.empty()) {
            m_error.msg = std::move(errMsg);
        }
    }

    return success;
}

jobject JsArgToArrayConverter::GetConvertedArg() {
    return (m_argsLen > 0) ? m_argsAsObject[0] : nullptr;
}

void JsArgToArrayConverter::SetConvertedObject(JEnv &env, int index, jobject obj, bool isGlobal) {
    m_argsAsObject[index] = obj;
    if ((obj != nullptr) && !isGlobal) {
        m_storedIndexes.push_back(index);
    }
}

int JsArgToArrayConverter::Length() const {
    return m_argsLen;
}

bool JsArgToArrayConverter::IsValid() const {
    return m_isValid;
}

JsArgToArrayConverter::Error JsArgToArrayConverter::GetError() const {
    return m_error;
}

jobjectArray JsArgToArrayConverter::ToJavaArray() {
    if ((m_arr == nullptr) && (m_argsLen > 0)) {
        if (m_argsLen >= JsArgToArrayConverter::MAX_JAVA_PARAMS_COUNT) {
            stringstream ss;
            ss << "You are trying to override more than the MAX_JAVA_PARAMS_COUNT: "
               << JsArgToArrayConverter::MAX_JAVA_PARAMS_COUNT;
            throw NativeScriptException(ss.str());
        }

        JEnv jEnv;

        if (JsArgToArrayConverter::JAVA_LANG_OBJECT_CLASS == nullptr) {
            JsArgToArrayConverter::JAVA_LANG_OBJECT_CLASS = jEnv.FindClass("java/lang/Object");
        }

        JniLocalRef tmpArr(
                jEnv.NewObjectArray(m_argsLen, JsArgToArrayConverter::JAVA_LANG_OBJECT_CLASS,
                                    nullptr));
        m_arr = (jobjectArray) jEnv.NewGlobalRef(tmpArr);

        for (int i = 0; i < m_argsLen; i++) {
            jEnv.SetObjectArrayElement(m_arr, i, m_argsAsObject[i]);
        }
    }

    return m_arr;
}

JsArgToArrayConverter::~JsArgToArrayConverter() {
    if (m_argsLen > 0) {
        JEnv env;

        env.DeleteGlobalRef(m_arr);

        int length = m_storedIndexes.size();
        for (int i = 0; i < length; i++) {
            int index = m_storedIndexes[i];
            env.DeleteLocalRef(m_argsAsObject[index]);
        }

        if (m_argsAsObject != m_inlineArgs) {
            delete[] m_argsAsObject;
        }
    }
}

jclass JsArgToArrayConverter::JAVA_LANG_OBJECT_CLASS = nullptr;
