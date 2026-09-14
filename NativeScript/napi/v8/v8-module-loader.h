#ifndef V8_MODULE_LOADER_H_
#define V8_MODULE_LOADER_H_

#include <string>
#include "js_native_api.h"
#include "v8.h"

namespace v8impl {

void RegisterESModule(v8::Isolate* isolate, const std::string& moduleId,
                      v8::Local<v8::Module> module);

// ES Module callbacks for V8
v8::MaybeLocal<v8::Module> ResolveModuleCallback(
    v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_assertions,
    v8::Local<v8::Module> referrer);

v8::MaybeLocal<v8::Promise> ImportModuleDynamicallyCallback(
    v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_assertions);

// Helper functions
std::string ResolveESModulePath(v8::Isolate* isolate, 
                               const std::string& baseDir, 
                               const std::string& moduleName);

v8::Local<v8::Value> LoadESModule(v8::Isolate* isolate, 
                                  const std::string& path);

v8::MaybeLocal<v8::Module> CompileESModule(v8::Isolate* isolate, 
                                         const std::string& path);

v8::Local<v8::String> WrapModuleContent(v8::Isolate* isolate, 
                                       const std::string& path);

bool IsESModule(const std::string& path);

// Initialize ES module system
void InitializeESModuleSystem(v8::Isolate* isolate);

// Cleanup ES module system (call before isolate disposal)
void CleanupESModuleSystem(v8::Isolate* isolate);

}  // namespace v8impl

#endif  // V8_MODULE_LOADER_H_
