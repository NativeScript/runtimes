#include "ObjectManager.h"
#include "NativeScriptAssert.h"
#include "MetadataNode.h"
#include "ArgConverter.h"
#include "Util.h"
#include "NativeScriptException.h"
#include "Runtime.h"
#include "CallbackHandlers.h"
#include <algorithm>
#include <sstream>

using namespace std;
using namespace tns;

// GetClassName is static so exception handling can resolve a Java class name
// without retrieving the runtime/ObjectManager (which may be unavailable
// mid-exception). These JNI ids are process-global once looked up.
jclass ObjectManager::JAVA_LANG_CLASS = nullptr;
jmethodID ObjectManager::GET_NAME_METHOD_ID = nullptr;

ObjectManager::ObjectManager(jobject javaRuntimeObject) :
        m_javaRuntimeObject(javaRuntimeObject),
        m_cache(NewWeakGlobalRefCallback, DeleteWeakGlobalRefCallback, ValidateWeakGlobalRefCallback, 1000, this),
        m_currentObjectId(0),
        m_rt(nullptr),
        m_proxyRegistry(std::make_shared<ProxyRegistry>()) {

    JEnv env;
    auto runtimeClass = env.FindClass("com/tns/Runtime");
    assert(runtimeClass != nullptr);

    GET_JAVAOBJECT_BY_ID_METHOD_ID = env.GetMethodID(runtimeClass, "getJavaObjectByID",
                                                     "(I)Ljava/lang/Object;");
    assert(GET_JAVAOBJECT_BY_ID_METHOD_ID != nullptr);

    GET_OR_CREATE_JAVA_OBJECT_ID_METHOD_ID = env.GetMethodID(runtimeClass,
                                                             "getOrCreateJavaObjectID",
                                                             "(Ljava/lang/Object;)I");
    assert(GET_OR_CREATE_JAVA_OBJECT_ID_METHOD_ID != nullptr);

    MAKE_INSTANCE_WEAK_METHOD_ID = env.GetMethodID(runtimeClass, "makeInstanceWeak",
                                                   "(I)V");
    assert(MAKE_INSTANCE_WEAK_METHOD_ID != nullptr);

    MAKE_INSTANCE_WEAK_BATCH_METHOD_ID = env.GetMethodID(runtimeClass, "makeInstanceWeak",
                                                         "(Ljava/nio/ByteBuffer;IZ)V");
    assert(MAKE_INSTANCE_WEAK_BATCH_METHOD_ID != nullptr);

    MAKE_INSTANCE_STRONG_METHOD_ID = env.GetMethodID(runtimeClass, "makeInstanceStrong",
                                                     "(I)V");
    assert(MAKE_INSTANCE_STRONG_METHOD_ID != nullptr);

    JAVA_LANG_CLASS = env.FindClass("java/lang/Class");
    assert(JAVA_LANG_CLASS != nullptr);

    GET_NAME_METHOD_ID = env.GetMethodID(JAVA_LANG_CLASS, "getName", "()Ljava/lang/String;");
    assert(GET_NAME_METHOD_ID != nullptr);
}


void ObjectManager::Init(JsRuntime &rt) {
    m_rt = &rt;

    JsFunction jsObjectCtor = JsFunction::createFromHostConstructor(
            rt, JsPropNameID::forAscii(rt, "JSObject"), 0,
            [](JsRuntime &rt, const JsValue &jsThis, const JsValue *argv, size_t argc) {
                return jsThis;
            });

    JsValue prototype = js_util::get_prototype(rt, JsValue(rt, jsObjectCtor));
    if (prototype.isObject()) {
        prototype.asObject(rt).setProperty(rt, PRIVATE_IS_NAPI, true);
    }

    m_jsObjectCtor = jsObjectCtor;
}


