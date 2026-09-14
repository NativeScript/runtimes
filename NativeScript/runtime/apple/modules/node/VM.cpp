#include "VM.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "js_native_api.h"
#include "jsr_common.h"
#include "native_api_util.h"

#if defined(TARGET_ENGINE_V8)
#include "v8-api.h"
#elif defined(TARGET_ENGINE_QUICKJS)
#include "quickjs.h"
#include "quicks-runtime.h"
#endif

namespace nativescript {

namespace {

constexpr char kContextSymbolName[] = "nativescript.vm.context";
constexpr char kDefaultFilename[] = "vm.js";
constexpr char kDefaultModuleIdentifier[] = "vm:module";

const char* GetVmEngineName() {
#if defined(TARGET_ENGINE_HERMES)
  return "Hermes";
#elif defined(TARGET_ENGINE_JSC)
  return "JavaScriptCore";
#elif defined(TARGET_ENGINE_QUICKJS)
  return "QuickJS";
#elif defined(TARGET_ENGINE_V8)
  return "V8";
#else
  return "this engine";
#endif
}

bool ThrowEngineVmUnsupported(napi_env env, const char* feature) {
  std::string message = std::string(feature) +
                        " is not supported by node:vm on " + GetVmEngineName() +
                        " yet";
  napi_throw_error(env, nullptr, message.c_str());
  return false;
}

napi_value ThrowEngineVmUnsupportedValue(napi_env env, const char* feature) {
  ThrowEngineVmUnsupported(env, feature);
  return nullptr;
}

bool IsNullOrUndefined(napi_env env, napi_value value) {
  if (value == nullptr) {
    return true;
  }

  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok) {
    return false;
  }

  return type == napi_null || type == napi_undefined;
}

bool IsObjectLike(napi_env env, napi_value value) {
  if (value == nullptr) {
    return false;
  }

  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok) {
    return false;
  }

  return type == napi_object || type == napi_function;
}

bool CoerceToString(napi_env env, napi_value value, std::string& out) {
  napi_value coerced;
  if (napi_coerce_to_string(env, value, &coerced) != napi_ok) {
    return false;
  }

  size_t length = 0;
  if (napi_get_value_string_utf8(env, coerced, nullptr, 0, &length) !=
      napi_ok) {
    return false;
  }

  std::vector<char> buffer(length + 1);
  if (napi_get_value_string_utf8(env, coerced, buffer.data(), buffer.size(),
                                 nullptr) != napi_ok) {
    return false;
  }

  out.assign(buffer.data(), length);
  return true;
}

bool ReadOptionalStringProperty(napi_env env, napi_value object,
                                const char* property, std::string& out) {
  if (IsNullOrUndefined(env, object)) {
    return false;
  }

  bool hasProperty = false;
  if (napi_has_named_property(env, object, property, &hasProperty) != napi_ok ||
      !hasProperty) {
    return false;
  }

  napi_value value;
  if (napi_get_named_property(env, object, property, &value) != napi_ok) {
    return false;
  }

  return CoerceToString(env, value, out);
}

bool ReadOptionalProperty(napi_env env, napi_value object, const char* property,
                          napi_value* out) {
  if (IsNullOrUndefined(env, object)) {
    return false;
  }

  bool hasProperty = false;
  if (napi_has_named_property(env, object, property, &hasProperty) != napi_ok ||
      !hasProperty) {
    return false;
  }

  return napi_get_named_property(env, object, property, out) == napi_ok;
}

std::string ReadIdentifierOption(
    napi_env env, napi_value options,
    const std::string& fallback = kDefaultModuleIdentifier) {
  std::string identifier;
  if (ReadOptionalStringProperty(env, options, "identifier", identifier) &&
      !identifier.empty()) {
    return identifier;
  }

  if (ReadOptionalStringProperty(env, options, "filename", identifier) &&
      !identifier.empty()) {
    return identifier;
  }

  if (!identifier.empty()) {
    return identifier;
  }

  return fallback;
}

napi_value CreateUint8ArrayCopy(napi_env env, const uint8_t* data,
                                size_t length) {
  void* bufferData = nullptr;
  napi_value arrayBuffer;
  if (napi_create_arraybuffer(env, length, &bufferData, &arrayBuffer) !=
      napi_ok) {
    return nullptr;
  }

  if (length > 0 && data != nullptr) {
    memcpy(bufferData, data, length);
  }

  napi_value typedArray;
  if (napi_create_typedarray(env, napi_uint8_array, length, arrayBuffer, 0,
                             &typedArray) != napi_ok) {
    return nullptr;
  }

  return typedArray;
}

napi_value CreateStringArray(napi_env env,
                             const std::vector<std::string>& values) {
  napi_value array;
  if (napi_create_array_with_length(env, values.size(), &array) != napi_ok) {
    return nullptr;
  }

  for (size_t index = 0; index < values.size(); ++index) {
    napi_value entry;
    if (napi_create_string_utf8(env, values[index].c_str(),
                                values[index].size(), &entry) != napi_ok ||
        napi_set_element(env, array, index, entry) != napi_ok) {
      return nullptr;
    }
  }

  return array;
}

bool GetStringArray(napi_env env, napi_value value,
                    std::vector<std::string>& out) {
  bool isArray = false;
  if (napi_is_array(env, value, &isArray) != napi_ok || !isArray) {
    return false;
  }

  uint32_t length = 0;
  if (napi_get_array_length(env, value, &length) != napi_ok) {
    return false;
  }

  out.clear();
  out.reserve(length);
  for (uint32_t index = 0; index < length; ++index) {
    napi_value element;
    std::string text;
    if (napi_get_element(env, value, index, &element) != napi_ok ||
        !CoerceToString(env, element, text)) {
      return false;
    }
    out.push_back(text);
  }

  return true;
}

bool CreatePromiseSettledWithUndefined(napi_env env, bool rejected,
                                       napi_value reasonOrValue,
                                       napi_value* result) {
  napi_deferred deferred;
  if (napi_create_promise(env, &deferred, result) != napi_ok) {
    return false;
  }

  napi_value undefined;
  napi_get_undefined(env, &undefined);

  if (rejected) {
    return napi_reject_deferred(
               env, deferred,
               reasonOrValue != nullptr ? reasonOrValue : undefined) == napi_ok;
  }

  return napi_resolve_deferred(
             env, deferred,
             reasonOrValue != nullptr ? reasonOrValue : undefined) == napi_ok;
}

