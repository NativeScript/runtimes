#include <unistd.h>
#include <thread>
#include "Runtime.h"
#include <string>
#include <csignal>
#include <sstream>
#include <mutex>
#include <cstdlib>
#include <exception>
#include <dlfcn.h>
// PRId64, for the memory-accounting trace below. It used to arrive by accident
// through libzip's zipconf.h, which this file included but never used.
#include <cinttypes>
#include "NativeScriptException.h"
#include <sys/system_properties.h>
#include "File.h"
#include <android/log.h>
#include "Version.h"
#include "SIGHandler.h"
#include "ArgConverter.h"
#include "NativeScriptAssert.h"
#include "CallbackHandlers.h"
#include "MetadataNode.h"
#include "Util.h"
#include "JsArgToArrayConverter.h"
#include "ArrayHelper.h"
#include "GlobalHelpers.h"

#include "AndroidRuntimeModules.h"
// The main-thread task queue, not a worker facility: this is how a Java
// callback arriving on another thread gets back onto the JS thread.
#include "LooperTasks.h"

// Everything below belongs to the standalone runtime only. Excluded from the
// guest build so it is not compiled into an app that can never reach it -- see
// the source filters in packages/react-native/android/CMakeLists.txt.
#if !defined(NS_JSI_HOST_RUNTIME)
#include "Console.h"
#include "Performance.h"
#include "SimpleProfiler.h"
#include "ManualInstrumentation.h"
#include "Timers.h"
#endif

using namespace tns;
using namespace std;

namespace {
    // std::terminate handler: log an uncaught native exception (with its message
    // where available) before aborting, so the crash is diagnosable instead of a
    // bare abort.
    void LogAndAbortUncaught() {
        try {
            throw;  // rethrow the current in-flight exception
        } catch (const tns::NativeScriptException &e) {
            __android_log_print(ANDROID_LOG_FATAL, "TNS.Native",
                                "Uncaught NativeScriptException: %s", e.what());
        } catch (const std::exception &e) {
            __android_log_print(ANDROID_LOG_FATAL, "TNS.Native",
                                "Uncaught std::exception: %s", e.what());
        } catch (...) {
            __android_log_print(ANDROID_LOG_FATAL, "TNS.Native",
                                "Uncaught unknown native exception");
        }

        // Preserve default abort behavior so crashes are visible to tooling.
        std::_Exit(EXIT_FAILURE);
    }

    // queueMicrotask(callback) per spec:
    // https://developer.mozilla.org/en-US/docs/Web/API/queueMicrotask
    // Implemented via Promise.resolve().then(callback) so it schedules a real
    // microtask on every engine the runtime targets (V8, QuickJS, Hermes, JSC).
    // This preserves ordering with Promise microtasks and runs before timers.
    engine::Value QueueMicrotaskCallback(engine::Runtime &rt, const engine::Value &,
                                         const engine::Value *args, size_t count) {
        if (count < 1 || !args[0].isObject() ||
            !args[0].asObjectBorrowed(rt).isFunction(rt)) {
            throw engine::JSError(rt, "queueMicrotask: callback must be a function");
        }

        engine::Object global = rt.global();
        engine::Object promiseCtor = global.getPropertyAsObject(rt, "Promise");
        engine::Value resolved =
                promiseCtor.getPropertyAsFunction(rt, "resolve").callWithThis(rt, promiseCtor);
        engine::Object resolvedObject = resolved.asObject(rt);
        resolvedObject.getPropertyAsFunction(rt, "then")
                .callWithThis(rt, resolvedObject, args, 1);

        return engine::Value::undefined();
    }
}

bool tns::LogEnabled = false;

void Runtime::Init(JavaVM *vm) {
    __android_log_print(ANDROID_LOG_INFO, "TNS.Runtime",
                        "NativeScript Runtime Version %s, commit %s", NATIVE_SCRIPT_RUNTIME_VERSION,
                        NATIVE_SCRIPT_RUNTIME_COMMIT_SHA);


    if (Runtime::java_vm == nullptr) {
        java_vm = vm;
        JEnv::Init(java_vm);
        NativeScriptException::Init();
    }

    // handle SIGABRT/SIGSEGV only on API level > 20 as the handling is not so efficient in older versions
    if (m_androidVersion > 20) {
        struct sigaction action = {};
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        action.sa_handler = SIGHandler;
        sigaction(SIGABRT, &action, NULL);
#ifndef __JSC__
        // JavaScriptCore installs and relies on its OWN SIGSEGV handler for normal,
        // non-fatal operation (concurrent GC / JIT). Overwriting it with a handler
        // that unconditionally throws a C++ exception hijacks those legitimate
        // faults and manufactures a crash — reliably reproduced under multithreaded
        // JNI access (testConcurrentAccess). No active test relies on converting a
        // SIGSEGV to a JS exception (the only such spec is disabled via xit), so on
        // JSC we leave SIGSEGV to the engine. SIGABRT (which JSC does not use) is
        // still converted. On every other engine both are converted as before.
        sigaction(SIGSEGV, &action, NULL);
#endif
    }

    // Log uncaught native exceptions before aborting.
    std::set_terminate(LogAndAbortUncaught);
}

