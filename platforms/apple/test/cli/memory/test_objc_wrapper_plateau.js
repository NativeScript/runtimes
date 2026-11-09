"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("objc-wrapper-plateau", async (t) => {
  const waves = 4;
  const objectsPerWave = 8000;
  const sampledWrappers = [];
  const nativeObjects = NSHashTable.weakObjectsHashTable();
  let checksum = 0;

  function allocateWave(wave) {
    t.autoreleasepool(() => {
      for (let index = 0; index < objectsPerWave; index++) {
        const value = wave * objectsPerWave + index;
        const stringValue = NSString.stringWithString(String(value));
        const numberValue = NSNumber.numberWithLongLong(value);
        const dictionary = NSMutableDictionary.dictionary();
        dictionary.setObjectForKey(stringValue, "s");
        dictionary.setObjectForKey(numberValue, "n");
        checksum += dictionary.count + numberValue;

        if (index % 100 === 0) {
          sampledWrappers.push(new WeakRef(dictionary));
          nativeObjects.addObject(dictionary);
        }
      }
    });
  }

  allocateWave(0);
  await t.forceGC(3, 24 * 1024 * 1024, 6);
  t.markRssBaseline();

  for (let wave = 1; wave < waves; wave++) {
    allocateWave(wave);
    await t.forceGC(3, 24 * 1024 * 1024, 6);
  }

  const collected = await t.forceCollectUntil(() => {
    return t.countAliveWeakRefs(sampledWrappers) <= 4 &&
      t.weakTableCount(nativeObjects) <= 4;
  }, {
    timeoutMs: 16_000,
    intervalMs: 25,
    gcRounds: 2,
    pressureBytes: 20 * 1024 * 1024,
    pauseMs: 6,
  });

  const wrappersAlive = t.countAliveWeakRefs(sampledWrappers);
  const nativeAlive = t.weakTableCount(nativeObjects);
  t.assert(
    collected,
    `wrapper waves retained wrappers=${wrappersAlive} native=${nativeAlive}`,
  );
  t.assert(checksum > 0, "checksum should be positive");

  return {
    waves,
    objectsPerWave,
    wrappersAlive,
    nativeAlive,
    checksum,
  };
}, { timeoutMs: 40_000 });