void ObjectManager::OnDisposeRuntime() {
    JEnv jEnv;
    // Every entry owns an engine handle; clearing the maps releases them while
    // the runtime is still alive, which QuickJS requires (JS_FreeRuntime asserts
    // an empty gc object list).
    m_idToProxy.clear();
    m_idToObject.clear();
    m_jsObjectCtor = JsFunction();

    // The host-object proxies are owned by the *engine*, and each holds an owned
    // handle to the instance it wraps. Nothing above reaches them: they are only
    // destroyed when the engine tears its heap down, at which point
    // ~HostObjectProxy cannot legally release anything (a JS_FreeValue inside
    // QuickJS' sweep corrupts the collector), so it leaks the handle instead --
    // and a leaked handle is exactly what JS_FreeRuntime's
    // list_empty(&rt->gc_obj_list) assertion catches. V8 and JSC only leak.
    //
    // So release those handles here, while the runtime is still healthy, and
    // clear objectManager to tell the destructor there is nothing left to do.
    {
        std::lock_guard<std::mutex> lock(m_proxyRegistry->mutex);
        for (auto *proxy: m_proxyRegistry->proxies) {
            proxy->target.reset();
            proxy->objectManager = nullptr;
        }
        m_proxyRegistry->proxies.clear();
    }
}

JsValue ObjectManager::GetOrCreateProxyWeak(jint javaObjectID, const JsValue &instance) {
    // A miss is expected here (the instance may carry no native state); the
    // proxy handles a null info.
    auto info = GetJSInstanceInfoShared(instance);
    // Transient (weak) proxy: borrows the instance's existing JSInstanceInfo.
    return CreateHostObjectProxy(instance, info.get(), /*isPrimary=*/false);
}

JsValue ObjectManager::GetOrCreateProxy(jint javaObjectID, const JsValue &instance) {
    auto it = m_idToProxy.find(javaObjectID);
    if (it != m_idToProxy.end() && !it->second.empty()) {
        JsValue proxy = it->second.lock(*m_rt);
        if (!js_util::is_null_or_undefined(proxy)) {
            return proxy;
        } else {
            m_idToProxy.erase(javaObjectID);
        }
    }

    DEBUG_WRITE("%s %d", "Creating a new proxy for java object with id:", javaObjectID);

    // Primary (cached) proxy: owns a fresh JSInstanceInfo and marks the java
    // instance weak when collected.
    auto info = new JSInstanceInfo(javaObjectID, nullptr);
    // Carry the class metadata from the raw instance's JSInstanceInfo (set in
    // Link) so GetInstanceMetadata resolves it from the proxy.
    auto rawInfo = GetJSInstanceInfoShared(instance);
    if (rawInfo != nullptr) {
        info->node = rawInfo->node;
    }
    JsValue proxy = CreateHostObjectProxy(instance, info, /*isPrimary=*/true);

    auto javaObjectIdFound = m_weakObjectIds.find(javaObjectID);
    if (javaObjectIdFound != m_weakObjectIds.end()) {
        m_weakObjectIds.erase(javaObjectID);
        JEnv jenv;
        jenv.CallVoidMethod(m_javaRuntimeObject,
                            MAKE_INSTANCE_STRONG_METHOD_ID,
                            javaObjectID);
        DEBUG_WRITE("Making instance strong: %d", javaObjectID);
    }

    m_idToProxy.emplace(javaObjectID, engine::WeakObject(*m_rt, proxy));

    return proxy;
}

JniLocalRef ObjectManager::GetJavaObjectByJsObject(const JsValue &object, int *objectId, bool *isSuper) {
    int32_t javaObjectId = (objectId) ? *objectId : -1;
    // Cache slot for the super-call flag on whichever per-object info we resolve;
    // resolved once from PRIVATE_CALLSUPER, then read from the cached field.
    int8_t *superSlot = nullptr;

    if (object.isObject()) {
        auto proxy = object.asObjectBorrowed(*m_rt).getHostObject<HostObjectProxy>(*m_rt);
        if (proxy != nullptr) {
            if (proxy->instanceInfo) javaObjectId = proxy->instanceInfo->JavaObjectID;
            superSlot = &proxy->isSuper;
        } else {
            JSInstanceInfo *jsInstanceInfo = GetJSInstanceInfo(object);
            if (jsInstanceInfo != nullptr) {
                javaObjectId = jsInstanceInfo->JavaObjectID;
                superSlot = &jsInstanceInfo->isSuper;
            }
        }
    }

    if (isSuper) {
        if (superSlot != nullptr) {
            if (*superSlot < 0) {
                JsValue superValue = js_util::get_property(*m_rt, object, PRIVATE_CALLSUPER);
                *superSlot = js_util::get_bool(superValue) ? 1 : 0;
            }
            *isSuper = (*superSlot == 1);
        } else {
            *isSuper = false;
        }
    }

    if (objectId) {
        *objectId = javaObjectId;
    }

    if (javaObjectId != -1) {
        try {
            return {GetJavaObjectByID(javaObjectId), true};
        } catch (NativeScriptException &e) {
            // Surface which object failed instead of a bare error — this usually
            // means the id belongs to a different runtime/thread.
            throw NativeScriptException("Failed to get Java object by ID. id=" +
                                        std::to_string(javaObjectId) + ". " + e.what());
        }
    }

    return {};
}

