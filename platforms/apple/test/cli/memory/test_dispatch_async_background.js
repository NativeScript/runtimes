"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("dispatch-async-background", async (t) => {
  const total = 1200;
  let backgroundExecuted = 0;
  let checksum = 0;

  for (let i = 0; i < total; i++) {
    dispatch_async(dispatch_get_global_queue(qos_class_t.UTILITY, 0), () => {
      const payload = new Uint8Array(4 * 1024);
      payload[0] = i & 0xff;
      payload[payload.length - 1] = (i * 7) & 0xff;
      checksum += payload[0] + payload[payload.length - 1];
      backgroundExecuted += 1;
    });
  }

  const finished = await t.waitUntil(() => backgroundExecuted === total, 10_000, 10);
  t.assert(finished, `dispatch_async background callbacks incomplete ${backgroundExecuted}/${total}`);

  await t.forceGC(4, 20 * 1024 * 1024, 5);
  t.assert(checksum > 0, "checksum must be positive");

  return {
    total,
    backgroundExecuted,
    checksum,
  };
}, { timeoutMs: 12_000 });