std::vector<std::string> ExtractModuleDependencySpecifiers(
    const std::string& source) {
  static const std::regex kImportFromPattern(
      R"((?:^|[\r\n])\s*import\s+[^;]*?\s+from\s+['"]([^'"]+)['"])",
      std::regex::ECMAScript);
  static const std::regex kImportOnlyPattern(
      R"((?:^|[\r\n])\s*import\s+['"]([^'"]+)['"])", std::regex::ECMAScript);

  std::vector<std::string> specifiers;
  std::unordered_set<std::string> seen;

  for (std::sregex_iterator
           it(source.begin(), source.end(), kImportFromPattern),
       end;
       it != end; ++it) {
    const std::string specifier = (*it)[1].str();
    if (seen.insert(specifier).second) {
      specifiers.push_back(specifier);
    }
  }

  for (std::sregex_iterator
           it(source.begin(), source.end(), kImportOnlyPattern),
       end;
       it != end; ++it) {
    const std::string specifier = (*it)[1].str();
    if (seen.insert(specifier).second) {
      specifiers.push_back(specifier);
    }
  }

  return specifiers;
}

std::string ReadFilenameOption(napi_env env, napi_value options,
                               const std::string& fallback = kDefaultFilename) {
  if (IsNullOrUndefined(env, options)) {
    return fallback;
  }

  napi_valuetype type;
  if (napi_typeof(env, options, &type) != napi_ok) {
    return fallback;
  }

  if (type == napi_string) {
    std::string filename;
    if (CoerceToString(env, options, filename) && !filename.empty()) {
      return filename;
    }
    return fallback;
  }

  if (type != napi_object) {
    return fallback;
  }

  bool hasFilename = false;
  if (napi_has_named_property(env, options, "filename", &hasFilename) !=
          napi_ok ||
      !hasFilename) {
    return fallback;
  }

  napi_value filenameValue;
  if (napi_get_named_property(env, options, "filename", &filenameValue) !=
      napi_ok) {
    return fallback;
  }

  std::string filename;
  if (CoerceToString(env, filenameValue, filename) && !filename.empty()) {
    return filename;
  }

  return fallback;
}

napi_value GetContextSymbol(napi_env env) {
  napi_value global;
  napi_value symbolCtor;
  napi_value symbolFor;
  napi_value description;
  napi_value symbol;

  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Symbol", &symbolCtor);
  napi_get_named_property(env, symbolCtor, "for", &symbolFor);
  napi_create_string_utf8(env, kContextSymbolName, NAPI_AUTO_LENGTH,
                          &description);
  napi_call_function(env, symbolCtor, symbolFor, 1, &description, &symbol);
  return symbol;
}

struct ContextState {
#if defined(TARGET_ENGINE_V8)
  v8::Global<v8::Context> context;
#elif defined(TARGET_ENGINE_QUICKJS)
  JSRuntime* runtime = nullptr;
  JSContext* context = nullptr;
#endif
  std::unordered_set<std::string> baselineKeys;

  ~ContextState() {
#if defined(TARGET_ENGINE_V8)
    context.Reset();
#elif defined(TARGET_ENGINE_QUICKJS)
    if (context != nullptr) {
      JS_FreeContext(context);
      context = nullptr;
    }
#endif
  }
};

struct ScriptState {
  std::string source;
  std::string filename;
};

enum class ModuleKind {
  kSourceText,
  kSynthetic,
};

struct ModuleState {
  napi_env env = nullptr;
  ModuleKind kind = ModuleKind::kSourceText;
  std::string identifier;
  std::vector<std::string> dependencySpecifiers;
  std::vector<std::string> exportNames;
  std::vector<ModuleState*> linkedModules;
  napi_ref contextRef = nullptr;
  ContextState* contextState = nullptr;
  napi_ref errorRef = nullptr;

#if defined(TARGET_ENGINE_V8)
  v8::Global<v8::Module> module;
#elif defined(TARGET_ENGINE_QUICKJS)
  JSRuntime* runtime = nullptr;
  JSContext* context = nullptr;
  JSValue moduleValue = JS_UNDEFINED;
  std::unordered_map<std::string, JSValue> syntheticExports;
  bool linked = false;
  bool evaluating = false;
  bool evaluated = false;
  bool errored = false;
  std::string errorMessage;
#endif
};

#if defined(TARGET_ENGINE_V8)
using V8ModuleRegistry = std::unordered_map<int, ModuleState*>;
std::unordered_map<v8::Isolate*, V8ModuleRegistry>& GetV8ModuleRegistries();
V8ModuleRegistry& GetV8ModuleRegistry(v8::Isolate* isolate);
#elif defined(TARGET_ENGINE_QUICKJS)
struct QuickJSModuleRegistry {
  bool installed = false;
  bool cleanupHookRegistered = false;
  std::unordered_map<std::string, ModuleState*> modulesById;
  std::unordered_map<JSModuleDef*, ModuleState*> modulesByDef;
  std::unordered_map<std::string, std::string> resolutions;
};

std::unordered_map<JSRuntime*, QuickJSModuleRegistry>&
GetQuickJSModuleRegistries();
std::string MakeQuickJSResolutionKey(const std::string& base,
                                     const std::string& specifier);
QuickJSModuleRegistry& EnsureQuickJSModuleRegistry(napi_env env,
                                                   JSRuntime* runtime);
std::string GetQuickJSModuleStatusString(ModuleState* state);
bool EnsureQuickJSImportMeta(ModuleState* state);
bool EnsureQuickJSLinked(napi_env env, ModuleState* state);
bool CacheQuickJSError(napi_env env, ModuleState* state, JSValue exception);
bool CreateQuickJSSourceTextModule(napi_env env, const std::string& sourceText,
                                   napi_value options, napi_value* result);
bool CreateQuickJSSyntheticModule(napi_env env,
                                  const std::vector<std::string>& exportNames,
                                  napi_value options, napi_value* result);
void CleanupQuickJSModuleState(ModuleState* state);
bool ApplyQuickJSSyntheticExports(ModuleState* state);
#endif

void FinalizeContextState(napi_env env, void* data, void* /*hint*/) {
  delete static_cast<ContextState*>(data);
}

void FinalizeScriptState(napi_env env, void* data, void* /*hint*/) {
  delete static_cast<ScriptState*>(data);
}

void FinalizeModuleState(napi_env env, void* data, void* /*hint*/) {
  ModuleState* state = static_cast<ModuleState*>(data);
  if (state == nullptr) {
    return;
  }

  if (state->contextRef != nullptr) {
    napi_delete_reference(env, state->contextRef);
    state->contextRef = nullptr;
  }

  if (state->errorRef != nullptr) {
    napi_delete_reference(env, state->errorRef);
    state->errorRef = nullptr;
  }

#if defined(TARGET_ENGINE_V8)
  auto& registries = GetV8ModuleRegistries();
  auto registryIt = registries.find(env->isolate);
  if (registryIt != registries.end()) {
    auto& registry = registryIt->second;
    for (auto it = registry.begin(); it != registry.end();) {
      if (it->second == state) {
        it = registry.erase(it);
      } else {
        ++it;
      }
    }
    if (registry.empty()) {
      registries.erase(registryIt);
    }
  }
  state->module.Reset();
#elif defined(TARGET_ENGINE_QUICKJS)
  CleanupQuickJSModuleState(state);
#endif

  delete state;
}

bool GetContextState(napi_env env, napi_value sandbox, ContextState** out) {
  *out = nullptr;

  if (!IsObjectLike(env, sandbox)) {
    return false;
  }

  napi_value symbol = GetContextSymbol(env);
  bool hasState = false;
  if (napi_has_property(env, sandbox, symbol, &hasState) != napi_ok ||
      !hasState) {
    return false;
  }

  napi_value stateValue;
  if (napi_get_property(env, sandbox, symbol, &stateValue) != napi_ok) {
    return false;
  }

  void* rawState = nullptr;
  if (napi_get_value_external(env, stateValue, &rawState) != napi_ok ||
      rawState == nullptr) {
    return false;
  }

  *out = static_cast<ContextState*>(rawState);
  return true;
}

bool RequireContextState(napi_env env, napi_value sandbox, ContextState** out) {
  if (GetContextState(env, sandbox, out)) {
    return true;
  }

  napi_throw_type_error(env, nullptr,
                        "The \"contextifiedObject\" argument must be a vm "
                        "context created by vm.createContext()");
  return false;
}

bool ReadModuleContextOption(napi_env env, napi_value options,
                             ContextState** outState,
                             napi_value* outContextObject) {
  *outState = nullptr;
  if (outContextObject != nullptr) {
    *outContextObject = nullptr;
  }

  napi_value contextValue;
  if (!ReadOptionalProperty(env, options, "context", &contextValue) ||
      IsNullOrUndefined(env, contextValue)) {
    return true;
  }

  if (!RequireContextState(env, contextValue, outState)) {
    return false;
  }

  if (outContextObject != nullptr) {
    *outContextObject = contextValue;
  }

  return true;
}

bool CreateModuleHandle(napi_env env, ModuleState* state, napi_value* result) {
  return napi_create_external(env, state, FinalizeModuleState, nullptr,
                              result) == napi_ok;
}

bool GetModuleState(napi_env env, napi_value value, ModuleState** out) {
  *out = nullptr;
  void* raw = nullptr;
  if (napi_get_value_external(env, value, &raw) != napi_ok || raw == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid vm.Module handle");
    return false;
  }

  *out = static_cast<ModuleState*>(raw);
  return true;
}

bool SetModuleError(napi_env env, ModuleState* state, napi_value error) {
  if (state->errorRef != nullptr) {
    napi_delete_reference(env, state->errorRef);
    state->errorRef = nullptr;
  }

  if (IsNullOrUndefined(env, error)) {
    return true;
  }

  return napi_create_reference(env, error, 1, &state->errorRef) == napi_ok;
}

napi_value GetStoredModuleError(napi_env env, ModuleState* state) {
  if (state->errorRef == nullptr) {
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  napi_value error;
  if (napi_get_reference_value(env, state->errorRef, &error) != napi_ok) {
    return nullptr;
  }

  return error;
}

napi_value GetStoredModuleContext(napi_env env, ModuleState* state) {
  if (state->contextRef == nullptr) {
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  napi_value context;
  if (napi_get_reference_value(env, state->contextRef, &context) != napi_ok) {
    return nullptr;
  }

  return context;
}

#if defined(TARGET_ENGINE_V8)

std::vector<std::string> CollectV8OwnStringKeys(v8::Isolate* isolate,
                                                v8::Local<v8::Context> context,
                                                v8::Local<v8::Object> object) {
  std::vector<std::string> result;

  v8::Local<v8::Array> names;
  if (!object->GetOwnPropertyNames(context).ToLocal(&names)) {
    return result;
  }

  const uint32_t length = names->Length();
  result.reserve(length);

  for (uint32_t index = 0; index < length; ++index) {
    v8::Local<v8::Value> key;
    if (!names->Get(context, index).ToLocal(&key) || !key->IsString()) {
      continue;
    }

    v8::String::Utf8Value utf8(isolate, key);
    if (*utf8 == nullptr) {
      continue;
    }

    result.emplace_back(*utf8, utf8.length());
  }

  return result;
}

std::unordered_set<std::string> ToKeySet(const std::vector<std::string>& keys) {
  return std::unordered_set<std::string>(keys.begin(), keys.end());
}


thread_local std::unordered_map<v8::Isolate*, V8ModuleRegistry>
    g_v8ModuleRegistries;

std::unordered_map<v8::Isolate*, V8ModuleRegistry>& GetV8ModuleRegistries() {
  return g_v8ModuleRegistries;
}

V8ModuleRegistry& GetV8ModuleRegistry(v8::Isolate* isolate) {
  return g_v8ModuleRegistries[isolate];
}

v8::Local<v8::Context> GetV8ModuleContext(napi_env env, ModuleState* state) {
  return state->contextState != nullptr
             ? state->contextState->context.Get(env->isolate)
             : env->context();
}

std::string GetV8ModuleStatusString(v8::Module::Status status) {
  switch (status) {
    case v8::Module::kUninstantiated:
      return "unlinked";
    case v8::Module::kInstantiating:
      return "linking";
    case v8::Module::kInstantiated:
      return "linked";
    case v8::Module::kEvaluating:
      return "evaluating";
    case v8::Module::kEvaluated:
      return "evaluated";
    case v8::Module::kErrored:
      return "errored";
  }

  return "unlinked";
}

bool SetV8ModuleRegistryEntry(v8::Local<v8::Module> module,
                              ModuleState* state) {
  GetV8ModuleRegistry(v8::Isolate::GetCurrent())[module->GetIdentityHash()] =
      state;
  return true;
}

v8::MaybeLocal<v8::Module> ResolveV8VmModuleByIndex(
    v8::Local<v8::Context> context, size_t index,
    v8::Local<v8::Module> referrer) {
  auto& registry = GetV8ModuleRegistry(v8::Isolate::GetCurrent());
  auto it = registry.find(referrer->GetIdentityHash());
  if (it == registry.end() || it->second == nullptr) {
    return v8::MaybeLocal<v8::Module>();
  }

  ModuleState* referrerState = it->second;
  if (index >= referrerState->linkedModules.size() ||
      referrerState->linkedModules[index] == nullptr) {
    return v8::MaybeLocal<v8::Module>();
  }

  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  return referrerState->linkedModules[index]->module.Get(isolate);
}

v8::MaybeLocal<v8::Value> EvaluateV8SyntheticModule(
    v8::Local<v8::Context> context, v8::Local<v8::Module> /*module*/) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context).ToLocalChecked();
  resolver->Resolve(context, v8::Undefined(isolate)).ToChecked();
  return resolver->GetPromise();
}

bool ExtractV8DependencySpecifiers(v8::Isolate* isolate,
                                   v8::Local<v8::Module> module,
                                   std::vector<std::string>& out) {
  out.clear();

  v8::Local<v8::FixedArray> requests = module->GetModuleRequests();
  const int length = requests->Length();
  out.reserve(length);

  for (int index = 0; index < length; ++index) {
    v8::Local<v8::Data> entry =
        requests->Get(isolate->GetCurrentContext(), index);
    v8::Local<v8::String> specifier =
        v8::ModuleRequest::Cast(*entry)->GetSpecifier();
    v8::String::Utf8Value text(isolate, specifier);
    if (*text == nullptr) {
      return false;
    }
    out.emplace_back(*text, text.length());
  }

  return true;
}

std::vector<std::string> CollectV8ModuleExportNames(napi_env env,
                                                    ModuleState* state) {
  v8::Isolate* isolate = env->isolate;
  v8::EscapableHandleScope scope(isolate);
  v8::Local<v8::Context> context = GetV8ModuleContext(env, state);
  v8::Context::Scope contextScope(context);

  v8::Local<v8::Module> module = state->module.Get(isolate);
  if (module.IsEmpty() || module->GetStatus() < v8::Module::kInstantiated) {
    return {};
  }

  v8::Local<v8::Object> ns = module->GetModuleNamespace().As<v8::Object>();
  return CollectV8OwnStringKeys(isolate, context, ns);
}

bool ThrowV8ModuleException(napi_env env, ModuleState* state) {
  v8::Isolate* isolate = env->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Module> module = state->module.Get(isolate);
  napi_throw(env, v8impl::JsValueFromV8LocalValue(module->GetException()));
  return false;
}

bool CacheV8ModuleError(napi_env env, ModuleState* state) {
  v8::Isolate* isolate = env->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Module> module = state->module.Get(isolate);
  return SetModuleError(
      env, state, v8impl::JsValueFromV8LocalValue(module->GetException()));
}

bool CreateV8SourceTextModule(napi_env env, const std::string& sourceText,
                              napi_value options, napi_value* result) {
  v8::Isolate* isolate = env->isolate;
  v8::EscapableHandleScope scope(isolate);

  ContextState* contextState = nullptr;
  napi_value contextObject = nullptr;
  if (!ReadModuleContextOption(env, options, &contextState, &contextObject)) {
    return false;
  }

  v8::Local<v8::Context> context = contextState != nullptr
                                       ? contextState->context.Get(isolate)
                                       : env->context();
  v8::Context::Scope contextScope(context);
  v8::TryCatch tryCatch(isolate);

  const std::string identifier = ReadIdentifierOption(env, options);
  v8::Local<v8::String> sourceValue =
      v8::String::NewFromUtf8(isolate, sourceText.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(sourceText.size()))
          .ToLocalChecked();
  v8::Local<v8::String> nameValue =
      v8::String::NewFromUtf8(isolate, identifier.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(identifier.size()))
          .ToLocalChecked();

#if V8_MAJOR_VERSION >= 14
  v8::ScriptOrigin origin(nameValue, 0, 0, false, -1, v8::Local<v8::Value>(),
                          false, false, true);
#else
  v8::ScriptOrigin origin(isolate, nameValue, 0, 0, false, -1,
                          v8::Local<v8::Value>(), false, false, true);
#endif

  v8::ScriptCompiler::Source source(sourceValue, origin);
  v8::Local<v8::Module> module;
  if (!v8::ScriptCompiler::CompileModule(isolate, &source).ToLocal(&module)) {
    if (tryCatch.HasCaught()) {
      napi_throw(env, v8impl::JsValueFromV8LocalValue(tryCatch.Exception()));
    } else {
      napi_throw_error(env, nullptr, "Failed to compile vm.SourceTextModule");
    }
    return false;
  }

  std::unique_ptr<ModuleState> state(new ModuleState());
  state->env = env;
  state->kind = ModuleKind::kSourceText;
  state->identifier = identifier;
  state->contextState = contextState;
  state->dependencySpecifiers.clear();
  if (!ExtractV8DependencySpecifiers(isolate, module,
                                     state->dependencySpecifiers)) {
    napi_throw_error(env, nullptr, "Failed to inspect module requests");
    return false;
  }
  state->module.Reset(isolate, module);
  if (contextObject != nullptr &&
      napi_create_reference(env, contextObject, 1, &state->contextRef) !=
          napi_ok) {
    return false;
  }

  SetV8ModuleRegistryEntry(module, state.get());
  if (!CreateModuleHandle(env, state.get(), result)) {
    return false;
  }

  state.release();
  return true;
}

bool CreateV8SyntheticModule(napi_env env,
                             const std::vector<std::string>& exportNames,
                             napi_value options, napi_value* result) {
  v8::Isolate* isolate = env->isolate;
  v8::EscapableHandleScope scope(isolate);

  ContextState* contextState = nullptr;
  napi_value contextObject = nullptr;
  if (!ReadModuleContextOption(env, options, &contextState, &contextObject)) {
    return false;
  }

  const std::string identifier = ReadIdentifierOption(env, options);
  std::vector<v8::Local<v8::String>> exports;
  exports.reserve(exportNames.size());
  for (const auto& exportName : exportNames) {
    exports.push_back(
        v8::String::NewFromUtf8(isolate, exportName.c_str(),
                                v8::NewStringType::kNormal,
                                static_cast<int>(exportName.size()))
            .ToLocalChecked());
  }

  v8::Local<v8::String> nameValue =
      v8::String::NewFromUtf8(isolate, identifier.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(identifier.size()))
          .ToLocalChecked();
  v8::Local<v8::Module> module = v8::Module::CreateSyntheticModule(
      isolate, nameValue,
      v8::MemorySpan<const v8::Local<v8::String>>(exports.data(),
                                                  exports.size()),
      &EvaluateV8SyntheticModule);

  std::unique_ptr<ModuleState> state(new ModuleState());
  state->env = env;
  state->kind = ModuleKind::kSynthetic;
  state->identifier = identifier;
  state->contextState = contextState;
  state->exportNames = exportNames;
  state->module.Reset(isolate, module);
  if (contextObject != nullptr &&
      napi_create_reference(env, contextObject, 1, &state->contextRef) !=
          napi_ok) {
    return false;
  }

  SetV8ModuleRegistryEntry(module, state.get());
  if (!CreateModuleHandle(env, state.get(), result)) {
    return false;
  }

  state.release();
  return true;
}

bool CompileOnlyV8(napi_env env, const std::string& source,
                   const std::string& filename) {
  v8::Isolate* isolate = env->isolate;
  v8::EscapableHandleScope handleScope(isolate);
  v8::TryCatch tryCatch(isolate);

  v8::Local<v8::Context> context = env->context();
  v8::Context::Scope contextScope(context);

  v8::Local<v8::String> sourceValue =
      v8::String::NewFromUtf8(isolate, source.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(source.size()))
          .ToLocalChecked();
  v8::Local<v8::String> filenameValue =
      v8::String::NewFromUtf8(isolate, filename.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(filename.size()))
          .ToLocalChecked();

#if V8_MAJOR_VERSION >= 14
  v8::ScriptOrigin origin(filenameValue);
#else
  v8::ScriptOrigin origin(isolate, filenameValue);
#endif

  if (v8::Script::Compile(context, sourceValue, &origin).IsEmpty()) {
    if (tryCatch.HasCaught()) {
      napi_throw(env, v8impl::JsValueFromV8LocalValue(tryCatch.Exception()));
    } else {
      napi_throw_error(env, nullptr, "Failed to compile vm.Script");
    }
    return false;
  }

  return true;
}

bool CreateContextState(napi_env env, ContextState* state) {
  v8::Isolate* isolate = env->isolate;
  v8::EscapableHandleScope handleScope(isolate);

  v8::Local<v8::Context> context = v8::Context::New(isolate);
  v8::Context::Scope contextScope(context);

  state->baselineKeys =
      ToKeySet(CollectV8OwnStringKeys(isolate, context, context->Global()));
  state->context.Reset(isolate, context);
  return true;
}

std::unordered_set<std::string> SyncSandboxToContext(napi_env env,
                                                     napi_value sandboxValue,
                                                     ContextState* state) {
  v8::Isolate* isolate = env->isolate;
  v8::Local<v8::Context> mainContext = env->context();
  v8::Local<v8::Object> sandbox =
      v8impl::V8LocalValueFromJsValue(sandboxValue).As<v8::Object>();
  v8::Local<v8::Context> childContext = state->context.Get(isolate);

  v8::Context::Scope childScope(childContext);
  v8::Local<v8::Object> childGlobal = childContext->Global();

  auto sandboxKeys = CollectV8OwnStringKeys(isolate, mainContext, sandbox);
  auto sandboxKeySet = ToKeySet(sandboxKeys);

  for (const auto& key : sandboxKeys) {
    v8::Local<v8::String> keyValue =
        v8::String::NewFromUtf8(isolate, key.c_str(),
                                v8::NewStringType::kNormal,
                                static_cast<int>(key.size()))
            .ToLocalChecked();

    v8::Local<v8::Value> value;
    if (!sandbox->Get(mainContext, keyValue).ToLocal(&value)) {
      continue;
    }

    childGlobal->Set(childContext, keyValue, value).FromMaybe(false);
  }

  return sandboxKeySet;
}

bool SyncContextToSandbox(napi_env env, napi_value sandboxValue,
                          ContextState* state,
                          const std::unordered_set<std::string>& sandboxKeys) {
  v8::Isolate* isolate = env->isolate;
  v8::Local<v8::Context> mainContext = env->context();
  v8::Local<v8::Object> sandbox =
      v8impl::V8LocalValueFromJsValue(sandboxValue).As<v8::Object>();
  v8::Local<v8::Context> childContext = state->context.Get(isolate);

  v8::Context::Scope childScope(childContext);
  v8::Local<v8::Object> childGlobal = childContext->Global();
  auto childKeys = CollectV8OwnStringKeys(isolate, childContext, childGlobal);
  auto childKeySet = ToKeySet(childKeys);

  for (const auto& key : sandboxKeys) {
    if (childKeySet.count(key) != 0) {
      continue;
    }

    v8::Local<v8::String> keyValue =
        v8::String::NewFromUtf8(isolate, key.c_str(),
                                v8::NewStringType::kNormal,
                                static_cast<int>(key.size()))
            .ToLocalChecked();
    sandbox->Delete(mainContext, keyValue).FromMaybe(false);
  }

  for (const auto& key : childKeys) {
    if (sandboxKeys.count(key) == 0 && state->baselineKeys.count(key) != 0) {
      continue;
    }

    v8::Local<v8::String> keyValue =
        v8::String::NewFromUtf8(isolate, key.c_str(),
                                v8::NewStringType::kNormal,
                                static_cast<int>(key.size()))
            .ToLocalChecked();

    v8::Local<v8::Value> value;
    if (!childGlobal->Get(childContext, keyValue).ToLocal(&value)) {
      continue;
    }

    sandbox->Set(mainContext, keyValue, value).FromMaybe(false);
  }

  return true;
}

napi_value RunInContextImpl(napi_env env, ContextState* state,
                            napi_value sandboxValue, const std::string& source,
                            const std::string& filename) {
  v8::Isolate* isolate = env->isolate;
  v8::EscapableHandleScope handleScope(isolate);
  v8::TryCatch tryCatch(isolate);

  const auto sandboxKeys = SyncSandboxToContext(env, sandboxValue, state);
  v8::Local<v8::Context> childContext = state->context.Get(isolate);
  v8::Context::Scope childScope(childContext);

  v8::Local<v8::String> sourceValue =
      v8::String::NewFromUtf8(isolate, source.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(source.size()))
          .ToLocalChecked();
  v8::Local<v8::String> filenameValue =
      v8::String::NewFromUtf8(isolate, filename.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(filename.size()))
          .ToLocalChecked();

#if V8_MAJOR_VERSION >= 14
  v8::ScriptOrigin origin(filenameValue);
#else
  v8::ScriptOrigin origin(isolate, filenameValue);
#endif

  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(childContext, sourceValue, &origin)
           .ToLocal(&script)) {
    if (tryCatch.HasCaught()) {
      napi_throw(env, v8impl::JsValueFromV8LocalValue(tryCatch.Exception()));
    } else {
      napi_throw_error(env, nullptr, "Failed to compile vm context script");
    }
    return nullptr;
  }

  v8::Local<v8::Value> result;
  if (!script->Run(childContext).ToLocal(&result)) {
    if (tryCatch.HasCaught()) {
      napi_throw(env, v8impl::JsValueFromV8LocalValue(tryCatch.Exception()));
    } else {
      napi_throw_error(env, nullptr, "Failed to run vm context script");
    }
    return nullptr;
  }

  if (!SyncContextToSandbox(env, sandboxValue, state, sandboxKeys)) {
    return nullptr;
  }

  return v8impl::JsValueFromV8LocalValue(handleScope.Escape(result));
}

#elif defined(TARGET_ENGINE_QUICKJS)

static inline JSValue QuickJSToValue(napi_value value) {
  return *reinterpret_cast<JSValue*>(value);
}

std::string GetQuickJSExceptionMessage(JSContext* context,
                                       JSValueConst exception);
JSValue CloneQuickJSValue(JSContext* sourceContext,
                          JSContext* destinationContext, JSValueConst value);
bool ThrowLatestQuickJSException(napi_env env, JSContext* context);

std::unordered_map<JSRuntime*, QuickJSModuleRegistry>&
GetQuickJSModuleRegistries() {
  static thread_local std::unordered_map<JSRuntime*, QuickJSModuleRegistry>
      registries;
  return registries;
}

std::string MakeQuickJSResolutionKey(const std::string& base,
                                     const std::string& specifier) {
  return base + "\n" + specifier;
}

char* NormalizeQuickJSVmModule(JSContext* ctx, const char* base_name,
                               const char* name, void* opaque) {
  QuickJSModuleRegistry* registry = static_cast<QuickJSModuleRegistry*>(opaque);
  if (registry == nullptr) {
    return js_strdup(ctx, name);
  }

  const std::string base = base_name != nullptr ? base_name : "";
  const auto it =
      registry->resolutions.find(MakeQuickJSResolutionKey(base, name));
  if (it != registry->resolutions.end()) {
    return js_strdup(ctx, it->second.c_str());
  }

  return js_strdup(ctx, name);
}

JSModuleDef* LoadQuickJSVmModule(JSContext* /*ctx*/, const char* module_name,
                                 void* opaque) {
  QuickJSModuleRegistry* registry = static_cast<QuickJSModuleRegistry*>(opaque);
  if (registry == nullptr) {
    return nullptr;
  }

  const auto it = registry->modulesById.find(module_name);
  if (it == registry->modulesById.end() || it->second == nullptr) {
    return nullptr;
  }

  return static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(it->second->moduleValue));
}

