//
// Created by Ammar Ahmed on 20/09/2024.
//
#include <cassert>
#include <chrono>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <unistd.h>
#include <dlfcn.h>
#include "JEnv.h"
#include "CallbackHandlers.h"
#include "Util.h"
#include "JniLocalRef.h"
#include "MetadataNode.h"
#include "MethodCache.h"
#include "ArgConverter.h"
#include "JsArgConverter.h"
#include "GlobalHelpers.h"
#include "ModuleInternal.h"
#include "WorkerWrapper.h"
#include <regex>

#ifdef USE_MIMALLOC

#include "mimalloc.h"

#endif

using namespace std;
using namespace tns;


namespace {
// Converts a NativeScriptException into a JS throw, the way every callback in
// this tree has to. The napi tree needed no equivalent: there a native error
// was reported with napi_throw and a plain return, so nothing could unwind out
// of a callback. Here a C++ throw is the mechanism, and an escapee unwinds
// through the engine's own frames.
template <typename Fn>
JsValue Guarded(JsRuntime &rt, Fn &&fn) {
    try {
        return fn();
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (JsError &) {
        throw;
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException(ss.str()).ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException(std::string("Error: c++ exception!")).ReThrowToJs(rt);
    }
}
}  // namespace

void CallbackHandlers::Init(JsRuntime &rt) {
    JEnv jEnv;

    JAVA_LANG_STRING = jEnv.FindClass("java/lang/String");
    assert(JAVA_LANG_STRING != nullptr);

    RUNTIME_CLASS = jEnv.FindClass("com/tns/Runtime");
    assert(RUNTIME_CLASS != nullptr);

    RESOLVE_CLASS_METHOD_ID = jEnv.GetMethodID(RUNTIME_CLASS, "resolveClass",
                                               "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Z)Ljava/lang/Class;");
    assert(RESOLVE_CLASS_METHOD_ID != nullptr);

    CURRENT_OBJECTID_FIELD_ID = jEnv.GetFieldID(RUNTIME_CLASS, "currentObjectId", "I");
    assert(CURRENT_OBJECTID_FIELD_ID != nullptr);

    MAKE_INSTANCE_STRONG_ID = jEnv.GetMethodID(RUNTIME_CLASS, "makeInstanceStrong",
                                               "(Ljava/lang/Object;I)V");
    assert(MAKE_INSTANCE_STRONG_ID != nullptr);

    GET_TYPE_METADATA = jEnv.GetStaticMethodID(RUNTIME_CLASS, "getTypeMetadata",
                                               "(Ljava/lang/String;I)[Ljava/lang/String;");
    assert(GET_TYPE_METADATA != nullptr);

    ENABLE_VERBOSE_LOGGING_METHOD_ID = jEnv.GetMethodID(RUNTIME_CLASS, "enableVerboseLogging",
                                                        "()V");
    assert(ENABLE_VERBOSE_LOGGING_METHOD_ID != nullptr);

    DISABLE_VERBOSE_LOGGING_METHOD_ID = jEnv.GetMethodID(RUNTIME_CLASS, "disableVerboseLogging",
                                                         "()V");
    assert(ENABLE_VERBOSE_LOGGING_METHOD_ID != nullptr);

    MetadataNode::Init(rt);

    MethodCache::Init();
}

JsValue CallbackHandlers::CallJavaMethod(JsRuntime &rt, const JsValue &caller, const string &className,
                                 const string &methodName, MetadataEntry *entry,
                                 bool isFromInterface, bool isStatic, bool isConstructorCall,
                                 const JsValue *argv, size_t argc,
                                 ObjectManager *objectManager) {

    JEnv jEnv;
    jclass clazz;
    jmethodID mid;
    string *sig = nullptr;
    string *returnType = nullptr;
    auto retType = MethodReturnType::Unknown;
    MethodCache::CacheMethodInfo mi;
    bool isSuper = false;

    if ((entry != nullptr) && entry->getIsResolved()) {
        auto &entrySignature = entry->getSig();
        isStatic = entry->isStatic;

        if (entry->memberId == nullptr) {
            clazz = jEnv.FindClass(className);

            if (clazz == nullptr) {
                MetadataNode *callerNode = MetadataNode::GetNodeFromHandle(rt, caller);
                const string callerClassName = callerNode->GetName();

                DEBUG_WRITE("Cannot resolve class: %s while calling method: %s callerClassName: %s",
                            className.c_str(), methodName.c_str(), callerClassName.c_str());
                clazz = jEnv.FindClass(callerClassName);
                if (clazz == nullptr) {
                    //todo: plamen5kov: throw exception here
                    DEBUG_WRITE("Cannot resolve caller's class name: %s", callerClassName.c_str());
                    return js_util::undefined();
                }

                if (isStatic) {
                    if (isFromInterface) {
                        auto methodAndClassPair = jEnv.GetInterfaceStaticMethodIDAndJClass(
                                className,
                                methodName,
                                entrySignature);
                        entry->memberId = methodAndClassPair.first;
                        clazz = methodAndClassPair.second;
                    } else {
                        entry->memberId = jEnv.GetStaticMethodID(clazz, methodName, entrySignature);
                    }
                } else {
                    entry->memberId = jEnv.GetMethodID(clazz, methodName, entrySignature);
                }

                if (entry->memberId == nullptr) {
                    //todo: plamen5kov: throw exception here
                    DEBUG_WRITE("Cannot resolve a method %s on caller class: %s",
                                methodName.c_str(), callerClassName.c_str());
                    return js_util::undefined();
                }
            } else {
                if (isStatic) {
                    if (isFromInterface) {
                        auto methodAndClassPair = jEnv.GetInterfaceStaticMethodIDAndJClass(
                                className,
                                methodName, entrySignature);
                        entry->memberId = methodAndClassPair.first;
                        clazz = methodAndClassPair.second;
                    } else {
                        entry->memberId = jEnv.GetStaticMethodID(clazz, methodName, entrySignature);
                    }
                } else {
                    entry->memberId = jEnv.GetMethodID(clazz, methodName, entrySignature);
                }

                if (entry->memberId == nullptr) {
                    //todo: plamen5kov: throw exception here
                    DEBUG_WRITE("Cannot resolve a method %s on class: %s", methodName.c_str(),
                                className.c_str());
                    return js_util::undefined();
                }
            }
            entry->clazz = clazz;
        }

        mid = reinterpret_cast<jmethodID>(entry->memberId);
        clazz = entry->clazz;
        sig = &entry->getSig();
        returnType = &entry->getReturnType();
        retType = entry->getRetType();
    } else {
        DEBUG_WRITE("Resolving method: %s on className %s", methodName.c_str(), className.c_str());

        clazz = jEnv.FindClass(className);
        if (clazz != nullptr) {
            mi = MethodCache::ResolveMethodSignature(rt, className, methodName, argc, argv, isStatic);
            if (mi.mid == nullptr) {
                DEBUG_WRITE("Cannot resolve class=%s, method=%s, isStatic=%d, isSuper=%d",
                            className.c_str(), methodName.c_str(), isStatic, isSuper);
                return js_util::undefined();
            }
        } else {
            MetadataNode *callerNode = MetadataNode::GetNodeFromHandle(rt, caller);
            const string callerClassName = callerNode->GetName();
            DEBUG_WRITE("Resolving method on caller class: %s.%s on className %s",
                        callerClassName.c_str(), methodName.c_str(), className.c_str());
            mi = MethodCache::ResolveMethodSignature(rt, callerClassName, methodName, argc, argv,
                                                     isStatic);
            if (mi.mid == nullptr) {
                DEBUG_WRITE(
                        "Cannot resolve class=%s, method=%s, isStatic=%d, isSuper=%d, callerClass=%s",
                        className.c_str(), methodName.c_str(), isStatic, isSuper,
                        callerClassName.c_str());
                return js_util::undefined();
            }
        }

        clazz = mi.clazz;
        mid = mi.mid;
        sig = &mi.signature;
        returnType = &mi.returnType;
        retType = mi.retType;
    }

    if (!isStatic) {
        DEBUG_WRITE("CallJavaMethod on instance %s", methodName.c_str());
    } else {
        DEBUG_WRITE("CallJavaMethod on class %s", methodName.c_str());
    }

    // The caller (MethodCallback) passes a cached ObjectManager*; only fall back
    // to the locked runtime map lookup when invoked without one. Resolved
    // before the converter so object-arg conversion can reuse it too.
    if (objectManager == nullptr) {
        objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
    }

    JsArgConverter argConverter = (entry != nullptr && entry->isExtensionFunction)
                                  ? JsArgConverter(rt, caller, argv, argc, *sig, entry, (JNIEnv *) jEnv, objectManager)
                                  : JsArgConverter(rt, argv, argc, false, *sig, entry, (JNIEnv *) jEnv, objectManager);


    if (!argConverter.IsValid()) {
        JsArgConverter::Error err = argConverter.GetError();
        throw NativeScriptException(err.msg);
    }

    JniLocalRef callerJavaObject;

    jvalue *javaArgs = argConverter.ToArgs();

    if (!isStatic) {
        int objectId = -1;

        callerJavaObject = objectManager->GetJavaObjectByJsObject(caller, &objectId, &isSuper);

        if (callerJavaObject.IsNull()) {
            stringstream ss;

            if (isConstructorCall) {
                ss << "No java object found on which to call \"" << methodName
                   << "\" method. It is possible your Javascript object is not linked with the corresponding Java class. Try passing context(this) to the constructor function.";
            } else {
                ss << "Failed calling " << methodName << " on a " << className
                   << " instance. The JavaScript instance no longer has available Java instance counterpart.";
            }
            throw NativeScriptException(ss.str());
        }
    }

    switch (retType) {
        case MethodReturnType::Void: {
            if (isStatic) {
                jEnv.CallStaticVoidMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                jEnv.CallNonvirtualVoidMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                jEnv.CallVoidMethodA(callerJavaObject, mid, javaArgs);
            }
            return js_util::undefined();
        }
        case MethodReturnType::Boolean: {
            jboolean result;
            if (isStatic) {
                result = jEnv.CallStaticBooleanMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualBooleanMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallBooleanMethodA(callerJavaObject, mid, javaArgs);
            }

            return JsValue(result != 0);
        }
        case MethodReturnType::Byte: {
            jbyte result;
            if (isStatic) {
                result = jEnv.CallStaticByteMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualByteMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallByteMethodA(callerJavaObject, mid, javaArgs);
            }

            return JsValue((int) result);
        }
        case MethodReturnType::Char: {
            jchar result;
            if (isStatic) {
                result = jEnv.CallStaticCharMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualCharMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallCharMethodA(callerJavaObject, mid, javaArgs);
            }

            // The napi tree round-trips the jchar through a jstring and takes one
            // byte of its UTF-8 form, which truncates anything outside ASCII.
            // engine::String is UTF-8 only, so the transcode is explicit here and
            // matches every other jchar path in this tree.
            return ArgConverter::convertToJsString(rt, &result, 1);
        }
        case MethodReturnType::Short: {
            jshort result;
            if (isStatic) {
                result = jEnv.CallStaticShortMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualShortMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallShortMethodA(callerJavaObject, mid, javaArgs);
            }

            return JsValue((int) result);
        }
        case MethodReturnType::Int: {
            jint result;
            if (isStatic) {
                result = jEnv.CallStaticIntMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualIntMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallIntMethodA(callerJavaObject, mid, javaArgs);
            }
            return JsValue((int) result);
        }
        case MethodReturnType::Long: {
            jlong result;
            if (isStatic) {
                result = jEnv.CallStaticLongMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualLongMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallLongMethodA(callerJavaObject, mid, javaArgs);
            }
            return ArgConverter::ConvertFromJavaLong(rt, result);
        }
        case MethodReturnType::Float: {
            jfloat result;
            if (isStatic) {
                result = jEnv.CallStaticFloatMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualFloatMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallFloatMethodA(callerJavaObject, mid, javaArgs);
            }
            return JsValue((double) result);
        }
        case MethodReturnType::Double: {
            jdouble result;
            if (isStatic) {
                result = jEnv.CallStaticDoubleMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualDoubleMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallDoubleMethodA(callerJavaObject, mid, javaArgs);
            }
            return JsValue((double) result);
        }
        case MethodReturnType::String: {
            jobject result = nullptr;

            if (isStatic) {
                result = jEnv.CallStaticObjectMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualObjectMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallObjectMethodA(callerJavaObject, mid, javaArgs);
            }

            if (result != nullptr) {
                JsValue returnValue = ArgConverter::jstringToJsString(rt,
                                                                      static_cast<jstring>(result));
                jEnv.DeleteLocalRef(result);
                return returnValue;
            }

            return js_util::null();
        }
        case MethodReturnType::Object: {
            jobject result = nullptr;

            if (isStatic) {
                result = jEnv.CallStaticObjectMethodA(clazz, mid, javaArgs);
            } else if (isSuper) {
                result = jEnv.CallNonvirtualObjectMethodA(callerJavaObject, clazz, mid, javaArgs);
            } else {
                result = jEnv.CallObjectMethodA(callerJavaObject, mid, javaArgs);
            }

            if (result == nullptr) {
                return js_util::null();
            }

            JsValue returnValue;

            // A declared array return can never be a java.lang.String, so skip
            // the per-return IsInstanceOf JNI probe on the array-return hot path.
            // Non-array Object/CharSequence returns can be polymorphic Strings,
            // so those still need the check.
            bool isArrayReturn = returnType != nullptr && !returnType->empty() &&
                                 (*returnType)[0] == '[';
            auto isString = !isArrayReturn && jEnv.IsInstanceOf(result, JAVA_LANG_STRING);

            if (isString) {
                returnValue = ArgConverter::jstringToJsString(rt, (jstring) result);
            } else {
                jint javaObjectID = objectManager->GetOrCreateObjectId(result);
                returnValue = objectManager->GetJsObjectByJavaObject(javaObjectID);

                if (js_util::is_null_or_undefined(returnValue)) {
                    returnValue = objectManager->CreateJSWrapper(javaObjectID, *returnType,
                                                                 result);
                }
            }

            jEnv.DeleteLocalRef(result);

            return returnValue;
        }
        default: {
            assert(false);
            return js_util::undefined();
        }
    }
}


bool CallbackHandlers::RegisterInstance(JsRuntime &rt, const JsValue &jsObject,
                                        const std::string &fullClassName,
                                        const ArgsWrapper &argWrapper,
                                        const JsValue &implementationObject,
                                        bool isInterface,
                                        JsValue *jsThisProxy,
                                        const std::string &baseClassName,
                                        MetadataNode *node) {
    bool success;

    DEBUG_WRITE("RegisterInstance called for '%s'", fullClassName.c_str());

    auto runtime = Runtime::GetRuntime(rt);
    auto objectManager = runtime->GetObjectManager();

    JEnv jEnv;

    jclass generatedJavaClass = ResolveClass(rt, baseClassName, fullClassName,
                                             implementationObject,
                                             isInterface);

    int javaObjectID = objectManager->GenerateNewObjectID();

    objectManager->Link(jsObject, javaObjectID, nullptr, node);

    // resolve constructor
    auto mi = MethodCache::ResolveConstructorSignature(rt, argWrapper, fullClassName,
                                                       generatedJavaClass, isInterface);

    // while the "instance" is being created, if an exception is thrown during the construction
    // this scope will guarantee the "javaObjectID" will be set to -1 and won't have an invalid value
    jobject instance;
    {
        JavaObjectIdScope objIdScope(jEnv, CURRENT_OBJECTID_FIELD_ID, runtime->GetJavaRuntime(),
                                     javaObjectID);

        if (argWrapper.type == ArgType::Interface) {
            instance = jEnv.NewObject(generatedJavaClass, mi.mid);
        } else {
            // resolve arguments before passing them on to the constructor
            JsArgConverter argConverter(rt, argWrapper.argv, argWrapper.argc, mi.signature);
            auto ctorArgs = argConverter.ToArgs();

            instance = jEnv.NewObjectA(generatedJavaClass, mi.mid, ctorArgs);
        }
    }

    // Set runtimeId field on interface and extended classes
    if (runtime->GetId() != 0 &&
        (isInterface || !js_util::is_null_or_undefined(implementationObject))) {
        jfieldID runtimeIdField;
        auto itFound = jclass_to_runtimeId_cache.find(generatedJavaClass);
        if (itFound != jclass_to_runtimeId_cache.end()) {
            runtimeIdField = itFound->second;
        } else {
            runtimeIdField = jEnv.GetFieldID(generatedJavaClass, "runtimeId", "I");
            jclass_to_runtimeId_cache.emplace(generatedJavaClass, runtimeIdField);
        }
        if (runtimeIdField != nullptr) {
            jint runtimeId = runtime->GetId(); // Assuming GetId() returns the current runtime's id
            DEBUG_WRITE("Setting runtimeId %d on instance of %s", runtimeId, fullClassName.c_str());
            jEnv.SetIntField(instance, runtimeIdField, runtimeId);
        }
    }

    jEnv.CallVoidMethod(runtime->GetJavaRuntime(), MAKE_INSTANCE_STRONG_ID, instance, javaObjectID);

    // Reuse the runtime we already resolved instead of re-querying via rt.
    runtime->AdjustAmountOfExternalAllocatedMemory();
    runtime->TryCallGC();

    JniLocalRef localInstance(instance);
    success = !localInstance.IsNull();

    if (success) {
        // ResolveClass already cached this exact (global) jclass under
        // fullClassName, so reuse it instead of a redundant FindClass lookup.
        objectManager->SetJavaClass(jsObject, generatedJavaClass);
        *jsThisProxy = objectManager->GetOrCreateProxy(javaObjectID, jsObject);
    } else {
        DEBUG_WRITE_FORCE("RegisterInstance failed with null new instance class: %s",
                          fullClassName.c_str());
    }

    return success;
}

jclass CallbackHandlers::ResolveClass(JsRuntime &rt, const string &baseClassName,
                                      const string &fullClassName,
                                      const JsValue &implementationObject, bool isInterface) {
    JEnv jEnv;
    jclass globalRefToGeneratedClass = jEnv.CheckForClassInCache(fullClassName);

    if (globalRefToGeneratedClass == nullptr) {

        // get needed arguments in order to load binding
        JniLocalRef javaBaseClassName(jEnv.NewStringUTF(baseClassName.c_str()));
        JniLocalRef javaFullClassName(jEnv.NewStringUTF(fullClassName.c_str()));

        jobjectArray methodOverrides = GetMethodOverrides(rt, jEnv, implementationObject);

        jobjectArray implementedInterfaces = GetImplementedInterfaces(rt, jEnv,
                                                                      implementationObject);

        auto runtime = Runtime::GetRuntime(rt);

        // create or load generated binding (java class)
        jclass generatedClass = (jclass) jEnv.CallObjectMethod(runtime->GetJavaRuntime(),
                                                               RESOLVE_CLASS_METHOD_ID,
                                                               (jstring) javaBaseClassName,
                                                               (jstring) javaFullClassName,
                                                               methodOverrides,
                                                               implementedInterfaces,
                                                               isInterface);

        globalRefToGeneratedClass = jEnv.InsertClassIntoCache(fullClassName, generatedClass);

        jEnv.DeleteGlobalRef(methodOverrides);
        jEnv.DeleteGlobalRef(implementedInterfaces);
    }

    return globalRefToGeneratedClass;
}

// Called by ExtendMethodCallback when extending a class
string CallbackHandlers::ResolveClassName(JsRuntime &rt, jclass &clazz) {
    auto runtime = Runtime::GetRuntime(rt);
    auto objectManager = runtime->GetObjectManager();
    auto className = objectManager->GetClassName(clazz);
    return className;
}

JsValue CallbackHandlers::GetArrayElement(JsRuntime &rt, const JsValue &array,
                                          uint32_t index, const string &arraySignature,
                                          ObjectManager *objectManager, jobject arrayObject) {
    return arrayElementAccessor.GetArrayElement(rt, array, index, arraySignature,
                                                objectManager, arrayObject);
}

void CallbackHandlers::SetArrayElement(JsRuntime &rt, const JsValue &array,
                                       uint32_t index,
                                       const string &arraySignature, const JsValue &value,
                                       ObjectManager *objectManager, jobject arrayObject) {

    arrayElementAccessor.SetArrayElement(rt, array, index, arraySignature, value,
                                         objectManager, arrayObject);
}

JsValue CallbackHandlers::GetJavaField(JsRuntime &rt, const JsValue &caller,
                                       FieldCallbackData *fieldData,
                                       ObjectManager *objectManager,
                                       JniLocalRef targetJavaObject) {
    return fieldAccessor.GetJavaField(rt, caller, fieldData, objectManager,
                                      std::move(targetJavaObject));
}

void CallbackHandlers::SetJavaField(JsRuntime &rt, const JsValue &target,
                                    const JsValue &value, FieldCallbackData *fieldData,
                                    ObjectManager *objectManager,
                                    JniLocalRef targetJavaObject) {
    fieldAccessor.SetJavaField(rt, target, value, fieldData, objectManager,
                               std::move(targetJavaObject));
}

void CallbackHandlers::AdjustAmountOfExternalAllocatedMemory(JsRuntime &rt) {
    auto runtime = Runtime::GetRuntime(rt);
     runtime->AdjustAmountOfExternalAllocatedMemory();
     runtime->TryCallGC();
}

JsValue CallbackHandlers::CreateJSWrapper(JsRuntime &rt, jint javaObjectID,
                                          const string &typeName) {
    auto runtime = Runtime::GetRuntime(rt);
    auto objectManager = runtime->GetObjectManager();

    return objectManager->CreateJSWrapper(javaObjectID, typeName);
}

jobjectArray
CallbackHandlers::GetImplementedInterfaces(JsRuntime &rt, JEnv &jEnv,
                                           const JsValue &implementationObject) {
    if (!implementationObject.isObject()) {
        return CallbackHandlers::GetJavaStringArray(jEnv, 0);
    }

    vector<jstring> interfacesToImplement;

    auto prop = implementationObject.asObjectBorrowed(rt).getProperty(rt, "interfaces");

    if (js_util::is_array(rt, prop)) {
        auto array = prop.asObjectBorrowed(rt).getArray(rt);
        size_t length = array.size(rt);

        for (size_t j = 0; j < length; j++) {
            auto element = array.getValueAtIndexBorrowed(rt, j);

            if (element.isObject()) {
                auto node = MetadataNode::GetTypeMetadataName(rt, element);

                node = Util::ReplaceAll(node, std::string("/"), std::string("."));

                jstring value = jEnv.NewStringUTF(node.c_str());
                interfacesToImplement.push_back(value);
            }
        }
    }

    int interfacesCount = interfacesToImplement.size();

    jobjectArray implementedInterfaces = CallbackHandlers::GetJavaStringArray(jEnv,
                                                                              interfacesCount);
    for (int i = 0; i < interfacesCount; i++) {
        jEnv.SetObjectArrayElement(implementedInterfaces, i, interfacesToImplement[i]);
    }

    for (int i = 0; i < interfacesCount; i++) {
        jEnv.DeleteLocalRef(interfacesToImplement[i]);
    }

    return implementedInterfaces;
}

jobjectArray
CallbackHandlers::GetMethodOverrides(JsRuntime &rt, JEnv &jEnv,
                                     const JsValue &implementationObject) {
    if (!implementationObject.isObject()) {
        return CallbackHandlers::GetJavaStringArray(jEnv, 0);
    }

    vector<jstring> methodNames;

    auto implObject = implementationObject.asObjectBorrowed(rt);

    // Object::getPropertyNames walks the prototype chain, where the napi tree
    // asked for napi_key_own_only | napi_key_all_properties. Object's own
    // getOwnPropertyNames is exactly that set (own, enumerable or not), so it is
    // used rather than the engine helper.
    auto getOwnPropertyNames =
            js_util::Builtins::of(rt).objectCtor.getPropertyAsFunction(rt, "getOwnPropertyNames");
    const JsValue nameArgs[] = {JsValue(rt, implObject)};
    auto propNamesValue = getOwnPropertyNames.call(rt, nameArgs, (size_t) 1);
    auto propNames = propNamesValue.asObjectBorrowed(rt).getArray(rt);

    size_t length = propNames.size(rt);

    for (size_t i = 0; i < length; i++) {
        auto element = propNames.getValueAtIndexBorrowed(rt, i);
        auto name = ArgConverter::ConvertToString(rt, element);

        if (name == "super") {
            continue;
        }

        auto method = implObject.getProperty(rt, element);

        bool methodFound = method.isObject() && method.asObjectBorrowed(rt).isFunction(rt);

        if (methodFound) {
            jstring value = jEnv.NewStringUTF(name.c_str());
            methodNames.push_back(value);
        }
    }

    int methodCount = methodNames.size();

    jobjectArray methodOverrides = CallbackHandlers::GetJavaStringArray(jEnv, methodCount);
    for (int i = 0; i < methodCount; i++) {
        jEnv.SetObjectArrayElement(methodOverrides, i, methodNames[i]);
    }

    for (int i = 0; i < methodCount; i++) {
        jEnv.DeleteLocalRef(methodNames[i]);
    }

    return methodOverrides;
}

JsValue CallbackHandlers::RunOnMainThreadCallback(JsRuntime &rt, const JsValue &thisVal,
                                                  const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        assert(argc == 1);
        assert(args[0].isObject() && args[0].asObjectBorrowed(rt).isFunction(rt));

        uint64_t key = ++count_;
        bool inserted;

        std::tie(std::ignore, inserted) = cache_.try_emplace(key, Runtime::GetRuntime(rt), rt,
                                                            args[0]);
        assert(inserted && "Main thread callback ID should not be duplicated");

        auto value = Callback(key);
        auto size = sizeof(Callback);
        auto wrote = write(Runtime::GetWriter(), &value, size);

        return js_util::undefined();
    });
}

