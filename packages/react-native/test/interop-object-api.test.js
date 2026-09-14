const assert = require("assert");
const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "../../..");

for (const relativePath of [
  "NativeScript/ffi/objc/shared/bridge/TypeConv.mm",
]) {
  const source = fs.readFileSync(path.join(repoRoot, relativePath), "utf8");
  assert(
    source.includes('PropNameID::forAscii(runtime, "object")'),
    `${relativePath} should expose interop.object`,
  );
  assert(
    source.includes("nativeObjectReturnTypeForClass(object_getClass(object))") &&
      source.includes("convertNativeReturnValue(runtime, bridge, type, &object)"),
    `${relativePath} should wrap Objective-C object pointers through the generic NativeScript object bridge`,
  );
  assert(
    source.includes(
      "parseIntegerTextToUintptr(args[0].asString(runtime).utf8(runtime)",
    ),
    `${relativePath} should accept string pointer addresses without treating them as C strings`,
  );
}

const declarations = fs.readFileSync(
  path.join(repoRoot, "packages/objc-node-api/index.d.ts"),
  "utf8",
);
assert(
  declarations.includes("function object<T extends NativeObject") &&
    declarations.includes("constructor(address: string)"),
  "interop declarations should expose object(pointer) and string Pointer addresses",
);

console.log("interop object API tests passed");