void CleanupQuickJSModuleRegistry(void* data) {
  JSRuntime* runtime = static_cast<JSRuntime*>(data);
  auto& registries = GetQuickJSModuleRegistries();
  auto registry = registries.find(runtime);
  if (registry == registries.end()) {
    return;
  }
  JS_SetModuleLoaderFunc(runtime, nullptr, nullptr, nullptr);
  registries.erase(registry);
}

QuickJSModuleRegistry& EnsureQuickJSModuleRegistry(napi_env env,
                                                   JSRuntime* runtime) {
  auto& registries = GetQuickJSModuleRegistries();
  QuickJSModuleRegistry& registry = registries[runtime];
  if (!registry.installed) {
    JS_SetModuleLoaderFunc(runtime, &NormalizeQuickJSVmModule,
                           &LoadQuickJSVmModule, &registry);
    registry.installed = true;
  }
  if (!registry.cleanupHookRegistered &&
      js_add_env_cleanup_hook(env, CleanupQuickJSModuleRegistry, runtime) ==
          napi_ok) {
    registry.cleanupHookRegistered = true;
  }
  return registry;
}

void CleanupQuickJSModuleState(ModuleState* state) {
  auto& registries = GetQuickJSModuleRegistries();
  auto registryIt = registries.find(state->runtime);
  if (registryIt != registries.end()) {
    auto& registry = registryIt->second;
    for (auto it = registry.modulesById.begin();
         it != registry.modulesById.end();) {
      if (it->second == state) {
        it = registry.modulesById.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = registry.modulesByDef.begin();
         it != registry.modulesByDef.end();) {
      if (it->second == state) {
        it = registry.modulesByDef.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = registry.resolutions.begin();
         it != registry.resolutions.end();) {
      if (it->second == state->identifier) {
        it = registry.resolutions.erase(it);
      } else {
        ++it;
      }
    }
  }

  for (auto& entry : state->syntheticExports) {
    JS_FreeValue(state->context, entry.second);
  }
  state->syntheticExports.clear();

  if (state->context != nullptr && !JS_IsUndefined(state->moduleValue)) {
    JS_FreeValue(state->context, state->moduleValue);
    state->moduleValue = JS_UNDEFINED;
  }
}

std::string GetQuickJSModuleStatusString(ModuleState* state) {
  if (state->errored) {
    return "errored";
  }
  if (state->evaluating) {
    return "evaluating";
  }
  if (state->evaluated) {
    return "evaluated";
  }
  if (state->linked) {
    return "linked";
  }
  return "unlinked";
}

bool EnsureQuickJSImportMeta(ModuleState* state) {
  JSModuleDef* module =
      static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(state->moduleValue));
  JSValue meta = JS_GetImportMeta(state->context, module);
  if (JS_IsException(meta)) {
    return false;
  }

  if (JS_DefinePropertyValueStr(
          state->context, meta, "url",
          JS_NewString(state->context, state->identifier.c_str()),
          JS_PROP_C_W_E) < 0 ||
      JS_DefinePropertyValueStr(state->context, meta, "main", JS_FALSE,
                                JS_PROP_C_W_E) < 0) {
    JS_FreeValue(state->context, meta);
    return false;
  }

  JS_FreeValue(state->context, meta);
  return true;
}

bool EnsureQuickJSLinked(napi_env env, ModuleState* state) {
  if (state->linked) {
    return true;
  }

  if (JS_ResolveModule(state->context, state->moduleValue) < 0) {
    state->errored = true;
    state->errorMessage = GetQuickJSExceptionMessage(
        state->context, JS_GetException(state->context));
    napi_throw_error(env, nullptr, state->errorMessage.c_str());
    return false;
  }

  state->linked = true;
  return true;
}

bool CacheQuickJSError(napi_env env, ModuleState* state, JSValue exception) {
  JSContext* mainContext = qjs_get_context(env);
  JSValue cloned = CloneQuickJSValue(state->context, mainContext, exception);
  JS_FreeValue(state->context, exception);
  if (JS_IsException(cloned)) {
    state->errorMessage =
        GetQuickJSExceptionMessage(mainContext, JS_GetException(mainContext));
    napi_throw_error(env, nullptr, state->errorMessage.c_str());
    return false;
  }

  napi_value error;
  if (qjs_create_scoped_value(env, cloned, &error) != napi_ok ||
      !SetModuleError(env, state, error)) {
    return false;
  }

  return true;
}

bool CreateQuickJSSourceTextModule(napi_env env, const std::string& sourceText,
                                   napi_value options, napi_value* result) {
  ContextState* contextState = nullptr;
  napi_value contextObject = nullptr;
  if (!ReadModuleContextOption(env, options, &contextState, &contextObject)) {
    return false;
  }

  JSContext* context =
      contextState != nullptr ? contextState->context : qjs_get_context(env);
  JSRuntime* runtime = JS_GetRuntime(context);
  QuickJSModuleRegistry& registry = EnsureQuickJSModuleRegistry(env, runtime);
  const std::string identifier = ReadIdentifierOption(env, options);

  JSValue moduleValue = JS_Eval(
      context, sourceText.c_str(), sourceText.size(), identifier.c_str(),
      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY |
          JS_EVAL_FLAG_COMPILE_ONLY_NO_RESOLVE);
  if (JS_IsException(moduleValue)) {
    return ThrowLatestQuickJSException(env, context);
  }

  std::unique_ptr<ModuleState> state(new ModuleState());
  state->env = env;
  state->kind = ModuleKind::kSourceText;
  state->identifier = identifier;
  state->contextState = contextState;
  state->runtime = runtime;
  state->context = context;
  state->moduleValue = moduleValue;
  state->dependencySpecifiers = ExtractModuleDependencySpecifiers(sourceText);
  if (contextObject != nullptr &&
      napi_create_reference(env, contextObject, 1, &state->contextRef) !=
          napi_ok) {
    return false;
  }

  registry.modulesById[identifier] = state.get();
  registry
      .modulesByDef[static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(moduleValue))] =
      state.get();
  if (!CreateModuleHandle(env, state.get(), result)) {
    return false;
  }

  state.release();
  return true;
}

bool ApplyQuickJSSyntheticExports(ModuleState* state) {
  JSModuleDef* moduleDef =
      static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(state->moduleValue));
  for (const auto& exportName : state->exportNames) {
    JSValue exportValue = JS_UNDEFINED;
    auto valueIt = state->syntheticExports.find(exportName);
    if (valueIt != state->syntheticExports.end()) {
      exportValue = JS_DupValue(state->context, valueIt->second);
    }

    if (JS_SetModuleExport(state->context, moduleDef, exportName.c_str(),
                           exportValue) < 0) {
      return false;
    }
  }

  return true;
}

int InitializeQuickJSSyntheticModule(JSContext* ctx, JSModuleDef* moduleDef) {
  auto& registries = GetQuickJSModuleRegistries();
  auto registryIt = registries.find(JS_GetRuntime(ctx));
  if (registryIt == registries.end()) {
    return -1;
  }

  auto stateIt = registryIt->second.modulesByDef.find(moduleDef);
  if (stateIt == registryIt->second.modulesByDef.end() ||
      stateIt->second == nullptr) {
    return -1;
  }

  return ApplyQuickJSSyntheticExports(stateIt->second) ? 0 : -1;
}

