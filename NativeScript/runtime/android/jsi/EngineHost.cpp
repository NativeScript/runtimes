#include "EngineHost.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "File.h"
#include "Runtime.h"

#if defined(TARGET_ENGINE_V8)
#include <libplatform/libplatform.h>
#endif

// Only the VM-owning build needs the Hermes VM headers; see EngineHost.h.
#if defined(TARGET_ENGINE_HERMES) && !defined(NS_JSI_HOST_RUNTIME)
#include <hermes/hermes.h>
#endif

#if defined(TARGET_ENGINE_JSC)
// JSGlobalContextSetUnhandledRejectionCallback lives in this private JSC header.
#include <JavaScriptCore/JSContextRefPrivate.h>
#endif

#if defined(TARGET_ENGINE_QUICKJS)
#ifdef USE_MIMALLOC
#include "mimalloc.h"

#ifdef __QJS_NG__
static void *js_mi_calloc(void *, size_t count, size_t size) {
    return mi_calloc(count, size);
}

static void *js_mi_malloc(void *, size_t size) { return mi_malloc(size); }

static void js_mi_free(void *, void *ptr) {
    if (ptr != nullptr) mi_free(ptr);
}

static void *js_mi_realloc(void *, void *ptr, size_t size) {
    return mi_realloc(ptr, size);
}

static const JSMallocFunctions kMiMallocFunctions = {
        js_mi_calloc, js_mi_malloc, js_mi_free, js_mi_realloc,
        mi_malloc_usable_size};
#else
#define NS_MALLOC_OVERHEAD 8

static void *js_mi_malloc(JSMallocState *s, size_t size) {
    if (s->malloc_size + size > s->malloc_limit) return nullptr;
    void *ptr = mi_malloc(size);
    if (ptr == nullptr) return nullptr;
    s->malloc_count++;
    s->malloc_size += mi_malloc_usable_size(ptr) + NS_MALLOC_OVERHEAD;
    return ptr;
}

static void js_mi_free(JSMallocState *s, void *ptr) {
    if (ptr == nullptr) return;
    s->malloc_count--;
    s->malloc_size -= mi_malloc_usable_size(ptr) + NS_MALLOC_OVERHEAD;
    mi_free(ptr);
}

static void *js_mi_realloc(JSMallocState *s, void *ptr, size_t size) {
    if (ptr == nullptr) {
        if (size == 0) return nullptr;
        return js_mi_malloc(s, size);
    }
    const size_t oldSize = mi_malloc_usable_size(ptr);
    if (size == 0) {
        s->malloc_count--;
        s->malloc_size -= oldSize + NS_MALLOC_OVERHEAD;
        mi_free(ptr);
        return nullptr;
    }
    if (s->malloc_size + size - oldSize > s->malloc_limit) return nullptr;
    ptr = mi_realloc(ptr, size);
    if (ptr == nullptr) return nullptr;
    s->malloc_size += mi_malloc_usable_size(ptr) - oldSize;
    return ptr;
}

static const JSMallocFunctions kMiMallocFunctions = {js_mi_malloc, js_mi_free,
                                                     js_mi_realloc,
                                                     mi_malloc_usable_size};
#endif  // __QJS_NG__
#endif  // USE_MIMALLOC

// `globalThis.gc`. This QuickJS is patched locally so that js_weakref_constructor
// pins its target via JS_KeepWeakRefTargetAlive at construction; the pin is only
// released by JS_ClearWeakRefKeepAlives, so without the clear nothing weakly
// referenced is ever collectable.
static JSValue nsRunGC(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_ClearWeakRefKeepAlives(rt);
    JS_RunGC(rt);
    return JS_UNDEFINED;
}

#endif  // TARGET_ENGINE_QUICKJS

using namespace tns;

void EngineHost::SetFlags(const char *flags) {
    if (flags == nullptr || *flags == '\0') return;
#if defined(TARGET_ENGINE_V8)
    v8::V8::SetFlagsFromString(flags);
#endif
}

#if defined(NS_JSI_HOST_RUNTIME)

std::shared_ptr<EngineHost> EngineHost::Adopt(::facebook::jsi::Runtime &runtime) {
    // Not make_shared: the constructor is private.
    std::shared_ptr<EngineHost> host(new EngineHost());
    // The non-owning engine::Runtime constructor (jsi/hermes/HermesRuntime.h).
    // There is no RuntimeConfig to mirror from the Create() path below: the
    // embedder already built and configured the VM, and re-deciding any of it
    // here would either be ignored or corrupt a runtime other code is using.
    host->m_runtime = std::make_unique<engine::Runtime>(runtime);
    return host;
}

