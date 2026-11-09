"use strict";

function toMs(start, end) {
  return end - start;
}

const benchmarkScale = (() => {
  const value = Number(
    typeof process === "object" && process && process.env
      ? process.env.NS_BENCH_SCALE
      : 1,
  );
  return Number.isFinite(value) && value > 0 ? value : 1;
})();

function scaledIterations(iterations) {
  return Math.max(1_000, Math.round(iterations * benchmarkScale));
}

function runScenario(name, iterations, fn) {
  // Warm-up to trigger JIT and metadata caches before timing.
  for (let i = 0; i < 5000; i++) {
    fn(i);
  }

  const startedAt = performance.now();
  let checksum = 0;
  for (let i = 0; i < iterations; i++) {
    checksum += fn(i) | 0;
  }
  const endedAt = performance.now();

  return {
    name,
    iterations,
    durationMs: toMs(startedAt, endedAt),
    perCallUs: ((endedAt - startedAt) * 1000) / iterations,
    checksum,
  };
}

function main() {
  const s1 = NSString.stringWithString("1.2.3");
  const s2 = NSString.stringWithString("1.10.0");
  const baseDate = NSDate.dateWithTimeIntervalSince1970(1700000000.25);
  const point = new CGPoint();
  point.x = 1;
  point.y = 2;

  const scenarios = [
    runScenario("objc.class.no_args.NSDate.date", scaledIterations(2_000_000), () => {
      const d = NSDate.date();
      return d ? 1 : 0;
    }),
    runScenario("objc.class.double_arg.NSDate.dateWithTimeIntervalSince1970", scaledIterations(1_500_000), (i) => {
      const d = NSDate.dateWithTimeIntervalSince1970(1700000000.25 + (i & 31));
      return d ? 1 : 0;
    }),
    runScenario("objc.instance.no_args_primitive_ret.NSDate.timeIntervalSince1970", scaledIterations(3_000_000), () => {
      return baseDate.timeIntervalSince1970 > 0 ? 1 : 0;
    }),
    runScenario("objc.instance.obj_plus_enum_arg.NSString.compareOptions", scaledIterations(2_000_000), () => {
      return s1.compareOptions(s2, NSStringCompareOptions.NSNumericSearch) + 2;
    }),
    runScenario("cfunc.no_args.CFAbsoluteTimeGetCurrent", scaledIterations(3_000_000), () => {
      return CFAbsoluteTimeGetCurrent() > 0 ? 1 : 0;
    }),
    runScenario("struct.field.get.CGPoint.x", scaledIterations(3_000_000), () => {
      return point.x;
    }),
    runScenario("struct.field.set.CGPoint.x", scaledIterations(2_000_000), (i) => {
      point.x = i & 7;
      return point.x;
    }),
    runScenario("struct.construct.CGPoint", scaledIterations(500_000), (i) => {
      const value = new CGPoint();
      value.x = i & 7;
      return value.x;
    }),
  ];

  const totalMs = scenarios.reduce((acc, item) => acc + item.durationMs, 0);
  const payload = {
    runtimePath: NSBundle.mainBundle.executablePath.toString(),
    scale: benchmarkScale,
    totalMs,
    scenarios,
  };

  console.log(`BENCH_RESULT:${JSON.stringify(payload)}`);
}

main();