bool CreateQuickJSSyntheticModule(napi_env env,
                                  const std::vector<std::string>& exportNames,
                                  napi_value options, napi_value* result) {
  ContextState* contextState = nullptr;
  napi_value contextObject = nullptr;
  if (!ReadModuleContextOption(env, options, &contextState, &contextObject)) {
    return false;
  }

  JSContext* context =
      contextState != nullptr ? contextState->context : qjs_get_context(env);
  JSRuntime* runtime = JS_GetRuntime(context);
  QuickJSModuleRegistry& registry = EnsureQuickJSModuleRegistry(env, runtime);
  const std::string identifier = ReadIdentifierOption(env, options);

  JSModuleDef* moduleDef = JS_NewCModule(context, identifier.c_str(),
                                         &InitializeQuickJSSyntheticModule);
  if (moduleDef == nullptr) {
    return ThrowLatestQuickJSException(env, context);
  }

  for (const auto& exportName : exportNames) {
    if (JS_AddModuleExport(context, moduleDef, exportName.c_str()) < 0) {
      return ThrowLatestQuickJSException(env, context);
    }
  }

  std::unique_ptr<ModuleState> state(new ModuleState());
  state->env = env;
  state->kind = ModuleKind::kSynthetic;
  state->identifier = identifier;
  state->contextState = contextState;
  state->runtime = runtime;
  state->context = context;
  state->exportNames = exportNames;
  state->moduleValue = JS_DupValue(context, JS_MKPTR(JS_TAG_MODULE, moduleDef));
  if (contextObject != nullptr &&
      napi_create_reference(env, contextObject, 1, &state->contextRef) !=
          napi_ok) {
    return false;
  }

  registry.modulesById[identifier] = state.get();
  registry.modulesByDef[moduleDef] = state.get();
  if (!EnsureQuickJSLinked(env, state.get())) {
    return false;
  }

  if (!CreateModuleHandle(env, state.get(), result)) {
    return false;
  }

  state.release();
  return true;
}

std::vector<std::string> CollectQuickJSOwnStringKeys(JSContext* context,
                                                     JSValueConst object) {
  std::vector<std::string> result;
  JSPropertyEnum* properties = nullptr;
  uint32_t count = 0;

  if (JS_GetOwnPropertyNames(context, &properties, &count, object,
                             JS_GPN_STRING_MASK) < 0) {
    return result;
  }

  result.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    const char* key = JS_AtomToCString(context, properties[index].atom);
    if (key != nullptr) {
      result.emplace_back(key);
      JS_FreeCString(context, key);
    }

    JS_FreeAtom(context, properties[index].atom);
  }

  js_free(context, properties);
  return result;
}

std::unordered_set<std::string> ToKeySet(const std::vector<std::string>& keys) {
  return std::unordered_set<std::string>(keys.begin(), keys.end());
}

std::string GetQuickJSExceptionMessage(JSContext* context,
                                       JSValueConst exception) {
  std::string message;

  JSValue stack = JS_GetPropertyStr(context, exception, "stack");
  if (!JS_IsException(stack) && !JS_IsUndefined(stack) && !JS_IsNull(stack)) {
    const char* stackChars = JS_ToCString(context, stack);
    if (stackChars != nullptr) {
      message.assign(stackChars);
      JS_FreeCString(context, stackChars);
    }
  }
  JS_FreeValue(context, stack);

  if (!message.empty()) {
    return message;
  }

  JSValue detail = JS_GetPropertyStr(context, exception, "message");
  if (!JS_IsException(detail) && !JS_IsUndefined(detail) &&
      !JS_IsNull(detail)) {
    const char* detailChars = JS_ToCString(context, detail);
    if (detailChars != nullptr) {
      message.assign(detailChars);
      JS_FreeCString(context, detailChars);
    }
  }
  JS_FreeValue(context, detail);

  if (!message.empty()) {
    return message;
  }

  const char* chars = JS_ToCString(context, exception);
  if (chars != nullptr) {
    message.assign(chars);
    JS_FreeCString(context, chars);
  }

  if (message.empty()) {
    message = "QuickJS vm execution failed";
  }

  return message;
}

bool ThrowQuickJSException(napi_env env, JSContext* context,
                           JSValue exception) {
  std::string message = GetQuickJSExceptionMessage(context, exception);
  JS_FreeValue(context, exception);
  napi_throw_error(env, nullptr, message.c_str());
  return false;
}

bool ThrowLatestQuickJSException(napi_env env, JSContext* context) {
  return ThrowQuickJSException(env, context, JS_GetException(context));
}

JSValue CloneQuickJSValue(JSContext* sourceContext,
                          JSContext* destinationContext, JSValueConst value) {
  if (JS_IsUndefined(value)) {
    return JS_UNDEFINED;
  }

  if (JS_IsNull(value)) {
    return JS_NULL;
  }

  if (JS_IsBool(value)) {
    int boolValue = JS_ToBool(sourceContext, value);
    if (boolValue < 0) {
      return JS_EXCEPTION;
    }
    return JS_NewBool(destinationContext, boolValue);
  }

  if (JS_IsString(value)) {
    size_t length = 0;
    const char* chars = JS_ToCStringLen(sourceContext, &length, value);
    if (chars == nullptr) {
      return JS_EXCEPTION;
    }

    JSValue result = JS_NewStringLen(destinationContext, chars, length);
    JS_FreeCString(sourceContext, chars);
    return result;
  }

  if (JS_IsBigInt(sourceContext, value)) {
    int64_t bigintValue = 0;
    if (JS_ToBigInt64(sourceContext, &bigintValue, value) < 0) {
      JS_ThrowTypeError(destinationContext,
                        "Only signed 64-bit BigInt values are supported in "
                        "vm contexts");
      return JS_EXCEPTION;
    }
    return JS_NewBigInt64(destinationContext, bigintValue);
  }

  if (JS_IsNumber(value)) {
    double numberValue = 0;
    if (JS_ToFloat64(sourceContext, &numberValue, value) < 0) {
      return JS_EXCEPTION;
    }

    if (std::isfinite(numberValue) && std::floor(numberValue) == numberValue &&
        numberValue >= static_cast<double>(INT32_MIN) &&
        numberValue <= static_cast<double>(INT32_MAX)) {
      return JS_NewInt32(destinationContext, static_cast<int32_t>(numberValue));
    }

    return JS_NewFloat64(destinationContext, numberValue);
  }

  if (JS_IsObject(value)) {
    if (JS_IsFunction(sourceContext, value)) {
      JS_ThrowTypeError(destinationContext,
                        "Functions are not supported in vm sandbox cloning");
      return JS_EXCEPTION;
    }

    JSValue json = JS_JSONStringify(sourceContext, const_cast<JSValue&>(value),
                                    JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) {
      JS_ThrowTypeError(destinationContext,
                        "Unsupported object value in vm sandbox");
      return JS_EXCEPTION;
    }

    size_t jsonLength = 0;
    const char* jsonChars = JS_ToCStringLen(sourceContext, &jsonLength, json);
    JS_FreeValue(sourceContext, json);
    if (jsonChars == nullptr) {
      return JS_EXCEPTION;
    }

    JSValue result =
        JS_ParseJSON(destinationContext, jsonChars, jsonLength, "<vm-context>");
    JS_FreeCString(sourceContext, jsonChars);
    return result;
  }

  JS_ThrowTypeError(destinationContext,
                    "Unsupported value type in vm sandbox cloning");
  return JS_EXCEPTION;
}

bool CompileOnlyQuickJS(napi_env env, const std::string& source,
                        const std::string& filename) {
  JSContext* context = qjs_get_context(env);
  JSValue compiled =
      JS_Eval(context, source.c_str(), source.size(), filename.c_str(),
              JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) {
    return ThrowLatestQuickJSException(env, context);
  }

  JS_FreeValue(context, compiled);
  return true;
}

bool CreateContextState(napi_env env, ContextState* state) {
  state->runtime = qjs_get_runtime(env);
  state->context = JS_NewContext(state->runtime);
  if (state->context == nullptr) {
    napi_throw_error(env, nullptr, "Failed to create QuickJS vm context");
    return false;
  }

  JSValue global = JS_GetGlobalObject(state->context);
  state->baselineKeys =
      ToKeySet(CollectQuickJSOwnStringKeys(state->context, global));
  JS_FreeValue(state->context, global);
  return true;
}

bool SyncSandboxToContext(napi_env env, napi_value sandboxValue,
                          ContextState* state,
                          std::unordered_set<std::string>& sandboxKeys) {
  JSContext* mainContext = qjs_get_context(env);
  JSContext* childContext = state->context;
  JSValue childGlobal = JS_GetGlobalObject(childContext);

  napi_value keyArray;
  if (napi_get_all_property_names(
          env, sandboxValue, napi_key_own_only,
          static_cast<napi_key_filter>(napi_key_all_properties |
                                       napi_key_skip_symbols),
          napi_key_numbers_to_strings, &keyArray) != napi_ok) {
    JS_FreeValue(childContext, childGlobal);
    return false;
  }

  uint32_t keyCount = 0;
  napi_get_array_length(env, keyArray, &keyCount);

  for (uint32_t index = 0; index < keyCount; ++index) {
    napi_value keyValue;
    if (napi_get_element(env, keyArray, index, &keyValue) != napi_ok) {
      continue;
    }

    std::string key;
    if (!CoerceToString(env, keyValue, key)) {
      continue;
    }

    sandboxKeys.insert(key);

    napi_value propertyValue;
    if (napi_get_named_property(env, sandboxValue, key.c_str(),
                                &propertyValue) != napi_ok) {
      continue;
    }

    JSValue cloned = CloneQuickJSValue(mainContext, childContext,
                                       QuickJSToValue(propertyValue));
    if (JS_IsException(cloned)) {
      JS_FreeValue(childContext, childGlobal);
      return ThrowLatestQuickJSException(env, childContext);
    }

    if (JS_SetPropertyStr(childContext, childGlobal, key.c_str(), cloned) < 0) {
      JS_FreeValue(childContext, childGlobal);
      return ThrowLatestQuickJSException(env, childContext);
    }
  }

  JS_FreeValue(childContext, childGlobal);
  return true;
}

bool SyncContextToSandbox(napi_env env, napi_value sandboxValue,
                          ContextState* state,
                          const std::unordered_set<std::string>& sandboxKeys) {
  JSContext* mainContext = qjs_get_context(env);
  JSContext* childContext = state->context;
  JSValue childGlobal = JS_GetGlobalObject(childContext);
  auto childKeys = CollectQuickJSOwnStringKeys(childContext, childGlobal);
  auto childKeySet = ToKeySet(childKeys);

  for (const auto& key : sandboxKeys) {
    if (childKeySet.count(key) != 0) {
      continue;
    }

    napi_value keyValue;
    if (napi_create_string_utf8(env, key.c_str(), key.size(), &keyValue) !=
        napi_ok) {
      JS_FreeValue(childContext, childGlobal);
      return false;
    }

    bool deleted = false;
    if (napi_delete_property(env, sandboxValue, keyValue, &deleted) !=
        napi_ok) {
      JS_FreeValue(childContext, childGlobal);
      return false;
    }
  }

  for (const auto& key : childKeys) {
    if (sandboxKeys.count(key) == 0 && state->baselineKeys.count(key) != 0) {
      continue;
    }

    JSValue childValue =
        JS_GetPropertyStr(childContext, childGlobal, key.c_str());
    if (JS_IsException(childValue)) {
      JS_FreeValue(childContext, childGlobal);
      return ThrowLatestQuickJSException(env, childContext);
    }

    JSValue cloned = CloneQuickJSValue(childContext, mainContext, childValue);
    JS_FreeValue(childContext, childValue);
    if (JS_IsException(cloned)) {
      JS_FreeValue(childContext, childGlobal);
      return ThrowLatestQuickJSException(env, mainContext);
    }

    napi_value resultValue;
    if (qjs_create_scoped_value(env, cloned, &resultValue) != napi_ok) {
      JS_FreeValue(childContext, childGlobal);
      return false;
    }

    if (napi_set_named_property(env, sandboxValue, key.c_str(), resultValue) !=
        napi_ok) {
      JS_FreeValue(childContext, childGlobal);
      return false;
    }
  }

  JS_FreeValue(childContext, childGlobal);
  return true;
}

napi_value RunInContextImpl(napi_env env, ContextState* state,
                            napi_value sandboxValue, const std::string& source,
                            const std::string& filename) {
  std::unordered_set<std::string> sandboxKeys;
  if (!SyncSandboxToContext(env, sandboxValue, state, sandboxKeys)) {
    return nullptr;
  }

  JSContext* childContext = state->context;
  JSValue result = JS_Eval(childContext, source.c_str(), source.size(),
                           filename.c_str(), JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result)) {
    ThrowLatestQuickJSException(env, childContext);
    return nullptr;
  }

  if (!SyncContextToSandbox(env, sandboxValue, state, sandboxKeys)) {
    JS_FreeValue(childContext, result);
    return nullptr;
  }

  JSContext* mainContext = qjs_get_context(env);
  JSValue clonedResult = CloneQuickJSValue(childContext, mainContext, result);
  JS_FreeValue(childContext, result);
  if (JS_IsException(clonedResult)) {
    ThrowLatestQuickJSException(env, mainContext);
    return nullptr;
  }

  napi_value resultValue;
  if (qjs_create_scoped_value(env, clonedResult, &resultValue) != napi_ok) {
    return nullptr;
  }

  return resultValue;
}

#else

bool CreateContextState(napi_env env, ContextState* state) {
  state->baselineKeys.clear();
  return true;
}

napi_value RunInContextImpl(napi_env env, ContextState* state,
                            napi_value sandboxValue, const std::string& source,
                            const std::string& filename) {
  return ThrowEngineVmUnsupportedValue(env, "Contextified execution");
}

#endif

bool CreateAndAttachContextState(napi_env env, napi_value sandbox,
                                 ContextState** out) {
  ContextState* existing = nullptr;
  if (GetContextState(env, sandbox, &existing)) {
    *out = existing;
    return true;
  }

  std::unique_ptr<ContextState> state(new ContextState());
  if (!CreateContextState(env, state.get())) {
    return false;
  }

  napi_value stateValue;
  if (napi_create_external(env, state.get(), FinalizeContextState, nullptr,
                           &stateValue) != napi_ok) {
    return false;
  }

  napi_value symbol = GetContextSymbol(env);
  if (napi_set_property(env, sandbox, symbol, stateValue) != napi_ok) {
    return false;
  }

  *out = state.release();
  return true;
}

bool ValidateSourceArgument(napi_env env, napi_value value,
                            std::string& source) {
  if (IsNullOrUndefined(env, value)) {
    napi_throw_type_error(env, nullptr,
                          "The \"code\" argument must be coercible to string");
    return false;
  }

  if (!CoerceToString(env, value, source)) {
    napi_throw_type_error(env, nullptr,
                          "The \"code\" argument must be coercible to string");
    return false;
  }

  return true;
}

