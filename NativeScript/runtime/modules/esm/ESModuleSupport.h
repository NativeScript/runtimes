#ifndef NS_RUNTIME_MODULES_ESM_ESMODULESUPPORT_H_
#define NS_RUNTIME_MODULES_ESM_ESMODULESUPPORT_H_

#include <filesystem>
#include <string>

// Engine-neutral ES module support shared by the Apple and Android runtimes.
//
// Engines without a native module loader (Hermes, JavaScriptCore, PrimJS, and
// every engine on Android, where no host import hooks are wired) evaluate ES
// modules through the CommonJS wrapper: the source is rewritten statement by
// statement into `require`/`exports` form and run as a regular module. The
// rewrite is textual and covers the shapes bundlers emit; it is not a parser.
//
// The rewritten source never touches the wrapper parameters `module`,
// `exports` and `require` directly: bundler output may declare top-level
// variables of the same names inside an ES module (rolldown's CommonJS interop
// emits `var module = { exports: {} }; var exports = module.exports;`), which
// would silently redirect the generated export assignments. It binds instead
// to `__esm_exports` and `__esm_require`, captured by the shims below at
// wrapper entry, plus `__dynamicImport` (replaces `import()`) and
// `__importMeta` (replaces `import.meta`), built from `__filename` and
// `__dirname`.

// Resolves `import()` through the module-scoped `require`, so relative
// specifiers resolve against the importing module rather than the app root.
#define NS_ESM_FALLBACK_DYNAMIC_IMPORT_SHIM                                \
  "const __esm_require = require; "                                        \
  "const __dynamicImport = (specifier) => Promise.resolve().then(() => { " \
  "const __loaded = __esm_require(specifier); "                            \
  "if (__loaded !== null && (typeof __loaded === 'object' || typeof "      \
  "__loaded === 'function')) { "                                           \
  "if (__loaded.__esModule) { return __loaded; } "                         \
  "return Object.assign({ default: __loaded }, __loaded); "                \
  "} "                                                                     \
  "return { default: __loaded }; "                                         \
  "}); "

#define NS_ESM_FALLBACK_MODULE_SHIM                                            \
  "const __esm_exports = exports; "                                            \
  "const __importMeta = { url: 'file://' + __filename, filename: __filename, " \
  "dirname: __dirname, main: false }; "

namespace nativescript::esm {

std::string StripShebang(const std::string& source);

// `import(` -> `__dynamicImport(` for CommonJS sources evaluated on engines
// whose parser rejects dynamic import or whose host has no import hook.
std::string RewriteCommonJSDynamicImportsForFallbackEngines(
    const std::string& source);

// Full ES module -> CommonJS rewrite: static imports/exports, `import.meta`
// and dynamic imports. The result must be wrapped by the CommonJS prologue
// together with both shims above.
std::string TransformESModuleForFallbackEngines(const std::string& source);

bool IsCJSModule(const std::string& path);

std::string FindNearestPackageJson(const std::filesystem::path& startDir);

// Cached per package.json path; see ClearPackageTypeCache.
bool IsPackageTypeModule(const std::string& packageJsonPath);

bool ShouldTreatJsAsESModule(const std::string& path);

// .mjs is always ESM, .cjs never, and .js follows the nearest package.json
// "type" field.
bool IsESModulePath(const std::string& path);

void ClearPackageTypeCache();

}  // namespace nativescript::esm

#endif  // NS_RUNTIME_MODULES_ESM_ESMODULESUPPORT_H_
