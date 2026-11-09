"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("reference-lifecycle", async (t) => {
  let payloadWeak = null;
  let payloadRef = null;

  (function createUntypedReference() {
    const payload = {
      bytes: new Uint8Array(64),
      label: "payload",
    };
    payloadWeak = new WeakRef(payload);
    payloadRef = new interop.Reference(payload);
  })();

  await t.forceGC(3, 8 * 1024 * 1024, 4);
  t.assert(!!payloadWeak.deref(), "untyped Reference should keep its init value alive while the Reference is reachable");
  t.assert(payloadRef.value.label === "payload", "untyped Reference should preserve the init value");

  const typedInt = new interop.Reference(interop.types.int32, 41);
  const initialHandle = interop.handleof(typedInt);
  typedInt.value = 42;
  t.assert(typedInt.value === 42, "typed Reference should update its numeric value");
  t.assert(
    interop.handleof(typedInt) === initialHandle,
    "typed Reference should keep a stable backing handle after writes",
  );

  let wrapperWeak = null;

  (function createReferenceWrapper() {
    const ref = new interop.Reference(interop.types.int32, 7);
    wrapperWeak = new WeakRef(ref);
  })();

  const collected = await t.forceCollectUntil(() => !wrapperWeak.deref(), {
    timeoutMs: 10_000,
    intervalMs: 20,
    gcRounds: 2,
    pressureBytes: 12 * 1024 * 1024,
    pauseMs: 4,
  });

  t.assert(
    collected,
    `Reference wrapper did not release alive=${!!wrapperWeak.deref()}`,
  );

  payloadRef = null;

  return {
    collected,
    typedValue: typedInt.value,
    engine: t.engine,
  };
}, { timeoutMs: 16_000 });