JniLocalRef ObjectManager::GetJavaObjectByJsObjectFast(const JsValue &object) {
    if (!object.isObject()) {
        return {};
    }

    JsObject borrowed = object.asObjectBorrowed(*m_rt);

    auto proxy = borrowed.getHostObject<HostObjectProxy>(*m_rt);
    if (proxy != nullptr) {
        if (proxy->instanceInfo) {
            return {GetJavaObjectByID(proxy->instanceInfo->JavaObjectID), true};
        }
    }

    auto info = borrowed.getNativeState<JSInstanceInfo>(*m_rt);
    if (info != nullptr) {
        return {GetJavaObjectByID(info->JavaObjectID), true};
    }

    return GetJavaObjectByJsObject(object);
}

std::shared_ptr<ObjectManager::JSInstanceInfo>
ObjectManager::GetJSInstanceInfoShared(const JsValue &object) {
    if (!object.isObject()) return nullptr;
    auto info = object.asObjectBorrowed(*m_rt).getNativeState<JSInstanceInfo>(*m_rt);
    if (info != nullptr) return info;

    // A host proxy carries no native state of its own; the instance it wraps
    // does. Which of the two an accessor receives is engine-dependent -- reading
    // `super` off an extended instance lands on the target under V8 and on the
    // proxy under QuickJS -- so resolve through the proxy rather than making
    // every caller know. One hop only: a target is never itself a proxy.
    auto proxy = object.asObjectBorrowed(*m_rt).getHostObject<HostObjectProxy>(*m_rt);
    if (proxy != nullptr && proxy->target != nullptr) {
        return proxy->target->asObjectBorrowed(*m_rt).getNativeState<JSInstanceInfo>(*m_rt);
    }
    return nullptr;
}

ObjectManager::JSInstanceInfo *ObjectManager::GetJSInstanceInfo(const JsValue &object) {
    if (!object.isObject()) return nullptr;

    auto proxy = object.asObjectBorrowed(*m_rt).getHostObject<HostObjectProxy>(*m_rt);
    if (proxy != nullptr) {
        if (proxy->instanceInfo) {
            return proxy->instanceInfo;
        }
    }

    if (!IsRuntimeJsObject(object)) return nullptr;
    return GetJSInstanceInfoFromRuntimeObject(object);
}

MetadataNode *ObjectManager::GetInstanceNode(const JsValue &object) {
    JSInstanceInfo *info = GetJSInstanceInfo(object);
    return info != nullptr ? info->node : nullptr;
}

bool ObjectManager::IsHostObject(const JsValue &object) {
    if (!object.isObject()) return false;
    return object.asObjectBorrowed(*m_rt).isHostObject<HostObjectProxy>(*m_rt);
}

// ----------------------------------------------------------------------------
//  Host object proxy: callbacks + lifecycle
//
//  The proxy forwards to the wrapped `instance` (kept in HostObjectProxy::target)
//  via these callbacks. Array-like instances route numeric-index get/set straight
//  into the native element accessor.
// ----------------------------------------------------------------------------

