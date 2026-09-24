#include "ArrayElementAccessor.h"
#include "JsArgToArrayConverter.h"
#include "ArgConverter.h"
#include "Util.h"
#include "NativeScriptException.h"
#include "Runtime.h"

using namespace std;
using namespace tns;

JsValue ArrayElementAccessor::GetArrayElement(JsRuntime &rt, const JsValue &array, uint32_t index,
                                              const string& arraySignature,
                                              ObjectManager* objectManager, jobject arrayObject) {
    JEnv jenv;

    if (objectManager == nullptr) {
        objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
    }

    // The caller may hand us the already-resolved Java array (single probe per
    // loop instead of per element); otherwise resolve it here.
    JniLocalRef localArr;
    jobject arr;
    if (arrayObject != nullptr) {
        arr = arrayObject;
    } else {
        localArr = objectManager->GetJavaObjectByJsObject(array);
        assertNonNullNativeArray(localArr);
        arr = localArr;
    }

    const jsize startIndex = index;
    const jsize length = 1;

    // Dispatch on the element-type char (no substr allocation, no string-compare
    // chain). Primitive element values are created inline.
    switch (arraySignature[1]) {
        case 'Z': {
            jboolean v;
            jenv.GetBooleanArrayRegion((jbooleanArray) arr, startIndex, length, &v);
            return JsValue((bool) v);
        }
        case 'B': {
            jbyte v;
            jenv.GetByteArrayRegion((jbyteArray) arr, startIndex, length, &v);
            return JsValue((int) v);
        }
        case 'C': {
            jchar v;
            jenv.GetCharArrayRegion((jcharArray) arr, startIndex, length, &v);
            // The napi tree round-trips the jchar through a jstring and then takes
            // one byte of its UTF-8 form, which truncates anything outside ASCII.
            // engine::String is UTF-8 only, so the transcode is explicit here and
            // is the same one every other jchar path in this tree uses.
            return ArgConverter::convertToJsString(rt, &v, 1);
        }
        case 'S': {
            jshort v;
            jenv.GetShortArrayRegion((jshortArray) arr, startIndex, length, &v);
            return JsValue((int) v);
        }
        case 'I': {
            jint v;
            jenv.GetIntArrayRegion((jintArray) arr, startIndex, length, &v);
            return JsValue((int) v);
        }
        case 'J': {
            jlong v;
            jenv.GetLongArrayRegion((jlongArray) arr, startIndex, length, &v);
            return JsValue((double) v);
        }
        case 'F': {
            jfloat v;
            jenv.GetFloatArrayRegion((jfloatArray) arr, startIndex, length, &v);
            return JsValue((double) v);
        }
        case 'D': {
            jdouble v;
            jenv.GetDoubleArrayRegion((jdoubleArray) arr, startIndex, length, &v);
            return JsValue((double) v);
        }
        default: {  // 'L' object or '[' nested array
            jobject result = jenv.GetObjectArrayElement((jobjectArray) arr, index);
            // Pass the element signature as a string_view into arraySignature (drop
            // the leading '[') instead of allocating a fresh substring per element.
            JsValue value = ConvertToJsValue(rt, objectManager, jenv,
                                             std::string_view(arraySignature).substr(1), &result);
            jenv.DeleteLocalRef(result);
            return value;
        }
    }
}