bool ResolveSandboxArgument(napi_env env, napi_value value,
                            napi_value* sandbox) {
  if (IsNullOrUndefined(env, value)) {
    return napi_create_object(env, sandbox) == napi_ok;
  }

  if (!IsObjectLike(env, value)) {
    napi_throw_type_error(env, nullptr,
                          "The \"sandbox\" argument must be an object");
    return false;
  }

  *sandbox = value;
  return true;
}

napi_value RunSourceInThisContext(napi_env env, const std::string& source,
                                  const std::string& filename) {
  napi_value script;
  napi_value result;
  if (napi_create_string_utf8(env, source.c_str(), source.size(), &script) !=
          napi_ok ||
      napi_run_script_source(env, script, filename.c_str(), &result) !=
          napi_ok) {
    return nullptr;
  }

  return result;
}

napi_value CreateContextCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  napi_value sandbox;
  if (argc < 1 || IsNullOrUndefined(env, argv[0])) {
    napi_create_object(env, &sandbox);
  } else if (!IsObjectLike(env, argv[0])) {
    napi_throw_type_error(env, nullptr,
                          "The \"contextObject\" argument must be an object");
    return nullptr;
  } else {
    sandbox = argv[0];
  }

  ContextState* state = nullptr;
  if (!CreateAndAttachContextState(env, sandbox, &state)) {
    return nullptr;
  }

  return sandbox;
}

napi_value IsContextCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  bool isContext = false;
  if (argc >= 1) {
    ContextState* state = nullptr;
    isContext = GetContextState(env, argv[0], &state) && state != nullptr;
  }

  napi_value result;
  napi_get_boolean(env, isContext, &result);
  return result;
}

napi_value RunInContextCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(3)

  if (argc < 2) {
    napi_throw_type_error(env, nullptr,
                          "vm.runInContext(code, contextifiedObject) requires "
                          "a contextified object");
    return nullptr;
  }

  std::string source;
  if (!ValidateSourceArgument(env, argv[0], source)) {
    return nullptr;
  }

  ContextState* state = nullptr;
  if (!RequireContextState(env, argv[1], &state)) {
    return nullptr;
  }

  const std::string filename =
      ReadFilenameOption(env, argc >= 3 ? argv[2] : nullptr);
  return RunInContextImpl(env, state, argv[1], source, filename);
}

napi_value RunInNewContextCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(3)

  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "vm.runInNewContext(code[, sandbox]) requires code");
    return nullptr;
  }

  std::string source;
  if (!ValidateSourceArgument(env, argv[0], source)) {
    return nullptr;
  }

  napi_value sandbox = nullptr;
  napi_value options = nullptr;

  if (argc >= 2 && IsObjectLike(env, argv[1])) {
    sandbox = argv[1];
    options = argc >= 3 ? argv[2] : nullptr;
  } else {
    options = argc >= 2 ? argv[1] : nullptr;
  }

  if (!ResolveSandboxArgument(env, sandbox, &sandbox)) {
    return nullptr;
  }

  ContextState* state = nullptr;
  if (!CreateAndAttachContextState(env, sandbox, &state)) {
    return nullptr;
  }

  const std::string filename = ReadFilenameOption(env, options);
  return RunInContextImpl(env, state, sandbox, source, filename);
}

napi_value RunInThisContextCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "vm.runInThisContext(code[, options]) requires code");
    return nullptr;
  }

  std::string source;
  if (!ValidateSourceArgument(env, argv[0], source)) {
    return nullptr;
  }

  const std::string filename =
      ReadFilenameOption(env, argc >= 2 ? argv[1] : nullptr);
  return RunSourceInThisContext(env, source, filename);
}

ScriptState* GetScriptState(napi_env env, napi_callback_info info,
                            napi_value* thisValue = nullptr) {
  napi_value jsThis;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr) !=
      napi_ok) {
    return nullptr;
  }

  if (thisValue != nullptr) {
    *thisValue = jsThis;
  }

  ScriptState* script = nullptr;
  if (napi_unwrap(env, jsThis, reinterpret_cast<void**>(&script)) != napi_ok ||
      script == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid vm.Script receiver");
    return nullptr;
  }

  return script;
}

napi_value ScriptConstructor(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "new vm.Script(code[, options]) requires code");
    return nullptr;
  }

  std::string source;
  if (!ValidateSourceArgument(env, argv[0], source)) {
    return nullptr;
  }

  const std::string filename =
      ReadFilenameOption(env, argc >= 2 ? argv[1] : nullptr);

#if defined(TARGET_ENGINE_V8)
  if (!CompileOnlyV8(env, source, filename)) {
    return nullptr;
  }
#elif defined(TARGET_ENGINE_QUICKJS)
  if (!CompileOnlyQuickJS(env, source, filename)) {
    return nullptr;
  }
#else
  // Hermes currently executes vm.Script bodies through the generic
  // runInThisContext path at call time. Separate pre-compilation support for
  // node:vm has not been wired up yet.
#endif

  ScriptState* script = new ScriptState{source, filename};
  if (napi_wrap(env, jsThis, script, FinalizeScriptState, nullptr, nullptr) !=
      napi_ok) {
    delete script;
    return nullptr;
  }

  return jsThis;
}

napi_value ScriptRunInContext(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  ScriptState* script = GetScriptState(env, info);
  if (script == nullptr) {
    return nullptr;
  }

  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "script.runInContext(contextifiedObject[, options]) "
                          "requires a context");
    return nullptr;
  }

  ContextState* state = nullptr;
  if (!RequireContextState(env, argv[0], &state)) {
    return nullptr;
  }

  const std::string filename =
      ReadFilenameOption(env, argc >= 2 ? argv[1] : nullptr, script->filename);
  return RunInContextImpl(env, state, argv[0], script->source, filename);
}

napi_value ScriptRunInNewContext(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  ScriptState* script = GetScriptState(env, info);
  if (script == nullptr) {
    return nullptr;
  }

  napi_value sandbox = nullptr;
  napi_value options = nullptr;

  if (argc >= 1 && IsObjectLike(env, argv[0])) {
    sandbox = argv[0];
    options = argc >= 2 ? argv[1] : nullptr;
  } else {
    options = argc >= 1 ? argv[0] : nullptr;
  }

  if (!ResolveSandboxArgument(env, sandbox, &sandbox)) {
    return nullptr;
  }

  ContextState* state = nullptr;
  if (!CreateAndAttachContextState(env, sandbox, &state)) {
    return nullptr;
  }

  const std::string filename =
      ReadFilenameOption(env, options, script->filename);
  return RunInContextImpl(env, state, sandbox, script->source, filename);
}

napi_value ScriptRunInThisContext(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  ScriptState* script = GetScriptState(env, info);
  if (script == nullptr) {
    return nullptr;
  }

  const std::string filename =
      ReadFilenameOption(env, argc >= 1 ? argv[0] : nullptr, script->filename);
  return RunSourceInThisContext(env, script->source, filename);
}

napi_value CreateSourceTextModuleCallback(napi_env env,
                                          napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "vm.SourceTextModule requires source text");
    return nullptr;
  }

  std::string sourceText;
  if (!ValidateSourceArgument(env, argv[0], sourceText)) {
    return nullptr;
  }

#if defined(TARGET_ENGINE_V8)
  napi_value result;
  if (!CreateV8SourceTextModule(env, sourceText, argc >= 2 ? argv[1] : nullptr,
                                &result)) {
    return nullptr;
  }
  return result;
#elif defined(TARGET_ENGINE_QUICKJS)
  napi_value result;
  if (!CreateQuickJSSourceTextModule(env, sourceText,
                                     argc >= 2 ? argv[1] : nullptr, &result)) {
    return nullptr;
  }
  return result;
#else
  return ThrowEngineVmUnsupportedValue(env, "vm.SourceTextModule");
#endif
}

napi_value CreateSyntheticModuleCallback(napi_env env,
                                         napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "vm.SyntheticModule requires export names");
    return nullptr;
  }

  std::vector<std::string> exportNames;
  if (!GetStringArray(env, argv[0], exportNames)) {
    napi_throw_type_error(env, nullptr,
                          "The \"exportNames\" argument must be an Array");
    return nullptr;
  }

#if defined(TARGET_ENGINE_V8)
  napi_value result;
  if (!CreateV8SyntheticModule(env, exportNames, argc >= 2 ? argv[1] : nullptr,
                               &result)) {
    return nullptr;
  }
  return result;
#elif defined(TARGET_ENGINE_QUICKJS)
  napi_value result;
  if (!CreateQuickJSSyntheticModule(env, exportNames,
                                    argc >= 2 ? argv[1] : nullptr, &result)) {
    return nullptr;
  }
  return result;
#else
  return ThrowEngineVmUnsupportedValue(env, "vm.SyntheticModule");
#endif
}

napi_value ModuleGetIdentifierCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

  napi_value result;
  napi_create_string_utf8(env, state->identifier.c_str(),
                          state->identifier.size(), &result);
  return result;
}

napi_value ModuleGetContextCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }
  return GetStoredModuleContext(env, state);
}

napi_value ModuleGetDependencySpecifiersCallback(napi_env env,
                                                 napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }
  return CreateStringArray(env, state->dependencySpecifiers);
}

napi_value ModuleGetStatusCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

  std::string statusText;
#if defined(TARGET_ENGINE_V8)
  statusText =
      GetV8ModuleStatusString(state->module.Get(env->isolate)->GetStatus());
#elif defined(TARGET_ENGINE_QUICKJS)
  statusText = GetQuickJSModuleStatusString(state);
#endif

  napi_value result;
  napi_create_string_utf8(env, statusText.c_str(), statusText.size(), &result);
  return result;
}

napi_value ModuleGetErrorCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

#if defined(TARGET_ENGINE_V8)
  if (state->module.Get(env->isolate)->GetStatus() == v8::Module::kErrored) {
    if (!CacheV8ModuleError(env, state)) {
      return nullptr;
    }
  }
#endif
  return GetStoredModuleError(env, state);
}

napi_value ModuleLinkRequestsCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)

  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

  bool isArray = false;
  if (argc < 2 || napi_is_array(env, argv[1], &isArray) != napi_ok ||
      !isArray) {
    napi_throw_type_error(env, nullptr,
                          "The \"linkedModules\" argument must be an Array");
    return nullptr;
  }

  uint32_t length = 0;
  napi_get_array_length(env, argv[1], &length);
  state->linkedModules.assign(length, nullptr);
  for (uint32_t index = 0; index < length; ++index) {
    napi_value entry;
    if (napi_get_element(env, argv[1], index, &entry) != napi_ok ||
        !GetModuleState(env, entry, &state->linkedModules[index])) {
      return nullptr;
    }
  }

#if defined(TARGET_ENGINE_V8)
  v8::Isolate* isolate = env->isolate;
  v8::HandleScope scope(isolate);
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Context> context = GetV8ModuleContext(env, state);
  v8::Context::Scope contextScope(context);
  v8::Local<v8::Module> module = state->module.Get(isolate);
  if (!module->InstantiateModule(context, &ResolveV8VmModuleByIndex)
           .FromMaybe(false)) {
    if (tryCatch.HasCaught()) {
      napi_throw(env, v8impl::JsValueFromV8LocalValue(tryCatch.Exception()));
    } else if (module->GetStatus() == v8::Module::kErrored) {
      CacheV8ModuleError(env, state);
      ThrowV8ModuleException(env, state);
    } else {
      napi_throw_error(env, nullptr, "Failed to link vm.Module");
    }
    return nullptr;
  }
#elif defined(TARGET_ENGINE_QUICKJS)
  QuickJSModuleRegistry& registry =
      EnsureQuickJSModuleRegistry(env, state->runtime);
  for (const auto& specifier : state->dependencySpecifiers) {
    registry.resolutions.erase(
        MakeQuickJSResolutionKey(state->identifier, specifier));
  }
  for (size_t index = 0; index < state->linkedModules.size() &&
                         index < state->dependencySpecifiers.size();
       ++index) {
    ModuleState* linked = state->linkedModules[index];
    if (linked == nullptr) {
      continue;
    }
    registry.resolutions[MakeQuickJSResolutionKey(
        state->identifier, state->dependencySpecifiers[index])] =
        linked->identifier;
  }

  if (!EnsureQuickJSLinked(env, state)) {
    return nullptr;
  }
#endif

  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ModuleInstantiateCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

#if defined(TARGET_ENGINE_V8)
  v8::Isolate* isolate = env->isolate;
  v8::HandleScope scope(isolate);
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Context> context = GetV8ModuleContext(env, state);
  v8::Context::Scope contextScope(context);
  v8::Local<v8::Module> module = state->module.Get(isolate);
  if (module->GetStatus() == v8::Module::kUninstantiated &&
      !module->InstantiateModule(context, &ResolveV8VmModuleByIndex)
           .FromMaybe(false)) {
    if (tryCatch.HasCaught()) {
      napi_throw(env, v8impl::JsValueFromV8LocalValue(tryCatch.Exception()));
    } else {
      napi_throw_error(env, nullptr, "Failed to instantiate vm.Module");
    }
    return nullptr;
  }
#elif defined(TARGET_ENGINE_QUICKJS)
  if (!EnsureQuickJSLinked(env, state)) {
    return nullptr;
  }
#endif

  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ModuleEvaluateCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

#if defined(TARGET_ENGINE_V8)
  v8::Isolate* isolate = env->isolate;
  v8::EscapableHandleScope scope(isolate);
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Context> context = GetV8ModuleContext(env, state);
  v8::Context::Scope contextScope(context);
  v8::Local<v8::Module> module = state->module.Get(isolate);

  v8::MaybeLocal<v8::Value> maybeResult = module->Evaluate(context);
  if (maybeResult.IsEmpty()) {
    if (tryCatch.HasCaught()) {
      napi_throw(env, v8impl::JsValueFromV8LocalValue(tryCatch.Exception()));
    } else if (module->GetStatus() == v8::Module::kErrored) {
      CacheV8ModuleError(env, state);
      ThrowV8ModuleException(env, state);
    } else {
      napi_throw_error(env, nullptr, "Failed to evaluate vm.Module");
    }
    return nullptr;
  }

  return v8impl::JsValueFromV8LocalValue(
      scope.Escape(maybeResult.ToLocalChecked()));
