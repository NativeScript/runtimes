"use strict";

const fs = require("fs");
const path = require("path");
const readline = require("readline");
const { spawn, execFile, execFileSync } = require("child_process");

const memoryThresholdsKB = {
  "weakref-finalization": 40 * 1024,
  "js-function-finalization": 40 * 1024,
  "js-heap-throughput": 120 * 1024,
  "objc-ownership-rules": 60 * 1024,
  "objc-unmanaged-transfer-semantics": 60 * 1024,
  "objc-wrapper-churn": 80 * 1024,
  "objc-wrapper-plateau": 80 * 1024,
  "appkit-navigation-throughput": 140 * 1024,
  "appkit-navigation-extreme": 220 * 1024,
  "block-lifecycle": 60 * 1024,
  "block-completion-safety": 40 * 1024,
  "dispatch-async-background": 40 * 1024,
  "runloop-pending-work": 70 * 1024,
  "plain-script-runloop-drain": 70 * 1024,
  "mixed-stress": 90 * 1024,
  "weakref-plain-script": 40 * 1024,
  "objc-wrapper-finalization": 50 * 1024,
  "pointer-c-buffer-semantics": 40 * 1024,
  "reference-lifecycle": 40 * 1024,
  "block-callback-finalization": 40 * 1024,
  "c-function-pointer-semantics": 40 * 1024,
  "nested-ffi-layout-lifecycle": 40 * 1024,
  "circular-native-wrapper-finalization": 60 * 1024,
  "circular-js-to-native-conversion": 60 * 1024,
  "selector-group-finalization": 48 * 1024,
  "worker-lifecycle": 120 * 1024,
  "worker-main-env-shutdown": 40 * 1024,
};

// JavaScriptCore keeps a larger reusable heap after allocation-heavy tests.
// These bounds cover its measured plateau without weakening other engines.
const engineMemoryThresholdsKB = {
  jsc: {
    "block-lifecycle": 84 * 1024,
    "dispatch-async-background": 52 * 1024,
    "mixed-stress": 104 * 1024,
    "nested-ffi-layout-lifecycle": 72 * 1024,
    "objc-wrapper-plateau": 104 * 1024,
    "weakref-finalization": 52 * 1024,
    "weakref-plain-script": 52 * 1024,
  },
};

const kMinValidRssKB = 4 * 1024;

function parseArgs(argv) {
  const args = argv.slice(2);
  const parsed = {
    timeoutMs: 120_000,
    repeat: 2,
    grep: null,
    runtime: null,
    excludeWindowTests: false,
  };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === "--timeout-ms" && args[i + 1]) {
      parsed.timeoutMs = Number(args[++i]);
      continue;
    }
    if (arg === "--repeat" && args[i + 1]) {
      parsed.repeat = Number(args[++i]);
      continue;
    }
    if (arg === "--grep" && args[i + 1]) {
      parsed.grep = String(args[++i]);
      continue;
    }
    if (arg === "--runtime" && args[i + 1]) {
      parsed.runtime = String(args[++i]);
      continue;
    }
    if (arg === "--exclude-window-tests") {
      parsed.excludeWindowTests = true;
      continue;
    }
  }

  if (!Number.isFinite(parsed.timeoutMs) || parsed.timeoutMs <= 0) {
    parsed.timeoutMs = 120_000;
  }
  if (!Number.isFinite(parsed.repeat) || parsed.repeat <= 0) {
    parsed.repeat = 1;
  }

  return parsed;
}

function median(values) {
  if (!values.length) {
    return null;
  }
  const sorted = values.slice().sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  if ((sorted.length & 1) === 1) {
    return sorted[mid];
  }
  return Math.round((sorted[mid - 1] + sorted[mid]) / 2);
}

function computeBaselineKB(samples, markerMs) {
  if (!samples.length) {
    return null;
  }
  if (markerMs != null) {
    const markedWindow = samples.filter(
      (sample) => sample.t >= markerMs - 600 && sample.t <= markerMs,
    );
    if (markedWindow.length) {
      return median(markedWindow.map((sample) => sample.rssKB));
    }
  }
  const warmupWindow = samples.filter((sample) => sample.t >= 400 && sample.t <= 1600);
  const baselineWindow = (warmupWindow.length >= 3 ? warmupWindow : samples).slice(0, 6);
  return median(baselineWindow.map((sample) => sample.rssKB));
}