#else

std::shared_ptr<EngineHost> EngineHost::Create() {
    // Not make_shared: the constructor is private.
    std::shared_ptr<EngineHost> host(new EngineHost());

#if defined(TARGET_ENGINE_V8)
    // Process-global, and exactly once: V8 aborts if the platform is initialised
    // twice, and every Worker creates a runtime. The isolate below stays
    // per-runtime, which is what actually isolates a worker.
    static std::once_flag platformOnce;
    static std::unique_ptr<v8::Platform> platform;
    std::call_once(platformOnce, [] {
        v8::V8::InitializeICUDefaultLocation(nullptr);
        platform = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(platform.get());
        v8::V8::Initialize();
    });

    v8::Isolate::CreateParams params;
    host->m_allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    params.array_buffer_allocator = host->m_allocator.get();
    host->m_isolate = v8::Isolate::New(params);

    {
        v8::Isolate::Scope isolateScope(host->m_isolate);
        v8::HandleScope handleScope(host->m_isolate);
        v8::Local<v8::Context> context = v8::Context::New(host->m_isolate);
        host->m_context.Reset(host->m_isolate, context);
        host->m_runtime = std::make_unique<engine::Runtime>(host->m_isolate, context);
    }
#elif defined(TARGET_ENGINE_HERMES)
    // Hermes has no isolate, locker or handle-scope model: the runtime object is
    // the whole of it. withMicrotaskQueue is what ExecutePendingJobs depends on.
    {
        ::hermes::vm::RuntimeConfig config =
                ::hermes::vm::RuntimeConfig::Builder()
                        .withMicrotaskQueue(true)
                        .withES6BlockScoping(true)
                        .withEnableAsyncGenerators(true)
                        .withAsyncBreakCheckInEval(true)
                        .build();
        host->m_threadSafe = facebook::hermes::makeThreadSafeHermesRuntime(config);
        host->m_runtime =
                std::make_unique<engine::Runtime>(host->m_threadSafe->getUnsafeRuntime());
    }
#elif defined(TARGET_ENGINE_QUICKJS)
    {
#ifdef USE_MIMALLOC
        host->m_jsRuntime = JS_NewRuntime2(&kMiMallocFunctions, nullptr);
#else
        host->m_jsRuntime = JS_NewRuntime();
#endif
        if (host->m_jsRuntime == nullptr) return nullptr;
        // 0 disables the stack-depth guard, which also removes any need to
        // re-record the stack top per thread; the runtime is entered from Java
        // threads.
        JS_SetMaxStackSize(host->m_jsRuntime, 0);

        host->m_jsContext = JS_NewContext(host->m_jsRuntime);
        if (host->m_jsContext == nullptr) {
            JS_FreeRuntime(host->m_jsRuntime);
            host->m_jsRuntime = nullptr;
            return nullptr;
        }

        JSValue global = JS_GetGlobalObject(host->m_jsContext);
        JS_SetPropertyStr(host->m_jsContext, global, "gc",
                          JS_NewCFunction(host->m_jsContext, nsRunGC, "gc", 0));
        JS_FreeValue(host->m_jsContext, global);

        host->m_runtime = std::make_unique<engine::Runtime>(host->m_jsContext);
    }
#elif defined(TARGET_ENGINE_JSC)
    // JSC has no isolate or locker to manage: JSGlobalContextRef is the whole of
    // it, and it takes its own JSLock internally on every API call.
    {
        host->m_jscContext = JSGlobalContextCreateInGroup(nullptr, nullptr);
        if (host->m_jscContext == nullptr) return nullptr;
        host->m_runtime = std::make_unique<engine::Runtime>(host->m_jscContext);

        engine::Runtime &rt = *host->m_runtime;
        JSGlobalContextRef context = host->m_jscContext;

        // `globalThis.gc`. JSC has no --expose_gc equivalent, so SetFlags cannot
        // supply it the way the V8 path does, and mainpage.js needs it.
        engine::Function gcFunction = engine::Function::createFromHostFunction(
                rt, engine::PropNameID::forAscii(rt, "gc"), 0,
                [context](engine::Runtime &, const engine::Value &, const engine::Value *,
                          size_t) -> engine::Value {
                    JSGarbageCollect(context);
                    return engine::Value::undefined();
                });
        rt.global().setProperty(rt, "gc", gcFunction);

        // Unhandled promise rejections, routed to the JS-side tracker that
        // ts_helpers.js installs. JSC only reports the "unhandled" event, never a
        // later retraction.
        engine::Function rejectionCallback = engine::Function::createFromHostFunction(
                rt, engine::PropNameID::forAscii(rt, "onUnhandledRejection"), 2,
                [](engine::Runtime &runtime, const engine::Value &, const engine::Value *args,
                   size_t count) -> engine::Value {
                    engine::Object global = runtime.global();
                    engine::Value tracker =
                            global.getProperty(runtime, "onUnhandledPromiseRejectionTracker");
                    if (tracker.isObject() && tracker.asObject(runtime).isFunction(runtime)) {
                        tracker.asObject(runtime).asFunction(runtime).callWithThis(
                                runtime, global, args, count);
                    }
                    return engine::Value::undefined();
                });
        // JSC keeps the callback alive: it is stored on, and marked by, the global
        // object.
        JSValueRef rejectionException = nullptr;
        JSGlobalContextSetUnhandledRejectionCallback(context, rejectionCallback.local(rt),
                                                     &rejectionException);
    }
#endif

    if (host->m_runtime == nullptr) return nullptr;
    return host;
}