void ArrayElementAccessor::SetArrayElement(JsRuntime &rt, const JsValue &array, uint32_t index,
                                           const string& arraySignature, const JsValue &value,
                                           ObjectManager* objectManager, jobject arrayObject) {
    JEnv jenv;

    if (objectManager == nullptr) {
        objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
    }

    JniLocalRef localArr;
    jobject arr;
    if (arrayObject != nullptr) {
        arr = arrayObject;
    } else {
        localArr = objectManager->GetJavaObjectByJsObject(array);
        assertNonNullNativeArray(localArr);
        arr = localArr;
    }

    // Dispatch on the element-type char (no substr allocation, no string-compare
    // chain).
    switch (arraySignature[1]) {
        case 'Z': { //bool
            jboolean v = static_cast<jboolean>(js_util::get_bool(value));
            jenv.SetBooleanArrayRegion((jbooleanArray) arr, index, 1, &v);
            break;
        }
        case 'B': { //byte
            jbyte v = static_cast<jbyte>(js_util::get_int32(value));
            jenv.SetByteArrayRegion((jbyteArray) arr, index, 1, &v);
            break;
        }
        case 'C': { //char
            string str = js_util::get_string_value(rt, value);
            JniLocalRef s(jenv.NewString(reinterpret_cast<const jchar*>(str.c_str()), 1));
            jboolean isCopy = false;
            const char* singleChar = jenv.GetStringUTFChars(s, &isCopy);
            jchar v = *singleChar;
            jenv.ReleaseStringUTFChars(s, singleChar);
            jenv.SetCharArrayRegion((jcharArray) arr, index, 1, &v);
            break;
        }
        case 'S': { //short
            jshort v = static_cast<jshort>(js_util::get_int32(value));
            jenv.SetShortArrayRegion((jshortArray) arr, index, 1, &v);
            break;
        }
        case 'I': { //int
            jint v = static_cast<jint>(js_util::get_int32(value));
            jenv.SetIntArrayRegion((jintArray) arr, index, 1, &v);
            break;
        }
        case 'J': { //long
            jlong v = static_cast<jlong>(js_util::get_number(value));
            jenv.SetLongArrayRegion((jlongArray) arr, index, 1, &v);
            break;
        }
        case 'F': { //float
            jfloat v = static_cast<jfloat>(js_util::get_number(value));
            jenv.SetFloatArrayRegion((jfloatArray) arr, index, 1, &v);
            break;
        }
        case 'D': { //double
            jdouble v = static_cast<jdouble>(js_util::get_number(value));
            jenv.SetDoubleArrayRegion((jdoubleArray) arr, index, 1, &v);
            break;
        }
        default: { //string or object
            if (value.isObject() || value.isString()) {
                JsArgToArrayConverter argConverter(rt, value, false, (int) Type::Null, objectManager);
                if (argConverter.IsValid()) {
                    jobject objectElementValue = argConverter.GetConvertedArg();
                    jenv.SetObjectArrayElement((jobjectArray) arr, index, objectElementValue);
                } else {
                    JsArgToArrayConverter::Error err = argConverter.GetError();
                    throw NativeScriptException(string(err.msg));
                }
            } else {
                throw NativeScriptException(string("Cannot assign primitive value to array of objects."));
            }
            break;
        }
    }
}

JsValue ArrayElementAccessor::ConvertToJsValue(JsRuntime &rt, ObjectManager* objectManager,
                                               JEnv& jenv, std::string_view elementSignature,
                                               const void* value) {
    switch (elementSignature[0]) {
        case 'Z':
            return JsValue((bool) *(jboolean*) value);
        case 'B':
            return JsValue((int) *(jbyte*) value);
        case 'C':
            return js_util::to_js_string(rt, std::string((const char*) value, 1));
        case 'S':
            return JsValue((int) *(jshort*) value);
        case 'I':
            return JsValue((int) *(jint*) value);
        case 'J':
            return JsValue((double) *(jlong*) value);
        case 'F':
            return JsValue((double) *(jfloat*) value);
        case 'D':
            return JsValue((double) *(jdouble*) value);
        default: {
            if (nullptr != (*(jobject*) value)) {
                bool isString = elementSignature == "Ljava/lang/String;";

                if (isString) {
                    return ArgConverter::jstringToJsString(rt, *(jstring *) value);
                }

                jint javaObjectID = objectManager->GetOrCreateObjectId(*(jobject*) value);
                JsValue jsValue = objectManager->GetJsObjectByJavaObject(javaObjectID);

                if (js_util::is_null_or_undefined(jsValue)) {
                    string className;
                    if (elementSignature[0] == '[') {
                        className = Util::JniClassPathToCanonicalName(string(elementSignature));
                    } else {
                        className = objectManager->GetClassName(*(jobject*) value);
                    }

                    jsValue = objectManager->CreateJSWrapper(javaObjectID, className);
                }

                return jsValue;
            }

            return js_util::null();
        }
    }
}

void ArrayElementAccessor::assertNonNullNativeArray(tns::JniLocalRef& arrayReference) {
    if(arrayReference.IsNull()){
        throw NativeScriptException("Failed calling indexer operator on native array. The JavaScript instance no longer has available Java instance counterpart.");
    }
}