function listTestFiles(memoryDir, grep, excludeWindowTests) {
  return fs.readdirSync(memoryDir)
    .filter((name) => name.startsWith("test_") && name.endsWith(".js"))
    .filter((name) => !grep || name.includes(grep))
    .filter((name) => !excludeWindowTests || !name.startsWith("test_appkit_navigation_"))
    .sort()
    .map((name) => path.join(memoryDir, name));
}

function sampleRssKB(pid) {
  return new Promise((resolve) => {
    execFile("ps", ["-o", "rss=", "-p", String(pid)], { encoding: "utf8" }, (error, stdout) => {
      if (error) {
        resolve(null);
        return;
      }
      const text = String(stdout).trim();
      if (!text) {
        resolve(null);
        return;
      }
      const value = Number(text);
      resolve(Number.isFinite(value) && value >= kMinValidRssKB ? value : null);
    });
  });
}

function ensureExecutableSignature(nsrPath) {
  try {
    execFileSync("codesign", ["--force", "--sign", "-", "--timestamp=none", nsrPath], {
      stdio: "ignore",
    });
  } catch (error) {
    console.warn(`warning: failed to refresh code signature for ${nsrPath}: ${error.message}`);
  }
}

async function runSingleTest({ nsrPath, cwd, testFile, timeoutMs }) {
  return new Promise((resolve) => {
    const child = spawn(nsrPath, ["run", testFile], {
      cwd,
      stdio: ["ignore", "pipe", "pipe"],
    });

    const startedAt = Date.now();
    const rssSamples = [];
    const logs = [];
    let timedOut = false;
    let parsedResult = null;
    let baselineMarkerMs = null;

    const addLine = (line) => {
      if (logs.length < 200) {
        logs.push(line);
      }
      const markerIndex = line.indexOf("MEMTEST_RESULT:");
      if (markerIndex >= 0) {
        const payload = line.slice(markerIndex + "MEMTEST_RESULT:".length).trim();
        try {
          parsedResult = JSON.parse(payload);
        } catch (_) {
          parsedResult = { pass: false, error: `Invalid MEMTEST_RESULT payload: ${payload}` };
        }
      }
      if (line.includes("MEMTEST_RSS_BASELINE")) {
        baselineMarkerMs = Date.now() - startedAt;
      }
    };

    const stdoutRl = readline.createInterface({ input: child.stdout });
    const stderrRl = readline.createInterface({ input: child.stderr });
    stdoutRl.on("line", addLine);
    stderrRl.on("line", addLine);

    let samplerStopped = false;
    let samplerTimer = null;

    const queueSample = (delayMs) => {
      if (samplerStopped) {
        return;
      }

      samplerTimer = setTimeout(async () => {
        samplerTimer = null;

        if (samplerStopped) {
          return;
        }

        if (child.pid) {
          const rssKB = await sampleRssKB(child.pid);
          if (rssKB != null) {
            rssSamples.push({
              t: Date.now() - startedAt,
              rssKB,
            });
          }
        }

        queueSample(200);
      }, delayMs);
    };

    child.on("spawn", () => {
      queueSample(200);
    });

    const killer = setTimeout(() => {
      timedOut = true;
      child.kill("SIGKILL");
    }, timeoutMs);

    child.on("close", (code, signal) => {
      samplerStopped = true;
      if (samplerTimer != null) {
        clearTimeout(samplerTimer);
      }
      clearTimeout(killer);
      stdoutRl.close();
      stderrRl.close();

      const baselineKB = computeBaselineKB(rssSamples, baselineMarkerMs);
      const peakKB = rssSamples.length ? rssSamples.reduce((acc, s) => Math.max(acc, s.rssKB), 0) : null;
      const endKB = rssSamples.length ? median(rssSamples.slice(-3).map((sample) => sample.rssKB)) : null;
      const driftKB = baselineKB != null && endKB != null ? endKB - baselineKB : null;
      const peakDeltaKB = baselineKB != null && peakKB != null ? peakKB - baselineKB : null;

      const logicalName = parsedResult && parsedResult.name
        ? parsedResult.name
        : path.basename(testFile, ".js")
          .replace(/^test_/, "")
          .replace(/_/g, "-");

      const engine = parsedResult && parsedResult.engine
        ? parsedResult.engine
        : parsedResult && parsedResult.details && parsedResult.details.engine
          ? parsedResult.details.engine
          : "unknown";
      const engineThresholds = engineMemoryThresholdsKB[engine];
      const driftThresholdKB =
        (engineThresholds && engineThresholds[logicalName]) ??
        memoryThresholdsKB[logicalName] ??
        (80 * 1024);
      const memoryPass = driftKB == null ? false : driftKB <= driftThresholdKB;
      const logicPass = !!(parsedResult && parsedResult.pass === true);
      const exitPass = code === 0 && !timedOut && !signal;

      resolve({
        testFile,
        testName: logicalName,
        engine,
        code,
        signal,
        timedOut,
        durationMs: Date.now() - startedAt,
        logicPass,
        exitPass,
        memoryPass,
        pass: logicPass && exitPass && memoryPass,
        parsedResult,
        rss: {
          baselineKB,
          peakKB,
          endKB,
          driftKB,
          peakDeltaKB,
          driftThresholdKB,
          baselineMarkerMs,
          samples: rssSamples,
        },
        logs,
      });
    });
  });
}

