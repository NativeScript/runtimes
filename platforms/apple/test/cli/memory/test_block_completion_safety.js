"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("block-completion-safety", async (t) => {
  const total = 120;
  let executed = 0;
  let completed = 0;

  for (let i = 0; i < total; i++) {
    const op = NSBlockOperation.blockOperationWithBlock(() => {
      executed += 1;
    });
    op.completionBlock = () => {
      completed += 1;
    };
    op.start();
  }

  await t.forceGC(3, 16 * 1024 * 1024, 4);
  await t.sleep(20);

  t.assert(executed === total, `executed mismatch ${executed}/${total}`);
  t.assert(completed === total, `completed mismatch ${completed}/${total}`);

  return {
    total,
    executed,
    completed,
  };
}, { timeoutMs: 10_000 });
