#ifndef RUNTIME_H
#define RUNTIME_H

#include "jni.h"
#include <string>
#include <memory>
#include "JniLocalRef.h"
#include "MessageLoopTimer.h"
#include "FinalizerQueue.h"
#include <android/looper.h>
#include "robin_hood.h"
#include "ModuleInternal.h"
#include <fcntl.h>
#include "ObjectManager.h"
#include "ArrayBufferHelper.h"
#include <thread>
#include "EngineHost.h"
#include "NativeScriptException.h"
#include <sstream>
#include "ConcurrentMap.h"

namespace tns {

    class JSMethodCache;
    class LooperTasks;

    class Runtime {
    public:

        ~Runtime();

        static Runtime *GetRuntime(int runtimeId);

        inline static Runtime *GetRuntime(engine::Runtime &rt) {
            const void *key = rt.identity();
            auto runtime = rt_to_runtime_cache.Get(key);
            if (runtime) return runtime;

            std::stringstream ss;
            ss << "Cannot find runtime for engine::Runtime: " << &rt;
            throw NativeScriptException(ss.str());
        }

        inline static Runtime *GetRuntimeUnchecked(engine::Runtime &rt) {
            const void *key = rt.identity();
            return rt_to_runtime_cache.Get(key);
        }

        // Engine-agnostic replacement for node_api_post_finalizer: schedules
        // `cb(rt, data, hint)` to run at the next runtime message-loop tick instead of
        // immediately. Call this from a GC finalizer that needs to delete
        // references / touch the JS heap (illegal during the GC sweep on every
        // engine). Falls back to running inline if the runtime is already tearing
        // down (no loop left to drain it).
        static void PostFinalizer(engine::Runtime &rt, FinalizerQueue::Finalize cb, void *data,
                                  void *hint) {
            Runtime *runtime = GetRuntimeUnchecked(rt);
            if (runtime != nullptr && runtime->m_finalizerQueue != nullptr &&
                !runtime->is_destroying) {
                runtime->m_finalizerQueue->Post(cb, data, hint);
            } else if (cb != nullptr) {
                cb(rt, data, hint);
            }
        }

        static void Init(JavaVM *vm);

        static void
        Init(JNIEnv *_env, jobject obj, int runtimeId, jstring filesPath, jstring nativeLibsDir,
             jboolean verboseLoggingEnabled, jboolean isDebuggable, jstring packageName,
             jobjectArray args, jstring callingDir, int maxLogcatObjectSize, bool forceLog);

        void Init(JNIEnv *env, jstring filesPath, jstring nativeLibsDir, bool verboseLoggingEnabled,
                  bool isDebuggable, jstring packageName, jobjectArray args, jstring callingDir,
                  int maxLogcatObjectSize, bool forceLog);

#if defined(NS_JSI_HOST_RUNTIME)
        // The guest counterpart of Init: binds to a jsi::Runtime the embedder
        // created and still owns. `filesRoot` is where the metadata .dat files
        // were staged; there is no app root to run a module from.
        static Runtime *Attach(JNIEnv *_env, jobject obj, int runtimeId,
                               ::facebook::jsi::Runtime &hostRuntime, const std::string &filesRoot,
                               bool verboseLoggingEnabled, int maxLogcatObjectSize, bool forceLog);

        void Attach(::facebook::jsi::Runtime &hostRuntime, const std::string &filesRoot,
                    bool verboseLoggingEnabled, int maxLogcatObjectSize, bool forceLog);
#endif

        jobject GetJavaRuntime() const;

        void DestroyRuntime();

        void RunModule(JNIEnv *_env, jobject obj, jstring scriptFile);

        void RunModule(const char *moduleName);

        void RunWorker(const std::string &filePath);

        // Tears down a worker's Runtime (engine scope + engine state release) and
        // deletes it. Called from the native worker thread during shutdown.
        static void DisposeWorkerRuntime(Runtime *runtime);

        jobject RunScript(JNIEnv *_env, jobject obj, jstring scriptFile);

        std::string ReadFileText(const std::string &filePath);

        bool NotifyGC(JNIEnv *jEnv, jobject obj, jintArray object_ids);

        bool TryCallGC();

        static int GetWriter();

        static int GetReader();

        static void SetManualInstrumentationMode(jstring mode);

        int GetId();

        static ObjectManager *GetObjectManager(engine::Runtime &rt);

        ObjectManager *GetObjectManager() const;

        engine::Runtime &GetJSRuntime();

        // The scope machinery lives on EngineHost, and JSScope holds a strong
        // reference to it. Callers that need to enter JS take a copy of this
        // rather than a raw pointer, which is what keeps the VM alive across a
        // teardown that runs inside its own scope.
        std::shared_ptr<EngineHost> GetEngineHost() const { return engineHost; }

        static ALooper *GetMainLooper() {
            return m_mainLooper;
        }

        static JavaVM *GetJVM() {
            return java_vm;
        }