int CallbackHandlers::RunOnMainThreadFdCallback(int fd, int events, void *data) {
    struct Callback value;
    auto size = sizeof(Callback);
    ssize_t nr = read(fd, &value, sizeof(value));

    auto key = value.id_;

    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return 1;
    }

    tns::Runtime *runtime = it->second.runtime_;
    JsRuntime &rt = runtime->GetJSRuntime();

    JSScope scope(runtime->GetEngineHost());

    // Copy the callback out before erasing: the entry owns the handle, and
    // erasing it releases it.
    JsValue cb(rt, it->second.callback_);

    auto global = rt.global();

    cache_.erase(it);

    try {
        cb.asObject(rt).asFunction(rt).callWithThis(rt, global);
    } catch (JsError &e) {
        DEBUG_WRITE("Error calling JavaScript callback: %s", e.what());
    }

    return 1;
}

JsValue CallbackHandlers::LogMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                            const JsValue *args, size_t argc) {
    try {
        if (argc > 0) {
            if (args[0].isString()) {
                std::string message = args[0].asString(rt).utf8(rt);
                DEBUG_WRITE("%s", message.c_str());
            }
        }
    }
    catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    }
    catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    }
    catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }

    return js_util::undefined();
}

JsValue CallbackHandlers::DrainMicrotaskCallback(JsRuntime &rt, const JsValue &thisVal,
                                                 const JsValue *args, size_t argc) {
    rt.drainMicrotasks();
    return js_util::undefined();
}

