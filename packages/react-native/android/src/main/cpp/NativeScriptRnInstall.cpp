// The guest entry point: binds the NativeScript runtime to the jsi::Runtime
// React Native owns.
//
// This is the Android counterpart of ffi/objc/hermes/NativeApiJsi.mm, and it is
// deliberately small. Everything that makes NativeScript work -- the metadata
// tree, the object manager, the JNI interop -- is the same code the standalone
// runtime compiles; all that differs is who created the runtime it installs
// into. See NativeScript/runtime/android/jsi/EngineHost.h.

#include <android/log.h>
#include <jni.h>
#include <jsi/jsi.h>
#include <unistd.h>

#include <exception>
#include <string>

#include "ArgConverter.h"
#include "NativeScriptException.h"
#include "Runtime.h"

using namespace tns;

namespace {

jmethodID g_runOnMainThread = nullptr;
jclass g_viewRegistry = nullptr;
jmethodID g_viewHandleFor = nullptr;
jmethodID g_viewContext = nullptr;

bool IsMainThread() { return gettid() == getpid(); }

// Handed to the main thread by address, through com.tns.Runtime.runOnMainThread,
// while the JS thread that owns it waits.
struct MainThreadTask {
    explicit MainThreadTask(engine::Value function) : function(std::move(function)) {}

    engine::Value function;
    engine::Value result;
    std::exception_ptr error;
    bool ran = false;
};

Runtime* FindRuntime(int runtimeId) {
    try {
        return Runtime::GetRuntime(runtimeId);
    } catch (NativeScriptException&) {
        return nullptr;
    }
}

engine::Value RunOnMainThread(int runtimeId, engine::Runtime& rt, const engine::Value* args,
                              size_t count) {
    if (count < 1 || !args[0].isObject() || !args[0].asObjectBorrowed(rt).isFunction(rt)) {
        throw engine::JSError(rt, "__nativeScriptRunOnMainThread expects a function");
    }
    if (IsMainThread()) {
        return args[0].asObject(rt).asFunction(rt).call(rt);
    }

    Runtime* runtime = FindRuntime(runtimeId);
    if (runtime == nullptr || runtime->is_destroying) {
        throw engine::JSError(rt, "NativeScript is no longer attached to this JavaScript runtime");
    }

    MainThreadTask task(engine::Value(rt, args[0]));
    JEnv env;
    env.CallVoidMethod(runtime->GetJavaRuntime(), g_runOnMainThread,
                       static_cast<jlong>(reinterpret_cast<intptr_t>(&task)));
    if (env.ExceptionCheck()) {
        NativeScriptException(env).ReThrowToJs(rt);
    }
    if (task.error) {
        std::rethrow_exception(task.error);
    }
    if (!task.ran) {
        throw engine::JSError(rt, "The main thread did not run the function: the app is shutting down");
    }
    return std::move(task.result);
}

engine::Value ViewHandle(int runtimeId, engine::Runtime& rt, const engine::Value* args,
                         size_t count) {
    Runtime* runtime = FindRuntime(runtimeId);
    JniLocalRef view;
    if (runtime != nullptr && count > 0 && args[0].isObject()) {
        view = runtime->GetObjectManager()->GetJavaObjectByJsObject(args[0]);
    }
    if (view.IsNull()) {
        throw engine::JSError(rt, "nativeViewHandle expects an android.view.View");
    }

    JEnv env;
    JniLocalRef handle(env.CallStaticObjectMethod(g_viewRegistry, g_viewHandleFor, (jobject) view));
    if (env.ExceptionCheck()) {
        NativeScriptException(env).ReThrowToJs(rt);
    }
    if (handle.IsNull()) {
        throw engine::JSError(rt, "nativeViewHandle expects an android.view.View");
    }
    return ArgConverter::jstringToJsString(rt, (jstring) (jobject) handle);
}

engine::Value ViewContext(int runtimeId, engine::Runtime& rt) {
    Runtime* runtime = FindRuntime(runtimeId);
    if (runtime == nullptr) {
        return engine::Value::null();
    }
    JEnv env;
    JniLocalRef context(env.CallStaticObjectMethod(g_viewRegistry, g_viewContext));
    if (env.ExceptionCheck()) {
        NativeScriptException(env).ReThrowToJs(rt);
    }
    if (context.IsNull()) {
        return engine::Value::null();
    }
    ObjectManager* objectManager = runtime->GetObjectManager();
    jint id = objectManager->GetOrCreateObjectId(context);
    engine::Value value = objectManager->GetJsObjectByJavaObject(id);
    if (js_util::is_null_or_undefined(value)) {
        value = objectManager->CreateJSWrapper(id, ObjectManager::GetClassName((jobject) context));
    }
    return value;
}

// What makes Android views reachable: run on the main thread, a Context to build
// a View with, and a name for a NativeScriptUIView to show it by. src/index.ts
// wraps them.
void InstallGuestFunctions(JNIEnv* env, jobject javaRuntime, Runtime* runtime) {
    if (g_runOnMainThread == nullptr) {
        g_runOnMainThread =
                env->GetMethodID(env->GetObjectClass(javaRuntime), "runOnMainThread", "(J)V");
        // The class has to be found here, on a thread entered from Java: FindClass
        // on any other thread searches only the system class loader.
        jclass registry = env->FindClass("org/nativescript/reactnative/NativeScriptViewRegistry");
        g_viewRegistry = static_cast<jclass>(env->NewGlobalRef(registry));
        g_viewHandleFor = env->GetStaticMethodID(g_viewRegistry, "handleFor",
                                                 "(Ljava/lang/Object;)Ljava/lang/String;");
        g_viewContext = env->GetStaticMethodID(g_viewRegistry, "viewContext",
                                               "()Landroid/content/Context;");
        env->DeleteLocalRef(registry);
    }

    JSScope scope(runtime->GetEngineHost());
    engine::Runtime& rt = runtime->GetEngineHost()->GetRuntime();
    engine::Object global = rt.global();
    const int runtimeId = runtime->GetId();

    engine_util::SetFunction(
            rt, global, "__nativeScriptIsMainThread",
            [](engine::Runtime&, const engine::Value&, const engine::Value*, size_t) {
                return engine::Value(IsMainThread());
            });
    engine_util::SetFunction(
            rt, global, "__nativeScriptRunOnMainThread",
            [runtimeId](engine::Runtime& rt, const engine::Value&, const engine::Value* args,
                        size_t count) { return RunOnMainThread(runtimeId, rt, args, count); },
            1);
    engine_util::SetFunction(
            rt, global, "__nativeScriptAndroidContext",
            [runtimeId](engine::Runtime& rt, const engine::Value&, const engine::Value*, size_t) {
                return ViewContext(runtimeId, rt);
            });
    engine_util::SetFunction(
            rt, global, "__nativeScriptAndroidViewHandle",
            [runtimeId](engine::Runtime& rt, const engine::Value&, const engine::Value* args,
                        size_t count) { return ViewHandle(runtimeId, rt, args, count); },
            1);
}

}  // namespace

