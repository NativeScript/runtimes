#include "ModuleInternal.h"
#include "File.h"
#include "JniLocalRef.h"
#include "ArgConverter.h"
#include "NativeScriptAssert.h"
#include "Constants.h"
#include "NativeScriptException.h"
#include "Util.h"
#include "CallbackHandlers.h"
#include "Runtime.h"
#include <sstream>
#include <mutex>
#include <libgen.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <ctime>
#include "GlobalHelpers.h"
#include <utime.h>



using namespace tns;
using namespace std;

ModuleInternal::ModuleInternal()
    : m_rt(nullptr) {
}

void ModuleInternal::DeInit() {
    // Dropping the owned values is the whole of it: there are no reference
    // counts to unwind, and the engine collects once nothing holds a handle.
    m_requireFunction = engine::Value::undefined();
    m_requireFactoryFunction = engine::Value::undefined();
    this->m_requireCache.clear();
    this->m_loadedModules.clear();
}

void ModuleInternal::Init(engine::Runtime& rt, const std::string& baseDir) {
    JEnv jenv;

    if (MODULE_CLASS == nullptr) {
        MODULE_CLASS = jenv.FindClass("com/tns/Module");
        assert(MODULE_CLASS != nullptr);

        RESOLVE_PATH_METHOD_ID = jenv.GetStaticMethodID(MODULE_CLASS, "resolvePath", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        assert(RESOLVE_PATH_METHOD_ID != nullptr);
    }

    m_rt = &rt;

    const char *requireFactoryScript = R"(
    (function () {
        return function require_factory(requireInternal, dirName) {
		return function require(modulePath) {
            if(typeof global.__requireOverride !== "undefined") {
				var result = global.__requireOverride(modulePath, dirName);
				if(result) {
					return result;
				}
			}
			return requireInternal(modulePath, dirName);
		}
	}
})();
)";

    engine::Object global = rt.global();

    m_requireFactoryFunction = Runtime::GetRuntime(rt)->GetEngineHost()->ExecuteScript(
            requireFactoryScript, "<require_factory>");

    engine::Function requireFunction = engine::Function::createFromHostFunction(
            rt, engine::PropNameID::forAscii(rt, "__nativeRequire"), 2,
            [this](engine::Runtime& runtime, const engine::Value&, const engine::Value* args,
                   size_t count) -> engine::Value {
                try {
                    return RequireCallbackImpl(runtime, args, count);
                } catch (NativeScriptException& e) {
                    e.ReThrowToJs(runtime);
                } catch (std::exception& e) {
                    stringstream ss;
                    ss << "Error: c++ exception: " << e.what() << endl;
                    NativeScriptException nsEx(ss.str());
                    nsEx.ReThrowToJs(runtime);
                } catch (...) {
                    NativeScriptException nsEx(std::string("Error: c++ exception!"));
                    nsEx.ReThrowToJs(runtime);
                }
                return engine::Value::undefined();
            });
    global.setProperty(rt, "__nativeRequire", requireFunction);
    m_requireFunction = engine::Value(rt, requireFunction);

    engine::Value globalRequire = GetRequireFunction(rt, baseDir.empty() ? Constants::APP_ROOT_FOLDER_PATH : baseDir);
    global.setProperty(rt, "require", globalRequire);
}

engine::Value ModuleInternal::GetRequireFunction(engine::Runtime& rt, const std::string& dirName) {
    auto itFound = m_requireCache.find(dirName);

    if (itFound != m_requireCache.end()) {
        return engine::Value(rt, itFound->second);
    }

    engine::Value args[2] = {
            engine::Value(rt, m_requireFunction),
            engine::Value(rt, engine::String::createFromUtf8(rt, dirName))
    };

    engine::Object thiz(rt);

    engine::Value result = m_requireFactoryFunction.asObject(rt).asFunction(rt)
            .callWithThis(rt, thiz, args, 2);

    assert(result.isObject() && result.asObject(rt).isFunction(rt));

    m_requireCache.emplace(dirName, engine::Value(rt, result));

    return result;
}