JsValue CallbackHandlers::TimeCallback(JsRuntime &rt, const JsValue &thisVal,
                                       const JsValue *args, size_t argc) {
    auto nano = std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now());
    double duration = nano.time_since_epoch().count();
    return JsValue(duration);
}

JsValue
CallbackHandlers::ReleaseNativeCounterpartCallback(JsRuntime &rt, const JsValue &thisVal,
                                                   const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        if (argc != 1) {
            throw JsError(rt, "Unexpected arguments count!");
        }

        if (!args[0].isObject()) {
            throw JsError(rt, "Argument is not an object!");
        }

        Runtime::GetRuntime(rt)->GetObjectManager()->ReleaseNativeObject(rt, args[0]);
        return js_util::undefined();
    });
}

JsValue
CallbackHandlers::DumpReferenceTablesMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                                    const JsValue *args, size_t argc) {
    DumpReferenceTablesMethod();
    return js_util::undefined();
}

void CallbackHandlers::DumpReferenceTablesMethod() {
    try {
        JEnv jEnv;
        jclass vmDbgClass = jEnv.FindClass("dalvik/system/VMDebug");
        if (vmDbgClass != nullptr) {
            jmethodID mid = jEnv.GetStaticMethodID(vmDbgClass, "dumpReferenceTables", "()V");
            if (mid != 0) {
                jEnv.CallStaticVoidMethod(vmDbgClass, mid);
            }
        }
    }
    catch (NativeScriptException &e) {
        // e.ReThrowToJs(rt);
    }
    catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        // nsEx.ReThrowToV8();
    }
    catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        // nsEx.ReThrowToV8();
    }
}

