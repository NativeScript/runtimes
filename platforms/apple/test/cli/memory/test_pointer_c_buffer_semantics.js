"use strict";

const { runPlainMemoryTest } = require("./_plain_harness");

runPlainMemoryTest("pointer-c-buffer-semantics", async (t) => {
  const pointer = interop.alloc(8);
  const pointerAgain = new interop.Pointer(pointer);
  const pointerFromHandle = interop.handleof(pointer);
  const pointerRoundTrip = pointer.add(4).subtract(4);

  t.assert(pointerAgain === pointer, "new interop.Pointer(pointer) should reuse the cached wrapper");
  t.assert(pointerFromHandle === pointer, "interop.handleof(pointer) should reuse the cached wrapper");
  t.assert(pointerRoundTrip === pointer, "pointer arithmetic should round-trip to the cached wrapper");

  const fakeStruct = { [Symbol.for("sizeof")]: 8 };
  t.assert(
    interop.handleof(fakeStruct) === null,
    "objects imitating struct metadata must not unwrap as native memory",
  );

  const buffer = interop.alloc(6);
  const bytes = new interop.Reference(interop.types.uint8, buffer);
  bytes[0] = "h".charCodeAt(0);
  bytes[1] = "e".charCodeAt(0);
  bytes[2] = "l".charCodeAt(0);
  bytes[3] = "l".charCodeAt(0);
  bytes[4] = "o".charCodeAt(0);
  bytes[5] = 0;

  const fromPointer = interop.stringFromCString(buffer);
  const fromReference = interop.stringFromCString(bytes);

  t.assert(fromPointer === "hello", `C string from pointer mismatch: ${fromPointer}`);
  t.assert(fromReference === "hello", `C string from reference mismatch: ${fromReference}`);
  t.assert(interop.handleof(bytes) === buffer, "typed reference handle should alias the original buffer");

  const owned = interop.alloc(32);
  const adopted = interop.adopt(owned);
  t.assert(adopted === owned, "interop.adopt(pointer) should preserve pointer wrapper identity");

  interop.free(buffer);
  t.assert(interop.stringFromCString(buffer) === null, "freed pointer should read back as null C string");

  interop.free(adopted);
  t.assert(interop.stringFromCString(adopted) === null, "freed adopted pointer should read back as null C string");

  return {
    adoptedPreservedIdentity: adopted === owned,
    engine: t.engine,
  };
}, { timeoutMs: 16_000 });