// Recognise a canonical array index in a host-object trap key. Every key reaches
// get()/set() as a PropNameID, so an index arrives as its canonical decimal
// string ("0", "1", ...) on every engine -- the napi tree also had to accept a
// napi_number here because V8 routed indices through a separate interceptor.
static bool TryGetArrayIndex(const std::string &name, uint32_t &outIndex) {
    size_t len = name.size();
    if (len == 0 || len > 10) return false; // a uint32 has at most 10 digits
    if (name[0] == '0') { // canonical form has no leading zeros; only "0" itself
        if (len != 1) return false;
        outIndex = 0;
        return true;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
        v = v * 10 + (uint64_t) (name[i] - '0');
    }
    if (v > 4294967294ULL) return false; // max array index is 2^32 - 2
    outIndex = (uint32_t) v;
    return true;
}

ObjectManager::HostObjectProxy::HostObjectProxy(ObjectManager *objectManager,
                                                JSInstanceInfo *instanceInfo, bool isPrimary,
                                                JsRuntime &rt, const JsValue &target)
        : objectManager(objectManager),
          instanceInfo(instanceInfo),
          isPrimary(isPrimary),
          rt(&rt),
          target(std::make_unique<JsValue>(rt, target)),
          isArray(false) {
    registry = objectManager->m_proxyRegistry;
    std::lock_guard<std::mutex> lock(registry->mutex);
    registry->proxies.insert(this);
}

ObjectManager::HostObjectProxy::~HostObjectProxy() {
    // Deregister first, and through the registry rather than the ObjectManager:
    // this destructor runs whenever the engine collects the proxy, which for a
    // worker is after its Runtime (and so its ObjectManager) has been deleted.
    {
        std::lock_guard<std::mutex> lock(registry->mutex);
        registry->proxies.erase(this);
    }

    // Neutralised by OnDisposeRuntime: the handle is already gone and the
    // runtime is on its way out, so there is nothing to defer.
    if (objectManager == nullptr) {
        return;
    }

    // Runs inside the engine's GC sweep. Releasing the owned target handle here
    // is illegal on every engine (V8's InvokeFinalizerFromGC; a reentrant
    // JS_FreeValue during a QuickJS sweep corrupts the collector), so the handle
    // and the remaining C++ cleanup are handed to the runtime's post-GC drain.
    auto *pending = new HostObjectProxy::PendingCleanup{target.release(), instanceInfo,
                                                        isPrimary, objectManager};
    Runtime::PostFinalizer(*rt, ObjectManager::HostObjectProxyPostFinalizer, pending, nullptr);
}

JsValue ObjectManager::HostObjectProxy::ArrayElementAt(JsRuntime &rt, uint32_t index) {
    jobject arr = instanceInfo
                  ? (jobject) objectManager->GetJavaObjectByID(instanceInfo->JavaObjectID)
                  : nullptr;
    const JsValue &host = receiver() != nullptr ? *receiver() : *target;
    return CallbackHandlers::GetArrayElement(rt, host, index, arraySignature, objectManager, arr);
}

void ObjectManager::HostObjectProxy::SetArrayElementAt(JsRuntime &rt, uint32_t index,
                                                       const JsValue &value) {
    jobject arr = instanceInfo
                  ? (jobject) objectManager->GetJavaObjectByID(instanceInfo->JavaObjectID)
                  : nullptr;
    const JsValue &host = receiver() != nullptr ? *receiver() : *target;
    CallbackHandlers::SetArrayElement(rt, host, index, arraySignature, value, objectManager, arr);
}

// Reached from an engine that delivers an index as an integer. The engine layer
// has already established that this proxy opted in, so no isArray re-check.
JsValue ObjectManager::HostObjectProxy::getValueAtIndex(JsRuntime &rt, uint32_t index) {
    try {
        return ArrayElementAt(rt, index);
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what();
        NativeScriptException(ss.str()).ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException("Error: unknown c++ exception in HostObjectGet").ReThrowToJs(rt);
    }
}

bool ObjectManager::HostObjectProxy::setValueAtIndex(JsRuntime &rt, uint32_t index,
                                                     const JsValue &value) {
    try {
        SetArrayElementAt(rt, index, value);
        return true;
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what();
        NativeScriptException(ss.str()).ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException("Error: unknown c++ exception in HostObjectSet").ReThrowToJs(rt);
    }
}