#endif  // NS_JSI_HOST_RUNTIME

void EngineHost::Lock() {
    m_mutex.lock();
    m_lockDepth++;
#if defined(TARGET_ENGINE_HERMES) && !defined(NS_JSI_HOST_RUNTIME)
    // Registers the calling thread with the VM. Hermes records per-thread stack
    // bounds for its overflow guard, so entering from a Java thread without this
    // faults rather than raising a RangeError.
    if (m_threadSafe != nullptr) m_threadSafe->lock();
#endif
}

void EngineHost::Unlock() {
    if (m_lockDepth > 0) m_lockDepth--;
#if defined(TARGET_ENGINE_HERMES) && !defined(NS_JSI_HOST_RUNTIME)
    if (m_threadSafe != nullptr) m_threadSafe->unlock();
#endif
    m_mutex.unlock();
}

void EngineHost::ExecutePendingJobs() {
    if (m_runtime == nullptr) return;
#if defined(NS_JSI_HOST_RUNTIME)
    // The embedder owns the queue and decides when it runs; see ~JSScope.
#else
    m_runtime->drainMicrotasks();
#endif
}

engine::Value EngineHost::ExecuteScript(const std::string &source, const std::string &sourceURL) {
    auto buffer = std::make_shared<engine::StringBuffer>(source);
    return m_runtime->evaluateJavaScript(buffer, sourceURL);
}

namespace {

// The bytecode container written by tools/bytecode-compiler:
//   [8-byte magic][4-byte format version, little endian][engine payload]
// Hermes is the exception -- it stores raw HBC with its own magic and no
// container, so the whole file is the payload.
constexpr size_t kContainerHeaderLen = 12;

// Peeks the first 8 bytes of `path` and compares them with `magic8`. Cheap
// enough to run for every module: a bytecode build hits it on the fast path and
// a source build pays one fopen/fread of 8 bytes.
bool HasBytecodeMagic(const std::string &path, const void *magic8) {
    uint8_t head[8];
    FILE *fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) return false;
    size_t read = fread(head, 1, sizeof(head), fp);
    fclose(fp);
    return read == sizeof(head) && memcmp(head, magic8, sizeof(head)) == 0;
}

}  // namespace

