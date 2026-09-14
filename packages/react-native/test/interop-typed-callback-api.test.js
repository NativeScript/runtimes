const assert = require("assert");
const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "../../..");
const packageRoot = path.resolve(__dirname, "..");

function readRepo(relativePath) {
  return fs.readFileSync(path.join(repoRoot, relativePath), "utf8");
}

function readPackage(relativePath) {
  return fs.readFileSync(path.join(packageRoot, relativePath), "utf8");
}

for (const relativePath of [
  "NativeScript/ffi/objc/shared/bridge/TypeConv.mm",
]) {
  const source = readRepo(relativePath);
  assert(
    source.includes('PropNameID::forAscii(runtime, "Block")'),
    `${relativePath} should expose interop.Block`,
  );
  assert(
    source.includes('PropNameID::forAscii(runtime, "FunctionReference"), 2'),
    `${relativePath} should allow typed FunctionReference callbacks`,
  );
  assert(
    source.includes("__nativeApiCallbackEncoding") &&
      source.includes("interopCallbackFromArguments"),
    `${relativePath} should store explicit callback encodings on generic interop callbacks`,
  );
  assert(
    !source.includes("__nativeScriptCallbackEncoding"),
    `${relativePath} should not use the RN-specific callback encoding marker`,
  );
}

for (const relativePath of [
  "NativeScript/ffi/objc/shared/bridge/Install.mm",
]) {
  const source = readRepo(relativePath);
  assert(
    source.includes("interop.Block = wrapInteropFactory"),
    `${relativePath} should wrap interop.Block like the other interop factories`,
  );
}

const declarations = readRepo("packages/objc-node-api/index.d.ts");
assert(
  declarations.includes("function Block<T extends") &&
    declarations.includes("function FunctionReference<T extends") &&
    declarations.includes("__nativeApiCallbackEncoding"),
  "interop declarations should expose typed Block and FunctionReference factories",
);

const publicApi = readPackage("src/index.ts");
assert(
  !publicApi.includes("objCBlock") && !publicApi.includes("objCFunctionPointer"),
  "typed callback construction should live on interop, not @nativescript/react-native",
);

console.log("interop typed callback API tests passed");
