"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("objc-unmanaged-transfer-semantics", async (t) => {
  const capabilityProbe = NSObject.alloc();
  if (
    typeof capabilityProbe.takeRetainedValue !== "function" ||
    typeof capabilityProbe.takeUnretainedValue !== "function"
  ) {
    return {
      supported: false,
      reason: "unmanaged transfer helpers are only exposed by direct engine backends",
    };
  }

  const rounds = 1000;
  let retainedFailures = 0;
  let unretainedFailures = 0;
  let consumedFailures = 0;

  function expectConsumed(value) {
    try {
      value.retainCount();
      consumedFailures += 1;
    } catch (_) {
      // Expected: the original wrapper has transferred its native value.
    }
  }

  for (let i = 0; i < rounds; i++) {
    const retainedSource = NSObject.alloc();
    const retained = retainedSource.takeRetainedValue();
    const retainedCount = retained.retainCount();
    if (!(retainedCount >= 1)) {
      retainedFailures += 1;
    }
    expectConsumed(retainedSource);

    const unretainedSource = NSObject.alloc().init();
    const unretained = unretainedSource.takeUnretainedValue();
    const unretainedCount = unretained.retainCount();
    if (!(unretainedCount >= 1)) {
      unretainedFailures += 1;
    }
    expectConsumed(unretainedSource);
  }

  t.assert(
    retainedFailures === 0,
    `retained transfer produced invalid wrappers in ${retainedFailures} rounds`,
  );
  t.assert(
    unretainedFailures === 0,
    `unretained transfer produced invalid wrappers in ${unretainedFailures} rounds`,
  );
  t.assert(
    consumedFailures === 0,
    `consumed unmanaged wrappers remained usable in ${consumedFailures} rounds`,
  );

  return {
    rounds,
    retainedFailures,
    unretainedFailures,
    consumedFailures,
  };
}, { timeoutMs: 20_000 });
