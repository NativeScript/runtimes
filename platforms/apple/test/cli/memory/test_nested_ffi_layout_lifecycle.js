"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("nested-ffi-layout-lifecycle", async (t) => {
  const rounds = 4_000;
  let checksum = 0;

  for (let i = 0; i < rounds; i++) {
    t.autoreleasepool(() => {
      const expected = `${i}.125`;
      const number = NSDecimalNumber.decimalNumberWithString(expected);
      const decimal = number.decimalValue;
      if (i === 0) {
        const inherited = Object.create(decimal);
        t.assert(
          interop.handleof(decimal) instanceof interop.Pointer,
          "struct handle should expose its backing storage",
        );
        t.assert(
          interop.handleof(inherited) === null,
          "an object inheriting from a struct must not inherit its native handle",
        );

        const prototype = Object.getPrototypeOf(decimal);
        const metadataProperties = new Set(["kind", "name", "sizeof", "address", "toString"]);
        const fieldOwner = [decimal, prototype].find((owner) => {
          return Object.getOwnPropertyNames(owner).some((name) => {
            const descriptor = Object.getOwnPropertyDescriptor(owner, name);
            return !metadataProperties.has(name) && descriptor &&
              (typeof descriptor.get === "function" ||
              typeof descriptor.set === "function");
          });
        });
        const field = fieldOwner && Object.getOwnPropertyNames(fieldOwner).find((name) => {
          const descriptor = Object.getOwnPropertyDescriptor(fieldOwner, name);
          return !metadataProperties.has(name) && descriptor &&
            (typeof descriptor.get === "function" ||
            typeof descriptor.set === "function");
        });
        if (field) {
          let readRejected = false;
          let writeRejected = false;
          try {
            void inherited[field];
          } catch (_) {
            readRejected = true;
          }
          try {
            inherited[field] = decimal[field];
          } catch (_) {
            writeRejected = true;
          }
          t.assert(readRejected, "an inherited object must not read its prototype's struct data");
          t.assert(writeRejected, "an inherited object must not write its prototype's struct data");
        }
      }
      const roundTrip = NSDecimalNumber.decimalNumberWithDecimal(decimal);
      checksum += roundTrip.intValue;
      t.assert(
        roundTrip.stringValue === expected,
        `nested NSDecimal layout corrupted at iteration ${i}`,
      );
    });
  }

  await t.forceGC(4, 24 * 1024 * 1024, 5);

  return {
    rounds,
    checksum,
    engine: t.engine,
  };
}, { timeoutMs: 30_000 });
