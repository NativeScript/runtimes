#include "FieldAccessor.h"
#include "ArgConverter.h"
#include "NativeScriptException.h"
#include "Runtime.h"
#include <sstream>

using namespace std;
using namespace tns;

JsValue
FieldAccessor::GetJavaField(JsRuntime &rt, const JsValue &target, FieldCallbackData *fieldData,
                            ObjectManager *objectManager, JniLocalRef targetJavaObject) {
    JEnv jEnv;

    if (objectManager == nullptr) {
        objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
    }

    auto &fieldMetadata = fieldData->metadata;

    const auto &fieldTypeName = fieldMetadata.getSig();
    auto isStatic = fieldMetadata.isStatic;

    auto isPrimitiveType = fieldTypeName.size() == 1;
    if (fieldData->fid == nullptr) {
        auto isFieldArray = fieldTypeName[0] == '[';
        auto fieldJniSig = isPrimitiveType
                           ? fieldTypeName
                           : (isFieldArray
                              ? fieldTypeName
                              : ("L" + fieldTypeName + ";"));

        if (isStatic) {
            fieldData->clazz = jEnv.FindClass(fieldMetadata.getDeclaringType());
            fieldData->fid = jEnv.GetStaticFieldID(fieldData->clazz, fieldMetadata.getName(),
                                                   fieldJniSig);
        } else {
            fieldData->clazz = jEnv.FindClass(fieldMetadata.getDeclaringType());
            fieldData->fid = jEnv.GetFieldID(fieldData->clazz, fieldMetadata.getName(),
                                             fieldJniSig);
        }
    }

    if (!isStatic) {
        // The caller usually pre-resolves this (single probe); only fall back to
        // resolving here when it wasn't supplied.
        if (targetJavaObject.IsNull()) {
            targetJavaObject = objectManager->GetJavaObjectByJsObjectFast(target);
        }

        if (targetJavaObject.IsNull()) {
            stringstream ss;
            ss << "Cannot access property '" << fieldMetadata.getName().c_str()
               << "' because there is no corresponding Java object";
            throw NativeScriptException(ss.str());
        }

    }


    auto fieldId = fieldData->fid;
    auto clazz = fieldData->clazz;

    if (isPrimitiveType) {
        switch (fieldTypeName[0]) {
            case 'Z': { // bool
                jboolean result;
                if (isStatic) {
                    result = jEnv.GetStaticBooleanField(clazz, fieldId);
                } else {
                    result = jEnv.GetBooleanField(targetJavaObject, fieldId);
                }
                return JsValue(result == JNI_TRUE);
            }
            case 'B': { // byte
                jbyte result;
                if (isStatic) {
                    result = jEnv.GetStaticByteField(clazz, fieldId);
                } else {
                    result = jEnv.GetByteField(targetJavaObject, fieldId);
                }
                return JsValue((int) result);
            }
            case 'C': { // char
                jchar result;
                if (isStatic) {
                    result = jEnv.GetStaticCharField(clazz, fieldId);
                } else {
                    result = jEnv.GetCharField(targetJavaObject, fieldId);
                }

                // The napi tree round-trips the jchar through a jstring and takes
                // one byte of its UTF-8 form, which truncates anything outside
                // ASCII. engine::String is UTF-8 only, so the transcode is
                // explicit here and matches every other jchar path in this tree.
                return ArgConverter::convertToJsString(rt, &result, 1);
            }
            case 'S': { // short
                jshort result;
                if (isStatic) {
                    result = jEnv.GetStaticShortField(clazz, fieldId);
                } else {
                    result = jEnv.GetShortField(targetJavaObject, fieldId);
                }
                return JsValue((int) result);
            }
            case 'I': { // int
                jint result;
                if (isStatic) {
                    result = jEnv.GetStaticIntField(clazz, fieldId);
                } else {
                    result = jEnv.GetIntField(targetJavaObject, fieldId);
                }

                return JsValue((int) result);
            }
            case 'J': { // long
                jlong result;
                if (isStatic) {
                    result = jEnv.GetStaticLongField(clazz, fieldId);
                } else {
                    result = jEnv.GetLongField(targetJavaObject, fieldId);
                }

                return ArgConverter::ConvertFromJavaLong(rt, result);
            }
            case 'F': { // float
                jfloat result;
                if (isStatic) {
                    result = jEnv.GetStaticFloatField(clazz, fieldId);
                } else {
                    result = jEnv.GetFloatField(targetJavaObject, fieldId);
                }
                return JsValue((double) result);
            }
            case 'D': { // double
                jdouble result;
                if (isStatic) {
                    result = jEnv.GetStaticDoubleField(clazz, fieldId);
                } else {
                    result = jEnv.GetDoubleField(targetJavaObject, fieldId);
                }
                return JsValue((double) result);
            }
            default: {
                stringstream ss;
                ss << "(InternalError): in FieldAccessor::GetJavaField: Unknown field type: '"
                   << fieldTypeName[0] << "'";
                throw NativeScriptException(ss.str());
            }
        }
    }

    jobject result;

    if (isStatic) {
        result = jEnv.GetStaticObjectField(clazz, fieldId);
    } else {
        result = jEnv.GetObjectField(targetJavaObject, fieldId);
    }

    if (result == nullptr) {
        return js_util::null();
    }

    JsValue fieldResult;

    bool isString = fieldTypeName == "java/lang/String";
    if (isString) {
        fieldResult = ArgConverter::jstringToJsString(rt, (jstring) result);
    } else {
        int javaObjectID = objectManager->GetOrCreateObjectId(result);
        auto objectResult = objectManager->GetJsObjectByJavaObject(javaObjectID);

        if (js_util::is_null_or_undefined(objectResult)) {
            objectResult = objectManager->CreateJSWrapper(javaObjectID, fieldTypeName, result);
        }

        fieldResult = objectResult;
    }
    jEnv.DeleteLocalRef(result);

    return fieldResult;
}

