const assert = require("assert");
const fs = require("fs");
const path = require("path");

const packageRoot = path.resolve(__dirname, "..");
const repoRoot = path.resolve(packageRoot, "../..");

function readPackage(relativePath) {
  return fs.readFileSync(path.join(packageRoot, relativePath), "utf8");
}

function readRepo(relativePath) {
  return fs.readFileSync(path.join(repoRoot, relativePath), "utf8");
}

const index = readPackage("src/index.ts");
assert(
  index.includes('export type NativeScriptCallbackThread = "js" | "runtime"'),
  "public JS API should expose a generic runtime callback thread policy",
);
assert(
  index.includes("export function runtimeInvoker"),
  "public JS API should export runtimeInvoker",
);
assert(
  !index.includes("export function objCBlock") &&
    !index.includes("export function objCFunctionPointer") &&
    !index.includes("typedObjCCallback"),
  "public JS API should not expose RN-specific typed ObjC callback helpers",
);
assert(
  index.includes('return callbackInvoker("runtime", callback)'),
  "runtimeInvoker should mark callbacks for their owning NativeScript runtime",
);
assert(
  index.includes("Object.getOwnPropertyNames(callback)") &&
    index.includes("Object.getOwnPropertySymbols(callback)") &&
    index.includes("Object.defineProperty(wrapped, key, descriptor)"),
  "callback invokers should preserve native callback/block metadata on wrappers",
);
assert(
  index.includes("interop.Block = wrapInteropFactory"),
  "interop.Block should be made callable/constructable when the runtime exposes it",
);
assert(
  index.includes('if (thread === "runtime")'),
  "eventBridge should route runtime callbacks through runtimeInvoker",
);
assert(
  !index.includes("afterUIKitTransition"),
  "runtime should not expose a transition-specific callback API",
);

const declarations = readPackage("src/index.d.ts");
assert(
  declarations.includes('NativeScriptCallbackThread = "js" | "runtime"'),
  "public declarations should include the runtime callback policy",
);
assert(
  declarations.includes("runtimeInvoker<T extends"),
  "public declarations should expose runtimeInvoker",
);
assert(
  !declarations.includes("objCBlock") &&
    !declarations.includes("objCFunctionPointer"),
  "public declarations should not expose typed ObjC callback helpers",
);
assert(
  !declarations.includes("afterUIKitTransition"),
  "public declarations should not expose transition-specific APIs",
);

const moduleSource = readPackage("ios/NativeScriptNativeApiModule.mm");
assert(
  moduleSource.includes("config.runtimeCallbackInvoker"),
  "Worklet runtime install should configure the generic runtime callback invoker",
);
assert(
  moduleSource.includes("runtimeStrong->schedule"),
  "runtime callbacks should schedule work onto the Worklet runtime",
);
assert(
  !moduleSource.includes("__nativeScriptAfterUIKitTransition"),
  "Native module should not install transition-specific host functions",
);

for (const relativePath of [
  "NativeScript/jsi/shared/NativeApiBackendConfig.h",
]) {
  const source = readRepo(relativePath);
  assert(
    source.includes("runtimeCallbackInvoker"),
    `${relativePath} should expose a generic runtime callback invoker`,
  );
}

for (const relativePath of [
  "NativeScript/ffi/objc/shared/bridge/Callbacks.mm",
]) {
  const source = readRepo(relativePath);
  assert(
    source.includes("NativeApiCallbackThreadPolicy::Runtime"),
    `${relativePath} should support the runtime callback policy`,
  );
  assert(
    source.includes('policy == "runtime"'),
    `${relativePath} should parse runtime callback policy markers`,
  );
  assert(
    source.includes("bridge_->runtimeCallbackInvoker()"),
    `${relativePath} should dispatch runtime-marked callbacks through the generic invoker`,
  );
  assert(
    source.includes("parseObjCCallbackEngineSignature") &&
      source.includes("objcSignatureEncoding"),
    `${relativePath} should build callbacks from explicit ObjC encodings`,
  );
}

console.log("runtime callback policy tests passed");