JsValue
CallbackHandlers::EnableVerboseLoggingMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                                     const JsValue *args, size_t argc) {
    try {
        tns::LogEnabled = true;
        JEnv jEnv;
        jEnv.CallVoidMethod(Runtime::GetRuntime(rt)->GetJavaRuntime(),
                            ENABLE_VERBOSE_LOGGING_METHOD_ID);
    }
    catch (NativeScriptException &e) {
        // e.ReThrowToJs(rt);
    }
    catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        // nsEx.ReThrowToV8();
    }
    catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        // nsEx.ReThrowToV8();
    }
    return js_util::undefined();
}

JsValue
CallbackHandlers::DisableVerboseLoggingMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                                      const JsValue *args, size_t argc) {
    try {
        tns::LogEnabled = false;
        JEnv jEnv;
        jEnv.CallVoidMethod(Runtime::GetRuntime(rt)->GetJavaRuntime(),
                            DISABLE_VERBOSE_LOGGING_METHOD_ID);
    }
    catch (NativeScriptException &e) {
        // e.ReThrowToJs(rt);
    }
    catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        // nsEx.ReThrowToV8();
    }
    catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        // nsEx.ReThrowToV8();
    }
    return js_util::undefined();
}

JsValue CallbackHandlers::ExitMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                             const JsValue *args, size_t argc) {
    auto msg = argc > 0 ? ArgConverter::ConvertToString(rt, args[0]) : std::string();
    DEBUG_WRITE_FATAL("FORCE EXIT: %s", msg.c_str());
    exit(-1);
    return js_util::undefined();
}

