"use strict";

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function drainPendingJobs() {
  if (typeof __drainMicrotaskQueue === "function") {
    __drainMicrotaskQueue();
  }
}

function makePressure(bytes) {
  const chunkSize = 64 * 1024;
  const chunks = Math.max(1, Math.floor(bytes / chunkSize));
  const holder = new Array(chunks);
  for (let i = 0; i < chunks; i++) {
    holder[i] = new Uint8Array(chunkSize);
    holder[i][0] = i & 0xff;
  }
  return holder;
}

async function forceGC(rounds, pressureBytes, pauseMs) {
  const gcRounds = rounds ?? 4;
  const bytes = pressureBytes ?? (24 * 1024 * 1024);
  const pause = pauseMs ?? 4;

  for (let i = 0; i < gcRounds; i++) {
    if (typeof gc === "function") {
      gc();
    }
    drainPendingJobs();
    const junk = makePressure(bytes);
    junk.length = 0;
    await sleep(pause);
    drainPendingJobs();
  }

  if (typeof gc === "function") {
    gc();
  }
  drainPendingJobs();
  await sleep(pause);
  drainPendingJobs();
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message || "assertion failed");
  }
}

function countAliveWeakRefs(refs) {
  let alive = 0;
  for (const ref of refs) {
    if (ref && typeof ref.deref === "function" && ref.deref()) {
      alive += 1;
    }
  }
  return alive;
}

function weakTableCount(table) {
  if (!table) {
    return 0;
  }

  let count = 0;
  for (const ignored of table) {
    void ignored;
    count += 1;
  }
  return count;
}

async function waitUntil(predicate, timeoutMs, intervalMs) {
  const timeout = timeoutMs ?? 8_000;
  const interval = intervalMs ?? 10;
  const start = Date.now();
  while (Date.now() - start < timeout) {
    if (predicate()) {
      return true;
    }
    await sleep(interval);
  }
  return !!predicate();
}

async function forceCollectUntil(predicate, options) {
  const opts = options || {};
  const timeoutMs = opts.timeoutMs ?? 10_000;
  const intervalMs = opts.intervalMs ?? 20;
  const gcRounds = opts.gcRounds ?? 2;
  const pressureBytes = opts.pressureBytes ?? (16 * 1024 * 1024);
  const pauseMs = opts.pauseMs ?? 4;
  const settleTicks = opts.settleTicks ?? 2;
  const start = Date.now();
  let stableTicks = 0;

  while (Date.now() - start < timeoutMs) {
    await forceGC(gcRounds, pressureBytes, pauseMs);

    if (predicate()) {
      stableTicks += 1;
      if (stableTicks >= settleTicks) {
        return true;
      }
    } else {
      stableTicks = 0;
    }

    await sleep(intervalMs);
  }

  return !!predicate();
}

async function drainRunLoopUntilIdle(predicate, options) {
  const opts = options || {};
  const timeoutMs = opts.timeoutMs ?? 10_000;
  const tickMs = opts.tickMs ?? 8;
  const settleTicks = opts.settleTicks ?? 3;
  const mode = NSDefaultRunLoopMode;
  const start = Date.now();
  let idleTicks = 0;

  while (Date.now() - start < timeoutMs) {
    NSRunLoop.mainRunLoop.runModeBeforeDate(
      mode,
      NSDate.dateWithTimeIntervalSinceNow(tickMs / 1000),
    );

    drainPendingJobs();

    if (predicate()) {
      idleTicks += 1;
      if (idleTicks >= settleTicks) {
        return true;
      }
    } else {
      idleTicks = 0;
    }

    await sleep(0);
  }

  return !!predicate();
}

function emitResult(result) {
  const engine =
    (typeof process === "object" && process && process.versions && process.versions.engine) ||
    "unknown";
  const payload = JSON.stringify({ engine, ...result });
  console.log(`MEMTEST_RESULT:${payload}`);
}

function markRssBaseline() {
  console.log("MEMTEST_RSS_BASELINE");
}

function autoreleasepool(fn) {
  if (typeof objc === "object" && typeof objc.autoreleasepool === "function") {
    return objc.autoreleasepool(fn);
  }
  return fn();
}

function runPlainMemoryTest(name, fn, options) {
  const opts = options || {};
  const timeoutMs = opts.timeoutMs ?? 40_000;
  const warmupMs = opts.warmupMs ?? 800;

  setTimeout(() => {
    let finished = false;

    const timeoutId = setTimeout(() => {
      if (finished) {
        return;
      }
      finished = true;
      emitResult({
        name,
        pass: false,
        reason: "timeout",
        timeoutMs,
      });
    }, timeoutMs);

    Promise.resolve()
      .then(() => {
        markRssBaseline();
        return fn({
          sleep,
          forceGC,
          forceCollectUntil,
          drainRunLoopUntilIdle,
          assert,
          waitUntil,
          makePressure,
          countAliveWeakRefs,
          weakTableCount,
          markRssBaseline,
          now: () => Date.now(),
          autoreleasepool,
          engine:
            (typeof process === "object" &&
              process &&
              process.versions &&
              process.versions.engine) ||
            "unknown",
        });
      })
      .then((details) => {
        if (finished) {
          return;
        }
        finished = true;
        clearTimeout(timeoutId);
        emitResult({
          name,
          pass: true,
          details: details || {},
        });
      })
      .catch((error) => {
        if (finished) {
          return;
        }
        finished = true;
        clearTimeout(timeoutId);
        const message = String(error && error.message ? error.message : error);
        const stack = error && error.stack ? String(error.stack) : "";
        emitResult({
          name,
          pass: false,
          error: stack && !stack.includes(message) ? `${message}\n${stack}` : (stack || message),
        });
      });
  }, warmupMs);
}

module.exports = {
  runPlainMemoryTest,
};