JsValue ObjectManager::HostObjectProxy::get(JsRuntime &rt, const JsPropNameID &name) {
    try {
        std::string key = name.utf8(rt);

        // Numeric keys on arrays: straight into the native element accessor.
        // Only an engine with no way to hand over an index as an integer gets
        // here (Hermes); the rest arrive at getValueAtIndex instead.
        uint32_t index = 0;
        if (isArray && !arraySignature.empty() && TryGetArrayIndex(key, index)) {
            return ArrayElementAt(rt, index);
        }

        // Mirrors the old "super" accessor: `proxy.super` resolves to
        // `target.super`. The napi tree installed it with napi_define_properties;
        // here there is no separate accessor slot, so it is one more name the get
        // trap answers -- and the forwarding below would answer it identically
        // anyway.
        // Everything else (incl. map/forEach/toString/Symbol.iterator/length, which
        // are native methods on the array prototype) forwards to the instance.
        return target->asObjectBorrowed(rt).getProperty(rt, key);
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what();
        NativeScriptException(ss.str()).ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException("Error: unknown c++ exception in HostObjectGet").ReThrowToJs(rt);
    }
}

bool ObjectManager::HostObjectProxy::set(JsRuntime &rt, const JsPropNameID &name,
                                         const JsValue &value) {
    try {
        std::string key = name.utf8(rt);

        uint32_t index = 0;
        if (isArray && !arraySignature.empty() && TryGetArrayIndex(key, index)) {
            SetArrayElementAt(rt, index, value);
            return true;
        }

        target->asObjectBorrowed(rt).setProperty(rt, key, value);
        return true;
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what();
        NativeScriptException(ss.str()).ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException("Error: unknown c++ exception in HostObjectSet").ReThrowToJs(rt);
    }
}

