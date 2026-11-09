globalThis.onmessage = (event) => {
  const value = event.data;
  const checks = {};

  checks.repeatedReference = value.repeated[0] === value.repeated[1];
  checks.cycle = value.cycle.self === value.cycle;
  checks.date = value.date instanceof Date && value.date.getTime() === 1700000000000;
  checks.regexp =
    value.regex instanceof RegExp &&
    value.regex.source === "native(script)?" &&
    value.regex.global &&
    value.regex.ignoreCase;
  checks.error =
    value.error instanceof Error &&
    value.error.name === "TypeError" &&
    value.error.message === "structured boom";
  checks.typedArray =
    value.typed instanceof Uint16Array &&
    value.typed.length === 4 &&
    value.typed[0] === 513 &&
    value.typed[1] === 1027;
  checks.dataView =
    value.view instanceof DataView &&
    value.view.byteOffset === 2 &&
    value.view.byteLength === 4 &&
    value.view.getUint16(0, true) === 1027;
  checks.map =
    value.map instanceof Map &&
    value.map.get(value.repeated[0]) === "shared-key" &&
    value.map.get("typed") instanceof Uint16Array &&
    value.map.get("typed")[0] === 513;
  checks.set =
    value.set instanceof Set &&
    value.set.has(value.repeated[0]) &&
    value.set.has("set-value");
  checks.bigint =
    value.bigintExpected
      ? typeof value.bigint === "bigint" &&
        value.bigint === BigInt("9007199254740993")
      : value.bigint === "no-bigint";

  globalThis.postMessage(checks);
};