engine::Value ModuleInternal::RequireCallbackImpl(engine::Runtime& rt, const engine::Value* args,
                                                  size_t count) {
    if (count != 2) {
        throw NativeScriptException(string("require should be called with two parameters"));
    }
    if (!args[0].isString()) {
        throw NativeScriptException(string("require's first parameter should be string"));
    }
    if (!args[1].isString()) {
        throw NativeScriptException(string("require's second parameter should be string"));
    }

    string moduleName = args[0].asString(rt).utf8(rt);
    string callingModuleDirName = args[1].asString(rt).utf8(rt);

    auto isData = false;

    auto moduleObj = LoadImpl(rt, moduleName, callingModuleDirName, isData);
    if (moduleObj.isUndefined() || moduleObj.isNull()) {
        return engine::Value::undefined();
    }

    if (isData) {
        return moduleObj;
    } else {
        // Throw rather than return undefined so a failed require surfaces as a JS
        // exception instead of silently evaluating to undefined.
        engine::Value exports = moduleObj.asObject(rt).getProperty(rt, "exports");
        if (exports.isUndefined() || exports.isNull()) {
            throw NativeScriptException("Failed to read exports for module: " + moduleName);
        }
        return exports;
    }
}

void ModuleInternal::Load(engine::Runtime& rt, const std::string& path) {
    engine::Object global = rt.global();

    engine::Value args[1] = {
            engine::Value(rt, engine::String::createFromUtf8(rt, path))
    };

    global.getPropertyAsFunction(rt, "require").callWithThis(rt, global, args, 1);
}

void ModuleInternal::LoadWorker(engine::Runtime& rt, const string& path) {
    // A failed load arrives as a thrown JSError rather than a pending-exception
    // flag, so the worker's onerror handler is driven from the catch.
    try {
        Load(rt, path);
    } catch (engine::JSError& error) {
        CallbackHandlers::CallWorkerScopeOnErrorHandle(rt, error);
    }
}

void ModuleInternal::CheckFileExists(engine::Runtime& rt, const std::string& path, const std::string& baseDir) {
    JEnv jEnv;
    JniLocalRef jsModulename(jEnv.NewStringUTF(path.c_str()));
    JniLocalRef jsBaseDir(jEnv.NewStringUTF(baseDir.c_str()));
    jEnv.CallStaticObjectMethod(MODULE_CLASS, RESOLVE_PATH_METHOD_ID, (jstring) jsModulename, (jstring) jsBaseDir);
}

engine::Value ModuleInternal::LoadInternalModule(engine::Runtime& rt, const std::string& moduleName) {
    if (moduleName == "url") {
        engine::Object moduleObj(rt);
        engine::Object exports(rt);
        exports.setProperty(rt, "URL", rt.global().getProperty(rt, "URL"));
        moduleObj.setProperty(rt, "exports", exports);
        engine_util::SetFunction(rt, exports, "pathToFileURL",
                                 [](engine::Runtime& runtime, const engine::Value&,
                                    const engine::Value*, size_t) -> engine::Value {
                                     return engine::Value(
                                             runtime,
                                             engine::String::createFromUtf8(runtime, "file://"));
                                 });
        return engine::Value(rt, moduleObj);
    }
    return engine::Value::undefined();
}