        std::shared_ptr<LooperTasks> GetLooperTasks() {
            return m_looperTasks;
        }

        void Lock();

        void Unlock();

        static Runtime *Current();

        jobject ConvertJsValueToJavaObject(JEnv &env, const engine::Value &value,
                                           int classReturnType);

        jint GenerateNewObjectId(JNIEnv *env, jobject obj);

        void
        CreateJSInstanceNative(JNIEnv *_env, jobject obj, jobject javaObject, jint javaObjectID,
                               jstring className);

        jobject CallJSMethodNative(JNIEnv *_env, jobject obj, jint javaObjectID, jclass claz,
                                   jstring methodName, jint retType, jboolean isConstructor,
                                   jobjectArray packagedArgs);

        void
        PassExceptionToJsNative(JNIEnv *env, jobject obj, jthrowable exception, jstring message,
                                jstring fullStackTrace, jstring jsStackTrace, jboolean isDiscarded,
                                jboolean isPendingError);

        void PassUncaughtExceptionFromWorkerToMainHandler(const engine::Value &message,
                                                          const engine::Value &stackTrace,
                                                          const engine::Value &filename,
                                                          int lineno);

        void AdjustAmountOfExternalAllocatedMemory();

        JSMethodCache *js_method_cache;

        bool is_destroying = false;

    private:

        Runtime(JNIEnv *env, jobject runtime, int id);

        // The part of startup that does not depend on who owns the runtime.
        // See the comment on its definition for what `guest` turns off.
        void InitCommon(const std::string &filesRoot, const std::string &callingDir,
                        int maxLogcatObjectSize, bool forceLog, bool guest);

        int m_id;
        jobject m_runtime;

        std::shared_ptr<EngineHost> engineHost;

        MessageLoopTimer *m_loopTimer;
        FinalizerQueue *m_finalizerQueue = nullptr;
        int64_t m_lastUsedMemory;
        // Owned, not a weak/borrowed handle: it is read on every GC notification,
        // long after the scope it was found in has gone.
        engine::Value m_gcFunc;
        volatile bool m_runGC;


        ObjectManager *m_objectManager;

        ArrayBufferHelper m_arrayBufferHelper;

        bool m_isMainThread;

        ModuleInternal m_module;

        std::shared_ptr<LooperTasks> m_looperTasks;

        static int GetAndroidVersion();

        static int m_androidVersion;

        static JavaVM *java_vm;

        static jmethodID GET_USED_MEMORY_METHOD_ID;

        static bool s_mainThreadInitialized;

        static ALooper *m_mainLooper;

        static int m_mainLooper_fd[2];

        static tns::ConcurrentMap<int, Runtime *> id_to_runtime_cache;

        // Keyed by engine::Runtime::identity(), not by &rt: the engine hands a
        // freshly constructed Runtime wrapper to every host callback, so its
        // address is not stable. See engine::Runtime::identity().
        static tns::ConcurrentMap<const void *, Runtime *> rt_to_runtime_cache;

        static tns::ConcurrentMap<std::thread::id, Runtime *> thread_id_to_rt_cache;

        static Runtime *s_main_rt;
        static std::thread::id s_main_thread_id;


        std::thread::id my_thread_id;

#ifdef APPLICATION_IN_DEBUG
        std::mutex m_fileWriteMutex;
#endif


    };

    class JSMethodCache {
    public:

        explicit JSMethodCache(Runtime *_rt) : rt(_rt) {}

        ~JSMethodCache() {
            cleanupCache();
        }

        // An owned engine::Value is what a napi_ref was here: a handle that
        // survives handle scopes. There is no refcount to manage, so the cache
        // stores the value directly and drops it by erasing the entry.
        void cacheMethod(int javaObjectId, const std::string &methodName,
                         const engine::Value &jsMethod) {
            methodCache[javaObjectId][methodName] =
                    engine::Value(rt->GetJSRuntime(), jsMethod);
        }

        engine::Value getCachedMethod(int javaObjectId, const std::string &methodName) {
            auto it = methodCache.find(javaObjectId);
            if (it == methodCache.end()) {
                return engine::Value::undefined();
            }

            auto methodIt = it->second.find(methodName);
            if (methodIt != it->second.end()) {
                if (methodIt->second.isUndefined() || methodIt->second.isNull()) {
                    it->second.erase(methodIt->first);
                    return engine::Value::undefined();
                }
                return engine::Value(rt->GetJSRuntime(), methodIt->second);
            }

            return engine::Value::undefined();
        }

        void cleanupObject(int javaObjectId) {
            auto it = methodCache.find(javaObjectId);
            if (it != methodCache.end()) {
                methodCache.erase(it);
            }
        }

        void cleanupCache() {
            methodCache.clear();
        }


    private:
        Runtime *rt;
        robin_hood::unordered_map<int, robin_hood::unordered_map<std::string, engine::Value>> methodCache;

    };

} // tns

#endif //RUNTIME_H
