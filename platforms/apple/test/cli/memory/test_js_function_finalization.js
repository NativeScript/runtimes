"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("js-function-finalization", async (t) => {
  const total = 2000;
  const refs = [];

  (function createFunctions() {
    for (let i = 0; i < total; i++) {
      const fn = function generatedFunction() {
        return i;
      };
      refs.push(new WeakRef(fn));
    }
  })();

  const collected = await t.forceCollectUntil(
    () => t.countAliveWeakRefs(refs) <= Math.floor(total * 0.1),
    {
      timeoutMs: 12_000,
      intervalMs: 25,
      gcRounds: 2,
      pressureBytes: 16 * 1024 * 1024,
      pauseMs: 4,
    },
  );
  const alive = t.countAliveWeakRefs(refs);

  t.assert(collected, `JS function targets still alive after forced GC: ${alive}/${total}`);
  return { total, alive, engine: t.engine };
}, { timeoutMs: 18_000 });
