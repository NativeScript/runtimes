"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("js-heap-throughput", async (t) => {
  const outerRounds = 8;
  const chunkSize = 128 * 1024;
  const chunksPerRound = 350;
  let bytesAllocated = 0;
  let checksum = 0;

  for (let round = 0; round < outerRounds; round++) {
    const holder = new Array(chunksPerRound);
    for (let i = 0; i < chunksPerRound; i++) {
      const buf = new Uint8Array(chunkSize);
      buf[0] = (round + i) & 0xff;
      buf[buf.length - 1] = (round * 3 + i) & 0xff;
      checksum += buf[0] + buf[buf.length - 1];
      holder[i] = buf;
      bytesAllocated += buf.byteLength;
    }

    holder.length = 0;

    if ((round + 1) % 2 === 0) {
      await t.forceGC(4, 20 * 1024 * 1024, 4);
    } else {
      await t.sleep(8);
    }
  }

  await t.forceGC(6, 24 * 1024 * 1024, 6);

  t.assert(checksum > 0, "checksum should be positive");

  return {
    outerRounds,
    chunksPerRound,
    chunkSize,
    totalAllocatedMB: Math.round(bytesAllocated / (1024 * 1024)),
    checksum,
  };
});
