"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("runloop-pending-work", async (t) => {
  const totalMainQueueCallbacks = 1500;
  let completedMainQueueCallbacks = 0;
  let checksum = 0;

  for (let i = 0; i < totalMainQueueCallbacks; i++) {
    dispatch_async(dispatch_get_global_queue(qos_class_t.UTILITY, 0), () => {
      const value = i & 0xff;
      NSOperationQueue.mainQueue.addOperationWithBlock(() => {
        checksum += value;
        completedMainQueueCallbacks += 1;
      });
    });
  }

  const drained = await t.drainRunLoopUntilIdle(
    () => completedMainQueueCallbacks === totalMainQueueCallbacks,
    { timeoutMs: 15_000, tickMs: 5, settleTicks: 4 },
  );

  t.assert(
    drained,
    `runloop did not drain pending main-queue callbacks ${completedMainQueueCallbacks}/${totalMainQueueCallbacks}`,
  );

  await t.forceGC(4, 24 * 1024 * 1024, 6);
  t.assert(checksum > 0, "checksum should be positive");

  return {
    totalMainQueueCallbacks,
    completedMainQueueCallbacks,
    checksum,
  };
}, { timeoutMs: 20_000 });
