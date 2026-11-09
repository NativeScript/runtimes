const assert = require("assert");
const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "../../..");

for (const relativePath of [
  "NativeScript/ffi/objc/shared/bridge/host_objects/Object.mm",
]) {
  const source = fs.readFileSync(path.join(repoRoot, relativePath), "utf8");

  assert(
    source.includes("runtimeWritablePropertySetter"),
    `${relativePath}: host objects should discover writable Objective-C runtime properties`,
  );
  assert(
    source.includes("runtimeReadablePropertyGetter"),
    `${relativePath}: host objects should discover readable Objective-C runtime properties`,
  );
  assert(
    source.includes("property_copyAttributeValue(prop, \"S\")"),
    `${relativePath}: runtime property fallback should honor custom Objective-C setters`,
  );
  assert(
    source.includes("property_copyAttributeValue(prop, \"R\")"),
    `${relativePath}: runtime property fallback should not assign readonly Objective-C properties`,
  );
  assert(
    source.includes("callObjCSelector(runtime, bridge_, object_, false,\n                       *setterSelectorName, nullptr, args, 1);"),
    `${relativePath}: runtime property fallback should invoke the discovered native setter`,
  );
  assert(
    source.includes("return callObjectSelector(runtime, *selector, nullptr, nullptr, 0);"),
    `${relativePath}: JS-extended instances should read discovered native properties before returning undefined`,
  );
}

console.log("runtime Objective-C property setter tests passed");