function printRunSummary(run) {
  const status = run.pass ? "PASS" : "FAIL";
  const driftMB = run.rss.driftKB == null ? "n/a" : (run.rss.driftKB / 1024).toFixed(1);
  const peakDeltaMB = run.rss.peakDeltaKB == null ? "n/a" : (run.rss.peakDeltaKB / 1024).toFixed(1);
  console.log(
    `[${status}] ${run.testName} (${path.basename(run.testFile)}) ` +
      `duration=${run.durationMs}ms drift=${driftMB}MB peakDelta=${peakDeltaMB}MB`,
  );
  if (!run.pass) {
    if (!run.logicPass) {
      const reason = run.parsedResult && (run.parsedResult.error || run.parsedResult.reason);
      console.log(`  logic: FAIL ${reason || ""}`.trim());
    }
    if (!run.exitPass) {
      console.log(`  exit: FAIL code=${run.code} signal=${run.signal || "none"} timeout=${run.timedOut}`);
    }
    if (!run.memoryPass) {
      console.log(
        `  memory: FAIL driftKB=${run.rss.driftKB} thresholdKB=${run.rss.driftThresholdKB}`,
      );
    }
  }
}

async function main() {
  const opts = parseArgs(process.argv);
  const repoRoot = path.resolve(__dirname, "..", "..", "..", "..", "..");
  const memoryDir = path.resolve(__dirname);
  const nsrPath = opts.runtime
    ? path.resolve(repoRoot, opts.runtime)
    : path.join(repoRoot, "dist", "nsr");

  if (!fs.existsSync(nsrPath)) {
    console.error(`Missing runtime binary: ${nsrPath}`);
    process.exit(1);
  }
  ensureExecutableSignature(nsrPath);

  const testFiles = listTestFiles(memoryDir, opts.grep, opts.excludeWindowTests);
  if (testFiles.length === 0) {
    console.error("No memory tests found.");
    process.exit(1);
  }

  const allRuns = [];
  for (let pass = 1; pass <= opts.repeat; pass++) {
    console.log(`\n=== Memory Test Pass ${pass}/${opts.repeat} ===`);
    for (const testFile of testFiles) {
      const run = await runSingleTest({
        nsrPath,
        cwd: repoRoot,
        testFile,
        timeoutMs: opts.timeoutMs,
      });
      run.passIndex = pass;
      allRuns.push(run);
      printRunSummary(run);
    }
  }

  const total = allRuns.length;
  const passed = allRuns.filter((r) => r.pass).length;
  const failed = total - passed;

  const report = {
    generatedAt: new Date().toISOString(),
    options: opts,
    totals: { total, passed, failed },
    runs: allRuns,
  };

  const resultsDir = path.join(repoRoot, "build", "test-results");
  fs.mkdirSync(resultsDir, { recursive: true });
  const reportPath = path.join(resultsDir, "memory-cli-report.json");
  fs.writeFileSync(reportPath, JSON.stringify(report, null, 2));

  console.log(`\nSummary: ${passed}/${total} passed`);
  console.log(`Report: ${reportPath}`);

  process.exit(failed === 0 ? 0 : 2);
}

if (require.main === module) {
  main().catch((error) => {
    console.error(error && error.stack ? error.stack : error);
    process.exit(1);
  });
}

module.exports = {
  parseArgs,
  runSingleTest,
  sampleRssKB,
  printRunSummary,
  ensureExecutableSignature,
  main,
};