engine::Value ModuleInternal::LoadImpl(engine::Runtime& rt, const std::string& moduleName, const std::string& baseDir, bool& isData) {
    auto pathKind = GetModulePathKind(moduleName);
    auto cachePathKey = (pathKind == ModulePathKind::Global) ? moduleName : (baseDir + "*" + moduleName);

    engine::Value result;

    DEBUG_WRITE(">>LoadImpl cachePathKey=%s", cachePathKey.c_str());

    auto it = m_loadedModules.find(cachePathKey);

    /**
     * Load internal modules like url,fs etc directly if someone does
     * require('url');
     */
    engine::Value moduleObj = ModuleInternal::LoadInternalModule(rt, moduleName);
    if (!moduleObj.isUndefined()) return moduleObj;

    if (it == m_loadedModules.end()) {
        std::string path;

        // Search App System libs
        std::string sys_lib("system_lib://");
        if (moduleName.rfind(sys_lib, 0) == 0) {
            auto pos = moduleName.find(sys_lib);
            path = std::string(moduleName);
            path.replace(pos, sys_lib.length(), "");
        } else if (Util::EndsWith(moduleName, ".so")) {
            path = "lib" + moduleName;
        } else if (Util::EndsWith(moduleName, ".node")) {
            std::string libName = moduleName;
            Util::ReplaceAll(libName, ".node", "");
            path = "lib" + libName + ".so";
        } else {
            JEnv jenv;
            JniLocalRef jsModulename(jenv.NewStringUTF(moduleName.c_str()));
            JniLocalRef jsBaseDir(jenv.NewStringUTF(baseDir.c_str()));
            JniLocalRef jsModulePath(
                    jenv.CallStaticObjectMethod(MODULE_CLASS, RESOLVE_PATH_METHOD_ID,
                                               (jstring) jsModulename, (jstring) jsBaseDir));

            path = ArgConverter::jstringToString((jstring) jsModulePath);
        }

        auto it2 = m_loadedModules.find(path);

        if (it2 == m_loadedModules.end()) {
            if (Util::EndsWith(path, ".js") || Util::EndsWith(path, ".so")) {
                isData = false;
                result = LoadModule(rt, path, cachePathKey);
            } else if (Util::EndsWith(path, ".json")) {
                isData = true;
                result = LoadData(rt, path);
            } else {
                std::string errMsg = "Unsupported file extension: " + path;
                throw NativeScriptException(errMsg);
            }
        } else {
            auto& cacheEntry = it2->second;
            isData = cacheEntry.isData;
            result = engine::Value(rt, cacheEntry.obj);
        }
    } else {
        auto& cacheEntry = it->second;
        isData = cacheEntry.isData;
        result = engine::Value(rt, cacheEntry.obj);
    }

    return result;
}

std::string ModuleInternal::EnsureFileProtocol(const std::string& path) {
    const std::string protocol = "file://";
    if (path.compare(0, protocol.length(), protocol) != 0) {
        return protocol + path;
    }
    return path;
}