void CallbackHandlers::CreateGlobalCastFunctions(JsRuntime &rt) {
    auto global = rt.global();
    castFunctions.CreateGlobalCastFunctions(rt, global);
}

vector<string> CallbackHandlers::GetTypeMetadata(const string &name, int index) {
    JEnv env;

    string canonicalName = Util::ConvertFromJniToCanonicalName(name);

    JniLocalRef className(env.NewStringUTF(canonicalName.c_str()));
    jint idx = index;

    JniLocalRef pubApi(
            env.CallStaticObjectMethod(RUNTIME_CLASS, GET_TYPE_METADATA, (jstring) className, idx));

    jsize length = env.GetArrayLength(pubApi);

    assert(length > 0);

    vector<string> result;

    for (jsize i = 0; i < length; i++) {
        JniLocalRef s(env.GetObjectArrayElement(pubApi, i));
        const char *pc = env.GetStringUTFChars(s, nullptr);
        result.push_back(string(pc));
        env.ReleaseStringUTFChars(s, pc);
    }

    return result;
}

JsValue CallbackHandlers::CallJSMethod(JsRuntime &rt, JNIEnv *_jEnv,
                                       const JsValue &jsObject, jclass claz,
                                       const string &methodName, int javaObjectId,
                                       jobjectArray args) {
    JEnv jEnv(_jEnv);

    auto runtime = Runtime::GetRuntime(rt);
    JsValue method = runtime->js_method_cache->getCachedMethod(javaObjectId, methodName);
    if (method.isUndefined()) {
        method = jsObject.asObjectBorrowed(rt).getProperty(rt, methodName);
        if (method.isObject() && method.asObjectBorrowed(rt).isFunction(rt)) {
            runtime->js_method_cache->cacheMethod(javaObjectId, methodName, method);
        }
    }

    if (!method.isObject() || !method.asObjectBorrowed(rt).isFunction(rt)) {
        stringstream ss;
        ss << "Cannot find method '" << methodName << "' implementation";
        throw NativeScriptException(ss.str());
    }

    DEBUG_WRITE("Calling JS Method %s", methodName.c_str());

    auto fn = method.asObjectBorrowed(rt).asFunction(rt);
    auto receiver = jsObject.asObjectBorrowed(rt);

    // The napi tree bracketed the call with napi_is_exception_pending to notice
    // a throw. engine:: propagates a JS throw as a JSError, so the equivalent is
    // to let it unwind and convert it here.
    try {
        int argc = jEnv.GetArrayLength(args) / 3;
        if (argc > 0) {
            JsValue *jsArgs = nullptr;
            JsValue stack_args[8];
            std::unique_ptr<JsValue[]> heap_args;
            if (argc <= 8) {
                jsArgs = stack_args;
            } else {
                heap_args = std::make_unique<JsValue[]>(argc);
                jsArgs = heap_args.get();
            }
            ArgConverter::ConvertJavaArgsToJsArgs(rt, args, argc, jsArgs);
            return fn.callWithThis(rt, receiver, jsArgs, (size_t) argc);
        }

        return fn.callWithThis(rt, receiver);
    } catch (JsError &e) {
        if (e.value() != nullptr) {
            throw NativeScriptException(rt, *e.value(), "Error calling js method: " + methodName);
        }
        throw NativeScriptException("Error calling js method: " + methodName + ": " + e.what());
    }
}

JsValue CallbackHandlers::FindClass(JsRuntime &rt, const char *name) {
    JEnv jEnv;
    jclass javaClass = jEnv.FindClass(name);
    if (jEnv.ExceptionCheck() == JNI_FALSE) {
        auto runtime = Runtime::GetRuntime(rt);
        auto objectManager = runtime->GetObjectManager();

        jint javaObjectID = objectManager->GetOrCreateObjectId(javaClass);
        JsValue clazz = objectManager->GetJsObjectByJavaObject(javaObjectID);

        if (js_util::is_null_or_undefined(clazz)) {
            clazz = objectManager->CreateJSWrapper(javaObjectID, "Ljava/lang/Class;", javaClass);
        }

        return clazz;
    }
    return js_util::undefined();
}

int CallbackHandlers::GetArrayLength(JsRuntime &rt, const JsValue &arr) {
    auto runtime = Runtime::GetRuntime(rt);
    auto objectManager = runtime->GetObjectManager();

    JEnv jEnv;

    auto javaArr = objectManager->GetJavaObjectByJsObjectFast(arr);

    auto length = jEnv.GetArrayLength(javaArr);

    return length;
}

