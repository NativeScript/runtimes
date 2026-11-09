"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("weakref-plain-script", async (t) => {
  const total = 5000;
  const refs = [];

  (function createWeakRefs() {
    for (let i = 0; i < total; i++) {
      const payload = { i, bytes: new Uint8Array(128) };
      refs.push(new WeakRef(payload));
    }
  })();

  const collected = await t.forceCollectUntil(() => {
    return t.countAliveWeakRefs(refs) <= Math.floor(total * 0.1);
  }, {
    timeoutMs: 10_000,
    intervalMs: 20,
    gcRounds: 3,
    pressureBytes: 24 * 1024 * 1024,
    pauseMs: 4,
  });

  const alive = t.countAliveWeakRefs(refs);
  t.assert(
    collected,
    `WeakRef targets unexpectedly alive in plain script: ${alive}/${total}`,
  );

  return {
    total,
    alive,
    engine: t.engine,
  };
}, { timeoutMs: 14_000 });
