#ifndef OBJECTMANAGER_H_
#define OBJECTMANAGER_H_

#include "Engine.h"
#include "JEnv.h"
#include "JniLocalRef.h"
#include "JniLocalRef.h"
#include "DirectBuffer.h"
#include "LRUCache.h"
#include <map>
#include <mutex>
#include <set>
#include <stack>
#include <vector>
#include <string>
#include "Constants.h"

class MetadataNode;

namespace tns {
    class ObjectManager {
    public:
        ObjectManager(jobject javaRuntimeObject);

        void OnDisposeRuntime();

        void Init(JsRuntime &rt);

        JniLocalRef GetJavaObjectByJsObject(const JsValue &object, int *objectId = nullptr,
                                            bool *isSuper = nullptr);


        JniLocalRef GetJavaObjectByJsObjectFast(const JsValue &object);

        void UpdateCache(int objectID, jobject obj);

        jclass GetJavaClass(const JsValue &value);

        void SetJavaClass(const JsValue &instance, jclass clazz);

        int GetOrCreateObjectId(jobject object);

        JsValue GetJsObjectByJavaObject(int javaObjectID);

        JsValue CreateJSWrapper(jint javaObjectID, const std::string &typeName);

        JsValue CreateJSWrapper(jint javaObjectID, const std::string &typeName, jobject instance);

        JsValue GetOrCreateProxy(jint javaObjectID, const JsValue &instance);

        JsValue GetOrCreateProxyWeak(jint javaObjectID, const JsValue &instance);

        void Link(const JsValue &object, uint32_t javaObjectID, jclass clazz,
                  MetadataNode *node = nullptr);

        // Returns the class metadata stored on the per-instance JSInstanceInfo
        // (host proxy's, or the raw instance's native state). Used by
        // MetadataNode::GetInstanceMetadata.
        MetadataNode *GetInstanceNode(const JsValue &object);

        bool CloneLink(const JsValue &src, const JsValue &dest);

        bool IsRuntimeJsObject(const JsValue &object);

        static std::string GetClassName(jobject javaObject);

        static std::string GetClassName(jclass clazz);

        int GenerateNewObjectID();

        JsValue GetEmptyObject();

        inline static void MarkObject(JsRuntime &rt, const JsValue &object) {
            if (!object.isObject()) return;
            object.asObjectBorrowed(rt).setProperty(rt, PRIVATE_IS_NAPI, true);
        }

        inline static void MarkSuperCall(JsRuntime &rt, const JsValue &object) {
            if (!object.isObject()) return;
            object.asObjectBorrowed(rt).setProperty(rt, PRIVATE_CALLSUPER, true);
        }

        void OnGarbageCollected(JNIEnv *jEnv, jintArray object_ids);

        void ReleaseNativeObject(JsRuntime &rt, const JsValue &object);

        inline static void ReleaseObjectNow(JsRuntime &rt, int javaObjectId);

        bool IsHostObject(const JsValue &object);

        // The JS<->Java identity record. In the napi tree this pointer was owned
        // twice -- once by a napi_external carrying a finalizer and once by an
        // ownership-free napi_wrap used for fast access. engine::Object's native
        // state slot is itself a fast, non-property lookup, so one shared_ptr in
        // one slot serves both roles and there is a single owner.
        //
        // It derives from engine::HostObject because that is what the native
        // state slot stores; it overrides none of the traps and is never exposed
        // to JS as an object of its own.
        struct JSInstanceInfo : public engine::HostObject {
        public:
            JSInstanceInfo(uint32_t javaObjectID, jclass claz)
                    : JavaObjectID(javaObjectID), ObjectClazz(claz) {
            }

            uint32_t JavaObjectID;
            jclass ObjectClazz;
            // Cached super-call flag (-1 = unresolved, 0 = false, 1 = true).
            int8_t isSuper = -1;
            // Per-instance class metadata; reachable via GetInstanceNode.
            MetadataNode *node = nullptr;
        };

    private:
        struct ProxyRegistry;

        // Backing host object for a wrapper. The napi tree gates this against its
        // JS-Proxy alternative with USE_HOST_OBJECT; there is no conditional here
        // because a host object is the only path, and the engine owning the
        // shared_ptr means this destructor *is* the finalizer.
        //
        // engine::HostObject exposes get/set/getPropertyNames only -- there are no
        // has/delete/ownKeys/indexed traps -- so `in`, `delete` and numeric index
        // access all arrive through get/set and are dispatched on the property
        // name here.
        class HostObjectProxy : public engine::HostObject {
        public:
            HostObjectProxy(ObjectManager *objectManager, JSInstanceInfo *instanceInfo,
                            bool isPrimary, JsRuntime &rt, const JsValue &target);

            ~HostObjectProxy() override;

            // What the destructor hands to the post-GC drain: the owned target
            // handle plus the C++ state that outlives the host object.
            struct PendingCleanup {
                JsValue *target;
                JSInstanceInfo *instanceInfo;
                bool isPrimary;
                ObjectManager *objectManager;
            };

