#ifndef NS_RUNTIME_ANDROID_JSI_ENGINE_HOST_H
#define NS_RUNTIME_ANDROID_JSI_ENGINE_HOST_H

// The jsi tree's replacement for the per-engine napi/<engine>/jsr.h.
//
// The napi runtime reaches the engine through the JSR contract (js_create_runtime,
// js_create_napi_env, js_execute_script, ...) plus that engine's NapiScope. There
// is no Node-API here, so the same two jobs -- own one engine, and enter it from
// the host -- are done by EngineHost and JSScope.
//
// EngineHost is held by shared_ptr and JSScope keeps a strong reference for its
// lifetime. That is what makes teardown safe by construction: the VM, and the
// recursive mutex the scope holds, cannot be freed while a scope is still
// unwinding over them. See Runtime::DisposeWorkerRuntime.

#include <memory>
#include <mutex>
#include <string>

#if defined(TARGET_ENGINE_V8)
#include "jsi/v8/V8Runtime.h"
#elif defined(TARGET_ENGINE_JSC)
#include "jsi/jsc/JSCRuntime.h"
#elif defined(TARGET_ENGINE_QUICKJS)
#include "jsi/quickjs/QuickJSRuntime.h"
#elif defined(TARGET_ENGINE_HERMES)
#include "jsi/hermes/HermesRuntime.h"
#else
#error "The engine-native Android runtime needs a TARGET_ENGINE_* definition."
#endif

#if defined(TARGET_ENGINE_V8)
#include "v8.h"
#endif

// NS_JSI_HOST_RUNTIME is the guest flavour (@nativescript/react-native): the
// jsi::Runtime belongs to the embedder, which created it, drives its microtask
// queue and guarantees the thread it is entered on. There is no VM to build and
// no ThreadSafeRuntime to register threads with, so the two things that need
// the Hermes VM headers -- makeThreadSafeHermesRuntime and jsi/threadsafe.h --
// are the only pieces of the engine that this build does not compile. That is
// what lets the binary link nothing but the embedder's libjsi.so.
#if defined(TARGET_ENGINE_HERMES) && !defined(NS_JSI_HOST_RUNTIME)
#include <jsi/threadsafe.h>
#endif

namespace engine = ::nativescript::engine;

namespace tns {

class EngineHost {
public:
    // Engine command-line flags (package.json's android.v8Flags), applied before
    // any runtime exists. The test app ships "--expose_gc" and mainpage.js does
    // `__collect = gc;` on its sixth line, so this is not optional on V8.
    static void SetFlags(const char *flags);

#if defined(NS_JSI_HOST_RUNTIME)
    // Adopts a runtime the embedder owns. The reference must outlive this host:
    // React Native tears its runtime down only after the TurboModule that holds
    // us has been invalidated, which is what ReleaseEngineState is wired to.
    static std::shared_ptr<EngineHost> Adopt(::facebook::jsi::Runtime &runtime);
#else
    static std::shared_ptr<EngineHost> Create();
#endif

    ~EngineHost();

    EngineHost(const EngineHost &) = delete;

    EngineHost &operator=(const EngineHost &) = delete;

    engine::Runtime &GetRuntime() const { return *m_runtime; }

    // Alive until ReleaseEngineState runs; callers that can be reached during
    // teardown must check.
    bool IsAlive() const { return m_runtime != nullptr; }

    void Lock();

    void Unlock();

    int LockDepth() const { return m_lockDepth; }

    void ExecutePendingJobs();

    engine::Value ExecuteScript(const std::string &source, const std::string &sourceURL);

    // Runs `path` as ahead-of-time compiled bytecode when the file carries this
    // engine's bytecode header, and returns true. Returns false when the file is
    // not bytecode for this engine -- including on V8 and JSC, which have no
    // compile-time bytecode format at all -- and the caller then compiles the
    // source as usual.
    //
    // This is the jsi tree's counterpart to js_run_bytecode_file. It lives here
    // rather than in NativeScript/jsi/ because the container format is a build
    // artefact of the Android toolchain (tools/bytecode-compiler), not part of
    // the engine abstraction Apple shares.
    bool ExecuteBytecodeFile(const std::string &path, const std::string &sourceURL,
                             engine::Value &result);

    int64_t AdjustExternalMemory(int64_t changeInBytes);

    int64_t EnginePtr() const;

    const char *EngineVersion() const;

    // Drops the engine::Runtime while the VM is still standing. Everything that
    // holds an engine handle must already be gone; the VM itself is torn down in
    // ~EngineHost, which cannot run before the last JSScope has unwound.
    void ReleaseEngineState();

#if defined(TARGET_ENGINE_V8)
    v8::Isolate *Isolate() const { return m_isolate; }

    v8::Local<v8::Context> Context() const { return m_context.Get(m_isolate); }
#endif

private:
    EngineHost() = default;

