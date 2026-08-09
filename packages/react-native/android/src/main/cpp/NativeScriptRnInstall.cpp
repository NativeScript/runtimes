// The guest entry point: binds the NativeScript runtime to the jsi::Runtime
// React Native owns.
//
// This is the Android counterpart of ffi/objc/hermes/NativeApiJsi.mm, and it is
// deliberately small. Everything that makes NativeScript work -- the metadata
// tree, the object manager, the JNI interop -- is the same code the standalone
// runtime compiles; all that differs is who created the runtime it installs
// into. See NativeScript/runtime/android/jsi/EngineHost.h.

#include <jni.h>
#include <jsi/jsi.h>

#include <string>

#include "ArgConverter.h"
#include "NativeScriptException.h"
#include "Runtime.h"

using namespace tns;

namespace {

// Set once the first attach succeeds. React Native can reload the JS bundle,
// which builds a fresh jsi::Runtime; the module checks this before installing
// so a reload rebinds rather than stacking a second runtime on the first.
bool g_attached = false;

}  // namespace

extern "C" JNIEXPORT void JNICALL Java_com_tns_Runtime_attachNativeScript(
        JNIEnv* env, jobject obj, jint runtimeId, jstring metadataPath, jlong hostRuntimePtr,
        jboolean verboseLoggingEnabled, jint maxLogcatObjectSize, jboolean forceLog) {
    try {
        if (hostRuntimePtr == 0) {
            throw NativeScriptException(
                    "No JavaScript runtime was provided. NativeScript needs the host's "
                    "jsi::Runtime, which React Native exposes through its "
                    "JavaScriptContextHolder.");
        }

        auto* hostRuntime = reinterpret_cast<facebook::jsi::Runtime*>(hostRuntimePtr);
        std::string metadataDir = ArgConverter::jstringToString(metadataPath);

        Runtime::Attach(env, obj, runtimeId, *hostRuntime, metadataDir,
                        verboseLoggingEnabled == JNI_TRUE, maxLogcatObjectSize,
                        forceLog == JNI_TRUE);
        g_attached = true;
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

extern "C" JNIEXPORT jboolean JNICALL
Java_org_nativescript_reactnative_NativeScriptNativeApiModule_nativeIsInstalled(JNIEnv*, jobject) {
    return g_attached ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nativescript_reactnative_NativeScriptNativeApiModule_nativeRuntimeBackend(JNIEnv* env,
                                                                                   jobject) {
    // What the *engine layer* is, not which VM sits underneath: a guest build
    // talks jsi and cannot tell Hermes from JSC, and does not need to.
    return env->NewStringUTF("jsi");
}