#elif defined(TARGET_ENGINE_QUICKJS)
  if (!EnsureQuickJSLinked(env, state) || !EnsureQuickJSImportMeta(state)) {
    return nullptr;
  }

  state->evaluating = true;
  JSValue result = JS_EvalFunction(
      state->context, JS_DupValue(state->context, state->moduleValue));
  if (JS_IsException(result)) {
    state->evaluating = false;
    state->errored = true;
    JSValue exception = JS_GetException(state->context);
    state->errorMessage = GetQuickJSExceptionMessage(state->context, exception);
    if (!CacheQuickJSError(env, state, exception)) {
      return nullptr;
    }
    napi_value error = GetStoredModuleError(env, state);
    napi_value promise;
    CreatePromiseSettledWithUndefined(env, true, error, &promise);
    return promise;
  }

  if (JS_IsObject(result) &&
      JS_PromiseState(state->context, result) == JS_PROMISE_PENDING) {
    while (JS_PromiseState(state->context, result) == JS_PROMISE_PENDING) {
      if (qjs_execute_pending_jobs(env) != napi_ok) {
        JS_FreeValue(state->context, result);
        return nullptr;
      }
    }
  }

  bool rejected =
      JS_IsObject(result) &&
      JS_PromiseState(state->context, result) == JS_PROMISE_REJECTED;
  if (rejected) {
    state->evaluating = false;
    state->errored = true;
    JSValue rejection = JS_PromiseResult(state->context, result);
    JS_FreeValue(state->context, result);
    if (!CacheQuickJSError(env, state, rejection)) {
      return nullptr;
    }
    napi_value error = GetStoredModuleError(env, state);
    napi_value promise;
    CreatePromiseSettledWithUndefined(env, true, error, &promise);
    return promise;
  }

  if (state->kind == ModuleKind::kSynthetic &&
      !ApplyQuickJSSyntheticExports(state)) {
    JS_FreeValue(state->context, result);
    return ThrowLatestQuickJSException(env, state->context), nullptr;
  }

  state->evaluating = false;
  state->evaluated = true;
  SetModuleError(env, state, nullptr);
  JS_FreeValue(state->context, result);
  napi_value promise;
  CreatePromiseSettledWithUndefined(env, false, nullptr, &promise);
  return promise;
#endif
}

napi_value ModuleGetExportNamesCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

  std::vector<std::string> exportNames = state->exportNames;
#if defined(TARGET_ENGINE_V8)
  if (exportNames.empty()) {
    exportNames = CollectV8ModuleExportNames(env, state);
  }
#elif defined(TARGET_ENGINE_QUICKJS)
  if (exportNames.empty() && state->evaluated) {
    JSModuleDef* module =
        static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(state->moduleValue));
    JSValue ns = JS_GetModuleNamespace(state->context, module);
    if (!JS_IsException(ns)) {
      exportNames = CollectQuickJSOwnStringKeys(state->context, ns);
      JS_FreeValue(state->context, ns);
    }
  }
#endif
  return CreateStringArray(env, exportNames);
}

napi_value ModuleGetNamespaceValueCallback(napi_env env,
                                           napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(2)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

  std::string exportName;
  if (argc < 2 || !CoerceToString(env, argv[1], exportName)) {
    napi_throw_type_error(env, nullptr, "Missing export name");
    return nullptr;
  }

#if defined(TARGET_ENGINE_V8)
  v8::Isolate* isolate = env->isolate;
  v8::EscapableHandleScope scope(isolate);
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Context> context = GetV8ModuleContext(env, state);
  v8::Context::Scope contextScope(context);
  v8::Local<v8::Object> ns =
      state->module.Get(isolate)->GetModuleNamespace().As<v8::Object>();
  v8::Local<v8::String> key =
      v8::String::NewFromUtf8(isolate, exportName.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(exportName.size()))
          .ToLocalChecked();
  v8::Local<v8::Value> value;
  if (!ns->Get(context, key).ToLocal(&value)) {
    if (tryCatch.HasCaught()) {
      napi_throw(env, v8impl::JsValueFromV8LocalValue(tryCatch.Exception()));
    }
    return nullptr;
  }
  return v8impl::JsValueFromV8LocalValue(scope.Escape(value));
#elif defined(TARGET_ENGINE_QUICKJS)
  JSModuleDef* module =
      static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(state->moduleValue));
  JSValue ns = JS_GetModuleNamespace(state->context, module);
  if (JS_IsException(ns)) {
    return nullptr;
  }

  JSValue value = JS_GetPropertyStr(state->context, ns, exportName.c_str());
  JS_FreeValue(state->context, ns);
  if (JS_IsException(value)) {
    return ThrowLatestQuickJSException(env, state->context), nullptr;
  }

  JSValue cloned =
      CloneQuickJSValue(state->context, qjs_get_context(env), value);
  JS_FreeValue(state->context, value);
  if (JS_IsException(cloned)) {
    return ThrowLatestQuickJSException(env, qjs_get_context(env)), nullptr;
  }

  napi_value result;
  if (qjs_create_scoped_value(env, cloned, &result) != napi_ok) {
    return nullptr;
  }
  return result;
#endif
}

napi_value ModuleCreateCachedDataCallback(napi_env env,
                                          napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

#if defined(TARGET_ENGINE_V8)
  v8::Isolate* isolate = env->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Module> module = state->module.Get(isolate);
  v8::ScriptCompiler::CachedData* cachedData =
      v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript());
  if (cachedData == nullptr) {
    return CreateUint8ArrayCopy(env, nullptr, 0);
  }
  napi_value result = CreateUint8ArrayCopy(
      env, reinterpret_cast<const uint8_t*>(cachedData->data),
      cachedData->length);
  delete cachedData;
  return result;
#elif defined(TARGET_ENGINE_QUICKJS)
  size_t length = 0;
  uint8_t* bytecodeData = JS_WriteObject(
      state->context, &length, state->moduleValue, JS_WRITE_OBJ_BYTECODE);
  if (bytecodeData == nullptr) {
    return ThrowLatestQuickJSException(env, state->context), nullptr;
  }
  napi_value result = CreateUint8ArrayCopy(env, bytecodeData, length);
  js_free(state->context, bytecodeData);
  return result;
#endif
}

napi_value ModuleHasTopLevelAwaitCallback(napi_env env,
                                          napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }
  bool resultBool = false;
#if defined(TARGET_ENGINE_V8)
  resultBool = state->module.Get(env->isolate)->HasTopLevelAwait();
#elif defined(TARGET_ENGINE_QUICKJS)
  resultBool = false;
#endif
  napi_value result;
  napi_get_boolean(env, resultBool, &result);
  return result;
}

napi_value ModuleHasAsyncGraphCallback(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }
  bool resultBool = false;
#if defined(TARGET_ENGINE_V8)
  if (state->module.Get(env->isolate)->GetStatus() >=
      v8::Module::kInstantiated) {
    resultBool = state->module.Get(env->isolate)->IsGraphAsync();
  }
#elif defined(TARGET_ENGINE_QUICKJS)
  resultBool = false;
#endif
  napi_value result;
  napi_get_boolean(env, resultBool, &result);
  return result;
}

