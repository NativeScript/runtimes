#include "NativeScript.h"
#include "Runtime.h"
#include "RuntimeConfig.h"
#include "cli/BundleLoader.h"
#include "runtime/apple/NativeScriptException.h"
#include "ffi/objc/shared/Tasks.h"
#include "js_native_api.h"
#include "jsr.h"
#include <memory>

using namespace nativescript;

@implementation Config

@synthesize BaseDir;
@synthesize ApplicationPath;
@synthesize MetadataPtr;
@synthesize IsDebug;

@end

@implementation NativeScript

extern char defaultStartOfMetadataSection __asm("section$start$__DATA$__TNSMetadata");

// Raw pointer, not unique_ptr: at process exit, static-destruction order is
// unspecified relative to the ObjC runtime/other statics, so an implicit
// unique_ptr destructor can run after dependencies it needs are already torn
// down. resetRuntime() gives us an explicit, ordered teardown point, and
// restartWithConfig: needs the old runtime to stay alive until the new one
// has finished Init() (see below).
Runtime* runtime_ = nullptr;

static void resetRuntime() {
  delete runtime_;
  runtime_ = nullptr;
}

- (void)runScriptString:(NSString*)script runLoop:(BOOL)runLoop {
  std::string cppScript = [script UTF8String];
  runtime_->RunScript(cppScript);
  if (runLoop) {
    runtime_->RunLoop();
  }
  Tasks::Drain();
}

- (void)runMainApplication {
  // Try CLI/test-runner bundle resolution first (bundle resourcePath,
  // executable-relative Contents/Resources, argv[0], _NSGetExecutablePath,
  // cwd); fall back to the configured application directory so the entry
  // resolves through its package.json "main" (falling back to index.*) — a
  // literal index.js is not guaranteed to exist (CLI-built apps ship
  // bundle.js).
  std::string spec = resolveMainPath();
  if (spec.empty()) {
    spec = RuntimeConfig.ApplicationPath;
  }
  try {
    runtime_->RunModule(spec);
  } catch (const NativeScriptException& e) {
    std::string description = e.Description();
    NSLog(@"NativeScript runMainApplication failed: %s", description.c_str());
    @throw [NSException exceptionWithName:@"NativeScriptException"
                                   reason:@(description.c_str())
                                 userInfo:nil];
  } catch (const std::exception& e) {
    NSLog(@"NativeScript runMainApplication failed: %s", e.what());
    @throw [NSException exceptionWithName:@"NativeScriptException" reason:@(e.what()) userInfo:nil];
  }
  runtime_->RunLoop();
  Tasks::Drain();
}

- (bool)liveSync {
  if (!runtime_) {
    return false;
  }

  napi_env env = runtime_->GetEnv();
  if (env == nullptr) {
    return false;
  }

  bool didInvokeCallback = false;

  NapiScope scope(env);

  napi_value global;
  if (napi_get_global(env, &global) != napi_ok) {
    return false;
  }

  bool hasLiveSyncCallback = false;
  if (napi_has_named_property(env, global, "__onLiveSync", &hasLiveSyncCallback) != napi_ok ||
      !hasLiveSyncCallback) {
    return false;
  }

  napi_value callback;
  if (napi_get_named_property(env, global, "__onLiveSync", &callback) != napi_ok) {
    return false;
  }

  napi_valuetype callbackType;
  if (napi_typeof(env, callback, &callbackType) != napi_ok || callbackType != napi_function) {
    return false;
  }

  napi_value context;
  if (napi_create_object(env, &context) != napi_ok) {
    return false;
  }

  napi_value result;
  napi_status status = napi_call_function(env, global, callback, 1, &context, &result);
  if (status != napi_ok) {
    bool hasPendingException = false;
    if (napi_is_exception_pending(env, &hasPendingException) == napi_ok && hasPendingException) {
      napi_value error;
      napi_get_and_clear_last_exception(env, &error);
    }
    return false;
  }

  didInvokeCallback = true;

  if (result != nullptr) {
    napi_valuetype returnType;
    if (napi_typeof(env, result, &returnType) == napi_ok && returnType == napi_boolean) {
      bool callbackResult = false;
      if (napi_get_value_bool(env, result, &callbackResult) == napi_ok) {
        didInvokeCallback = callbackResult;
      }
    }
  }

  Tasks::Drain();
  return didInvokeCallback;
}

- (void)shutdownRuntime {
  resetRuntime();
}

- (instancetype)initWithConfig:(Config*)config {
  if (self = [super init]) {
    RuntimeConfig.BaseDir = [config.BaseDir UTF8String];
    if (config.ApplicationPath != nil) {
      RuntimeConfig.ApplicationPath =
          [[config.BaseDir stringByAppendingPathComponent:config.ApplicationPath] UTF8String];
    } else {
      RuntimeConfig.ApplicationPath =
          [[config.BaseDir stringByAppendingPathComponent:@"app"] UTF8String];
    }
    RuntimeConfig.Arguments.clear();
    if (config.Arguments != nullptr && config.ArgumentsCount > 0) {
      RuntimeConfig.Arguments.reserve((size_t)config.ArgumentsCount);
      for (int i = 0; i < config.ArgumentsCount; i++) {
        const char* arg = config.Arguments[i];
        RuntimeConfig.Arguments.emplace_back(arg != nullptr ? arg : "");
      }
    } else {
      NSArray<NSString*>* processArgs = [[NSProcessInfo processInfo] arguments];
      RuntimeConfig.Arguments.reserve((size_t)[processArgs count]);
      for (NSString* arg in processArgs) {
        RuntimeConfig.Arguments.emplace_back([arg UTF8String]);
      }
    }
    if (config.MetadataPtr != nil) {
      RuntimeConfig.MetadataPtr = [config MetadataPtr];
    } else {
      RuntimeConfig.MetadataPtr = &defaultStartOfMetadataSection;
    }
    RuntimeConfig.IsDebug = [config IsDebug];
    RuntimeConfig.LogToSystemConsole = [config LogToSystemConsole];
    RuntimeConfig.CustomLogCallback = [config CustomLogCallback];

    // Build and Init the new runtime before tearing down the old one — the
    // old runtime (and anything it holds live, e.g. in-flight callbacks)
    // must outlive the new runtime's Init() on restartWithConfig:.
    std::unique_ptr<Runtime> runtime(new Runtime());

    // TODO: separate runtime init and measure the time
    runtime->Init();
    resetRuntime();
    runtime_ = runtime.release();

    if (RuntimeConfig.IsDebug) {
      // TODO: Inspector for debugging
      // runtime_->enableInspector();
    }
  }
  return self;
}

- (instancetype)initializeWithConfig:(Config*)config {
  return [self initWithConfig:config];
}

- (void)restartWithConfig:(Config*)config {
  [self shutdownRuntime];
  [self initWithConfig:config];
}

@end
