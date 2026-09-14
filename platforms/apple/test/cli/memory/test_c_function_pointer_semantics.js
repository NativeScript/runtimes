"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("c-function-pointer-semantics", async (t) => {
  const array = NSMutableArray.arrayWithArray([
    NSString.stringWithString("a"),
    NSString.stringWithString("b"),
    NSString.stringWithString("c"),
    NSString.stringWithString("d"),
  ]);
  const range = {
    location: 0,
    length: array.count,
  };

  let plainCalls = 0;
  CFArrayApplyFunction(array, range, function(value, context) {
    plainCalls += 1;
    t.assert(value instanceof interop.Pointer, "CFArrayApplyFunction should expose raw values as pointers");
    t.assert(context === null, "null CFArrayApplyFunction context should round-trip as null");
  }, null);

  t.assert(plainCalls === array.count, `plain C callback count mismatch ${plainCalls}/${array.count}`);

  const persistentCallback = new interop.FunctionReference(function(value, context) {
    plainCalls += 1;
    t.assert(value instanceof interop.Pointer, "FunctionReference callback should receive pointer values");
    t.assert(context === null, "FunctionReference callback should preserve a null context");
  });

  let threwBeforeInit = false;
  try {
    interop.handleof(persistentCallback);
  } catch (_) {
    threwBeforeInit = true;
  }

  t.assert(
    threwBeforeInit,
    "FunctionReference handle should not exist before the function pointer has been materialized",
  );

  CFArrayApplyFunction(array, range, persistentCallback, null);
  const handleAfterFirstCall = interop.handleof(persistentCallback);

  t.assert(
    handleAfterFirstCall instanceof interop.Pointer,
    "FunctionReference should expose a native handle after first materialization",
  );

  CFArrayApplyFunction(array, range, persistentCallback, null);
  const handleAfterSecondCall = interop.handleof(persistentCallback);

  t.assert(
    handleAfterSecondCall === handleAfterFirstCall,
    "FunctionReference should keep a stable native function pointer across calls",
  );
  t.assert(
    plainCalls === array.count * 3,
    `combined C callback count mismatch ${plainCalls}/${array.count * 3}`,
  );

  return {
    count: array.count,
    totalCalls: plainCalls,
    engine: t.engine,
  };
}, { timeoutMs: 16_000 });
