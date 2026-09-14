const assert = require("assert");
const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "../../..");

for (const relativePath of [
  "NativeScript/ffi/objc/shared/bridge/TypeConv.mm",
]) {
  const source = fs.readFileSync(path.join(repoRoot, relativePath), "utf8");
  const blockCaseIndex = source.indexOf(
    "case metagen::mdTypeBlock:\n    case metagen::mdTypeFunctionPointer:",
  );
  assert.notStrictEqual(
    blockCaseIndex,
    -1,
    `${relativePath} should convert block/function-pointer arguments`,
  );

  const nilGuardIndex = source.indexOf(
    "if (value.isNull() || value.isUndefined())",
    blockCaseIndex,
  );
  const callbackCreationIndex = source.indexOf(
    "createEngineCallback(",
    blockCaseIndex,
  );

  assert(
    nilGuardIndex !== -1 && nilGuardIndex < callbackCreationIndex,
    `${relativePath} should convert null/undefined callbacks to nullptr before callback metadata lookup`,
  );
  const encodingMarkerIndex = source.indexOf("__nativeApiCallbackEncoding");
  const encodingReadIndex = source.indexOf(
    "stringPropertyOrEmpty(runtime, object, kNativeApiCallbackEncodingProperty)",
    blockCaseIndex,
  );
  const explicitCallbackIndex = source.indexOf(
    "runtime, bridge, callbackEncoding",
    blockCaseIndex,
  );
  assert(
    encodingMarkerIndex !== -1 &&
      encodingReadIndex !== -1 &&
      explicitCallbackIndex !== -1 &&
      encodingReadIndex < explicitCallbackIndex,
    `${relativePath} should honor explicit interop callback encodings before metadata-only callback creation`,
  );
}

console.log("native null callback conversion tests passed");
