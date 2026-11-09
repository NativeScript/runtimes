"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("circular-js-to-native-conversion", async (t) => {
  const iterations = 160;
  const roots = [];
  const wrappers = [];
  const nativeObjects = NSHashTable.weakObjectsHashTable();
  let rejected = 0;

  function makeCircularGraph(index, native) {
    switch (index % 5) {
      case 0: {
        const array = [native];
        array.push(array);
        return array;
      }
      case 1: {
        const object = { native };
        object.self = object;
        return object;
      }
      case 2: {
        const array = [native];
        const object = { array };
        array.push(object);
        return object;
      }
      case 3: {
        const map = new Map([["native", native]]);
        map.set("self", map);
        return map;
      }
      default: {
        const arrayLike = { 0: native, length: 2 };
        arrayLike[1] = arrayLike;
        return arrayLike;
      }
    }
  }

  for (let i = 0; i < iterations; i++) {
    t.autoreleasepool(function attemptCircularConversion() {
      const native = NSMutableDictionary.dictionary();
      native.setObjectForKey(NSNumber.numberWithInt(i), "index");
      nativeObjects.addObject(native);

      const root = makeCircularGraph(i, native);
      roots.push(new WeakRef(root));
      wrappers.push(new WeakRef(native));

      try {
        NSArray.arrayWithArray(root);
      } catch (error) {
        t.assert(
          String(error).includes("Circular JavaScript object graphs"),
          `unexpected circular conversion error at iteration ${i}: ${error}`,
        );
        rejected++;
        return;
      }

      throw new Error("circular JS-to-native conversion was accepted");
    });

    if ((i + 1) % 20 === 0) {
      await t.forceGC(2, 8 * 1024 * 1024, 3);
    }
  }

  t.assert(rejected === iterations, `rejected ${rejected}/${iterations}`);

  const collected = await t.forceCollectUntil(() => {
    return t.countAliveWeakRefs(roots) <= 2 &&
      t.countAliveWeakRefs(wrappers) <= 2 &&
      t.weakTableCount(nativeObjects) <= 2;
  }, {
    timeoutMs: 14_000,
    intervalMs: 25,
    gcRounds: 2,
    pressureBytes: 16 * 1024 * 1024,
    pauseMs: 4,
  });

  const rootsAlive = t.countAliveWeakRefs(roots);
  const wrappersAlive = t.countAliveWeakRefs(wrappers);
  const nativeAlive = t.weakTableCount(nativeObjects);

  t.assert(
    collected,
    `rejected circular conversions retained values roots=${rootsAlive} wrappers=${wrappersAlive} native=${nativeAlive}`,
  );

  return {
    iterations,
    rejected,
    rootsAlive,
    wrappersAlive,
    nativeAlive,
    engine: t.engine,
  };
}, { timeoutMs: 24_000 });
