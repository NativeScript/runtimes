const assert = require("assert");
const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "../../..");
const callbackSourcePaths = [
  "NativeScript/ffi/objc/shared/bridge/Callbacks.mm",
];

for (const relativePath of callbackSourcePaths) {
  const callbacksSource = fs.readFileSync(
    path.join(repoRoot, relativePath),
    "utf8",
  );

  const nativeCallerPolicyIndex = callbacksSource.indexOf(
    "if (nativeCallerThreadCallbacks && !currentThreadIsJs)",
  );
  assert.notStrictEqual(
    nativeCallerPolicyIndex,
    -1,
    `${relativePath} should have an explicit off-JS native-caller fast path`,
  );

  const asyncZeroArgBlockIndex = callbacksSource.indexOf(
    "dispatchZeroArgVoidBlockAsync()",
  );
  assert(
    nativeCallerPolicyIndex < asyncZeroArgBlockIndex,
    `${relativePath} must not route native-caller callbacks through the JS async block fallback first`,
  );

  const legacyVoidBlockFallback = callbacksSource.indexOf(
    "!currentThreadIsJs && returnsVoid && block_ &&\n               jsThreadCallbackInvoker",
  );
  assert.strictEqual(
    legacyVoidBlockFallback,
    -1,
    `${relativePath} must not force void native blocks from a caller-thread runtime onto JS`,
  );
}

console.log("callback thread policy tests passed");
