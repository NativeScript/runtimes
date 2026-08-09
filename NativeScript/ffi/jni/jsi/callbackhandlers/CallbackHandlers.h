#ifndef CALLBACKHANDLERS_H_
#define CALLBACKHANDLERS_H_

#include <string>
#include <map>
#include <vector>
#include "JEnv.h"
#include "ArgsWrapper.h"
#include "MetadataEntry.h"
#include "FieldCallbackData.h"
#include "MetadataTreeNode.h"
#include "NumericCasts.h"
#include "FieldAccessor.h"
#include "ArrayElementAccessor.h"
#include "ObjectManager.h"
#include "robin_hood.h"
#include <errno.h>
#include "NativeScriptAssert.h"
#include "NativeScriptException.h"
#include "Runtime.h"

namespace tns {
    class CallbackHandlers {
    public:
        static void Init(JsRuntime &rt);

        static JsValue
        CreateJSWrapper(JsRuntime &rt, jint javaObjectID, const std::string &typeName);

        static bool RegisterInstance(JsRuntime &rt, const JsValue &jsObject,
                                     const std::string &fullClassName,
                                     const ArgsWrapper &argWrapper,
                                     const JsValue &implementationObject,
                                     bool isInterface,
                                     JsValue *jsThisProxy,
                                     const std::string &baseClassName = std::string(),
                                     MetadataNode *node = nullptr);

        static jclass ResolveClass(JsRuntime &rt, const std::string &baseClassName,
                                   const std::string &fullClassName,
                                   const JsValue &implementationObject,
                                   bool isInterface);

        static std::string ResolveClassName(JsRuntime &rt, jclass &clazz);

        static JsValue
        GetArrayElement(JsRuntime &rt, const JsValue &array, uint32_t index,
                        const std::string &arraySignature,
                        ObjectManager *objectManager = nullptr, jobject arrayObject = nullptr);

        static void
        SetArrayElement(JsRuntime &rt, const JsValue &array, uint32_t index,
                        const std::string &arraySignature, const JsValue &value,
                        ObjectManager *objectManager = nullptr, jobject arrayObject = nullptr);

        static int GetArrayLength(JsRuntime &rt, const JsValue &arr);

        // `isConstructorCall` replaces the napi tree's napi_get_new_target probe,
        // which needed the napi_callback_info. engine:: host functions do not
        // carry a new.target, so the one caller that cared (MethodCallback, for
        // an error message) passes what it already knows.
        static JsValue
        CallJavaMethod(JsRuntime &rt, const JsValue &caller, const std::string &className,
                       const std::string &methodName, MetadataEntry *entry, bool isFromInterface,
                       bool isStatic, bool isConstructorCall, const JsValue *argv, size_t argc,
                       ObjectManager *objectManager = nullptr);

        static JsValue
        CallJSMethod(JsRuntime &rt, JNIEnv *jEnv, const JsValue &jsObject, jclass claz,
                     const std::string &methodName, int javaObjectId, jobjectArray args);

        static JsValue
        GetJavaField(JsRuntime &rt, const JsValue &caller,
                     FieldCallbackData *fieldData, ObjectManager *objectManager = nullptr,
                     JniLocalRef targetJavaObject = JniLocalRef());

        static void SetJavaField(JsRuntime &rt, const JsValue &target,
                                 const JsValue &value, FieldCallbackData *fieldData,
                                 ObjectManager *objectManager = nullptr,
                                 JniLocalRef targetJavaObject = JniLocalRef());

        static JsValue RunOnMainThreadCallback(JsRuntime &rt, const JsValue &thisVal,
                                               const JsValue *args, size_t argc);

        static int RunOnMainThreadFdCallback(int fd, int events, void *data);

        static JsValue LogMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                         const JsValue *args, size_t argc);

        static JsValue TimeCallback(JsRuntime &rt, const JsValue &thisVal,
                                    const JsValue *args, size_t argc);