napi_value ModuleSetSyntheticExportCallback(napi_env env,
                                            napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(3)
  ModuleState* state = nullptr;
  if (!GetModuleState(env, argv[0], &state)) {
    return nullptr;
  }

  std::string exportName;
  if (argc < 3 || !CoerceToString(env, argv[1], exportName)) {
    napi_throw_type_error(env, nullptr, "Synthetic export name is required");
    return nullptr;
  }

#if defined(TARGET_ENGINE_V8)
  v8::Isolate* isolate = env->isolate;
  v8::HandleScope scope(isolate);
  v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(argv[2]);
  v8::Local<v8::String> key =
      v8::String::NewFromUtf8(isolate, exportName.c_str(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(exportName.size()))
          .ToLocalChecked();
  if (!state->module.Get(isolate)
           ->SetSyntheticModuleExport(isolate, key, value)
           .FromMaybe(false)) {
    napi_throw_error(env, nullptr, "Failed to set synthetic export");
    return nullptr;
  }
#elif defined(TARGET_ENGINE_QUICKJS)
  JSValue cloned = CloneQuickJSValue(qjs_get_context(env), state->context,
                                     QuickJSToValue(argv[2]));
  if (JS_IsException(cloned)) {
    return ThrowLatestQuickJSException(env, state->context), nullptr;
  }

  auto existing = state->syntheticExports.find(exportName);
  if (existing != state->syntheticExports.end()) {
    JS_FreeValue(state->context, existing->second);
    existing->second = cloned;
  } else {
    state->syntheticExports.emplace(exportName, cloned);
  }

  if (state->evaluated &&
      JS_SetModuleExport(
          state->context,
          static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(state->moduleValue)),
          exportName.c_str(),
          JS_DupValue(state->context, state->syntheticExports[exportName])) <
          0) {
    return ThrowLatestQuickJSException(env, state->context), nullptr;
  }
#endif

  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value CreatePublicVmExports(napi_env env, napi_value binding) {
  static const char* kWrapperSource = R"JS(
(function(binding) {
  const NativeScriptCtor = binding.Script;
  const scriptState = new WeakMap();
  const nativeScript = new WeakMap();
  const moduleState = new WeakMap();
  const kDontContextify = Object.freeze({ __vmDontContextify: true });
  const kUseMainContextDefaultLoader = Symbol('vm.useMainContextDefaultLoader');
  const constants = Object.freeze({
    DONT_CONTEXTIFY: kDontContextify,
    USE_MAIN_CONTEXT_DEFAULT_LOADER: kUseMainContextDefaultLoader,
  });
  const engine =
    typeof binding.engine === 'string' && binding.engine
      ? binding.engine
      : (
          typeof process === 'object' &&
          process !== null &&
          process.versions &&
          typeof process.versions.engine === 'string'
            ? process.versions.engine
            : ''
        );
  const usesContextExecutionFallback = !!binding.useContextExecutionFallback;
  const usesJsModuleFallback = !!binding.useJsModuleFallback;

  let nextId = 1;

  function nextToken(prefix) {
    return `__ns_vm_${prefix}_${nextId++}`;
  }

  function isObjectLike(value) {
    return value !== null && (typeof value === 'object' || typeof value === 'function');
  }

  function normalizeFilename(options, fallback = 'vm.js') {
    if (typeof options === 'string' && options) {
      return options;
    }

    if (!options || typeof options !== 'object') {
      return fallback;
    }

    if (typeof options.filename === 'string' && options.filename) {
      return options.filename;
    }

    return fallback;
  }

  function extractSourceMapURL(source) {
    const match = String(source).match(/[#@]\s*sourceMappingURL\s*=\s*(\S+)\s*$/m);
    return match ? match[1] : undefined;
  }

  function encodeSource(source) {
    const text = String(source);
    const bytes = new Uint8Array(text.length);
    for (let i = 0; i < text.length; i += 1) {
      bytes[i] = text.charCodeAt(i) & 0xff;
    }
    return bytes;
  }

  function ensureContextObject(value) {
    if (value === kDontContextify || value == null) {
      return binding.createContext({});
    }

    if (!isObjectLike(value)) {
      throw new TypeError('The "contextObject" argument must be an object');
    }

    return binding.createContext(value);
  }

  function ensureExistingContext(value) {
    if (!binding.isContext(value)) {
      throw new TypeError(
        'The "contextifiedObject" argument must be a vm context created by vm.createContext()',
      );
    }
    return value;
  }

  function copyExtensionProps(target, extensions) {
    const backups = [];
    for (const extension of extensions) {
      if (!isObjectLike(extension)) {
        continue;
      }

      for (const key of Object.keys(extension)) {
        backups.push({
          key,
          existed: Object.prototype.hasOwnProperty.call(target, key),
          value: target[key],
        });
        target[key] = extension[key];
      }
    }
    return backups;
  }

  function restoreExtensionProps(target, backups) {
    for (let index = backups.length - 1; index >= 0; index -= 1) {
      const entry = backups[index];
      if (entry.existed) {
        target[entry.key] = entry.value;
      } else {
        delete target[entry.key];
      }
    }
  }

  function resolveRunInNewContextArgs(contextObject, options) {
    if (arguments.length === 0) {
      return { sandbox: binding.createContext({}), options: undefined };
    }

    if (contextObject == null || contextObject === kDontContextify) {
      return { sandbox: binding.createContext({}), options };
    }

    if (isObjectLike(contextObject)) {
      return { sandbox: binding.createContext(contextObject), options };
    }

    return { sandbox: binding.createContext({}), options: contextObject };
  }

  function runFallbackInContext(source, contextifiedObject, options) {
    const context = ensureExistingContext(contextifiedObject);
    const filename = normalizeFilename(options);
    const globalObject = globalThis;
    const beforeKeys = new Set(Object.getOwnPropertyNames(globalObject));
    const overlayKeys = Object.keys(context);
    const backups = [];

    for (const key of overlayKeys) {
      backups.push({
        descriptor: Object.getOwnPropertyDescriptor(globalObject, key),
        existed: Object.prototype.hasOwnProperty.call(globalObject, key),
        key,
      });
      globalObject[key] = context[key];
    }

    const restoreGlobals = () => {
      const afterKeys = Object.getOwnPropertyNames(globalObject);
      const afterKeySet = new Set(afterKeys);

      for (const key of afterKeys) {
        if (!beforeKeys.has(key) || Object.prototype.hasOwnProperty.call(context, key)) {
          context[key] = globalObject[key];
        }
      }

      for (let index = backups.length - 1; index >= 0; index -= 1) {
        const backup = backups[index];
        if (backup.existed) {
          Object.defineProperty(globalObject, backup.key, backup.descriptor);
        } else {
          delete globalObject[backup.key];
        }
        afterKeySet.delete(backup.key);
      }

      for (const key of afterKeySet) {
        if (!beforeKeys.has(key)) {
          delete globalObject[key];
        }
      }
    };

    try {
      return binding.runInThisContext(String(source), { filename });
    } finally {
      restoreGlobals();
    }
  }

  function executeVmSource(source, contextifiedObject, options) {
    if (contextifiedObject) {
      if (usesContextExecutionFallback) {
        return runFallbackInContext(source, contextifiedObject, options);
      }
      return binding.runInContext(source, contextifiedObject, options);
    }

    return binding.runInThisContext(source, options);
  }

  function extractModuleDependencySpecifiers(source) {
    const specifiers = [];
    const seen = new Set();
    const patterns = [
      /(?:^|[\r\n])\s*import\s+[^;]*?\s+from\s+['"]([^'"]+)['"]/g,
      /(?:^|[\r\n])\s*import\s+['"]([^'"]+)['"]/g,
    ];

    for (const pattern of patterns) {
      for (const match of String(source).matchAll(pattern)) {
        const specifier = match[1];
        if (!seen.has(specifier)) {
          seen.add(specifier);
          specifiers.push(specifier);
        }
      }
    }

    return specifiers;
  }

  function parseNamedBindings(bindings) {
    return bindings
      .split(',')
      .map((entry) => entry.trim())
      .filter(Boolean)
      .map((entry) => {
        const aliasMatch = entry.match(/^([A-Za-z0-9_$]+)\s+as\s+([A-Za-z0-9_$]+)$/);
        if (aliasMatch) {
          return `${aliasMatch[1]}: ${aliasMatch[2]}`;
        }
        return entry;
      })
      .join(', ');
  }

  function transformFallbackModuleSource(sourceText) {
    return String(sourceText)
      .replace(
        /^\s*import\s+\{([^}]+)\}\s+from\s+['"]([^'"]+)['"]\s*;?\s*$/gm,
        (_match, bindings, specifier) =>
          `const { ${parseNamedBindings(bindings)} } = __imports[${JSON.stringify(specifier)}];`,
      )
      .replace(
        /^\s*import\s+([A-Za-z0-9_$]+)\s+from\s+['"]([^'"]+)['"]\s*;?\s*$/gm,
        (_match, binding, specifier) =>
          `const ${binding} = __imports[${JSON.stringify(specifier)}].default;`,
      )
      .replace(
        /^\s*export\s+default\s+([^;]+);?\s*$/gm,
        (_match, expression) => `__exports.default = (${expression});`,
      )
      .replace(
        /^\s*export\s+\{\s*([^}]+)\s*\}\s*;?\s*$/gm,
        (_match, bindings) =>
          bindings
            .split(',')
            .map((entry) => entry.trim())
            .filter(Boolean)
            .map((entry) => {
              const aliasMatch = entry.match(
                /^([A-Za-z0-9_$]+)\s+as\s+([A-Za-z0-9_$]+)$/,
              );
              if (aliasMatch) {
                return `__exports.${aliasMatch[2]} = ${aliasMatch[1]};`;
              }
              return `__exports.${entry} = ${entry};`;
            })
            .join('\n'),
      )
      .replace(
        /^\s*export\s+(const|let|var)\s+([A-Za-z0-9_$]+)\s*=\s*([^;]+);?\s*$/gm,
        (_match, declaration, name, expression) =>
          `${declaration} ${name} = ${expression};\n__exports.${name} = ${name};`,
      );
  }

  function createHermesNamespace(state) {
    if (state.namespace) {
      return state.namespace;
    }

    function createNamespaceGetter(exportName) {
      return function namespaceGetter() {
        return state.exports[exportName];
      };
    }

    const namespace = {};
    for (const name of state.exportNames) {
      const exportName = String(name);
      Object.defineProperty(namespace, name, {
        configurable: false,
        enumerable: true,
        get: createNamespaceGetter(exportName),
      });
    }
    state.namespace = Object.freeze(namespace);
    return state.namespace;
  }

  class Script {
    constructor(code, options) {
      const source = String(code);
      const filename = normalizeFilename(options);
      if (!usesContextExecutionFallback) {
        const native = new NativeScriptCtor(source, options);
        nativeScript.set(this, native);
      }
      scriptState.set(this, {
        source,
        filename,
        cachedDataRejected: false,
        sourceMapURL: extractSourceMapURL(source),
      });
    }

    runInContext(contextifiedObject, options) {
      const state = scriptState.get(this);
      const context = ensureExistingContext(contextifiedObject);
      if (usesContextExecutionFallback) {
        return executeVmSource(state.source, context, {
          filename: normalizeFilename(options, state.filename),
        });
      }
      return nativeScript.get(this).runInContext(context, options);
    }

    runInNewContext(contextObject, options) {
      const resolved = resolveRunInNewContextArgs(contextObject, options);
      const state = scriptState.get(this);
      if (usesContextExecutionFallback) {
        return executeVmSource(state.source, resolved.sandbox, {
          filename: normalizeFilename(resolved.options, state.filename),
        });
      }
      return nativeScript.get(this).runInNewContext(resolved.sandbox, resolved.options);
    }

    runInThisContext(options) {
      const state = scriptState.get(this);
      if (usesContextExecutionFallback) {
        return executeVmSource(state.source, null, {
          filename: normalizeFilename(options, state.filename),
        });
      }
      return nativeScript.get(this).runInThisContext(options);
    }

    createCachedData() {
      return encodeSource(scriptState.get(this).source);
    }

    get cachedDataRejected() {
      return scriptState.get(this).cachedDataRejected;
    }

    get sourceMapURL() {
      return scriptState.get(this).sourceMapURL;
    }
  }

  function createContext(contextObject) {
    return ensureContextObject(contextObject);
  }

  function isContext(value) {
    return binding.isContext(value);
  }

  function runInContext(code, contextifiedObject, options) {
    return executeVmSource(
      code,
      ensureExistingContext(contextifiedObject),
      options,
    );
  }

  function runInNewContext(code, contextObject, options) {
    const resolved = resolveRunInNewContextArgs(contextObject, options);
    return executeVmSource(code, resolved.sandbox, resolved.options);
  }

  function runInThisContext(code, options) {
    return binding.runInThisContext(code, options);
  }

  function normalizeParams(params) {
    if (params == null) {
      return [];
    }

    if (!Array.isArray(params)) {
      throw new TypeError('The "params" argument must be an Array');
    }

    return params.map((item) => String(item));
  }

  function compileFunction(code, params, options = {}) {
    const body = String(code);
    const paramNames = normalizeParams(params);
    const filename = normalizeFilename(options);
    const parsingContext = options.parsingContext
      ? ensureExistingContext(options.parsingContext)
      : null;
    const contextExtensions = Array.isArray(options.contextExtensions)
      ? options.contextExtensions
      : [];
    const sourceMapURL = extractSourceMapURL(body);

    const invoke = function invoke(thisArg, argsLike) {
      const args = Array.from(argsLike);
      const target = parsingContext || globalThis;
      const argsKey = nextToken('args');
      const thisKey = nextToken('this');
      const backups = parsingContext ? copyExtensionProps(target, contextExtensions) : [];

      target[argsKey] = args;
      target[thisKey] = thisArg;

      const invocationSource = `
        (function() {
          const __vmArgs = globalThis[${JSON.stringify(argsKey)}];
          const __vmThis = globalThis[${JSON.stringify(thisKey)}];
          return (function(${paramNames.join(',')}) {
${body}
          }).apply(__vmThis, __vmArgs);
        })()
      `;

      try {
        return executeVmSource(
          invocationSource,
          parsingContext ? target : null,
          { filename },
        );
      } finally {
        delete target[argsKey];
        delete target[thisKey];
        if (parsingContext) {
          restoreExtensionProps(target, backups);
        }
      }
    };

    const wrapperFactory = Function(
      'invoke',
      `return function(${paramNames.join(',')}) { 'use strict'; return invoke(this, arguments, new.target); };`,
    );
    const compiled = wrapperFactory(invoke);

    Object.defineProperties(compiled, {
      cachedDataRejected: {
        configurable: true,
        enumerable: false,
        get() {
          return false;
        },
      },
      sourceMapURL: {
        configurable: true,
        enumerable: false,
        get() {
          return sourceMapURL;
        },
      },
      createCachedData: {
        configurable: true,
        enumerable: false,
        writable: true,
        value() {
          return encodeSource(body);
        },
      },
    });

    return compiled;
  }

  function getModuleData(module) {
    const state = moduleState.get(module);
    if (!state) {
      throw new TypeError('Invalid vm.Module receiver');
    }
    return state;
  }

  class NativeModuleBase {
    constructor(handle) {
      if (new.target === NativeModuleBase) {
        throw new TypeError('vm.Module is an abstract class');
      }

      moduleState.set(this, {
        handle,
        namespace: null,
        linkedModules: [],
        linkerPromise: null,
        evaluatePromise: null,
        localError: undefined,
        statusOverride: null,
      });
    }

    get identifier() {
      return binding.moduleGetIdentifier(getModuleData(this).handle);
    }

    get context() {
      return binding.moduleGetContext(getModuleData(this).handle);
    }

    get status() {
      const state = getModuleData(this);
      return state.statusOverride || binding.moduleGetStatus(state.handle);
    }

    get namespace() {
      const state = getModuleData(this);
      if (this.status === 'unlinked') {
        throw new Error('Module has not been linked');
      }

      if (!state.namespace) {
        const handle = state.handle;
        const exportNames = binding.moduleGetExportNames(handle);
        const namespace = {};
        for (const name of exportNames) {
          Object.defineProperty(namespace, name, {
            configurable: false,
            enumerable: true,
            get() {
              return binding.moduleGetNamespaceValue(handle, name);
            },
          });
        }
        state.namespace = Object.freeze(namespace);
      }

      return state.namespace;
    }

    get error() {
      const state = getModuleData(this);
      return state.localError !== undefined ? state.localError : binding.moduleGetError(state.handle);
    }

    async link(linker) {
      if (typeof linker !== 'function') {
        throw new TypeError('The "linker" argument must be a function');
      }

      const state = getModuleData(this);
      if (state.linkerPromise) {
        return state.linkerPromise;
      }

      state.statusOverride = 'linking';
      state.localError = undefined;
      state.linkerPromise = (async () => {
        const dependencySpecifiers = binding.moduleGetDependencySpecifiers(state.handle);
        const linked = [];
        for (const specifier of dependencySpecifiers) {
          const resolved = await linker(specifier, this);
          if (!(resolved instanceof NativeModuleBase)) {
            throw new TypeError('Linker must return vm.Module instances');
          }
          linked.push(resolved);
        }
        state.linkedModules = linked;
        binding.moduleLinkRequests(
          state.handle,
          linked.map((module) => getModuleData(module).handle),
        );
        state.statusOverride = null;
      })().catch((error) => {
        state.statusOverride = 'errored';
        state.localError = error;
        throw error;
      });

      return state.linkerPromise;
    }

    async evaluate() {
      throw new TypeError('evaluate() is not implemented for this vm.Module');
    }
  }

  class NativeSourceTextModule extends NativeModuleBase {
    constructor(sourceText, options = {}) {
      const source = String(sourceText);
      super(binding.createSourceTextModule(source, options));
      const state = getModuleData(this);
      state.sourceText = source;
      state.sourceMapURL = extractSourceMapURL(state.sourceText);
    }

    get dependencySpecifiers() {
      return binding.moduleGetDependencySpecifiers(getModuleData(this).handle);
    }

    get moduleRequests() {
      return this.dependencySpecifiers.map((specifier) => ({
        specifier,
        attributes: Object.freeze({}),
        phase: 'evaluation',
      }));
    }

    get sourceMapURL() {
      return getModuleData(this).sourceMapURL;
    }

    createCachedData() {
      return binding.moduleCreateCachedData(getModuleData(this).handle);
    }

    hasTopLevelAwait() {
      return binding.moduleHasTopLevelAwait(getModuleData(this).handle);
    }

    hasAsyncGraph() {
      return binding.moduleHasAsyncGraph(getModuleData(this).handle);
    }

    linkRequests(modules) {
      const state = getModuleData(this);
      if (!Array.isArray(modules)) {
        throw new TypeError('The "modules" argument must be an Array');
      }
      if (modules.length !== this.dependencySpecifiers.length) {
        throw new Error('linkRequests() module count must match dependencySpecifiers');
      }

      for (let index = 0; index < modules.length; index += 1) {
        const mod = modules[index];
        if (!(mod instanceof NativeModuleBase)) {
          throw new TypeError('linkRequests() expects vm.Module instances');
        }
      }
      state.linkedModules = modules.slice();
    }

    instantiate() {
      const state = getModuleData(this);
      if (this.status === 'linked') {
        return;
      }
      if (state.linkedModules.length !== this.dependencySpecifiers.length) {
        throw new Error('linkRequests() module count must match dependencySpecifiers');
      }
      binding.moduleLinkRequests(
        state.handle,
        state.linkedModules.map((module) => {
          if (!(module instanceof NativeModuleBase)) {
            throw new TypeError('linkRequests() expects vm.Module instances');
          }
          return getModuleData(module).handle;
        }),
      );
      binding.moduleInstantiate(state.handle);
    }

    async evaluate() {
      const state = getModuleData(this);
      if (this.status === 'evaluated') {
        return undefined;
      }
      if (this.status === 'errored') {
        throw this.error;
      }
      if (state.evaluatePromise) {
        return state.evaluatePromise;
      }
      if (this.status === 'unlinked') {
        this.instantiate();
      }

      state.statusOverride = 'evaluating';
      state.localError = undefined;
      state.evaluatePromise = (async () => {
        for (const mod of state.linkedModules) {
          if (!(mod instanceof NativeModuleBase)) {
            throw new Error('Missing linked module');
          }
          await mod.evaluate();
        }
        await Promise.resolve(binding.moduleEvaluate(state.handle));
        state.statusOverride = null;
        return undefined;
      })().catch((error) => {
        state.statusOverride =
          binding.moduleGetStatus(state.handle) === 'errored' ? null : 'errored';
        state.localError = error;
        throw error;
      });
      return state.evaluatePromise;
    }
  }

  class NativeSyntheticModule extends NativeModuleBase {
    constructor(exportNames, evaluateCallback, options = {}) {
      if (!Array.isArray(exportNames)) {
        throw new TypeError('The "exportNames" argument must be an Array');
      }
      if (typeof evaluateCallback !== 'function') {
        throw new TypeError('The "evaluateCallback" argument must be a function');
      }

      super(binding.createSyntheticModule(exportNames.map((name) => String(name)), options));
      const state = getModuleData(this);
      state.exportNames = exportNames.map((name) => String(name));
      state.evaluateCallback = evaluateCallback;
    }

    setExport(name, value) {
      const state = getModuleData(this);
      const exportName = String(name);
      if (!state.exportNames.includes(exportName)) {
        throw new Error(`Unknown synthetic module export "${exportName}"`);
      }
      binding.moduleSetSyntheticExport(state.handle, exportName, value);
      state.namespace = null;
    }

    linkRequests(modules) {
      if (Array.isArray(modules) && modules.length !== 0) {
        throw new Error('SyntheticModule does not accept linked requests');
      }
    }

    instantiate() {
      binding.moduleInstantiate(getModuleData(this).handle);
    }

    async evaluate() {
      const state = getModuleData(this);
      if (this.status === 'evaluated') {
        return undefined;
      }
      if (this.status === 'errored') {
        throw this.error;
      }
      if (state.evaluatePromise) {
        return state.evaluatePromise;
      }

      state.statusOverride = 'evaluating';
      state.localError = undefined;
      state.evaluatePromise = (async () => {
        this.instantiate();
        await state.evaluateCallback.call(this);
        await Promise.resolve(binding.moduleEvaluate(state.handle));
        state.statusOverride = null;
        return undefined;
      })().catch((error) => {
        state.statusOverride =
          binding.moduleGetStatus(state.handle) === 'errored' ? null : 'errored';
        state.localError = error;
        throw error;
      });
      return state.evaluatePromise;
    }
  }

  class HermesModuleBase {
    constructor(state) {
      if (new.target === HermesModuleBase) {
        throw new TypeError('vm.Module is an abstract class');
      }
      moduleState.set(this, state);
    }

    get identifier() {
      return getModuleData(this).identifier;
    }

    get context() {
      return getModuleData(this).context;
    }

    get status() {
      return getModuleData(this).status;
    }

    get namespace() {
      const state = getModuleData(this);
      if (state.status === 'unlinked') {
        throw new Error('Module has not been linked');
      }
      return createHermesNamespace(state);
    }

    get error() {
      return getModuleData(this).localError;
    }

    async link(linker) {
      if (typeof linker !== 'function') {
        throw new TypeError('The "linker" argument must be a function');
      }

      const state = getModuleData(this);
      if (state.linkerPromise) {
        return state.linkerPromise;
      }

      state.status = 'linking';
      state.localError = undefined;
      state.linkerPromise = (async () => {
        const linked = [];
        for (const specifier of state.dependencySpecifiers) {
          const resolved = await linker(specifier, this);
          if (!(resolved instanceof HermesModuleBase)) {
            throw new TypeError('Linker must return vm.Module instances');
          }
          linked.push(resolved);
        }
        state.linkedModules = linked;
        state.status = 'linked';
      })().catch((error) => {
        state.status = 'errored';
        state.localError = error;
        throw error;
      });

      return state.linkerPromise;
    }

    async evaluate() {
      throw new TypeError('evaluate() is not implemented for this vm.Module');
    }
  }

  class HermesSourceTextModule extends HermesModuleBase {
    constructor(sourceText, options = {}) {
      const source = String(sourceText);
      super({
        context:
          options && options.context
            ? ensureExistingContext(options.context)
            : undefined,
        dependencySpecifiers: extractModuleDependencySpecifiers(source),
        evaluatePromise: null,
        exports: Object.create(null),
        exportNames: [],
        identifier:
          (options && typeof options.identifier === 'string' && options.identifier) ||
          (options && typeof options.filename === 'string' && options.filename) ||
          'vm:module',
        linkedModules: [],
        linkerPromise: null,
        localError: undefined,
        namespace: null,
        sourceMapURL: extractSourceMapURL(source),
        sourceText: source,
        status: 'unlinked',
      });
    }

    get dependencySpecifiers() {
      return getModuleData(this).dependencySpecifiers.slice();
    }

    get moduleRequests() {
      return this.dependencySpecifiers.map((specifier) => ({
        specifier,
        attributes: Object.freeze({}),
        phase: 'evaluation',
      }));
    }

    get sourceMapURL() {
      return getModuleData(this).sourceMapURL;
    }

    createCachedData() {
      return encodeSource(getModuleData(this).sourceText);
    }

    hasTopLevelAwait() {
      return false;
    }

    hasAsyncGraph() {
      return false;
    }

    linkRequests(modules) {
      const state = getModuleData(this);
      if (!Array.isArray(modules)) {
        throw new TypeError('The "modules" argument must be an Array');
      }
      if (modules.length !== state.dependencySpecifiers.length) {
        throw new Error('linkRequests() module count must match dependencySpecifiers');
      }

      for (const module of modules) {
        if (!(module instanceof HermesModuleBase)) {
          throw new TypeError('linkRequests() expects vm.Module instances');
        }
      }

      state.linkedModules = modules.slice();
    }

    instantiate() {
      const state = getModuleData(this);
      if (state.status === 'linked' || state.status === 'evaluated') {
        return;
      }
      if (state.linkedModules.length !== state.dependencySpecifiers.length) {
        throw new Error('linkRequests() module count must match dependencySpecifiers');
      }
      state.status = 'linked';
    }

    async evaluate() {
      const state = getModuleData(this);
      if (state.status === 'evaluated') {
        return undefined;
      }
      if (state.status === 'errored') {
        throw state.localError;
      }
      if (state.evaluatePromise) {
        return state.evaluatePromise;
      }
      if (state.status === 'unlinked') {
        this.instantiate();
      }

      state.status = 'evaluating';
      state.localError = undefined;
      state.evaluatePromise = (async () => {
        const imports = Object.create(null);
        for (let index = 0; index < state.linkedModules.length; index += 1) {
          const linkedModule = state.linkedModules[index];
          if (!(linkedModule instanceof HermesModuleBase)) {
            throw new Error('Missing linked module');
          }
          await linkedModule.evaluate();
          imports[state.dependencySpecifiers[index]] = linkedModule.namespace;
        }

        const evaluator = Function(
          '__imports',
          '__exports',
          `'use strict';\n${transformFallbackModuleSource(state.sourceText)}\nreturn __exports;`,
        );
        evaluator(imports, state.exports);
        state.exportNames = Object.keys(state.exports);
        state.namespace = null;
        state.status = 'evaluated';
        return undefined;
      })().catch((error) => {
        state.status = 'errored';
        state.localError = error;
        throw error;
      });

      return state.evaluatePromise;
    }
  }

  class HermesSyntheticModule extends HermesModuleBase {
    constructor(exportNames, evaluateCallback, options = {}) {
      if (!Array.isArray(exportNames)) {
        throw new TypeError('The "exportNames" argument must be an Array');
      }
      if (typeof evaluateCallback !== 'function') {
        throw new TypeError('The "evaluateCallback" argument must be a function');
      }

      super({
        context:
          options && options.context
            ? ensureExistingContext(options.context)
            : undefined,
        dependencySpecifiers: [],
        evaluateCallback,
        evaluatePromise: null,
        exports: Object.create(null),
        exportNames: exportNames.map((name) => String(name)),
        identifier:
          (options && typeof options.identifier === 'string' && options.identifier) ||
          (options && typeof options.filename === 'string' && options.filename) ||
          'vm:module',
        linkedModules: [],
        linkerPromise: null,
        localError: undefined,
        namespace: null,
        status: 'unlinked',
      });
    }

    setExport(name, value) {
      const state = getModuleData(this);
      const exportName = String(name);
      if (!state.exportNames.includes(exportName)) {
        throw new Error(`Unknown synthetic module export "${exportName}"`);
      }
      state.exports[exportName] = value;
      state.namespace = null;
    }

    linkRequests(modules) {
      if (Array.isArray(modules) && modules.length !== 0) {
        throw new Error('SyntheticModule does not accept linked requests');
      }
    }

    instantiate() {
      const state = getModuleData(this);
      if (state.status === 'unlinked') {
        state.status = 'linked';
      }
    }

    async evaluate() {
      const state = getModuleData(this);
      if (state.status === 'evaluated') {
        return undefined;
      }
      if (state.status === 'errored') {
        throw state.localError;
      }
      if (state.evaluatePromise) {
        return state.evaluatePromise;
      }

      state.status = 'evaluating';
      state.localError = undefined;
      state.evaluatePromise = (async () => {
        this.instantiate();
        await state.evaluateCallback.call(this);
        state.namespace = null;
        state.status = 'evaluated';
        return undefined;
      })().catch((error) => {
        state.status = 'errored';
        state.localError = error;
        throw error;
      });

      return state.evaluatePromise;
    }
  }

  const ModuleBase = usesJsModuleFallback ? HermesModuleBase : NativeModuleBase;
  const SourceTextModule = usesJsModuleFallback
    ? HermesSourceTextModule
    : NativeSourceTextModule;
  const SyntheticModule = usesJsModuleFallback
    ? HermesSyntheticModule
    : NativeSyntheticModule;

  function measureMemory() {
    const usage =
      typeof process !== 'undefined' && process && typeof process.memoryUsage === 'function'
        ? process.memoryUsage()
        : { heapUsed: 0 };
    const estimate = Number(usage.heapUsed) || 0;
    return Promise.resolve({
      total: {
        jsMemoryEstimate: estimate,
        jsMemoryRange: [estimate, estimate],
      },
      current: {
        jsMemoryEstimate: estimate,
        jsMemoryRange: [estimate, estimate],
      },
      other: [],
    });
  }

  return {
    Script,
    Module: ModuleBase,
    SourceTextModule,
    SyntheticModule,
    compileFunction,
    constants,
    createContext,
    isContext,
    measureMemory,
    runInContext,
    runInNewContext,
    runInThisContext,
  };
})
)JS";

  napi_value source;
  napi_value factory;
  napi_value global;
  napi_value exports;
  if (napi_create_string_utf8(env, kWrapperSource, NAPI_AUTO_LENGTH, &source) !=
          napi_ok ||
      napi_run_script_source(env, source, "node:vm-wrapper.js", &factory) !=
          napi_ok ||
      napi_get_global(env, &global) != napi_ok ||
      napi_call_function(env, global, factory, 1, &binding, &exports) !=
          napi_ok) {
    return nullptr;
  }

  return exports;
}

}  // namespace

