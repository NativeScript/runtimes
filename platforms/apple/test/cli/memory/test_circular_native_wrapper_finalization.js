"use strict";

const vm = require("node:vm");
const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("circular-native-wrapper-finalization", async (t) => {
  const rounds = 8;
  const objectsPerRound = 80;
  const total = rounds * objectsPerRound;
  const rootWeakRefs = [];
  const wrapperWeakRefs = [];
  const nativeWeakObjects = NSHashTable.weakObjectsHashTable();

  async function sampleHeap() {
    await t.forceGC(2, 8 * 1024 * 1024, 4);
    const usage = process.memoryUsage();
    const measured = await vm.measureMemory({ mode: "summary" });
    return {
      heapUsed: Number(usage.heapUsed) || 0,
      estimate:
        Number(measured && measured.total && measured.total.jsMemoryEstimate) || 0,
    };
  }

  const before = await sampleHeap();

  for (let round = 0; round < rounds; round++) {
    t.autoreleasepool(function createCircularGraphs() {
      for (let i = 0; i < objectsPerRound; i++) {
        const index = round * objectsPerRound + i;
        const native = NSMutableDictionary.dictionary();
        native.setObjectForKey(NSNumber.numberWithInt(index), "index");
        nativeWeakObjects.addObject(native);

        const root = {
          index,
          native,
          payload: new Uint8Array(4 * 1024),
        };
        const peer = { root, native };
        const array = [root, peer, native];
        const map = new Map();
        const set = new Set();

        root.self = root;
        root.peer = peer;
        root.array = array;
        root.map = map;
        root.set = set;
        peer.peer = peer;
        array.push(array);
        map.set(root, peer);
        map.set(map, native);
        set.add(root);
        set.add(set);

        rootWeakRefs.push(new WeakRef(root));
        wrapperWeakRefs.push(new WeakRef(native));
      }
    });

    await t.forceGC(2, 8 * 1024 * 1024, 4);
  }

  const collected = await t.forceCollectUntil(() => {
    return t.countAliveWeakRefs(rootWeakRefs) <= 2 &&
      t.countAliveWeakRefs(wrapperWeakRefs) <= 2 &&
      t.weakTableCount(nativeWeakObjects) <= 2;
  }, {
    timeoutMs: 14_000,
    intervalMs: 25,
    gcRounds: 2,
    pressureBytes: 16 * 1024 * 1024,
    pauseMs: 4,
  });

  const afterSamples = [];
  for (let i = 0; i < 3; i++) {
    afterSamples.push(await sampleHeap());
  }
  const after = afterSamples.reduce((best, sample) => {
    return sample.heapUsed < best.heapUsed ? sample : best;
  });

  const rootsAlive = t.countAliveWeakRefs(rootWeakRefs);
  const wrappersAlive = t.countAliveWeakRefs(wrapperWeakRefs);
  const nativeAlive = t.weakTableCount(nativeWeakObjects);
  const heapDrift = after.heapUsed - before.heapUsed;
  const heapDriftLimit = 8 * 1024 * 1024;

  t.assert(
    collected,
    `circular native wrapper graphs were retained roots=${rootsAlive} wrappers=${wrappersAlive} native=${nativeAlive}`,
  );
  if (before.heapUsed > 0 && after.heapUsed > 0) {
    t.assert(
      heapDrift <= heapDriftLimit,
      `JS heap retained circular wrapper graphs drift=${heapDrift} limit=${heapDriftLimit}`,
    );
  }

  return {
    rounds,
    objectsPerRound,
    total,
    rootsAlive,
    wrappersAlive,
    nativeAlive,
    heapBefore: before,
    heapAfter: after,
    heapDrift,
    heapDriftLimit,
    engine: t.engine,
  };
}, { timeoutMs: 24_000 });
