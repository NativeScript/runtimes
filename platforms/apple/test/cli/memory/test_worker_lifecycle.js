"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("worker-lifecycle", async (t) => {
  const rounds = 10;
  const workersPerRound = 2;
  const workerRefs = [];
  let replies = 0;

  function runWorker(index) {
    return new Promise((resolve, reject) => {
      let worker = new Worker(
        "platforms/apple/test/cli/memory/_worker_lifecycle_child.js",
      );
      workerRefs.push(new WeakRef(worker));

      worker.onerror = (error) => {
        if (worker) {
          worker.terminate();
          worker = null;
        }
        reject(new Error(`worker ${index} failed: ${String(error)}`));
      };

      worker.onmessage = (event) => {
        const result = event.data;
        try {
          t.assert(result.index === index, `worker ${index} returned the wrong index`);
          t.assert(result.validCycle, `worker ${index} lost its circular reference`);
          t.assert(result.validTypedArray, `worker ${index} lost its typed array`);
        } catch (error) {
          if (worker) {
            worker.terminate();
          }
          worker = null;
          reject(error);
          return;
        }

        replies++;
        if (worker && index % 2 === 0) {
          worker.terminate();
        }
        worker = null;
        resolve();
      };

      const payload = {
        index,
        closeFromWorker: index % 2 === 1,
        typed: new Uint32Array([index, index + 1]),
      };
      payload.self = payload;
      worker.postMessage(payload);
    });
  }

  for (let round = 0; round < rounds; round++) {
    const offset = round * workersPerRound;
    await Promise.all(
      Array.from({ length: workersPerRound }, (_, index) => {
        return runWorker(offset + index);
      }),
    );
    await t.forceGC(2, 8 * 1024 * 1024, 4);
    if (round === 0) {
      await t.sleep(750);
      t.markRssBaseline();
      await t.sleep(500);
    }
  }

  const total = rounds * workersPerRound;
  t.assert(replies === total, `received ${replies}/${total} worker replies`);

  const collected = await t.forceCollectUntil(
    () => t.countAliveWeakRefs(workerRefs) <= 2,
    {
      timeoutMs: 20_000,
      intervalMs: 25,
      gcRounds: 2,
      pressureBytes: 16 * 1024 * 1024,
      pauseMs: 4,
    },
  );
  const workersAlive = t.countAliveWeakRefs(workerRefs);

  t.assert(collected, `worker wrappers were retained: ${workersAlive}/${total}`);

  return {
    rounds,
    workersPerRound,
    total,
    replies,
    workersAlive,
    engine: t.engine,
  };
}, { timeoutMs: 60_000 });