extern "C" JNIEXPORT void JNICALL Java_com_tns_Runtime_attachNativeScript(
        JNIEnv* env, jobject obj, jint runtimeId, jstring metadataPath, jlong hostRuntimePtr,
        jboolean verboseLoggingEnabled, jint maxLogcatObjectSize, jboolean forceLog,
        jstring bootstrapScript) {
    try {
        if (hostRuntimePtr == 0) {
            throw NativeScriptException(
                    "No JavaScript runtime was provided. NativeScript needs the host's "
                    "jsi::Runtime, which React Native exposes through its "
                    "JavaScriptContextHolder.");
        }

        auto* hostRuntime = reinterpret_cast<facebook::jsi::Runtime*>(hostRuntimePtr);
        std::string metadataDir = ArgConverter::jstringToString(metadataPath);

        Runtime* runtime = Runtime::Attach(env, obj, runtimeId, *hostRuntime, metadataDir,
                                           verboseLoggingEnabled == JNI_TRUE, maxLogcatObjectSize,
                                           forceLog == JNI_TRUE);

        // ts_helpers.js. The standalone runtime runs this as its first module;
        // there is no module loader here, so the embedder reads it out of the
        // library's assets and hands the source over. Without it there is no
        // __extends, no JavaProxy and -- the one that matters most -- no
        // __createNativeProxy, which every Java object handed to JS goes through.
        if (bootstrapScript != nullptr) {
            std::string source = ArgConverter::jstringToString(bootstrapScript);
            if (!source.empty()) {
                JSScope scope(runtime->GetEngineHost());
                runtime->GetEngineHost()->ExecuteScript(source, "internal/ts_helpers.js");
            }
        }

        InstallGuestFunctions(env, obj, runtime);
    } catch (NativeScriptException& e) {
        e.ReThrowToJava(nullptr);
    } catch (std::exception& e) {
        NativeScriptException nsEx(std::string("Error: c++ exception: ") + e.what());
        nsEx.ReThrowToJava(nullptr);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJava(nullptr);
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_tns_Runtime_detachNativeScript(JNIEnv*, jobject,
                                                                         jint runtimeId) {
    try {
        Runtime::GetRuntime(runtimeId)->Detach();
    } catch (NativeScriptException& e) {
        e.ReThrowToJava(nullptr);
    } catch (std::exception& e) {
        NativeScriptException nsEx(std::string("Error: c++ exception: ") + e.what());
        nsEx.ReThrowToJava(nullptr);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJava(nullptr);
    }
}

// Per host runtime: a reload replaces the jsi::Runtime, and the new one has
// none of NativeScript's globals until it is attached in turn.
extern "C" JNIEXPORT jboolean JNICALL
Java_org_nativescript_reactnative_NativeScriptNativeApiModule_nativeIsInstalled(
        JNIEnv*, jobject, jlong hostRuntimePtr) {
    if (hostRuntimePtr == 0) {
        return JNI_FALSE;
    }
    // Never dereferenced: the wrapper only carries the pointer as its identity.
    engine::Runtime probe(*reinterpret_cast<facebook::jsi::Runtime*>(hostRuntimePtr));
    Runtime* runtime = Runtime::GetRuntimeUnchecked(probe);
    return (runtime != nullptr && !runtime->is_destroying) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nativescript_reactnative_NativeScriptNativeApiModule_nativeRuntimeBackend(JNIEnv* env,
                                                                                   jobject) {
    // What the *engine layer* is, not which VM sits underneath: a guest build
    // talks jsi and cannot tell Hermes from JSC, and does not need to.
    return env->NewStringUTF("jsi");
}

// The guest half of com.tns.MainThreadLoan.

extern "C" JNIEXPORT void JNICALL Java_com_tns_Runtime_runMainThreadTaskNative(JNIEnv*, jobject,
                                                                              jint runtimeId,
                                                                              jlong taskPtr) {
    auto* task = reinterpret_cast<MainThreadTask*>(static_cast<intptr_t>(taskPtr));
    try {
        Runtime* runtime = Runtime::GetRuntime(runtimeId);
        JSScope scope(runtime->GetEngineHost());
        engine::Runtime& rt = runtime->GetEngineHost()->GetRuntime();
        task->result = task->function.asObject(rt).asFunction(rt).call(rt);
    } catch (...) {
        task->error = std::current_exception();
    }
    task->ran = true;
}

extern "C" JNIEXPORT jint JNICALL Java_com_tns_Runtime_releaseEngineLockNative(JNIEnv*, jobject,
                                                                              jint runtimeId) {
    Runtime* runtime = FindRuntime(runtimeId);
    return runtime != nullptr ? runtime->GetEngineHost()->ReleaseAll() : 0;
}

extern "C" JNIEXPORT void JNICALL Java_com_tns_Runtime_reacquireEngineLockNative(JNIEnv*, jobject,
                                                                                jint runtimeId,
                                                                                jint depth) {
    Runtime* runtime = FindRuntime(runtimeId);
    if (runtime != nullptr) {
        runtime->GetEngineHost()->Reacquire(depth);
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_tns_Runtime_drainMicrotasksNative(JNIEnv*, jobject,
                                                                            jint runtimeId) {
    Runtime* runtime = FindRuntime(runtimeId);
    if (runtime == nullptr || runtime->is_destroying) {
        return;
    }
    JSScope scope(runtime->GetEngineHost());
    facebook::jsi::Runtime& rt = runtime->GetEngineHost()->GetRuntime().jsi();
    try {
        rt.drainMicrotasks();
    } catch (facebook::jsi::JSError& e) {
        // Where React Native sends an error thrown by one of its own tasks.
        try {
            auto errorUtils = rt.global().getProperty(rt, "ErrorUtils");
            if (errorUtils.isObject()) {
                errorUtils.asObject(rt)
                        .getPropertyAsFunction(rt, "reportFatalError")
                        .call(rt, e.value());
                return;
            }
        } catch (std::exception&) {
        }
        __android_log_print(ANDROID_LOG_ERROR, "TNS.Native", "Uncaught error in a microtask: %s",
                            e.what());
    } catch (std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "TNS.Native", "Uncaught error in a microtask: %s",
                            e.what());
    }
}
