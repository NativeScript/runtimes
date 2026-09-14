const worker = new Worker("platforms/apple/test/cli/structured_worker.js");

worker.onerror = (error) => {
  console.error("structured worker error", error);
  process.exit(1);
};

const shared = { label: "shared" };
const cycle = { name: "cycle" };
cycle.self = cycle;

const arrayBuffer = new ArrayBuffer(8);
const typed = new Uint16Array(arrayBuffer);
typed[0] = 513;
typed[1] = 1027;

const payload = {
  repeated: [shared, shared],
  cycle,
  date: new Date(1700000000000),
  regex: /native(script)?/gi,
  error: new TypeError("structured boom"),
  typed,
  view: new DataView(arrayBuffer, 2, 4),
  map: new Map([
    [shared, "shared-key"],
    ["typed", typed],
  ]),
  set: new Set([shared, "set-value"]),
  bigint:
    typeof BigInt === "function" ? BigInt("9007199254740993") : "no-bigint",
  bigintExpected: typeof BigInt === "function",
};

worker.onmessage = (event) => {
  const checks = event.data;
  const failed = Object.keys(checks).filter((key) => !checks[key]);
  if (failed.length === 0) {
    console.log("structured worker PASS");
  } else {
    console.log("structured worker FAIL", failed.join(","));
  }
  worker.terminate();
  process.exit(failed.length === 0 ? 0 : 1);
};

worker.postMessage(payload);

NSApplicationMain(0, null);