engine::Value ModuleInternal::LoadModule(engine::Runtime& rt, const std::string& modulePath, const std::string& moduleCacheKey) {
    engine::Object context = rt.global();

    engine::Object moduleObj(rt);
    engine::Object exportsObj(rt);
    moduleObj.setProperty(rt, "exports", exportsObj);

    engine::String fullRequiredModulePath = engine::String::createFromUtf8(rt, modulePath);
    moduleObj.setProperty(rt, "filename", fullRequiredModulePath);

    TempModule tempModule(this, modulePath, moduleCacheKey, engine::Value(rt, moduleObj));

    engine::Value moduleFunc;

    if (Util::EndsWith(modulePath, ".js")) {
        DEBUG_WRITE("%s", modulePath.c_str());

        // Fast path: if the build compiled this module to engine bytecode, run
        // it directly. This peeks the file header only -- the source is never
        // read or wrapped for a bytecode module. Bytecode is the compiled form
        // of the *wrapped* module content, so it yields the same wrapper
        // function. Mirrors the napi tree's js_run_bytecode_file call.
        auto engineHost = Runtime::GetRuntime(rt)->GetEngineHost();
        try {
            if (!engineHost->ExecuteBytecodeFile(modulePath, EnsureFileProtocol(modulePath),
                                                 moduleFunc)) {
                moduleFunc = engineHost->ExecuteScript(WrapModuleContent(modulePath),
                                                       EnsureFileProtocol(modulePath));
            }
        } catch (engine::JSError& error) {
            throw NativeScriptException(rt, error, "Error running script " + modulePath);
        }
    } else if (Util::EndsWith(modulePath, ".so")) {
        // The napi version dlopen()s the library and calls its
        // napi_register_module_v1 entry point with the runtime's napi_env. That
        // is a Node-API ABI contract with a prebuilt third-party binary, and
        // there is no napi_env on this path to hand it -- a native addon is
        // linked against Node-API, not against nativescript::engine.
        std::string errMsg("Native modules are not supported by this binding layer: " + modulePath);
        throw NativeScriptException(errMsg);
    } else {
        std::string errMsg = "Unsupported file extension: " + modulePath;
        throw NativeScriptException(errMsg);
    }

    engine::String fileName = engine::String::createFromUtf8(rt, modulePath);

    char pathcopy[1024];
    strcpy(pathcopy, modulePath.c_str());
    std::string strDirName(dirname(pathcopy));

    engine::String dirName = engine::String::createFromUtf8(rt, strDirName);

    engine::Value require = GetRequireFunction(rt, strDirName);

    engine::Value requireArgs[5] = {
            engine::Value(rt, moduleObj),
            engine::Value(rt, exportsObj),
            engine::Value(rt, require),
            engine::Value(rt, fileName),
            engine::Value(rt, dirName)
    };

    moduleObj.setProperty(rt, "require", require);
    moduleObj.setProperty(rt, "id", fileName);

    engine::Object thiz(rt);
    thiz.setProperty(rt, "__extends", context.getProperty(rt, "__extends"));

    try {
        moduleFunc.asObject(rt).asFunction(rt).callWithThis(rt, thiz, requireArgs, 5);
    } catch (engine::JSError& error) {
        throw NativeScriptException(rt, error, "Error calling module function: ");
    }

    tempModule.SaveToCache();

    return engine::Value(rt, moduleObj);
}

engine::Value ModuleInternal::LoadData(engine::Runtime& rt, const std::string& path) {
    std::string jsonData = Runtime::GetRuntime(rt)->ReadFileText(path);
    engine::Value json;
    try {
        json = JsonParseString(rt, jsonData);
    } catch (engine::JSError& error) {
        throw NativeScriptException(rt, error, "JSON is not valid, file=" + path);
    }

    if (!json.isObject()) {
        throw NativeScriptException("JSON is not valid, file=" + path);
    }

    m_loadedModules.emplace(path, ModuleCacheEntry(engine::Value(rt, json), true /* isData */));
    return json;
}

std::string ModuleInternal::WrapModuleContent(const std::string& path) {

    std::string content = Runtime::GetRuntime(*m_rt)->ReadFileText(path);

    // TODO: Use statically allocated buffer for better performance
    std::string result(MODULE_PROLOGUE);
    result.reserve(content.length() + 1024);
    result += content;
    result += MODULE_EPILOGUE;

    return result;
}

ModuleInternal::ModulePathKind ModuleInternal::GetModulePathKind(const std::string& path) {
    ModulePathKind kind;
    switch (path[0]) {
    case '.':
        kind = ModulePathKind::Relative;
        break;
    case '/':
        kind = ModulePathKind::Absolute;
        break;
    default:
        kind = ModulePathKind::Global;
        break;
    }
    return kind;
}

jclass ModuleInternal::MODULE_CLASS = nullptr;
jmethodID ModuleInternal::RESOLVE_PATH_METHOD_ID = nullptr;

const char* ModuleInternal::MODULE_PROLOGUE = "(function(module, exports, require, __filename, __dirname){ ";
const char* ModuleInternal::MODULE_EPILOGUE = "\n})";
int ModuleInternal::MODULE_PROLOGUE_LENGTH = std::string(ModuleInternal::MODULE_PROLOGUE).length();