    std::unique_ptr<engine::Runtime> m_runtime;
    std::recursive_mutex m_mutex;
    int m_lockDepth = 0;

#if defined(TARGET_ENGINE_V8)
    v8::Isolate *m_isolate = nullptr;
    v8::Global<v8::Context> m_context;
    std::unique_ptr<v8::ArrayBuffer::Allocator> m_allocator;
#endif

#if defined(TARGET_ENGINE_HERMES) && !defined(NS_JSI_HOST_RUNTIME)
    // Owns the VM. engine::Runtime only borrows getUnsafeRuntime().
    std::unique_ptr<facebook::jsi::ThreadSafeRuntime> m_threadSafe;
#endif

#if defined(TARGET_ENGINE_QUICKJS)
    JSRuntime *m_jsRuntime = nullptr;
    JSContext *m_jsContext = nullptr;
#endif

#if defined(TARGET_ENGINE_JSC)
    JSGlobalContextRef m_jscContext = nullptr;
#endif
};

// Entering JS from the host.
//
// Stack-only: v8::HandleScope has a private operator new and cannot be held
// across calls, so the engine scopes have to be members of an object that lives
// on the caller's stack.
class JSScope {
public:
    // The napi tree enters JS as `NapiScope scope(env)`; the same call sites here
    // only have the engine::Runtime, so this resolves its owning EngineHost.
    explicit JSScope(engine::Runtime &rt) : JSScope(HostFor(rt)) {}

    explicit JSScope(std::shared_ptr<EngineHost> host)
            : m_host(std::move(host))
#if defined(TARGET_ENGINE_V8)
            , m_locker(m_host->Isolate())
            , m_isolateScope(m_host->Isolate())
            , m_handleScope(m_host->Isolate())
            , m_context(m_host->Context())
            , m_contextScope(m_context)
#endif
    {
    }

    ~JSScope() {
#if (defined(TARGET_ENGINE_HERMES) && !defined(NS_JSI_HOST_RUNTIME)) || \
        defined(TARGET_ENGINE_QUICKJS)
        // Hermes and QuickJS run Promise jobs from an explicit microtask queue and
        // nothing else on Android drains it; V8 and JSC run theirs themselves when
        // the host stack unwinds. Draining as the *outermost* scope leaves JS is
        // what keeps the ordering the specs assert: a microtask queued during a
        // call runs before the next timer callback, which enters through its own
        // scope.
        //
        // NS_JSI_HOST_RUNTIME is excluded because the embedder already drains:
        // React Native's RuntimeScheduler runs the queue at the end of each task
        // it dispatches. Draining from inside a nested scope of ours would run
        // application microtasks at a point RN did not choose.
        if (m_host->LockDepth() <= 1 && m_host->IsAlive()) {
            m_host->ExecutePendingJobs();
        }
#endif
    }

    JSScope(const JSScope &) = delete;

    JSScope &operator=(const JSScope &) = delete;

private:
    // Defined in EngineHost.cpp, which can include Runtime.h; the header cannot
    // (Runtime.h includes this one).
    static std::shared_ptr<EngineHost> HostFor(engine::Runtime &rt);

    // Declaration order is load-bearing: members are destroyed in reverse, which
    // is the order V8 requires -- the lock is taken before the isolate is
    // entered, and the context scope unwinds before the isolate does.
    struct HostLock {
        std::shared_ptr<EngineHost> host;

        explicit HostLock(std::shared_ptr<EngineHost> h) : host(std::move(h)) { host->Lock(); }

        ~HostLock() { host->Unlock(); }

        EngineHost *operator->() const { return host.get(); }
    };

    HostLock m_host;
#if defined(TARGET_ENGINE_V8)
    v8::Locker m_locker;
    v8::Isolate::Scope m_isolateScope;
    v8::HandleScope m_handleScope;
    // Held as a member so it outlives the scope that references it; passing
    // Context() straight into Context::Scope would bind a temporary.
    v8::Local<v8::Context> m_context;
    v8::Context::Scope m_contextScope;
#endif
};

// Mirrors the napi tree's JSEnterScope, which expands to an engine-specific
// scope object. `engineHost` is the member name Runtime uses.
#define JSEnterScope tns::JSScope __ns_enter_scope(engineHost);

// The engine:: equivalents of the napi_util helpers the runtime uses when
// installing its globals.
namespace engine_util {

inline void SetFunction(engine::Runtime &rt, engine::Object &target, const char *name,
                        engine::HostFunctionType callback, unsigned int paramCount = 0) {
    target.setProperty(rt, name,
                       engine::Function::createFromHostFunction(
                               rt, engine::PropNameID::forAscii(rt, name), paramCount,
                               std::move(callback)));
}

}

}

#endif //NS_RUNTIME_ANDROID_JSI_ENGINE_HOST_H