bool EngineHost::ExecuteBytecodeFile(const std::string &path, const std::string &sourceURL,
                                     engine::Value &result) {
    if (m_runtime == nullptr) return false;

#if defined(TARGET_ENGINE_QUICKJS)
#ifdef __QJS_NG__
    static const char *kMagic = "NSBCNGS";  // 7 chars + NUL = 8-byte magic
#else
    static const char *kMagic = "NSBCQJS";
#endif
    if (!HasBytecodeMagic(path, kMagic)) return false;

    int length = 0;
    auto data = static_cast<uint8_t *>(File::ReadBinary(path, length));
    if (data == nullptr) return false;
    if (static_cast<size_t>(length) <= kContainerHeaderLen) {
        delete[] data;
        return false;
    }

    JSContext *ctx = m_runtime->context();
    // JS_ReadObject copies what it needs, so the buffer can be freed right after.
    JSValue funObj = JS_ReadObject(ctx, data + kContainerHeaderLen,
                                   static_cast<size_t>(length) - kContainerHeaderLen,
                                   JS_READ_OBJ_BYTECODE);
    delete[] data;

    // Past the magic check the file IS bytecode, so a failure here must be
    // surfaced rather than silently retried as source -- retrying would compile
    // the binary blob and produce a nonsense error much further away.
    if (JS_IsException(funObj)) {
        throw engine::quickjsengine::caughtError(*m_runtime, "QuickJS bytecode could not be read.");
    }

    // JS_EvalFunction consumes funObj.
    JSValue evalResult = JS_EvalFunction(ctx, funObj);
    if (JS_IsException(evalResult)) {
        throw engine::quickjsengine::caughtError(*m_runtime, "QuickJS bytecode evaluation failed.");
    }

    result = engine::Value(*m_runtime, evalResult);
    JS_FreeValue(ctx, evalResult);
    return true;
#elif defined(TARGET_ENGINE_HERMES)
    // Hermes bytecode (HBC) magic, first 8 bytes little endian
    // (0x1F1903C103BC1FC6).
    static const uint8_t kMagic[8] = {0xc6, 0x1f, 0xbc, 0x03, 0xc1, 0x03, 0x19, 0x1f};
    if (!HasBytecodeMagic(path, kMagic)) return false;

    int length = 0;
    auto data = static_cast<uint8_t *>(File::ReadBinary(path, length));
    if (data == nullptr) return false;

    // HermesRuntime::evaluateJavaScript detects an HBC buffer and skips the
    // parser, so the same entry point serves source and bytecode. The buffer is
    // binary and contains NULs, hence the (ptr, len) string constructor.
    auto buffer = std::make_shared<engine::StringBuffer>(
            std::string(reinterpret_cast<const char *>(data), static_cast<size_t>(length)));
    delete[] data;

    result = m_runtime->evaluateJavaScript(buffer, sourceURL);
    return true;
#else
    // V8 and JSC have no compile-time bytecode format; both cache compiled code
    // at runtime instead, so their release builds ship plain source.
    (void) path;
    (void) sourceURL;
    (void) result;
    return false;
#endif
}

int64_t EngineHost::AdjustExternalMemory(int64_t changeInBytes) {
#if defined(TARGET_ENGINE_V8)
    if (m_isolate != nullptr) {
        return m_isolate->AdjustAmountOfExternalAllocatedMemory(changeInBytes);
    }
#endif
    // Engines with no external-memory accounting report no change; the caller
    // uses this as a GC hint, not for correctness.
    (void) changeInBytes;
    return 0;
}

int64_t EngineHost::EnginePtr() const {
#if defined(TARGET_ENGINE_V8)
    return reinterpret_cast<int64_t>(m_isolate);
#elif defined(TARGET_ENGINE_QUICKJS)
    // The JSRuntime is QuickJS' closest analogue of an isolate: it owns the heap
    // and the GC.
    return reinterpret_cast<int64_t>(m_jsRuntime);
#else
    return 0;
#endif
}

const char *EngineHost::EngineVersion() const {
#if defined(TARGET_ENGINE_V8)
    return v8::V8::GetVersion();
#elif defined(TARGET_ENGINE_QUICKJS)
#ifdef __QJS_NG__
    return "QuickJS-NG";
#else
    return "QuickJS";
#endif
#else
    return "unknown";
#endif
}

void EngineHost::ReleaseEngineState() {
    m_runtime.reset();
}

EngineHost::~EngineHost() {
    // Under NS_JSI_HOST_RUNTIME this drops the non-owning engine::Runtime
    // wrapper only; the jsi::Runtime behind it belongs to the embedder.
    m_runtime.reset();
#if defined(TARGET_ENGINE_HERMES) && !defined(NS_JSI_HOST_RUNTIME)
    m_threadSafe.reset();
#endif
#if defined(TARGET_ENGINE_QUICKJS)
    if (m_jsContext != nullptr) {
        engine::quickjsengine::releaseStateForContext(m_jsContext);
        JS_FreeContext(m_jsContext);
        m_jsContext = nullptr;
    }
    if (m_jsRuntime != nullptr) {
        JS_FreeRuntime(m_jsRuntime);
        m_jsRuntime = nullptr;
    }
#endif
#if defined(TARGET_ENGINE_JSC)
    if (m_jscContext != nullptr) {
        JSGlobalContextRelease(m_jscContext);
        m_jscContext = nullptr;
    }
#endif
#if defined(TARGET_ENGINE_V8)
    m_context.Reset();
    if (m_isolate != nullptr) {
        m_isolate->Dispose();
        m_isolate = nullptr;
    }
#endif
}

std::shared_ptr<EngineHost> tns::JSScope::HostFor(engine::Runtime &rt) {
    return tns::Runtime::GetRuntime(rt)->GetEngineHost();
}
