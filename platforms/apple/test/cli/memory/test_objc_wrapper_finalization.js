"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("objc-wrapper-finalization", async (t) => {
  const total = 320;
  const weakRefs = [];

  (function createObjects() {
    for (let i = 0; i < total; i++) {
      const object = NSMutableDictionary.dictionary();
      object.setObjectForKey(NSString.stringWithString(`value-${i}`), "value");
      object.setObjectForKey(NSNumber.numberWithInt(i), "index");
      weakRefs.push(new WeakRef(object));
    }
  })();

  const collected = await t.forceCollectUntil(() => {
    return t.countAliveWeakRefs(weakRefs) <= 2;
  }, {
    timeoutMs: 12_000,
    intervalMs: 25,
    gcRounds: 2,
    pressureBytes: 16 * 1024 * 1024,
    pauseMs: 4,
  });

  const jsAlive = t.countAliveWeakRefs(weakRefs);

  t.assert(
    collected,
    `Objective-C wrappers were not released cleanly jsAlive=${jsAlive}/${total}`,
  );

  return {
    total,
    jsAlive,
    engine: t.engine,
  };
}, { timeoutMs: 18_000 });
