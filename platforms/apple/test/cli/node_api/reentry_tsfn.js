const addon = require(__dirname + "/addon.dylib");

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function withTimeout(promise, label, timeoutMs = 2000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      reject(new Error(`${label} timed out after ${timeoutMs}ms`));
    }, timeoutMs);

    promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (error) => {
        clearTimeout(timer);
        reject(error);
      }
    );
  });
}

async function testMakeCallbackReentry() {
  await withTimeout(
    new Promise((resolve, reject) => {
      addon.makeCallbackFromNative(() => {
        try {
          resolve();
        } catch (error) {
          reject(error);
        }
      });
    }),
    "makeCallbackFromNative"
  );
}

async function testMakeCallbackReentryStress() {
  for (let i = 0; i < 64; i++) {
    await testMakeCallbackReentry();
  }
}

async function testThreadsafeFunction() {
  const values = [];

  await withTimeout(
    new Promise((resolve, reject) => {
      addon.startThreadsafeDemo((value) => {
        values.push(value);
        try {
          if (values.length < 3) {
            return;
          }
          assert(values.length === 3, `Expected 3 TSFN values, got ${values.length}`);
          assert(
            values[0] === 1 && values[1] === 2 && values[2] === 3,
            `Expected TSFN values [1,2,3], got [${values.join(",")}]`
          );
          resolve();
        } catch (error) {
          reject(error);
        }
      });
    }),
    "startThreadsafeDemo"
  );
}

async function testThreadsafeFunctionStress() {
  for (let i = 0; i < 32; i++) {
    await testThreadsafeFunction();
  }
}

async function testMissingNodeApis() {
  const version = addon.getNodeVersion();
  assert(version && typeof version === "object", "Expected version object");
  assert(typeof version.major === "number", "Expected version.major number");
  assert(typeof version.minor === "number", "Expected version.minor number");
  assert(typeof version.patch === "number", "Expected version.patch number");
  assert(typeof version.release === "string", "Expected version.release string");

  let didThrow = false;
  try {
    addon.triggerFatalException("fatal-exception-test");
  } catch (error) {
    didThrow = true;
    const text = String(error && error.message ? error.message : error);
    assert(
      text.includes("fatal-exception-test"),
      `Expected fatal exception message, got ${text}`
    );
  }
  assert(didThrow, "Expected triggerFatalException to throw");

  addon.testEnvCleanupHooks();
  addon.testAsyncCleanupHooks();
}

(async () => {
  await testMissingNodeApis();
  await testMakeCallbackReentryStress();
  await testThreadsafeFunctionStress();
  console.log("node_api gaps+reentry+tsfn PASS");
})().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
