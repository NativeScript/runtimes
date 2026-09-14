"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("block-callback-finalization", async (t) => {
  const total = 120;
  const weakRefs = [];
  let executed = 0;
  const queue = dispatch_get_current_queue();

  (function scheduleBlocks() {
    for (let i = 0; i < total; i++) {
      const callback = function() {
        executed += 1;
      };
      weakRefs.push(new WeakRef(callback));
      dispatch_async(queue, callback);
    }
  })();

  const ranAll = await t.waitUntil(() => executed === total, 5_000, 10);
  t.assert(ranAll, `block callbacks incomplete ${executed}/${total}`);

  const collected = await t.forceCollectUntil(() => {
    return t.countAliveWeakRefs(weakRefs) === 0;
  }, {
    timeoutMs: 10_000,
    intervalMs: 20,
    gcRounds: 2,
    pressureBytes: 12 * 1024 * 1024,
    pauseMs: 4,
  });

  const alive = t.countAliveWeakRefs(weakRefs);
  t.assert(
    collected,
    `block callbacks were not released alive=${alive}/${total}`,
  );

  return {
    total,
    executed,
    alive,
    engine: t.engine,
  };
}, { timeoutMs: 16_000 });