napi_value VM::CreateModule(napi_env env) {
  napi_value module;
  napi_value binding;
  napi_value scriptCtor;
  napi_value engineName;
  napi_value useContextExecutionFallback;
  napi_value useJsModuleFallback;

  napi_create_object(env, &module);
  napi_create_object(env, &binding);
  napi_create_string_utf8(env, GetVmEngineName(), NAPI_AUTO_LENGTH,
                          &engineName);
#if defined(TARGET_ENGINE_HERMES) || defined(TARGET_ENGINE_JSC)
  napi_get_boolean(env, true, &useContextExecutionFallback);
  napi_get_boolean(env, true, &useJsModuleFallback);
#else
  napi_get_boolean(env, false, &useContextExecutionFallback);
  napi_get_boolean(env, false, &useJsModuleFallback);
#endif

  const napi_property_descriptor scriptProperties[] = {
      napi_util::desc("runInContext", ScriptRunInContext, nullptr),
      napi_util::desc("runInNewContext", ScriptRunInNewContext, nullptr),
      napi_util::desc("runInThisContext", ScriptRunInThisContext, nullptr),
  };

  napi_define_class(env, "Script", NAPI_AUTO_LENGTH, ScriptConstructor, nullptr,
                    3, scriptProperties, &scriptCtor);

  const napi_property_descriptor bindingProperties[] = {
      napi_util::desc("engine", engineName),
      napi_util::desc("useContextExecutionFallback",
                      useContextExecutionFallback),
      napi_util::desc("useJsModuleFallback", useJsModuleFallback),
      napi_util::desc("Script", scriptCtor),
      napi_util::desc("createContext", CreateContextCallback, nullptr),
      napi_util::desc("isContext", IsContextCallback, nullptr),
      napi_util::desc("createSourceTextModule", CreateSourceTextModuleCallback,
                      nullptr),
      napi_util::desc("createSyntheticModule", CreateSyntheticModuleCallback,
                      nullptr),
      napi_util::desc("moduleGetIdentifier", ModuleGetIdentifierCallback,
                      nullptr),
      napi_util::desc("moduleGetContext", ModuleGetContextCallback, nullptr),
      napi_util::desc("moduleGetDependencySpecifiers",
                      ModuleGetDependencySpecifiersCallback, nullptr),
      napi_util::desc("moduleGetStatus", ModuleGetStatusCallback, nullptr),
      napi_util::desc("moduleGetError", ModuleGetErrorCallback, nullptr),
      napi_util::desc("moduleLinkRequests", ModuleLinkRequestsCallback,
                      nullptr),
      napi_util::desc("moduleInstantiate", ModuleInstantiateCallback, nullptr),
      napi_util::desc("moduleEvaluate", ModuleEvaluateCallback, nullptr),
      napi_util::desc("moduleGetExportNames", ModuleGetExportNamesCallback,
                      nullptr),
      napi_util::desc("moduleGetNamespaceValue",
                      ModuleGetNamespaceValueCallback, nullptr),
      napi_util::desc("moduleCreateCachedData", ModuleCreateCachedDataCallback,
                      nullptr),
      napi_util::desc("moduleHasTopLevelAwait", ModuleHasTopLevelAwaitCallback,
                      nullptr),
      napi_util::desc("moduleHasAsyncGraph", ModuleHasAsyncGraphCallback,
                      nullptr),
      napi_util::desc("moduleSetSyntheticExport",
                      ModuleSetSyntheticExportCallback, nullptr),
      napi_util::desc("runInContext", RunInContextCallback, nullptr),
      napi_util::desc("runInNewContext", RunInNewContextCallback, nullptr),
      napi_util::desc("runInThisContext", RunInThisContextCallback, nullptr),
  };

  napi_define_properties(
      env, binding, sizeof(bindingProperties) / sizeof(bindingProperties[0]),
      bindingProperties);
  napi_value exports = CreatePublicVmExports(env, binding);
  if (exports == nullptr) {
    return nullptr;
  }
  napi_set_named_property(env, module, "exports", exports);
  return module;
}

}  // namespace nativescript