/**
 * Returns the runtime based on the current thread id
 * Defaults to returning the main runtime if no runtime is found.
 *
 * One thread can only host a single runtime at the moment. Multiple runtimes
 * on a single thread are not supported.
 * @return
 */
Runtime *Runtime::Current() {
    if (!s_mainThreadInitialized) return nullptr;
    auto id = this_thread::get_id();
    auto rt = Runtime::thread_id_to_rt_cache.Get(id);
    if (rt) return rt;

    return s_main_rt;
}

Runtime::Runtime(JNIEnv *jEnv, jobject runtime, int id)
        : m_id(id), m_lastUsedMemory(0) {
    m_runtime = jEnv->NewGlobalRef(runtime);
    m_objectManager = new ObjectManager(m_runtime);
    m_loopTimer = new MessageLoopTimer();
    id_to_runtime_cache.Insert(id, this);

    js_method_cache = new JSMethodCache(this);

    auto tid = this_thread::get_id();
    Runtime::thread_id_to_rt_cache.Insert(tid, this);
    this->my_thread_id = tid;

    if (GET_USED_MEMORY_METHOD_ID == nullptr) {
        auto RUNTIME_CLASS = jEnv->FindClass("com/tns/Runtime");
        assert(RUNTIME_CLASS != nullptr);
        GET_USED_MEMORY_METHOD_ID = jEnv->GetMethodID(RUNTIME_CLASS, "getUsedMemory", "()J");
        assert(GET_USED_MEMORY_METHOD_ID != nullptr);
    }
}

Runtime *Runtime::GetRuntime(int runtimeId) {
    auto runtime = id_to_runtime_cache.Get(runtimeId);

    if (runtime == nullptr) {
        stringstream ss;
        ss << "Cannot find runtime for id:" << runtimeId;
        throw NativeScriptException(ss.str());
    }

    return runtime;
}

jobject Runtime::GetJavaRuntime() const {
    return m_runtime;
}

// The VM-owning entry points. A guest build (NS_JSI_HOST_RUNTIME) has no
// EngineHost::Create to call and is handed its runtime by the embedder, so this
// is where the two flavours part; they rejoin in InitCommon below.
#if !defined(NS_JSI_HOST_RUNTIME)

void
Runtime::Init(JNIEnv *_env, jobject obj, int runtimeId, jstring filesPath, jstring nativeLibsDir,
              jboolean verboseLoggingEnabled, jboolean isDebuggable, jstring packageName,
              jobjectArray args, jstring callingDir, int maxLogcatObjectSize, bool forceLog) {
    JEnv env(_env);
    auto runtime = new Runtime(env, obj, runtimeId);
    auto enableLog = verboseLoggingEnabled == JNI_TRUE;

    runtime->Init(env, filesPath, nativeLibsDir, enableLog, isDebuggable, packageName, args,
                  callingDir, maxLogcatObjectSize, forceLog);
}

void Runtime::Init(JNIEnv *_env, jstring filesPath, jstring nativeLibsDir,
                   bool verboseLoggingEnabled, bool isDebuggable, jstring packageName,
                   jobjectArray args, jstring callingDir, int maxLogcatObjectSize, bool forceLog) {

    LogEnabled = verboseLoggingEnabled;
    auto filesRoot = ArgConverter::jstringToString(filesPath);
    auto nativeLibDirStr = ArgConverter::jstringToString(nativeLibsDir);
    auto packageNameStr = ArgConverter::jstringToString(packageName);
    auto callingDirStr = ArgConverter::jstringToString(callingDir);

    Constants::APP_ROOT_FOLDER_PATH = filesRoot + "/app/";

    DEBUG_WRITE("Initializing NativeScript JSI Runtime");

    auto flags = ArgConverter::jstringToString(JniLocalRef(_env->GetObjectArrayElement(args, 0)));

    JniLocalRef cacheCode(_env->GetObjectArrayElement(args, 1));
    Constants::CACHE_COMPILED_CODE = (bool) cacheCode;

    JniLocalRef profilerOutputDir(_env->GetObjectArrayElement(args, 2));

    EngineHost::SetFlags(flags.c_str());
    engineHost = EngineHost::Create();
    if (engineHost == nullptr) {
        throw NativeScriptException("Failed to create JS runtime");
    }

    InitCommon(filesRoot, callingDirStr, maxLogcatObjectSize, forceLog, /*guest*/ false);
}

