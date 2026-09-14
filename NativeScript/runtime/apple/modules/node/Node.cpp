#include "Node.h"

#include "FS.h"
#include "Path.h"
#include "Process.h"
#include "VM.h"
#include "native_api_util.h"

namespace nativescript {

void Node::Init(napi_env env, napi_value global) { Process::Init(env, global); }

napi_value Node::LoadInternalModule(napi_env env,
                                    const std::string& moduleName) {
  if (moduleName == "fs" || moduleName == "node:fs") {
    return FS::CreateModule(env);
  }

  if (moduleName == "path" || moduleName == "node:path") {
    return Path::CreateModule(env);
  }

  if (moduleName == "process" || moduleName == "node:process") {
    return Process::CreateModule(env);
  }

  if (moduleName == "vm" || moduleName == "node:vm") {
    return VM::CreateModule(env);
  }

  if (moduleName == "fs/promises" || moduleName == "node:fs/promises") {
    napi_value fsModule = FS::CreateModule(env);
    if (napi_util::is_null_or_undefined(env, fsModule)) {
      return nullptr;
    }

    napi_value fsExports;
    if (napi_get_named_property(env, fsModule, "exports", &fsExports) !=
        napi_ok) {
      return nullptr;
    }

    napi_value promises;
    if (napi_get_named_property(env, fsExports, "promises", &promises) !=
        napi_ok) {
      return nullptr;
    }

    napi_value moduleObj;
    napi_create_object(env, &moduleObj);
    napi_set_named_property(env, moduleObj, "exports", promises);
    return moduleObj;
  }

  return nullptr;
}

}  // namespace nativescript