void FieldAccessor::SetJavaField(JsRuntime &rt, const JsValue &target, const JsValue &value,
                                 FieldCallbackData *fieldData, ObjectManager *objectManager,
                                 JniLocalRef targetJavaObject) {
    JEnv jEnv;

    if (objectManager == nullptr) {
        objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
    }

    auto &fieldMetadata = fieldData->metadata;

    const auto &fieldTypeName = fieldMetadata.getSig();
    auto isStatic = fieldMetadata.isStatic;

    auto isPrimitiveType = fieldTypeName.size() == 1;
    auto isFieldArray = fieldTypeName[0] == '[';

    if (fieldData->fid == nullptr) {
        auto fieldJniSig = isPrimitiveType
                           ? fieldTypeName
                           : (isFieldArray
                              ? fieldTypeName
                              : ("L" + fieldTypeName + ";"));

        if (isStatic) {
            fieldData->clazz = jEnv.FindClass(fieldMetadata.getDeclaringType());
            assert(fieldData->clazz != nullptr);
            fieldData->fid = jEnv.GetStaticFieldID(fieldData->clazz, fieldMetadata.getName(),
                                                   fieldJniSig);
            assert(fieldData->fid != nullptr);
        } else {
            fieldData->clazz = jEnv.FindClass(fieldMetadata.getDeclaringType());
            assert(fieldData->clazz != nullptr);
            fieldData->fid = jEnv.GetFieldID(fieldData->clazz, fieldMetadata.getName(),
                                             fieldJniSig);
            assert(fieldData->fid != nullptr);
        }
    }

    if (!isStatic) {
        // The caller usually pre-resolves this (single probe); only fall back to
        // resolving here when it wasn't supplied.
        if (targetJavaObject.IsNull()) {
            targetJavaObject = objectManager->GetJavaObjectByJsObjectFast(target);
        }

        if (targetJavaObject.IsNull()) {
            stringstream ss;
            ss << "Cannot access property '" << fieldMetadata.getName().c_str()
               << "' because there is no corresponding Java object";
            throw NativeScriptException(ss.str());
        }
    }

    auto fieldId = fieldData->fid;
    auto clazz = fieldData->clazz;

    if (isPrimitiveType) {
        switch (fieldTypeName[0]) {
            case 'Z': { // bool
                // TODO: validate value is a boolean before calling
                bool boolValue = value.isBool() ? value.getBool() : false;
                if (isStatic) {
                    jEnv.SetStaticBooleanField(clazz, fieldId, boolValue);
                } else {
                    jEnv.SetBooleanField(targetJavaObject, fieldId,
                                         boolValue);
                }
                break;
            }
            case 'B': { // byte
                // TODO: validate value is a byte before calling
                jbyte intValue = value.isNumber() ? js_util::get_int32(value) : 0;
                if (isStatic) {
                    jEnv.SetStaticByteField(clazz, fieldId, intValue);
                } else {
                    jEnv.SetByteField(targetJavaObject, fieldId, intValue);
                }
                break;
            }
            case 'C': { // char
                std::string stringValue = js_util::get_string_value(rt, value);
                JniLocalRef strValue(jEnv.NewStringUTF(stringValue.substr(0, 1).c_str()));
                const char *chars = jEnv.GetStringUTFChars(strValue, 0);

                if (isStatic) {
                    jEnv.SetStaticCharField(clazz, fieldId, chars[0]);
                } else {
                    jEnv.SetCharField(targetJavaObject, fieldId, chars[0]);
                }
                jEnv.ReleaseStringUTFChars(strValue, chars);
                break;
            }
            case 'S': { // short
                // TODO: validate value is a short before calling
                short shortValue = value.isNumber() ? js_util::get_int32(value) : 0;
                if (isStatic) {
                    jEnv.SetStaticShortField(clazz, fieldId, shortValue);
                } else {
                    jEnv.SetShortField(targetJavaObject, fieldId, shortValue);
                }
                break;
            }
            case 'I': { // int
                // TODO: validate value is a int before calling
                int intValue = value.isNumber() ? js_util::get_int32(value) : 0;
                if (isStatic) {
                    jEnv.SetStaticIntField(clazz, fieldId, intValue);
                } else {
                    jEnv.SetIntField(targetJavaObject, fieldId, intValue);
                }
                break;
            }
            case 'J': { // long
                jlong longValue = static_cast<jlong>(ArgConverter::ConvertToJavaLong(rt, value));
                if (isStatic) {
                    jEnv.SetStaticLongField(clazz, fieldId, longValue);
                } else {
                    jEnv.SetLongField(targetJavaObject, fieldId, longValue);
                }
                break;
            }
            case 'F': { // float
                float floatValue = value.isNumber() ? js_util::get_number(value) : 0.0;
                if (isStatic) {
                    jEnv.SetStaticFloatField(clazz, fieldId,
                                             static_cast<jfloat>(floatValue));
                } else {
                    jEnv.SetFloatField(targetJavaObject, fieldId,
                                       static_cast<jfloat>(floatValue));
                }
                break;
            }
            case 'D': { // double
                double doubleValue = value.isNumber() ? js_util::get_number(value) : 0.0;
                if (isStatic) {
                    jEnv.SetStaticDoubleField(clazz, fieldId, doubleValue);
                } else {
                    jEnv.SetDoubleField(targetJavaObject, fieldId, doubleValue);
                }
                break;
            }
            default: {
                stringstream ss;
                ss << "(InternalError): in FieldAccessor::SetJavaField: Unknown field type: '"
                   << fieldTypeName[0] << "'";
                throw NativeScriptException(ss.str());
            }
        }
    } else {
        bool isString = fieldTypeName == "java/lang/String";
        JniLocalRef result;

        if (!js_util::is_null_or_undefined(value)) {
            if (isString) {
                // TODO: validate valie is a string;
                result = ArgConverter::ConvertToJavaString(rt, value);
            } else {
                result = objectManager->GetJavaObjectByJsObject(value);
            }
        }

        if (isStatic) {
            jEnv.SetStaticObjectField(clazz, fieldId, result);
        } else {
            jEnv.SetObjectField(targetJavaObject, fieldId, result);
        }
    }
}