#else  // NS_JSI_HOST_RUNTIME

Runtime *Runtime::Attach(JNIEnv *_env, jobject obj, int runtimeId,
                         ::facebook::jsi::Runtime &hostRuntime, const std::string &filesRoot,
                         bool verboseLoggingEnabled, int maxLogcatObjectSize, bool forceLog) {
    JEnv env(_env);
    auto runtime = new Runtime(env, obj, runtimeId);
    runtime->Attach(hostRuntime, filesRoot, verboseLoggingEnabled, maxLogcatObjectSize, forceLog);
    return runtime;
}

void Runtime::Attach(::facebook::jsi::Runtime &hostRuntime, const std::string &filesRoot,
                     bool verboseLoggingEnabled, int maxLogcatObjectSize, bool forceLog) {
    LogEnabled = verboseLoggingEnabled;

    // Only the metadata reader and the (unused here) module loader read this.
    Constants::APP_ROOT_FOLDER_PATH = filesRoot + "/app/";

    DEBUG_WRITE("Attaching NativeScript to a host-provided JSI runtime");

    // No SetFlags: the embedder built the VM long before we were loaded, and on
    // every engine the flags this would set are read at VM construction.
    engineHost = EngineHost::Adopt(hostRuntime);

    InitCommon(filesRoot, /*callingDir*/ "", maxLogcatObjectSize, forceLog, /*guest*/ true);
}

#endif  // NS_JSI_HOST_RUNTIME

