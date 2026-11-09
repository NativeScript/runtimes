"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("worker-main-env-shutdown", async (t) => {
  let worker = new Worker(
    "platforms/apple/test/cli/memory/_worker_lifecycle_child.js",
  );

  // Let one-time worker runtime initialization reach its steady RSS before
  // measuring shutdown behavior. The worker intentionally remains active so
  // the main environment must stop it during teardown.
  await t.sleep(2_000);
  worker = null;
  t.markRssBaseline();
  await t.sleep(800);

  return {
    activeWorkerReleasedByEnvironmentCleanup: true,
    engine: t.engine,
  };
}, { timeoutMs: 15_000, warmupMs: 100 });
