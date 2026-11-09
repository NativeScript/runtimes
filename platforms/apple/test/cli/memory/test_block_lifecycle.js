"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("block-lifecycle", async (t) => {
  let queue = NSOperationQueue.new();
  queue.maxConcurrentOperationCount = 8;

  const totalOperations = 900;
  const chunkSize = 128 * 1024;
  let executed = 0;
  let completed = 0;

  for (let i = 0; i < totalOperations; i++) {
    const op = NSBlockOperation.blockOperationWithBlock(() => {
      const payload = new Uint8Array(chunkSize);
      payload[0] = i & 0xff;
      payload[payload.length - 1] = (i * 11) & 0xff;
      executed += payload[0] + payload[payload.length - 1] >= 0 ? 1 : 0;
    });
    op.completionBlock = () => {
      completed += 1;
    };
    queue.addOperation(op);
  }

  const start = t.now();
  const deadline = start + 20_000;
  while ((executed < totalOperations || completed < totalOperations) && t.now() < deadline) {
    await t.sleep(10);
  }

  t.assert(executed === totalOperations, `executed blocks mismatch ${executed}/${totalOperations}`);
  t.assert(completed === totalOperations, `completion blocks mismatch ${completed}/${totalOperations}`);

  queue.cancelAllOperations();
  queue = null;
  await t.forceGC(8, 40 * 1024 * 1024, 5);

  return {
    totalOperations,
    executed,
    completed,
    elapsedMs: t.now() - start,
  };
});
