#ifndef JNI_MODULE_H_
#define JNI_MODULE_H_

#include "JEnv.h"
#include "EngineHost.h"
#include <string>
#include <map>
#include "robin_hood.h"

namespace tns {
class ModuleInternal {
    public:
        ModuleInternal();

        void Init(engine::Runtime& rt, const std::string& baseDir = "");

        void Load(engine::Runtime& rt, const std::string& path);

        /*
         * Reuses `Load` logic and adds TryCatch exception handling to push any unhandled exceptions
         * during script's initial load through the worker scope's `onerror` handler (if implemented before the exception was thrown)
         */
        void LoadWorker(engine::Runtime& rt, const std::string& path);

        /*
         * Checks if target script exists, will throw if negative
         * Used before initializing workers, to ensure a thread will not be created, when the file doesn't exist
         */
        static void CheckFileExists(engine::Runtime& rt, const std::string& path, const std::string& baseDir);
        static std::string EnsureFileProtocol(const std::string& path);

        static int MODULE_PROLOGUE_LENGTH;
        void DeInit();
    private:
        enum class ModulePathKind {
            Global,
            Relative,
            Absolute
        };

        struct ModuleCacheEntry {
            ModuleCacheEntry(engine::Value _obj)
                    : obj(std::move(_obj)), isData(false) {
            }

            ModuleCacheEntry(engine::Value _obj, bool _isData)
                    : obj(std::move(_obj)), isData(_isData) {
            }

            bool isData;
            engine::Value obj;
        };

        static engine::Value LoadInternalModule(engine::Runtime& rt, const std::string& moduleName);

        engine::Value RequireCallbackImpl(engine::Runtime& rt, const engine::Value* args, size_t count);

        std::string WrapModuleContent(const std::string& path);

        engine::Value LoadImpl(engine::Runtime& rt, const std::string& moduleName, const std::string& baseDir, bool& isData);

        engine::Value LoadModule(engine::Runtime& rt, const std::string& path, const std::string& moduleCacheKey);

        engine::Value LoadData(engine::Runtime& rt, const std::string& path);

        engine::Value GetRequireFunction(engine::Runtime& rt, const std::string& dirName);

        ModulePathKind GetModulePathKind(const std::string& path);

        static jclass MODULE_CLASS;
        static jmethodID RESOLVE_PATH_METHOD_ID;
        static const char* MODULE_PROLOGUE;
        static const char* MODULE_EPILOGUE;

        engine::Runtime* m_rt;
        engine::Value m_requireFunction;
        engine::Value m_requireFactoryFunction;
        robin_hood::unordered_map<std::string, engine::Value> m_requireCache;
        robin_hood::unordered_map<std::string, ModuleCacheEntry> m_loadedModules;

        class TempModule {
            public:
                TempModule(ModuleInternal* module, const std::string& modulePath, const std::string& cacheKey, engine::Value moduleObj)
                    :m_module(module), m_dispose(true), m_modulePath(modulePath), m_cacheKey(cacheKey) {
                    m_module->m_loadedModules.emplace(m_modulePath, ModuleCacheEntry(moduleObj));
                    m_module->m_loadedModules.emplace(m_cacheKey, ModuleCacheEntry(std::move(moduleObj)));
                }

                ~TempModule() {
                    if (m_dispose) {
                        m_module->m_loadedModules.erase(m_modulePath);
                        m_module->m_loadedModules.erase(m_cacheKey);
                    }
                }

                void SaveToCache() {
                    m_dispose = false;
                }

            private:
                bool m_dispose;
                ModuleInternal* m_module;
                std::string m_modulePath;
                std::string m_cacheKey;
        };

};
}

#endif /* JNI_MODULE_H_ */
