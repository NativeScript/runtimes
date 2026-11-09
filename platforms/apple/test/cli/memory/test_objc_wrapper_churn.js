"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("objc-wrapper-churn", async (t) => {
  const outerRounds = 16;
  const innerRounds = 2000;
  let checksum = 0;

  for (let outer = 0; outer < outerRounds; outer++) {
    t.autoreleasepool(() => {
      for (let inner = 0; inner < innerRounds; inner++) {
        const value = outer * innerRounds + inner;
        const stringValue = NSString.stringWithString(String(value));
        const numberValue = NSNumber.numberWithLongLong(value);
        const dict = NSMutableDictionary.new();
        dict.setObjectForKey(stringValue, "s");
        dict.setObjectForKey(numberValue, "n");
        const array = NSMutableArray.arrayWithObject(stringValue);

        checksum += numberValue + array.count + dict.count;
      }
    });

    if ((outer + 1) % 2 === 0) {
      await t.forceGC(2, 16 * 1024 * 1024, 3);
    } else {
      await t.sleep(6);
    }
  }

  t.assert(checksum > 0, "checksum should be positive");
  return {
    outerRounds,
    innerRounds,
    checksum,
  };
});