            JsValue get(JsRuntime &rt, const JsPropNameID &name) override;

            bool set(JsRuntime &rt, const JsPropNameID &name, const JsValue &value) override;

            std::vector<JsPropNameID> getPropertyNames(JsRuntime &rt) override;

            // Java array element access. An engine that can hand over an index
            // without stringifying it reaches these directly; the get/set traps
            // above route to the same element accessors for an engine that
            // cannot (see the note there).
            JsValue getValueAtIndex(JsRuntime &rt, uint32_t index) override;

            bool setValueAtIndex(JsRuntime &rt, uint32_t index, const JsValue &value) override;

            // Cleared by ObjectManager::OnDisposeRuntime to mark this proxy
            // neutralised; the destructor then does nothing. See there for why.
            ObjectManager *objectManager;
            // Outlives the ObjectManager; see m_proxyRegistry.
            std::shared_ptr<ProxyRegistry> registry;
            JSInstanceInfo *instanceInfo;  // java object id holder
            bool isPrimary;                // owns instanceInfo + marks weak on GC
            JsRuntime *rt;
            // Heap-held so the destructor can hand the owned handle to the
            // post-GC drain instead of releasing it inside the sweep.
            std::unique_ptr<JsValue> target;
            bool isArray;
            std::string arraySignature;    // jni array signature (arrays only)
            int8_t isSuper = -1;           // cached super-call flag (-1=unresolved)
            int64_t arrayLength = -1;      // cached fixed length (arrays only; -1=unresolved)

        private:
            // The element accessors without exception translation, so the
            // get/set traps can reach them from inside their own try blocks.
            JsValue ArrayElementAt(JsRuntime &rt, uint32_t index);

            void SetArrayElementAt(JsRuntime &rt, uint32_t index, const JsValue &value);
        };

        struct ProxyRegistry {
            std::mutex mutex;
            std::set<HostObjectProxy *> proxies;
        };

        JsValue CreateHostObjectProxy(const JsValue &instance, JSInstanceInfo *instanceInfo,
                                      bool isPrimary);

        // Actual cleanup, deferred to the runtime's safe post-GC finalizer drain
        // (Runtime::PostFinalizer) so its handle-releasing work is legal.
        static void HostObjectProxyPostFinalizer(JsRuntime &rt, void *data, void *hint);

        std::shared_ptr<JSInstanceInfo> GetJSInstanceInfoShared(const JsValue &object);

        JSInstanceInfo *GetJSInstanceInfo(const JsValue &object);

        JSInstanceInfo *GetJSInstanceInfoFromRuntimeObject(const JsValue &object);

        JsValue CreateJSWrapperHelper(jint javaObjectID, const std::string &typeName, jclass clazz);

        jweak GetJavaObjectByID(uint32_t javaObjectID);

        jobject GetJavaObjectByIDImpl(uint32_t javaObjectID);

        static jweak NewWeakGlobalRefCallback(const int &javaObjectID, void *state);

        static void DeleteWeakGlobalRefCallback(const jweak &object, void *state);

        static bool ValidateWeakGlobalRefCallback(const int &javaObjectID, const jweak &object, void *state);

        jobject m_javaRuntimeObject;

        JsRuntime *m_rt;

        // The napi tree stored a weak napi_ref for proxies and a strong one for
        // instances; those map onto engine::WeakObject and an owned Value.
        robin_hood::unordered_map<int, engine::WeakObject> m_idToProxy;
        robin_hood::unordered_map<int, JsValue> m_idToObject;
        robin_hood::unordered_set<int> m_weakObjectIds;
        robin_hood::unordered_set<int> m_markedAsWeakIds;

        // Every live host-object proxy. The engine owns the proxies, so without
        // this the only thing that ever destroys them is the engine's own
        // teardown -- far too late to release the handles they hold. See
        // OnDisposeRuntime.
        //
        // Held by shared_ptr, and every proxy holds one too, because a proxy can
        // outlive this ObjectManager: a worker's runtime is deleted before its
        // VM is, so the engine destroys the remaining proxies afterwards and
        // ~HostObjectProxy would otherwise erase itself through a dangling
        // pointer. The mutex is needed for the same reason -- those destructors
        // run on the engine's own thread.
        std::shared_ptr<ProxyRegistry> m_proxyRegistry;

        LRUCache<int, jweak> m_cache;

        volatile int m_currentObjectId;

        DirectBuffer m_buff;

        DirectBuffer m_outBuff;

        static jclass JAVA_LANG_CLASS;

        static jmethodID GET_NAME_METHOD_ID;

        jmethodID GET_JAVAOBJECT_BY_ID_METHOD_ID;

        jmethodID GET_OR_CREATE_JAVA_OBJECT_ID_METHOD_ID;

        jmethodID MAKE_INSTANCE_WEAK_BATCH_METHOD_ID;

        jmethodID MAKE_INSTANCE_WEAK_METHOD_ID;

        jmethodID MAKE_INSTANCE_STRONG_METHOD_ID;

        JsFunction m_jsObjectCtor;
    };
}

#endif /* OBJECTMANAGER_H_ */
