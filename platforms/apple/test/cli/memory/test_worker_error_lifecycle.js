"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("worker-error-lifecycle", async (t) => {
  const workerRefs = [];
  const total = 8;

  for (let index = 0; index < total; index++) {
    let worker = new Worker(
      "platforms/apple/test/cli/memory/_missing_worker_module.js",
    );
    workerRefs.push(new WeakRef(worker));

    await new Promise((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(new Error(`worker ${index} error was not reported`)),
        8_000,
      );
      worker.onerror = () => {
        clearTimeout(timeout);
        worker = null;
        resolve();
      };
    });

    await t.forceGC(2, 8 * 1024 * 1024, 4);
    if (index === 0) {
      await t.sleep(750);
      t.markRssBaseline();
      await t.sleep(500);
    }
  }

  const collected = await t.forceCollectUntil(
    () => t.countAliveWeakRefs(workerRefs) <= 1,
    {
      timeoutMs: 12_000,
      intervalMs: 25,
      gcRounds: 2,
      pressureBytes: 12 * 1024 * 1024,
      pauseMs: 4,
    },
  );
  const workersAlive = t.countAliveWeakRefs(workerRefs);
  t.assert(collected, `failed worker wrappers were retained: ${workersAlive}/${total}`);

  return {
    total,
    workersAlive,
    engine: t.engine,
  };
}, { timeoutMs: 45_000 });