// Everything both flavours install.
//
// `guest` is true when the runtime belongs to an embedder. A host like React
// Native already provides console, timers, a module loader, performance and
// queueMicrotask, and ours are wired to a message loop and a require() root
// that only exist in a standalone app -- installing them over the host's would
// replace working implementations with broken ones. What a guest still needs is
// the part no host can provide: the metadata tree, the object manager, and the
// JNI interop callbacks.
void Runtime::InitCommon(const std::string &filesRoot, const std::string &callingDir,
                         int maxLogcatObjectSize, bool forceLog, bool guest) {
    // The napi runtime opens a process-lifetime handle scope here (global_scope)
    // so that napi_values created outside any explicit scope have somewhere to
    // live. There is no such thing to hold open: an owned engine::Value roots
    // itself, so initialisation only needs the ordinary entry scope below.
    JSEnterScope

    engine::Runtime &rt = engineHost->GetRuntime();

    const void *rtKey = rt.identity();
    rt_to_runtime_cache.Insert(rtKey, this);

    engine::Object global = rt.global();

    // Newer JSC ships a native `WeakRef` global, so the old polyfill (which was
    // actually a strong reference and leaked) is no longer needed.

#if !defined(NS_JSI_HOST_RUNTIME)
    if (!guest) {
        Console::createConsole(rt, maxLogcatObjectSize, forceLog);

        Timers::InitStatic(rt, global);
    }
#endif

    // Bound to this (runtime) thread's Looper; drains deferred finalizers posted
    // via Runtime::PostFinalizer at a safe point off the GC sweep.
    m_finalizerQueue = new FinalizerQueue(&rt);

    engine_util::SetFunction(rt, global, "__log", CallbackHandlers::LogMethodCallback);
    engine_util::SetFunction(rt, global, "__dumpReferenceTables",
                             CallbackHandlers::DumpReferenceTablesMethodCallback);
    engine_util::SetFunction(rt, global, "__drainMicrotaskQueue",
                             CallbackHandlers::DrainMicrotaskCallback);
    engine_util::SetFunction(rt, global, "__enableVerboseLogging",
                             CallbackHandlers::EnableVerboseLoggingMethodCallback);
    engine_util::SetFunction(rt, global, "__disableVerboseLogging",
                             CallbackHandlers::DisableVerboseLoggingMethodCallback);
    engine_util::SetFunction(rt, global, "__exit", CallbackHandlers::ExitMethodCallback);

    global.setProperty(rt, "__runtimeVersion",
                       engine::String::createFromUtf8(rt, NATIVE_SCRIPT_RUNTIME_VERSION));

    global.setProperty(rt, "__engine",
                       engine::String::createFromUtf8(rt, engineHost->EngineVersion()));

    const char *engineVariant = "UNKNOWN";
#if defined(__HERMES__)
    engineVariant = "HERMES";
#elif defined(__JSC__)
    engineVariant = "JSC";
#elif defined(__V8_13__)
    engineVariant = "V8-13";
#elif defined(__V8_11__)
    engineVariant = "V8-11";
#elif defined(__V8_10__)
    engineVariant = "V8-10";
#elif defined(__V8__)
    engineVariant = "V8";
#elif defined(__PRIMJS__)
    engineVariant = "PRIMJS";
#elif defined(__QJS_NG__)
    engineVariant = "QUICKJS_NG";
#elif defined(__QJS__)
    engineVariant = "QUICKJS";
#endif
    global.setProperty(rt, "__engineVariant", engine::String::createFromUtf8(rt, engineVariant));

    engine_util::SetFunction(rt, global, "__time", CallbackHandlers::TimeCallback);
    engine_util::SetFunction(rt, global, "__releaseNativeCounterpart",
                             CallbackHandlers::ReleaseNativeCounterpartCallback);
    engine_util::SetFunction(rt, global, "__postFrameCallback",
                             CallbackHandlers::PostFrameCallback);
    engine_util::SetFunction(rt, global, "__removeFrameCallback",
                             CallbackHandlers::RemoveFrameCallback);
    engine_util::SetFunction(rt, global, "__markingMode",
                             [](engine::Runtime &, const engine::Value &, const engine::Value *,
                                size_t) -> engine::Value {
                                 return engine::Value(0);
                             });

    engine_util::SetFunction(rt, global, "napiFunction",
                             [](engine::Runtime &, const engine::Value &, const engine::Value *,
                                size_t) -> engine::Value {
                                 return engine::Value::undefined();
                             });

#if !defined(NS_JSI_HOST_RUNTIME)
    SimpleProfiler::Init(rt, global);
#endif

    CallbackHandlers::CreateGlobalCastFunctions(rt);

    CallbackHandlers::Init(rt);

    ArgConverter::Init(rt);

    AndroidRuntimeModules::Init(rt, global);

    m_objectManager->Init(rt);

#if !defined(NS_JSI_HOST_RUNTIME)
    if (!guest) {
        m_module.Init(rt, callingDir);
    }
#endif

    if (!s_mainThreadInitialized) {
        m_isMainThread = true;

        s_main_rt = this;
        s_main_thread_id = this_thread::get_id();

        pipe2(m_mainLooper_fd, O_NONBLOCK | O_CLOEXEC);
        m_mainLooper = ALooper_forThread();

        ALooper_acquire(m_mainLooper);

        // try using 2MB
        int ret = fcntl(m_mainLooper_fd[1], F_SETPIPE_SZ, 2 * (1024 * 1024));

        // try using 1MB
        if (ret != 0) {
            ret = fcntl(m_mainLooper_fd[1], F_SETPIPE_SZ, 1 * (1024 * 1024));
        }

        // try using 512KB
        if (ret != 0) {
            ret = fcntl(m_mainLooper_fd[1], F_SETPIPE_SZ, (512 * 1024));
        }

        ALooper_addFd(m_mainLooper, m_mainLooper_fd[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                      CallbackHandlers::RunOnMainThreadFdCallback, nullptr);
    }
        /*
         * Emulate a `WorkerGlobalScope`
         * Attach 'postMessage', 'close' to the global object of every non-main
         * (worker) env.
         *
         * A guest is only ever attached to the embedder's runtime, which is a
         * main-thread runtime by construction, so this branch is unreachable
         * there and the worker callbacks it needs are not built.
         */
    else {
        m_isMainThread = false;
#if !defined(NS_JSI_HOST_RUNTIME)
        engine_util::SetFunction(rt, global, "postMessage",
                                 CallbackHandlers::WorkerGlobalPostMessageCallback);
        engine_util::SetFunction(rt, global, "close",
                                 CallbackHandlers::WorkerGlobalCloseCallback);
        engine_util::SetFunction(rt, global, "terminate",
                                 CallbackHandlers::WorkerGlobalCloseCallback);
        global.setProperty(rt, "__ns__worker", true);
#endif
    }

    /*
     * Attach the `Worker` object constructor to EVERY env's global object so
     * that nested workers (a worker spawning its own workers) are supported.
     */
    // A worker gets its own VM, which a guest has no way to build -- and React
    // Native ships its own Worker story anyway.
#if !defined(NS_JSI_HOST_RUNTIME)
    if (!guest) {
        engine::Function worker = engine::Function::createFromHostConstructor(
                rt, engine::PropNameID::forAscii(rt, "Worker"), 0,
                CallbackHandlers::NewThreadCallback);
        engine::Object prototype = worker.getPropertyAsObject(rt, "prototype");
        engine_util::SetFunction(rt, prototype, "postMessage",
                                 CallbackHandlers::WorkerObjectPostMessageCallback);
        engine_util::SetFunction(rt, prototype, "terminate",
                                 CallbackHandlers::WorkerObjectTerminateCallback);
        global.setProperty(rt, "Worker", worker);
    }
#endif

    // The napi runtime installs `global` (and `self`) as accessors returning the
    // current global object. nativescript::engine has no accessor API, and a
    // plain self-reference is what globalThis already is, so these are data
    // properties here. A guest leaves it alone: React Native has already defined
    // `global`, and it is the same object either way.
    if (!guest) {
        global.setProperty(rt, "global", global);
    }

    if (!s_mainThreadInitialized) {
        MetadataNode::BuildMetadata(filesRoot);
    } else {
        // Do not set 'self' accessor to main thread
        global.setProperty(rt, "self", global);
    }

    MetadataNode::CreateTopLevelNamespaces(rt);

    ArrayHelper::Init(rt);

#if !defined(NS_JSI_HOST_RUNTIME)
    if (!guest) {
        Performance::createPerformance(rt, global);

        engine_util::SetFunction(rt, global, "queueMicrotask", QueueMicrotaskCallback, 1);
    }
#endif

    m_arrayBufferHelper.CreateConvertFunctions(rt, global, m_objectManager);

    // Drives our own timers and GC hints off this thread's looper. A guest's
    // timers belong to the host, and the GC hint would be measuring a heap we
    // do not own.
    if (!guest) {
        m_loopTimer->Init(engineHost);
    }

    // Per-runtime task queue bound to this thread's looper. Child workers post
    // their outbound messages/errors/cleanup onto their parent runtime's queue.
    // Looper.prepare() has already run for worker threads (initWorkerRuntime),
    // so ALooper_forThread() returns the looper that runWorkerLoop() will pump.
    m_looperTasks = std::make_shared<LooperTasks>();
    m_looperTasks->Initialize(ALooper_forThread());

    s_mainThreadInitialized = true;

    DEBUG_WRITE("%s", "NativeScript Runtime Loaded!");
}

int Runtime::GetAndroidVersion() {
    char sdkVersion[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk", sdkVersion);

    std::stringstream strValue;
    strValue << sdkVersion;

    unsigned int intValue;
    strValue >> intValue;

    return intValue;
}

ObjectManager *Runtime::GetObjectManager(engine::Runtime &rt) {
    return GetRuntime(rt)->GetObjectManager();
}

ObjectManager *Runtime::GetObjectManager() const {
    return m_objectManager;
}

Runtime::~Runtime() {
    delete this->m_objectManager;
    delete this->m_loopTimer;

    // The napi runtime frees the engine runtime here under V8 and inside
    // DestroyRuntime everywhere else. Here it is neither: engineHost is a
    // shared_ptr that every live JSScope also holds, so the VM goes away when
    // the last scope has unwound, whichever of the two runs last.
    engineHost.reset();

    if (m_isMainThread) {
        if (m_mainLooper_fd[0] != -1) {
            ALooper_removeFd(m_mainLooper, m_mainLooper_fd[0]);
        }
        ALooper_release(m_mainLooper);

        if (m_mainLooper_fd[0] != -1) {
            close(m_mainLooper_fd[0]);
        }

        if (m_mainLooper_fd[1] != -1) {
            close(m_mainLooper_fd[1]);
        }
    }
}

std::string Runtime::ReadFileText(const std::string &filePath) {
#ifdef APPLICATION_IN_DEBUG
    std::lock_guard<std::mutex> lock(m_fileWriteMutex);
#endif
    return File::ReadText(filePath);
}

void Runtime::DestroyRuntime() {
    is_destroying = true;
    engine::Runtime &rt = engineHost->GetRuntime();
    if (m_looperTasks != nullptr) {
        m_looperTasks->Terminate();
    }
    MetadataNode::onDisposeRuntime(rt);
    ArgConverter::onDisposeRuntime(rt);
    tns::GlobalHelpers::onDisposeRuntime(rt);
    this->js_method_cache->cleanupCache();
    delete this->js_method_cache;
    this->js_method_cache = nullptr;
#if !defined(NS_JSI_HOST_RUNTIME)
    this->m_module.DeInit();
    Console::onDisposeRuntime(rt);
    // The napi version tears the timers down from a finalizer on the global
    // object; there is no equivalent here, so it is driven from the same place
    // as every other per-runtime teardown.
    Timers::onDisposeRuntime(rt);
#endif
    CallbackHandlers::RemoveEnvEntries(rt);
    this->m_objectManager->OnDisposeRuntime();
    // Release the finalizer handler and flush any still-queued cleanup while the
    // runtime is still valid; finalizers firing during teardown below then run
    // inline (Runtime::PostFinalizer's fallback).
    if (m_finalizerQueue != nullptr) {
        m_finalizerQueue->Destroy();
        delete m_finalizerQueue;
        m_finalizerQueue = nullptr;
    }
    // Every engine handle this runtime still owns must go while the VM is up.
    m_gcFunc = engine::Value::undefined();
    // Last, because everything above may still call a js_util helper on its way
    // out: js_util::Builtins holds ~18 owned engine handles per runtime
    // (Object.defineProperty, the Error constructor, ...). Nothing released them
    // before, which QuickJS catches directly -- JS_FreeRuntime asserts
    // list_empty(&rt->gc_obj_list) and aborts, where V8 and JSC only leak.
    js_util::Builtins::dispose(rt);
    Runtime::thread_id_to_rt_cache.Remove(this->my_thread_id);
    id_to_runtime_cache.Remove(m_id);
    const void *rtKey = rt.identity();
    rt_to_runtime_cache.Remove(rtKey);
    // Deliberately NOT engineHost->ReleaseEngineState() here: this runs inside a
    // JSScope on the worker path, and dropping the engine::Runtime under it is
    // exactly what made the napi path SIGSEGV on Hermes and hang on JSC. The
    // scope holds a shared_ptr to the host, so the VM outlives it either way,
    // and ~EngineHost does the teardown once nothing is standing on it.
}

bool Runtime::NotifyGC(JNIEnv *jEnv, jobject obj, jintArray object_ids) {
    if (this->is_destroying) return true;
    m_objectManager->OnGarbageCollected(jEnv, object_ids);
    bool success = __sync_bool_compare_and_swap(&m_runGC, false, true);
    return success;
}


void Runtime::AdjustAmountOfExternalAllocatedMemory() {
    JEnv jEnv;
    int64_t usedMemory = jEnv.CallLongMethod(m_runtime, GET_USED_MEMORY_METHOD_ID);
    int64_t changeInBytes = usedMemory - m_lastUsedMemory;
    int64_t externalMemory = 0;

    if (changeInBytes != 0) {
        externalMemory = engineHost->AdjustExternalMemory(changeInBytes);
    }

    DEBUG_WRITE("usedMemory=%" PRId64 " changeInBytes=%" PRId64 " externalMemory=%" PRId64,
                usedMemory, changeInBytes, externalMemory);

    m_lastUsedMemory = usedMemory;
}

bool Runtime::TryCallGC() {
    if (this->is_destroying) return true;
    engine::Runtime &rt = engineHost->GetRuntime();
    engine::Object global = rt.global();
    if (m_gcFunc.isUndefined()) {
        engine::Value gc = global.getProperty(rt, "gc");
        if (gc.isUndefined() || gc.isNull()) return true;
        m_gcFunc = engine::Value(rt, gc);
    }

    bool success = __sync_bool_compare_and_swap(&m_runGC, true, false);

    if (success) {
        m_gcFunc.asObject(rt).asFunction(rt).callWithThis(rt, global);
    }

    return success;
}

// Module loading, workers, and the worker->main exception hop are all the
// standalone runtime's. A guest never evaluates a file: the embedder's bundler
// does, and it hands the runtime an already-running VM.
#if !defined(NS_JSI_HOST_RUNTIME)
void Runtime::RunModule(JNIEnv *_jEnv, jobject obj, jstring scriptFile) {
    JEnv jEnv(_jEnv);
    string filePath = ArgConverter::jstringToString(scriptFile);
    engine::Runtime &rt = engineHost->GetRuntime();
    // The engine layer reports a failed evaluation by throwing, so there is no
    // pending-exception flag to test afterwards.
    try {
        m_module.Load(rt, filePath);
    } catch (engine::JSError &error) {
        throw NativeScriptException(rt, error, string("Error running module at path: ") + filePath);
    }
}

void Runtime::RunModule(const char *moduleName) {
    m_module.Load(engineHost->GetRuntime(), moduleName);
}

void Runtime::RunWorker(const std::string &filePath) {
    m_module.LoadWorker(engineHost->GetRuntime(), filePath);
}

void Runtime::DisposeWorkerRuntime(Runtime *runtime) {
    // `engineHost` is referenced by the JSEnterScope macro below.
    std::shared_ptr<EngineHost> engineHost = runtime->GetEngineHost();
    {
        JSEnterScope
        runtime->DestroyRuntime();
    }
    // Both this scope's reference and the runtime's are gone by the end of the
    // next line, and whichever drops last runs ~EngineHost. Nothing frees the VM
    // while the scope above is still unwinding over it.
    delete runtime;
}

jobject Runtime::RunScript(JNIEnv *_env, jobject obj, jstring scriptFile) {
    auto filename = ArgConverter::jstringToString(scriptFile);
    auto sourceUrl = ModuleInternal::EnsureFileProtocol(filename);

    DEBUG_WRITE("%s", filename.c_str());

    // Precompiled bytecode first, source otherwise -- same order as the napi
    // path's js_run_bytecode_file.
    try {
        engine::Value result;
        if (!engineHost->ExecuteBytecodeFile(filename, sourceUrl, result)) {
            engineHost->ExecuteScript(ReadFileText(filename), sourceUrl);
        }
    } catch (engine::JSError &error) {
        throw NativeScriptException(engineHost->GetRuntime(), error,
                                    "Error running script " + filename);
    }

    return nullptr;
}
#endif


engine::Runtime &Runtime::GetJSRuntime() {
    return engineHost->GetRuntime();
}

int Runtime::GetId() {
    return this->m_id;
}

int Runtime::GetWriter() {
    return m_mainLooper_fd[1];
}

int Runtime::GetReader() {
    return m_mainLooper_fd[0];
}

jobject
Runtime::CallJSMethodNative(JNIEnv *_jEnv, jobject obj, jint javaObjectID, jclass claz,
                            jstring methodName,
                            jint retType, jboolean isConstructor, jobjectArray packagedArgs) {
    JEnv jEnv(_jEnv);
    engine::Runtime &rt = engineHost->GetRuntime();

    DEBUG_WRITE("CallJSMethodNative called javaObjectID=%d", javaObjectID);

    auto jsObject = m_objectManager->GetJsObjectByJavaObject(javaObjectID);

    if (jsObject.isUndefined() || jsObject.isNull()) {
        stringstream ss;
        ss << "JavaScript object for Java ID " << javaObjectID << " not found." << endl;
        ss << "Attempting to call method " << ArgConverter::jstringToString(methodName) << endl;
        throw NativeScriptException(ss.str());
    }

    if (isConstructor) {
        DEBUG_WRITE("CallJSMethodNative: Updating linked instance with its real class");
        jclass instanceClass = jEnv.GetObjectClass(obj);
        m_objectManager->SetJavaClass(jsObject, instanceClass);
    }

    string method_name = ArgConverter::jstringToString(methodName);

    DEBUG_WRITE("CallJSMethodNative called jsObject %s", method_name.c_str());

    auto jsResult = CallbackHandlers::CallJSMethod(rt, jEnv, jsObject, claz, method_name,
                                                   javaObjectID, packagedArgs);

    if (jsResult.isUndefined() || jsResult.isNull()) return nullptr;

    int classReturnType = retType;
    jobject javaObject = ConvertJsValueToJavaObject(jEnv, jsResult, classReturnType);


    return javaObject;
}

void
Runtime::CreateJSInstanceNative(JNIEnv *_jEnv, jobject obj, jobject javaObject, jint javaObjectID,
                                jstring className) {
    DEBUG_WRITE("createJSInstanceNative called");
    JEnv jEnv(_jEnv);
    engine::Runtime &rt = engineHost->GetRuntime();

    string existingClassName = ArgConverter::jstringToString(className);

    string jniName = Util::ConvertFromCanonicalToJniName(existingClassName);

    auto proxyClassName = m_objectManager->GetClassName(javaObject);

    DEBUG_WRITE("createJSInstanceNative class %s", proxyClassName.c_str());

    MetadataNode *extNode = nullptr;
    engine::Value jsInstance = MetadataNode::CreateExtendedJSWrapper(rt, m_objectManager,
                                                                    proxyClassName, javaObjectID,
                                                                    &extNode);

    if (jsInstance.isUndefined() || jsInstance.isNull()) {
        throw NativeScriptException(
                string("Failed to create JavaScript extend wrapper for class '" + proxyClassName +
                       "'"));
    }

    engine::Value implementationObject = MetadataNode::GetImplementationObject(rt, jsInstance);

    if (implementationObject.isUndefined() || implementationObject.isNull()) {
        string msg("createJSInstanceNative: implementationObject is empty");
        throw NativeScriptException(msg);
    }

    DEBUG_WRITE("createJSInstanceNative: implementationObject");

    m_objectManager->Link(jsInstance, javaObjectID, nullptr, extNode);
}

jint Runtime::GenerateNewObjectId(JNIEnv *jEnv, jobject obj) {
    int objectId = m_objectManager->GenerateNewObjectID();
    return objectId;
}

jobject Runtime::ConvertJsValueToJavaObject(JEnv &jEnv, const engine::Value &value,
                                            int classReturnType) {
    JsArgToArrayConverter argConverter(engineHost->GetRuntime(), value,
                                       false /*is implementation object*/,
                                       classReturnType);
    jobject jr = argConverter.GetConvertedArg();
    jobject javaResult = nullptr;
    if (jr != nullptr) {
        javaResult = jEnv.NewLocalRef(jr);
    }

    return javaResult;
}

void
Runtime::PassExceptionToJsNative(JNIEnv *jEnv, jobject obj, jthrowable exception, jstring message,
                                 jstring fullStackTrace, jstring jsStackTrace,
                                 jboolean isDiscarded, jboolean isPendingError) {
    engine::Runtime &rt = engineHost->GetRuntime();

    std::string errMsg = ArgConverter::jstringToString(message);

    engine::Object errObj = GlobalHelpers::CreateError(rt, errMsg);

    // Create a new native exception js object
    jint javaObjectID = m_objectManager->GetOrCreateObjectId((jobject) exception);
    engine::Value nativeExceptionObject = m_objectManager->GetJsObjectByJavaObject(javaObjectID);

    if (nativeExceptionObject.isUndefined() || nativeExceptionObject.isNull()) {
        std::string className = m_objectManager->GetClassName((jobject) exception);
        // Create proxy object that wraps the java err
        nativeExceptionObject = m_objectManager->CreateJSWrapper(javaObjectID, className);
        if (nativeExceptionObject.isUndefined() || nativeExceptionObject.isNull()) {
            nativeExceptionObject = engine::Value(rt, engine::Object(rt));
        }
    }

    // Create a JS error object
    errObj.setProperty(rt, "nativeException", nativeExceptionObject);
    errObj.setProperty(rt, "stackTrace",
                       engine::String::createFromUtf8(
                               rt, ArgConverter::jstringToString(fullStackTrace)));
    if (jsStackTrace != nullptr) {
        errObj.setProperty(rt, "stack",
                           engine::String::createFromUtf8(
                                   rt, ArgConverter::jstringToString(jsStackTrace)));
    }

    // Pass err to JS
    NativeScriptException::CallJsFuncWithErr(rt, engine::Value(rt, errObj), isDiscarded);
}

#if !defined(NS_JSI_HOST_RUNTIME)
void
Runtime::PassUncaughtExceptionFromWorkerToMainHandler(const engine::Value &message,
                                                      const engine::Value &stackTrace,
                                                      const engine::Value &filename, int lineno) {
    JEnv jEnv;
    engine::Runtime &rt = engineHost->GetRuntime();
    auto runtimeClass = jEnv.GetObjectClass(m_runtime);


    auto mId = jEnv.GetStaticMethodID(runtimeClass, "passUncaughtExceptionFromWorkerToMain",
                                      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");

    auto jMsg = ArgConverter::ConvertToJavaString(rt, message);
    auto jfileName = ArgConverter::ConvertToJavaString(rt, filename);
    auto stckTrace = ArgConverter::ConvertToJavaString(rt, stackTrace);

    JniLocalRef jMsgLocal(jMsg);
    JniLocalRef jfileNameLocal(jfileName);
    JniLocalRef stTrace(stckTrace);

    jEnv.CallStaticVoidMethod(runtimeClass, mId, (jstring) jMsgLocal, (jstring) jfileNameLocal,
                              (jstring) stTrace, (jint) lineno);
}
#endif


#if !defined(NS_JSI_HOST_RUNTIME)
void Runtime::SetManualInstrumentationMode(jstring mode) {
    auto modeStr = ArgConverter::jstringToString(mode);
    if (modeStr == "timeline") {
        tns::instrumentation::Frame::enable();
    }
}
#endif

void Runtime::Lock() {
#ifdef APPLICATION_IN_DEBUG
    m_fileWriteMutex.lock();
#endif
}

void Runtime::Unlock() {
#ifdef APPLICATION_IN_DEBUG
    m_fileWriteMutex.unlock();
#endif
}


JavaVM *Runtime::java_vm = nullptr;
jmethodID Runtime::GET_USED_MEMORY_METHOD_ID = nullptr;
tns::ConcurrentMap<int, Runtime *> Runtime::id_to_runtime_cache;
tns::ConcurrentMap<const void *, Runtime *> Runtime::rt_to_runtime_cache;
bool Runtime::s_mainThreadInitialized = false;
int Runtime::m_androidVersion = Runtime::GetAndroidVersion();
ALooper *Runtime::m_mainLooper = nullptr;
tns::ConcurrentMap<std::thread::id, Runtime *> Runtime::thread_id_to_rt_cache;

int Runtime::m_mainLooper_fd[2];

Runtime *Runtime::s_main_rt = nullptr;
std::thread::id Runtime::s_main_thread_id;