        static JsValue DumpReferenceTablesMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                                         const JsValue *args, size_t argc);

        static JsValue DrainMicrotaskCallback(JsRuntime &rt, const JsValue &thisVal,
                                              const JsValue *args, size_t argc);

        static void DumpReferenceTablesMethod();

        static JsValue ExitMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t argc);

        static void CreateGlobalCastFunctions(JsRuntime &rt);

        static std::vector<std::string> GetTypeMetadata(const std::string &name, int index);

        /*
         * Gets all methods in the implementation object, and packs them in a jobjectArray
         * to pass them to Java Land, so that their corresponding Java callbacks are written when
         * the dexFactory generates the class
         */
        static jobjectArray
        GetMethodOverrides(JsRuntime &rt, JEnv &jEnv, const JsValue &implementationObject);

        /*
         * Gets all interfaces declared in the 'interfaces' array inside the implementation object,
         * and packs them in a jobjectArray to pass them to Java Land, so that they may be
         * implemented when the dexFactory generates the corresponding class
         */
        static jobjectArray
        GetImplementedInterfaces(JsRuntime &rt, JEnv &jEnv, const JsValue &implementationObject);

        /*
         * The name sets the two Get* helpers above are built from. Exposed so that
         * content-keyed class names can hash the same sets the generated binding is
         * built out of, without a second copy of the filtering rules.
         */
        // functionsOnly selects what the names are for: true builds the array the
        // dex generator overrides against, false the set a content-keyed class
        // name hashes. The two differ deliberately -- see the implementation.
        static std::vector<std::string>
        CollectMethodOverrideNames(JsRuntime &rt, const JsValue &implementationObject,
                                   bool functionsOnly = true);

        static std::vector<std::string>
        CollectImplementedInterfaceNames(JsRuntime &rt, const JsValue &implementationObject);

        static JsValue EnableVerboseLoggingMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                                          const JsValue *args, size_t argc);

        static JsValue DisableVerboseLoggingMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                                           const JsValue *args, size_t argc);

        static JsValue ReleaseNativeCounterpartCallback(JsRuntime &rt, const JsValue &thisVal,
                                                        const JsValue *args, size_t argc);

        static JsValue FindClass(JsRuntime &rt, const char *name);

        static JsValue NewThreadCallback(JsRuntime &rt, const JsValue &thisVal,
                                         const JsValue *args, size_t argc);

        /*
         * main -> worker messaging
         * Fired when a Worker instance's postMessage is called
         */
        static JsValue WorkerObjectPostMessageCallback(JsRuntime &rt, const JsValue &thisVal,
                                                       const JsValue *args, size_t argc);

        /*
         * worker -> main thread messaging
         * Fired when a Worker script's "postMessage" is called
         */
        static JsValue WorkerGlobalPostMessageCallback(JsRuntime &rt, const JsValue &thisVal,
                                                       const JsValue *args, size_t argc);

        /*
         * Fired when a Worker instance's terminate is called (cooperatively
         * stops the worker thread's looper)
         */
        static JsValue WorkerObjectTerminateCallback(JsRuntime &rt, const JsValue &thisVal,
                                                     const JsValue *args, size_t argc);

        /*
         * Fired when a Worker script's close is called
         */
        static JsValue WorkerGlobalCloseCallback(JsRuntime &rt, const JsValue &thisVal,
                                                 const JsValue *args, size_t argc);

        /*
         * Is called when an unhandled exception is thrown inside the worker
         * Will execute 'onerror' if one is provided inside the Worker Scope
         * Will make the exception "bubble up" through to the parent, to be handled by the Worker Object
         * if 'onerror' isn't implemented or returns false
         *
         * Takes the caught JSError rather than the napi tree's napi_value: an
         * engine:: throw arrives as a JSError, and its payload (which is what the
         * napi version received) is JSError::value().
         */
        static void CallWorkerScopeOnErrorHandle(JsRuntime &rt, const JsError &error);

        static JsValue PostFrameCallback(JsRuntime &rt, const JsValue &thisVal,
                                         const JsValue *args, size_t argc);

        static JsValue RemoveFrameCallback(JsRuntime &rt, const JsValue &thisVal,
                                           const JsValue *args, size_t argc);

        static void RemoveEnvEntries(JsRuntime &rt);

        struct AChoreographer;

        typedef void (*AChoreographer_frameCallback)(long frameTimeNanos, void *data);

        typedef void (*AChoreographer_frameCallback64)(int64_t frameTimeNanos, void *data);

        typedef AChoreographer *(*func_AChoreographer_getInstance)();

        typedef void (*func_AChoreographer_postFrameCallback)(
                AChoreographer *choreographer, AChoreographer_frameCallback callback,
                void *data);

        typedef void (*func_AChoreographer_postFrameCallback64)(
                AChoreographer *choreographer, AChoreographer_frameCallback64 callback,
                void *data);

        typedef void (*func_AChoreographer_postFrameCallbackDelayed)(
                AChoreographer *choreographer, AChoreographer_frameCallback callback,
                void *data, long delayMillis);

        typedef void (*func_AChoreographer_postFrameCallbackDelayed64)(
                AChoreographer *choreographer, AChoreographer_frameCallback64 callback,
                void *data, uint32_t delayMillis);

        // The napi tree also declared `lastCallId` / `lastCallValue` here. Both
        // were dead -- defined once, never read -- and lastCallValue as an owned
        // engine handle in a process-wide static would outlive its runtime, so
        // they are not carried over.

    private:
        CallbackHandlers() {
        }

        static void AdjustAmountOfExternalAllocatedMemory(JsRuntime &rt);

        /*
         * Helper method that creates a java string array for sending strings over JNI
         */
        static jobjectArray GetJavaStringArray(JEnv &jEnv, int length);

        static short MAX_JAVA_STRING_ARRAY_LENGTH;

        static jclass RUNTIME_CLASS;

        static jclass JAVA_LANG_STRING;

        static jmethodID RESOLVE_CLASS_METHOD_ID;

        static jfieldID CURRENT_OBJECTID_FIELD_ID;

        static jmethodID MAKE_INSTANCE_STRONG_ID;

        static jmethodID GET_TYPE_METADATA;

        static jmethodID ENABLE_VERBOSE_LOGGING_METHOD_ID;

        static jmethodID DISABLE_VERBOSE_LOGGING_METHOD_ID;

        static NumericCasts castFunctions;

        static ArrayElementAccessor arrayElementAccessor;

        static FieldAccessor fieldAccessor;

        struct JavaObjectIdScope {
            JavaObjectIdScope(JEnv &_jEnv, jfieldID fieldId, jobject runtime, int javaObjectId)
                    : jEnv(_jEnv), _fieldID(fieldId), _runtime(runtime) {
                jEnv.SetIntField(_runtime, _fieldID, javaObjectId);
            }

            ~JavaObjectIdScope() {
                jEnv.SetIntField(_runtime, _fieldID, -1);
            }

        private:
            JEnv jEnv;
            jfieldID _fieldID;
            jobject _runtime;
        };

        static std::atomic_int64_t count_;

        struct Callback {
            Callback() {}

            Callback(uint64_t id)
                    : id_(id) {
            }

            uint64_t id_;
        };

        // An owned engine::Function replaces the napi_ref: it survives handle
        // scopes and is released when the entry is erased, so there is no
        // destructor left to write.
        //
        // The owning tns::Runtime is stored rather than the engine::Runtime the
        // callback was registered from: that one is a stack temporary built for
        // the host call (see engine::Runtime::identity()), so its address would
        // dangle by the time this entry runs.
        struct CacheEntry {
            CacheEntry(tns::Runtime *runtime, JsRuntime &rt, const JsValue &callback)
                    : runtime_(runtime), callback_(rt, callback) {
            }

            tns::Runtime *runtime_;
            JsValue callback_;
        };

        static robin_hood::unordered_map<uint64_t, CacheEntry> cache_;

        static robin_hood::unordered_map<jclass, jfieldID> jclass_to_runtimeId_cache;

        static std::atomic_uint64_t frameCallbackCount_;

        struct FrameCallbackCacheEntry {
            // See CacheEntry: the owning tns::Runtime, not the call-scoped
            // engine::Runtime wrapper.
            FrameCallbackCacheEntry(tns::Runtime *runtime, JsRuntime &_rt,
                                    const JsValue &callback_, uint64_t aId)
                    : runtime(runtime), callback(_rt, callback_), id(aId) {
            }

            tns::Runtime *runtime;
            JsValue callback;
            uint64_t id;

            bool isScheduled() {
                return scheduled;
            }

            void markScheduled() {
                scheduled = true;
                removed = false;
            }

            void markRemoved() {
                // we can never unschedule a callback, so we just mark it as removed
                removed = true;
            }

            AChoreographer_frameCallback frameCallback_ = [](long ts, void *data) {
                execute((double) ts, data);
            };

            AChoreographer_frameCallback64 frameCallback64_ = [](int64_t ts, void *data) {
                execute((double) ts, data);
            };

            static void execute(double ts, void *data);

        private:
            bool removed = false;
            bool scheduled = false;

            void markUnscheduled() {
                scheduled = false;
                removed = true;
            }

            bool shouldRemoveBeforeCall() {
                return removed;
            }

            bool shouldRemoveAfterCall() {
                return !scheduled && removed;
            }
        };

        static robin_hood::unordered_map<uint64_t, FrameCallbackCacheEntry> frameCallbackCache_;

        static void InitChoreographer();

        static void PostCallback(JsRuntime &rt, const JsValue *args, size_t argc,
                                 FrameCallbackCacheEntry *entry);

    };
}

#endif /* CALLBACKHANDLERS_H_ */