jobjectArray CallbackHandlers::GetJavaStringArray(JEnv &jEnv, int length) {
    if (length > CallbackHandlers::MAX_JAVA_STRING_ARRAY_LENGTH) {
        stringstream ss;
        ss << "You are trying to override more methods than the limit of "
           << CallbackHandlers::MAX_JAVA_STRING_ARRAY_LENGTH;
        throw NativeScriptException(ss.str());
    }

    JniLocalRef tmpArr(jEnv.NewObjectArray(length, JAVA_LANG_STRING, nullptr));
    return (jobjectArray) jEnv.NewGlobalRef(tmpArr);
}

CallbackHandlers::func_AChoreographer_getInstance AChoreographer_getInstance_;

CallbackHandlers::func_AChoreographer_postFrameCallback AChoreographer_postFrameCallback_;
CallbackHandlers::func_AChoreographer_postFrameCallbackDelayed AChoreographer_postFrameCallbackDelayed_;

CallbackHandlers::func_AChoreographer_postFrameCallback64 AChoreographer_postFrameCallback64_;
CallbackHandlers::func_AChoreographer_postFrameCallbackDelayed64 AChoreographer_postFrameCallbackDelayed64_;

void CallbackHandlers::FrameCallbackCacheEntry::execute(double ts, void *data) {
    if (data == nullptr) {
        return;
    }

    auto entry = static_cast<FrameCallbackCacheEntry *>(data);
    if (entry->shouldRemoveBeforeCall()) {
        frameCallbackCache_.erase(entry->id); // invalidates *entry
        return;
    }

    tns::Runtime *runtime = entry->runtime;
    JsRuntime &rt = runtime->GetJSRuntime();
    JSScope scope(runtime->GetEngineHost());

    JsValue cb(rt, entry->callback);
    auto global = rt.global();

    entry->markUnscheduled();

    const JsValue args[] = {JsValue(ts)};
    try {
        cb.asObject(rt).asFunction(rt).callWithThis(rt, global, args, (size_t) 1);
    } catch (JsError &e) {
        DEBUG_WRITE("Error in frame callback: %s", e.what());
    }

    // check if we should remove it (it should be both unscheduled and removed)
    if (entry->shouldRemoveAfterCall()) {
        frameCallbackCache_.erase(entry->id); // invalidates *entry
    }
}

void CallbackHandlers::PostCallback(JsRuntime &rt, const JsValue *args, size_t argc,
                                    CallbackHandlers::FrameCallbackCacheEntry *entry) {
    ALooper_prepare(0);
    auto instance = AChoreographer_getInstance_();
    bool hasDelay = argc > 1 && args[1].isNumber();

    if (android_get_device_api_level() >= 29) {
        if (hasDelay) {
            auto delayValue = (uint32_t) js_util::get_number(args[1]);
            AChoreographer_postFrameCallbackDelayed64_(instance, entry->frameCallback64_, entry,
                                                       delayValue);
        } else {
            AChoreographer_postFrameCallback64_(instance, entry->frameCallback64_, entry);
        }
    } else {
        if (hasDelay) {
            auto delayValue = (int64_t) js_util::get_number(args[1]);
            AChoreographer_postFrameCallbackDelayed_(instance, entry->frameCallback_, entry,
                                                     static_cast<long>(delayValue));
        } else {
            AChoreographer_postFrameCallback_(instance, entry->frameCallback_, entry);
        }
    }
}

JsValue CallbackHandlers::PostFrameCallback(JsRuntime &rt, const JsValue &thisVal,
                                            const JsValue *args, size_t argc) {
  return Guarded(rt, [&]() -> JsValue {
    if (android_get_device_api_level() >= 24) {
        InitChoreographer();

        if (argc < 1 || !args[0].isObject() || !args[0].asObjectBorrowed(rt).isFunction(rt)) {
            throw JsError(rt, "Frame callback argument is not a function");
        }

        auto func = args[0].asObjectBorrowed(rt);

        auto pId = func.getPropertyBorrowed(rt, "_postFrameCallbackId");
        if (pId.isNumber()) {
            auto id = (uint64_t) js_util::get_number(pId);
            auto cb = frameCallbackCache_.find(id);
            if (cb != frameCallbackCache_.end()) {
                bool shouldReschedule = !cb->second.isScheduled();
                cb->second.markScheduled();
                if (shouldReschedule) {
                    PostCallback(rt, args, argc, &cb->second);
                }
                return js_util::undefined();
            }
        }

        uint64_t key = ++frameCallbackCount_;

        func.setProperty(rt, "_postFrameCallbackId", JsValue((double) key));

        auto [val, inserted] = frameCallbackCache_.try_emplace(key, Runtime::GetRuntime(rt), rt,
                                                               args[0], key);
        assert(inserted && "Frame callback ID should not be duplicated");

        val->second.markScheduled();
        PostCallback(rt, args, argc, &val->second);

    }
    return js_util::undefined();
  });
}

JsValue CallbackHandlers::RemoveFrameCallback(JsRuntime &rt, const JsValue &thisVal,
                                              const JsValue *args, size_t argc) {
  return Guarded(rt, [&]() -> JsValue {
    if (android_get_device_api_level() >= 24) {
        InitChoreographer();

        if (argc < 1 || !args[0].isObject() || !args[0].asObjectBorrowed(rt).isFunction(rt)) {
            throw JsError(rt, "Frame callback argument is not a function");
        }

        auto func = args[0].asObjectBorrowed(rt);

        auto pId = func.getPropertyBorrowed(rt, "_postFrameCallbackId");

        if (pId.isNumber()) {
            auto id = (uint64_t) js_util::get_number(pId);
            auto cb = frameCallbackCache_.find(id);
            if (cb != frameCallbackCache_.end()) {
                cb->second.markRemoved();
            }
        }
    }
    return js_util::undefined();
  });
}

void CallbackHandlers::InitChoreographer() {
    if (AChoreographer_getInstance_ == nullptr) {
        void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (lib != nullptr) {
            AChoreographer_getInstance_ = reinterpret_cast<func_AChoreographer_getInstance>(
                    dlsym(lib, "AChoreographer_getInstance"));
            AChoreographer_postFrameCallback_ = reinterpret_cast<func_AChoreographer_postFrameCallback>(
                    dlsym(lib, "AChoreographer_postFrameCallback"));
            AChoreographer_postFrameCallbackDelayed_ = reinterpret_cast<func_AChoreographer_postFrameCallbackDelayed>(
                    dlsym(lib, "AChoreographer_postFrameCallbackDelayed"));

            assert(AChoreographer_getInstance_);
            assert(AChoreographer_postFrameCallback_);
            assert(AChoreographer_postFrameCallbackDelayed_);

            if (android_get_device_api_level() >= 29) {
                AChoreographer_postFrameCallback64_ = reinterpret_cast<func_AChoreographer_postFrameCallback64>(
                        dlsym(lib, "AChoreographer_postFrameCallback64"));
                AChoreographer_postFrameCallbackDelayed64_ = reinterpret_cast<func_AChoreographer_postFrameCallbackDelayed64>(
                        dlsym(lib, "AChoreographer_postFrameCallbackDelayed64"));

                assert(AChoreographer_postFrameCallback64_);
                assert(AChoreographer_postFrameCallbackDelayed64_);
            }
        }
    }
}

