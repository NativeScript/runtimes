"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("mixed-stress", async (t) => {
  const rounds = 20;
  const perRound = 500;
  let blockHits = 0;
  let checksum = 0;

  for (let round = 0; round < rounds; round++) {
    t.autoreleasepool(() => {
      for (let i = 0; i < perRound; i++) {
        const value = round * perRound + i;
        const nativeString = NSString.stringWithString(`v-${value}`);
        const array = NSMutableArray.arrayWithObject(nativeString);
        const dict = NSMutableDictionary.new();
        dict.setObjectForKey(array, "a");
        dict.setObjectForKey(NSNumber.numberWithInt(value), "n");

        const op = NSBlockOperation.blockOperationWithBlock(() => {
          blockHits += 1;
          checksum += dict.count + array.count + nativeString.length;
        });
        op.start();
      }
    });

    if ((round + 1) % 4 === 0) {
      await t.forceGC(2, 16 * 1024 * 1024, 4);
    } else {
      await t.sleep(2);
    }
  }

  await t.forceGC(8, 32 * 1024 * 1024, 4);

  t.assert(blockHits === rounds * perRound, `block hits mismatch ${blockHits}/${rounds * perRound}`);
  t.assert(checksum > 0, "checksum should be positive");

  return {
    rounds,
    perRound,
    totalOps: rounds * perRound,
    blockHits,
    checksum,
  };
});