std::vector<JsPropNameID> ObjectManager::HostObjectProxy::getPropertyNames(JsRuntime &rt) {
    std::vector<JsPropNameID> names;
    try {
        JsArray keys = target->asObjectBorrowed(rt).getPropertyNames(rt);
        size_t size = keys.size(rt);
        names.reserve(size);
        for (size_t i = 0; i < size; i++) {
            JsValue key = keys.getValueAtIndexBorrowed(rt, i);
            if (key.isString()) {
                names.push_back(JsPropNameID::forAscii(rt, key.asString(rt).utf8(rt)));
            }
        }
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (std::exception &e) {
        std::stringstream ss;
        ss << "Error: c++ exception: " << e.what();
        NativeScriptException(ss.str()).ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException("Error: unknown c++ exception in HostObjectOwnKeys").ReThrowToJs(rt);
    }
    return names;
}

void ObjectManager::HostObjectProxyPostFinalizer(JsRuntime &rt, void *data, void *hint) {
    auto *pending = reinterpret_cast<HostObjectProxy::PendingCleanup *>(data);
    if (pending == nullptr) return;

    auto rtOwner = Runtime::GetRuntimeUnchecked(rt);
    // Once the runtime is tearing down, the dispose path owns every outstanding
    // handle: OnDisposeRuntime clears the id maps. Releasing the target here in
    // that window would touch a runtime that is already going away.
    bool destroying = (rtOwner == nullptr) || rtOwner->is_destroying;

    if (pending->target != nullptr) {
        if (destroying) {
            // Leak the handle rather than release it against a dying runtime;
            // the engine frees the whole heap immediately afterwards.
            (void) pending->target;
        } else {
            delete pending->target;
        }
    }

    // Primary (cached) proxies own their JSInstanceInfo and mark the java
    // instance weak on collection.
    if (pending->isPrimary && pending->instanceInfo) {
        if (!destroying) {
            auto objManager = rtOwner->GetObjectManager();
            auto javaObjectID = pending->instanceInfo->JavaObjectID;
            if (objManager->m_weakObjectIds.find(javaObjectID) ==
                objManager->m_weakObjectIds.end()) {
                objManager->m_weakObjectIds.emplace(javaObjectID);
                JEnv jEnv;
                jEnv.CallVoidMethod(objManager->m_javaRuntimeObject,
                                    objManager->MAKE_INSTANCE_WEAK_METHOD_ID,
                                    javaObjectID);
            }
        }
        delete pending->instanceInfo;
    }

    delete pending;
}

JsValue ObjectManager::CreateHostObjectProxy(const JsValue &instance,
                                             JSInstanceInfo *instanceInfo,
                                             bool isPrimary) {
    auto proxy = std::make_shared<HostObjectProxy>(this, instanceInfo, isPrimary, *m_rt, instance);

    proxy->isArray = js_util::has_property(*m_rt, instance, "__is__javaArray");
    if (proxy->isArray) {
        // Cache the jni array signature so numeric index access goes straight
        // into the native element accessor (no JS getValueAtIndex dispatch).
        // node is already on the raw instance's JSInstanceInfo (set in Link).
        MetadataNode *node = GetInstanceNode(instance);
        if (node != nullptr) {
            proxy->arraySignature = node->GetName();
        }
        // Opt this proxy into the engine's indexed path, which is what lets
        // `a[0]` skip being spelled out as the property name "0" and parsed
        // back. Only meaningful with a signature: without one there is nothing
        // to marshal through and the named path would have declined too.
        proxy->setIndexedAccess(!proxy->arraySignature.empty());
    }

    // A native instance, not an opaque host object: the proxy is given the Java
    // class prototype just below, and that is where the field accessors and
    // methods live. On V8 this selects the non-masking named interceptor, so a
    // field read resolves on the prototype (and can form a load IC) instead of
    // entering the trap, crossing into C++ and re-reading the same property off
    // the wrapped target. Other backends do not distinguish the two.
    JsObject proxyObject =
            JsObject::createNativeInstanceHostObject<HostObjectProxy>(*m_rt, proxy);

    // The engine layer does not touch the prototype chain for host objects, so
    // do it here to preserve behaviour (instanceof checks, super dispatch).
    js_util::setPrototypeOf(*m_rt, JsValue(*m_rt, proxyObject),
                            js_util::getPrototypeOf(*m_rt, instance));

    return JsValue(*m_rt, proxyObject);
}

ObjectManager::JSInstanceInfo *
ObjectManager::GetJSInstanceInfoFromRuntimeObject(const JsValue &object) {
    auto info = GetJSInstanceInfoShared(object);

    if (info == nullptr) {
        JsValue proto = js_util::get__proto__(*m_rt, object);
        //Typescript object layout has an object instance as child of the actual registered instance. checking for that
        if (!js_util::is_null_or_undefined(proto)) {
            if (IsRuntimeJsObject(proto)) {
                info = GetJSInstanceInfoShared(proto);
            }
        }
    }

    return info.get();
}

bool ObjectManager::IsRuntimeJsObject(const JsValue &object) {
    if (!object.isObject()) return false;

    return js_util::has_property(*m_rt, object, PRIVATE_IS_NAPI);
}

jweak ObjectManager::GetJavaObjectByID(uint32_t javaObjectID) {
    return m_cache(javaObjectID);
}

jobject ObjectManager::GetJavaObjectByIDImpl(uint32_t javaObjectID) {
    JEnv env;
    jobject object = env.CallObjectMethod(m_javaRuntimeObject, GET_JAVAOBJECT_BY_ID_METHOD_ID,
                                          javaObjectID);
    return object;
}

void ObjectManager::UpdateCache(int objectID, jobject obj) {
    m_cache.update(objectID, obj);
}

jclass ObjectManager::GetJavaClass(const JsValue &value) {
    JSInstanceInfo *jsInfo = GetJSInstanceInfo(value);
    jclass clazz = jsInfo->ObjectClazz;

    return clazz;
}

void ObjectManager::SetJavaClass(const JsValue &value, jclass clazz) {
    JSInstanceInfo *jsInfo = GetJSInstanceInfo(value);
    jsInfo->ObjectClazz = clazz;
}

int ObjectManager::GetOrCreateObjectId(jobject object) {
    JEnv env;
    jint javaObjectID = env.CallIntMethod(m_javaRuntimeObject,
                                          GET_OR_CREATE_JAVA_OBJECT_ID_METHOD_ID, object);
    return javaObjectID;
}

JsValue ObjectManager::GetJsObjectByJavaObject(int javaObjectID) {
    auto it = m_idToObject.find(javaObjectID);
    if (it == m_idToObject.end()) {
        return js_util::undefined();
    }

    JsValue instance = it->second;
    if (js_util::is_null_or_undefined(instance)) return js_util::undefined();
    return GetOrCreateProxy(javaObjectID, instance);
}


JsValue ObjectManager::CreateJSWrapper(jint javaObjectID, const std::string &typeName) {
    return CreateJSWrapperHelper(javaObjectID, typeName, nullptr);
}

JsValue ObjectManager::CreateJSWrapper(jint javaObjectID, const std::string &typeName,
                                       jobject instance) {
    JEnv jenv;
    JniLocalRef clazz(jenv.GetObjectClass(instance));

    return CreateJSWrapperHelper(javaObjectID, typeName, clazz);
}

JsValue ObjectManager::CreateJSWrapperHelper(jint javaObjectID, const std::string &typeName,
                                             jclass clazz) {
    auto className = (clazz != nullptr) ? GetClassName(clazz) : typeName;

    auto node = MetadataNode::GetOrCreate(className);
    JsValue proxy = js_util::undefined();
    JsValue jsWrapper = node->CreateJSWrapper(*m_rt, this);
    if (jsWrapper.isObject()) {
        // Reuse the class we already resolved via GetObjectClass on the instance
        // path instead of re-resolving it with a JNI FindClass. The class is only
        // stored on JSInstanceInfo::ObjectClazz, which nothing on this path reads,
        // so a fresh FindClass is pure overhead; only fall back to it for the
        // typeName-only overload where no instance class was available.
        jclass linkClazz = clazz;
        if (linkClazz == nullptr) {
            JEnv jenv;
            linkClazz = jenv.FindClass(className);
        }
        Link(jsWrapper, javaObjectID, linkClazz, node);
        if (node->isArray()) {
            jsWrapper.asObject(*m_rt).setProperty(*m_rt, "__is__javaArray", true);
        }
        proxy = GetOrCreateProxy(javaObjectID, jsWrapper);
    }

    return proxy;
}

void ObjectManager::Link(const JsValue &object, uint32_t javaObjectID, jclass clazz,
                         MetadataNode *node) {
    if (!IsRuntimeJsObject(object)) {
        std::string errMsg("Trying to link invalid 'this' to a Java object");
        throw NativeScriptException(errMsg);
    }

    DEBUG_WRITE("Linking js object and java instance id: %d", javaObjectID);

    auto jsInstanceInfo = std::make_shared<JSInstanceInfo>(javaObjectID, clazz);
    jsInstanceInfo->node = node;

    // One slot, one owner: the native state both carries the record and keeps it
    // alive, replacing the napi tree's external-plus-wrap pair.
    object.asObjectBorrowed(*m_rt).setNativeState<JSInstanceInfo>(*m_rt, jsInstanceInfo);

    m_idToObject.emplace(javaObjectID, JsValue(*m_rt, object));
}

bool ObjectManager::CloneLink(const JsValue &src, const JsValue &dest) {
    auto jsInfo = GetJSInstanceInfoShared(src);

    auto success = jsInfo != nullptr;

    if (success) {
        dest.asObjectBorrowed(*m_rt).setNativeState<JSInstanceInfo>(*m_rt, jsInfo);
    }

    return success;
}

string ObjectManager::GetClassName(jobject javaObject) {
    JEnv env;
    JniLocalRef objectClass(env.GetObjectClass(javaObject));

    return GetClassName((jclass) objectClass);
}

string ObjectManager::GetClassName(jclass clazz) {
    JEnv env;
    JniLocalRef javaCanonicalName(env.CallObjectMethod(clazz, GET_NAME_METHOD_ID));

    string className = ArgConverter::jstringToString(javaCanonicalName);

    std::replace(className.begin(), className.end(), '.', '/');

    return className;
}

int ObjectManager::GenerateNewObjectID() {
    const int one = 1;
    int oldValue = __sync_fetch_and_add(&m_currentObjectId, one);
    return oldValue;
}

jweak ObjectManager::NewWeakGlobalRefCallback(const int &javaObjectID, void *state) {
    auto objManager = reinterpret_cast<ObjectManager *>(state);
    JniLocalRef obj(objManager->GetJavaObjectByIDImpl(javaObjectID));
    JEnv jEnv;
    jweak weakRef = jEnv.NewWeakGlobalRef(obj);

    return weakRef;
}

void ObjectManager::DeleteWeakGlobalRefCallback(const jweak &object, void *state) {
    JEnv jEnv;
    jEnv.DeleteWeakGlobalRef(object);
}

bool ObjectManager::ValidateWeakGlobalRefCallback(const int &javaObjectID, const jweak &object,
                                                  void *state) {
    JEnv jEnv;
    // A weak ref that is now IsSameObject(NULL) points to a collected object and
    // must not be reused; report it as invalid so the cache evicts it.
    return !jEnv.isSameObject(object, NULL);
}

JsValue ObjectManager::GetEmptyObject() {
    JsValue jsWrapper = m_jsObjectCtor.callAsConstructor(
            *m_rt, static_cast<const JsValue *>(nullptr), (size_t) 0);
    if (jsWrapper.isObject()) {
        return jsWrapper;
    }

    JsObject plain(*m_rt);
    MarkObject(*m_rt, JsValue(*m_rt, plain));
    auto prototype = js_util::get_prototype(*m_rt, JsValue(*m_rt, m_jsObjectCtor));
    if (!js_util::is_null_or_undefined(prototype)) {
        js_util::setPrototypeOf(*m_rt, JsValue(*m_rt, plain), prototype);
    }

    return JsValue(*m_rt, plain);
}

void ObjectManager::ReleaseObjectNow(JsRuntime &rt, int javaObjectId) {
    auto rtOwner = Runtime::GetRuntimeUnchecked(rt);
    if (!rtOwner || rtOwner->is_destroying) return;
    ObjectManager *objMgr = rtOwner->GetObjectManager();

    auto itFound = objMgr->m_weakObjectIds.find(javaObjectId);
    if (itFound == objMgr->m_weakObjectIds.end()) {
        JEnv jEnv;
        jEnv.CallVoidMethod(objMgr->m_javaRuntimeObject, objMgr->MAKE_INSTANCE_WEAK_METHOD_ID,
                            javaObjectId);
        objMgr->m_weakObjectIds.emplace(javaObjectId);
    }

    objMgr->m_idToProxy.erase(javaObjectId);
    objMgr->m_idToObject.erase(javaObjectId);

    Runtime::GetRuntime(rt)->js_method_cache->cleanupObject(javaObjectId);
}

void ObjectManager::ReleaseNativeObject(JsRuntime &rt, const JsValue &object) {
    int32_t javaObjectId = -1;

    JSInstanceInfo *jsInstanceInfo = GetJSInstanceInfo(object);

    if (jsInstanceInfo) {
        javaObjectId = jsInstanceInfo->JavaObjectID;
    }

    if (javaObjectId == -1) {
        throw NativeScriptException("Trying to release a non native object!");
    }

    ReleaseObjectNow(rt, javaObjectId);
}

void ObjectManager::OnGarbageCollected(JNIEnv *jEnv, jintArray object_ids) {
    JEnv jenv(jEnv);
    jsize length = jenv.GetArrayLength(object_ids);
    int *cppArray = jenv.GetIntArrayElements(object_ids, nullptr);
    for (jsize i = 0; i < length; i++) {
        auto rt = Runtime::GetRuntimeUnchecked(*m_rt);
        if (rt && rt->is_destroying) return;
        int javaObjectId = cppArray[i];
        auto itFound = this->m_idToObject.find(javaObjectId);
        if (itFound != this->m_idToObject.end()) {
            this->m_idToObject.erase(javaObjectId);

            if (rt && !rt->is_destroying) {
                rt->js_method_cache->cleanupObject(javaObjectId);
            }

            DEBUG_WRITE("JS Object released for object id: %d", javaObjectId);
        }

    }
}