void CallbackHandlers::RemoveEnvEntries(JsRuntime &rt) {
    // Erasing while iterating (as the napi tree does) invalidates the iterator
    // on the very entry the loop then advances; collect first, erase after.
    tns::Runtime *runtime = Runtime::GetRuntimeUnchecked(rt);
    std::vector<uint64_t> staleCallbacks;
    for (auto &item: cache_) {
        if (item.second.runtime_ == runtime) {
            staleCallbacks.push_back(item.first);
        }
    }
    for (auto key: staleCallbacks) {
        cache_.erase(key);
    }

    std::vector<uint64_t> staleFrameCallbacks;
    for (auto &item: frameCallbackCache_) {
        if (item.second.runtime == runtime) {
            staleFrameCallbacks.push_back(item.first);
        }
    }
    for (auto key: staleFrameCallbacks) {
        frameCallbackCache_.erase(key);
    }
}

// Worker

JsValue CallbackHandlers::NewThreadCallback(JsRuntime &rt, const JsValue &thisVal,
                                            const JsValue *args, size_t argc) {
    try {
        // The napi tree rejected a plain `Worker(...)` call by checking
        // napi_get_new_target. engine:: exposes no new.target, and
        // createFromHostConstructor does NOT imply a construct call -- V8's
        // ConstructorBehavior::kAllow permits both, and the other backends
        // likewise route a plain call here. What distinguishes the two on every
        // backend is the receiver: a construct call gets a fresh object built
        // from the constructor's prototype, while a plain call gets undefined
        // (strict) or the global object (sloppy).
        if (js_util::is_null_or_undefined(thisVal) ||
            js_util::strict_equal(rt, thisVal, JsValue(rt, rt.global()))) {
            throw NativeScriptException("Worker should be called as a constructor!");
        }

        if (argc != 1) {
            throw NativeScriptException(
                    "Worker should be called with one parameter (name of file to run) or a URL/URL OBJECT to the file");
        }

        if (!args[0].isString() && !args[0].isObject()) {
            throw NativeScriptException(
                    "Worker should be called with one parameter (name of file to run) or a URL/URL OBJECT to the file");
        }

        JsValue workerFilePath;
        if (args[0].isObject()) {
            workerFilePath = args[0].asObjectBorrowed(rt).getProperty(rt, "href");
            if (js_util::is_null_or_undefined(workerFilePath)) {
                throw NativeScriptException(
                        "Worker should be called with one parameter (name of file to run) or a URL to the file");
            }
        } else {
            workerFilePath = JsValue(rt, args[0]);
        }

        auto frames = tns::BuildStacktraceFrames(rt, nullptr, 1);
        string currentExecutingScriptNameStr =
                frames.size() < 3 ? frames[0].filename : frames[2].filename;

        auto lastForwardSlash = currentExecutingScriptNameStr.find_last_of("/");
        auto currentDir = currentExecutingScriptNameStr.substr(0, lastForwardSlash + 1);
        std::string fileSchema("file://");
        if (currentDir.compare(0, fileSchema.length(), fileSchema) == 0) {
            currentDir = currentDir.substr(fileSchema.length());
        }

        std::string workerPath = ArgConverter::ConvertToString(rt, workerFilePath);

        if (workerPath.compare(0, fileSchema.length(), fileSchema) == 0) {
            workerPath = workerPath.substr(fileSchema.length());
            auto workerPathPrefix = workerPath.substr(0, 1) == "/" ? "~" : "~/";
            workerPath = workerPathPrefix  + workerPath;
        }

        DEBUG_WRITE("Worker Path: %s, Current Dir: %s", workerPath.c_str(), currentDir.c_str());



        // Will throw if path is invalid or doesn't exist
        ModuleInternal::CheckFileExists(rt, workerPath, currentDir);

        // Resolve the JNI handles used by the worker thread bootstrap while we
        // are still on the parent (main, for the first worker) thread.
        WorkerWrapper::EnsureJniCached();

        auto workerId = WorkerWrapper::NextWorkerId();
        auto jsThis = thisVal.asObjectBorrowed(rt);
        jsThis.setProperty(rt, "workerId", JsValue((int) workerId));

        DEBUG_WRITE("Called Worker constructor id=%d", workerId);

        // THREAD_PRIORITY_BACKGROUND (android.os.Process) == 10
        const int kThreadPriorityBackground = 10;
        auto wrapper = std::make_shared<WorkerWrapper>(rt, workerId, workerPath, currentDir,
                                                       kThreadPriorityBackground, thisVal);
        WorkerWrapper::Insert(workerId, wrapper);
        wrapper->Start();

        auto error = GlobalHelpers::CreateError(rt, "");
        jsThis.setProperty(rt, "__stack__", error.getProperty(rt, "stack"));

        return JsValue(rt, thisVal);
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (JsError &) {
        throw;
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c exception!"));
        nsEx.ReThrowToJs(rt);
    }
}

JsValue
CallbackHandlers::WorkerObjectPostMessageCallback(JsRuntime &rt, const JsValue &thisVal,
                                                  const JsValue *args, size_t argc) {
    try {
        if (argc != 1) {
            NativeScriptException exception(
                    "Failed to execute 'postMessage' on 'Worker': 1 argument required.");
            throw exception;
        }

        auto jsThis = thisVal.asObjectBorrowed(rt);

        auto isTerminated = jsThis.getPropertyBorrowed(rt, "isTerminated");
        if (isTerminated.isBool() && isTerminated.getBool()) {
            return js_util::undefined();
        }

        std::string msg = tns::JsonStringifyObject(rt, args[0], false);

        // get worker's ID that is associated with this Worker object
        auto jsId = jsThis.getPropertyBorrowed(rt, "workerId");
        auto id = js_util::get_int32(jsId);

        auto wrapper = WorkerWrapper::GetById(id);
        if (wrapper != nullptr) {
            wrapper->PostMessage(std::make_shared<worker::Message>(
                    worker::Message::MakeData(std::move(msg))));
        }

        DEBUG_WRITE(
                "MAIN: WorkerObjectPostMessageCallback called postMessage on Worker object(id=%d)",
                id);
    } catch (NativeScriptException &ex) {
        ex.ReThrowToJs(rt);
    } catch (JsError &) {
        throw;
    } catch (std::exception &e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }
    return js_util::undefined();
}

JsValue
CallbackHandlers::WorkerGlobalPostMessageCallback(JsRuntime &rt, const JsValue &thisVal,
                                                  const JsValue *args, size_t argc) {
    try {
        if (argc != 1) {
            throw JsError(rt,
                          "Failed to execute 'postMessage' on WorkerGlobalScope: 1 argument required.");
        }

        // The napi tree drained a pending exception here before stringifying.
        // engine:: has no pending-exception state -- a throw is already unwinding
        // as a JSError -- so there is nothing to drain.

        std::string msg = tns::JsonStringifyObject(rt, args[0], false);

        auto wrapper = WorkerWrapper::FromRuntime(rt);
        if (wrapper != nullptr) {
            wrapper->PostMessageToParent(std::make_shared<worker::Message>(
                    worker::Message::MakeData(std::move(msg))));
        }

        DEBUG_WRITE("WORKER: WorkerGlobalPostMessageCallback called.");
    } catch (NativeScriptException &ex) {
        ex.ReThrowToJs(rt);
    } catch (JsError &) {
        throw;
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }

    return js_util::undefined();
}

JsValue CallbackHandlers::WorkerObjectTerminateCallback(JsRuntime &rt, const JsValue &thisVal,
                                                        const JsValue *args, size_t argc) {
    DEBUG_WRITE("WORKER: WorkerObjectTerminateCallback called.");

    try {
        auto thiz = thisVal.asObjectBorrowed(rt);

        auto jsId = thiz.getPropertyBorrowed(rt, "workerId");
        int32_t id = js_util::get_int32(jsId);

        auto isTerminated = thiz.getPropertyBorrowed(rt, "isTerminated");
        if (isTerminated.isBool() && isTerminated.getBool()) {
            return js_util::undefined();
        }

        thiz.setProperty(rt, "isTerminated", true);

        auto wrapper = WorkerWrapper::GetById(id);
        if (wrapper != nullptr) {
            wrapper->Terminate();
        }
    } catch (NativeScriptException &ex) {
        ex.ReThrowToJs(rt);
    } catch (JsError &) {
        throw;
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }

    return js_util::undefined();
}

JsValue CallbackHandlers::WorkerGlobalCloseCallback(JsRuntime &rt, const JsValue &thisVal,
                                                    const JsValue *args, size_t argc) {
    DEBUG_WRITE("WORKER: WorkerThreadCloseCallback called.");

    try {
        auto global = rt.global();

        auto isTerminated = global.getPropertyBorrowed(rt, "isTerminating");
        if (isTerminated.isBool() && isTerminated.getBool()) {
            return js_util::undefined();
        }

        global.setProperty(rt, "isTerminating", true);

        auto callback = global.getProperty(rt, "onclose");
        if (callback.isObject() && callback.asObjectBorrowed(rt).isFunction(rt)) {
            try {
                callback.asObjectBorrowed(rt).asFunction(rt).callWithThis(rt, global);
            } catch (JsError &error) {
                CallWorkerScopeOnErrorHandle(rt, error);
            }
        }

        auto wrapper = WorkerWrapper::FromRuntime(rt);
        if (wrapper != nullptr) {
            wrapper->Close();
        }
    } catch (NativeScriptException &ex) {
        ex.ReThrowToJs(rt);
    } catch (JsError &) {
        throw;
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }

    return js_util::undefined();
}

namespace {
    // Pulls (message, stack, frames) out of a thrown value the way the napi tree
    // did: an object carries them as properties, anything else is coerced.
    struct ErrorDetails {
        std::string message;
        std::string stack;
        std::vector<JsStacktraceFrame> frames;
    };

    ErrorDetails DescribeThrown(JsRuntime &rt, const JsValue &error) {
        ErrorDetails details;
        if (error.isObject()) {
            details.frames = tns::BuildStacktraceFrames(rt, &error, 1);
            auto object = error.asObjectBorrowed(rt);
            auto message = object.getProperty(rt, "message");
            auto stack = object.getProperty(rt, "stack");
            details.message = message.isString() ? message.asString(rt).utf8(rt) : std::string();
            details.stack = stack.isString() ? stack.asString(rt).utf8(rt) : std::string();
        } else {
            details.message = js_util::coerce_to_string(rt, error);
        }
        return details;
    }
}

void CallbackHandlers::CallWorkerScopeOnErrorHandle(JsRuntime &rt, const JsError &error) {
    try {
        auto global = rt.global();

        auto callback = global.getProperty(rt, "onerror");

        // A message-only JSError has no JS payload to hand to onerror, so one is
        // built from its text; the napi tree always had a napi_value here.
        JsValue errorValue = error.value() != nullptr
                             ? JsValue(rt, *error.value())
                             : JsValue(rt, GlobalHelpers::CreateError(rt, error.what()));

        ErrorDetails details = DescribeThrown(rt, errorValue);

        if (callback.isObject() && callback.asObjectBorrowed(rt).isFunction(rt)) {
            JsValue result;
            bool threw = false;
            ErrorDetails pending;

            const JsValue args[] = {errorValue};
            try {
                result = callback.asObjectBorrowed(rt).asFunction(rt)
                        .callWithThis(rt, global, args, (size_t) 1);
            } catch (JsError &perror) {
                threw = true;
                JsValue pendingValue = perror.value() != nullptr
                                       ? JsValue(rt, *perror.value())
                                       : JsValue(rt, GlobalHelpers::CreateError(rt, perror.what()));
                pending = DescribeThrown(rt, pendingValue);
            }

            if (threw) {
                auto line = 0;
                std::string filename;
                if (!pending.frames.empty()) {
                    line = pending.frames[0].line;
                    filename = pending.frames[0].filename;
                }
                auto wrapper = WorkerWrapper::FromRuntime(rt);
                if (wrapper != nullptr) {
                    wrapper->PassUncaughtExceptionFromWorkerToParent(
                            pending.message, filename, pending.stack, line);
                }
            } else if (!js_util::is_null_or_undefined(result)) {
                if (js_util::get_bool(result)) {
                    return;
                }
            }
        }

        auto line = 0;
        std::string filename;
        if (!details.frames.empty()) {
            line = details.frames[0].line;
            filename = details.frames[0].filename;
        }
        auto wrapper = WorkerWrapper::FromRuntime(rt);
        if (wrapper != nullptr) {
            wrapper->PassUncaughtExceptionFromWorkerToParent(
                    details.message, filename, details.stack, line);
        }


    } catch (NativeScriptException &ex) {
        ex.ReThrowToJs(rt);
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what() << std::endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }
}

robin_hood::unordered_map<uint64_t, CallbackHandlers::CacheEntry> CallbackHandlers::cache_;
robin_hood::unordered_map<jclass, jfieldID> CallbackHandlers::jclass_to_runtimeId_cache;

robin_hood::unordered_map<uint64_t, CallbackHandlers::FrameCallbackCacheEntry> CallbackHandlers::frameCallbackCache_;

std::atomic_int64_t CallbackHandlers::count_ = {0};
std::atomic_uint64_t CallbackHandlers::frameCallbackCount_ = {0};

short CallbackHandlers::MAX_JAVA_STRING_ARRAY_LENGTH = 100;
jclass CallbackHandlers::RUNTIME_CLASS = nullptr;
jclass CallbackHandlers::JAVA_LANG_STRING = nullptr;
jfieldID CallbackHandlers::CURRENT_OBJECTID_FIELD_ID = nullptr;
jmethodID CallbackHandlers::RESOLVE_CLASS_METHOD_ID = nullptr;
jmethodID CallbackHandlers::MAKE_INSTANCE_STRONG_ID = nullptr;
jmethodID CallbackHandlers::GET_TYPE_METADATA = nullptr;
jmethodID CallbackHandlers::ENABLE_VERBOSE_LOGGING_METHOD_ID = nullptr;
jmethodID CallbackHandlers::DISABLE_VERBOSE_LOGGING_METHOD_ID = nullptr;

NumericCasts CallbackHandlers::castFunctions;

ArrayElementAccessor CallbackHandlers::arrayElementAccessor;

FieldAccessor CallbackHandlers::fieldAccessor;
