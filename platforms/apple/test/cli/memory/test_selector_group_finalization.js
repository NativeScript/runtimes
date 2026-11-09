"use strict";

const vm = require("node:vm");
const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("selector-group-finalization", async (t) => {
  const rounds = 10;
  const objectsPerRound = 4000;
  const sampledWrapperRefs = [];
  const sampledFunctionRefs = [];
  const seenFunctions = new WeakSet();
  const nativeObjects = NSHashTable.weakObjectsHashTable();

  async function settledMemory() {
    const samples = [];
    for (let i = 0; i < 3; i++) {
      await t.forceGC(2, 12 * 1024 * 1024, 4);
      const usage = process.memoryUsage();
      const measured = await vm.measureMemory({ mode: "summary" });
      samples.push({
        rss: Number(usage.rss) || 0,
        heapUsed: Number(usage.heapUsed) || 0,
        estimate:
          Number(measured && measured.total && measured.total.jsMemoryEstimate) || 0,
      });
    }
    return samples.reduce((best, sample) => {
      return sample.heapUsed < best.heapUsed ? sample : best;
    });
  }

  const before = await settledMemory();
  let resolvedFunctions = 0;
  let uniqueFunctions = 0;

  for (let round = 0; round < rounds; round++) {
    t.autoreleasepool(function createBoundSelectorGroups() {
      for (let i = 0; i < objectsPerRound; i++) {
        const object = NSMutableDictionary.dictionary();
        nativeObjects.addObject(object);
        const method = object.respondsToSelector;
        t.assert(typeof method === "function", "expected a bound selector function");
        resolvedFunctions++;
        if (!seenFunctions.has(method)) {
          seenFunctions.add(method);
          uniqueFunctions++;
        }

        if (i % 100 === 0) {
          sampledWrapperRefs.push(new WeakRef(object));
          sampledFunctionRefs.push(new WeakRef(method));
        }
      }
    });
    await t.forceGC(3, 16 * 1024 * 1024, 4);
  }

  const collectedNative = await t.forceCollectUntil(() => {
    return t.countAliveWeakRefs(sampledWrapperRefs) <= 2 &&
      t.weakTableCount(nativeObjects) <= 2;
  }, {
    timeoutMs: 16_000,
    intervalMs: 25,
    gcRounds: 2,
    pressureBytes: 16 * 1024 * 1024,
    pauseMs: 4,
  });

  const after = await settledMemory();
  const rssDrift = after.rss - before.rss;
  const heapDrift = after.heapUsed - before.heapUsed;
  const estimateDrift = after.estimate - before.estimate;
  const rssDriftLimit = (t.engine === "jsc" ? 160 : 32) * 1024 * 1024;
  const heapDriftLimit = 16 * 1024 * 1024;
  const wrappersAlive = t.countAliveWeakRefs(sampledWrapperRefs);
  const functionsAlive = t.countAliveWeakRefs(sampledFunctionRefs);
  const nativeAlive = t.weakTableCount(nativeObjects);

  t.assert(
    collectedNative,
    `selector groups retained wrappers=${wrappersAlive} native=${nativeAlive}`,
  );
  if (before.heapUsed > 0 && after.heapUsed > 0) {
    t.assert(
      heapDrift <= heapDriftLimit,
      `selector-group churn retained JS heap drift=${heapDrift} limit=${heapDriftLimit}`,
    );
  }
  if (before.rss > 0 && after.rss > 0) {
    t.assert(
      rssDrift <= rssDriftLimit,
      `selector-group churn retained native memory drift=${rssDrift} limit=${rssDriftLimit}`,
    );
  }

  return {
    rounds,
    objectsPerRound,
    resolvedFunctions,
    uniqueFunctions,
    wrappersAlive,
    functionsAlive,
    nativeAlive,
    memoryBefore: before,
    memoryAfter: after,
    rssDrift,
    rssDriftLimit,
    heapDrift,
    heapDriftLimit,
    estimateDrift,
    engine: t.engine,
  };
}, { timeoutMs: 40_000 });
