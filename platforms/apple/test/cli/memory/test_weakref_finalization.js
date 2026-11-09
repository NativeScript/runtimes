"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("weakref-finalization", async (t) => {
  const total = 2000;
  const refs = [];

  function createWeakRefs() {
    for (let i = 0; i < total; i++) {
      const payload = { i, bytes: new Uint8Array(256) };
      refs.push(new WeakRef(payload));
    }
  }

  createWeakRefs();

  const start = t.now();
  let alive = total;
  while (t.now() - start < 5000) {
    await t.forceGC(4, 24 * 1024 * 1024, 6);
    alive = 0;
    for (let i = 0; i < refs.length; i++) {
      if (refs[i].deref()) {
        alive += 1;
      }
    }
    if (alive === 0) {
      break;
    }
    await t.sleep(20);
  }

  t.assert(
    alive <= Math.floor(total * 0.1),
    `WeakRef targets still alive after forced GC: ${alive}/${total}`,
  );

  return {
    total,
    alive,
    elapsedMs: t.now() - start,
  };
}, { timeoutMs: 12_000 });
