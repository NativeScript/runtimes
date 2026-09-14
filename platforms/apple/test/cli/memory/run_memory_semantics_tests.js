"use strict";

const fs = require("fs");
const path = require("path");
const { execFile } = require("child_process");
const {
  parseArgs,
  ensureExecutableSignature,
} = require("./run_memory_tests");

const semanticsTests = [
  "test_weakref_finalization.js",
  "test_js_function_finalization.js",
  "test_weakref_plain_script.js",
  "test_objc_ownership_rules.js",
  "test_objc_unmanaged_transfer_semantics.js",
  "test_objc_wrapper_finalization.js",
  "test_pointer_c_buffer_semantics.js",
  "test_reference_lifecycle.js",
  "test_block_completion_safety.js",
  "test_block_callback_finalization.js",
  "test_c_function_pointer_semantics.js",
  "test_nested_ffi_layout_lifecycle.js",
  "test_circular_native_wrapper_finalization.js",
  "test_circular_js_to_native_conversion.js",
  "test_selector_group_finalization.js",
];

function resolveSemanticsTests(memoryDir, grep) {
  return semanticsTests
    .filter((name) => !grep || name.includes(grep))
    .map((name) => path.join(memoryDir, name));
}

function parseMemtestResult(output) {
  const lines = String(output || "").split(/\r?\n/);
  let parsedResult = null;

  for (const line of lines) {
    const markerIndex = line.indexOf("MEMTEST_RESULT:");
    if (markerIndex < 0) {
      continue;
    }

    const payload = line.slice(markerIndex + "MEMTEST_RESULT:".length).trim();
    try {
      parsedResult = JSON.parse(payload);
    } catch (_) {
      parsedResult = {
        pass: false,
        error: `Invalid MEMTEST_RESULT payload: ${payload}`,
      };
    }
  }

  return parsedResult;
}

function runSemanticsTest({ nsrPath, cwd, testFile, timeoutMs }) {
  return new Promise((resolve) => {
    const startedAt = Date.now();

    execFile(
      nsrPath,
      ["run", testFile],
      {
        cwd,
        timeout: timeoutMs,
        maxBuffer: 2 * 1024 * 1024,
        encoding: "utf8",
      },
      (error, stdout, stderr) => {
        const combinedOutput = `${stdout || ""}${stderr || ""}`;
        const parsedResult = parseMemtestResult(combinedOutput);
        const lines = combinedOutput
          .split(/\r?\n/)
          .map((line) => line.trimEnd())
          .filter(Boolean)
          .slice(-200);

        const timedOut = !!(error && error.killed && error.signal === "SIGTERM");
        const code = error && typeof error.code === "number" ? error.code : 0;
        const signal = error && error.signal ? error.signal : null;
        const logicPass = !!(parsedResult && parsedResult.pass === true);
        const exitPass = !error;

        resolve({
          testFile,
          testName: parsedResult && parsedResult.name
            ? parsedResult.name
            : path.basename(testFile, ".js"),
          code,
          signal,
          timedOut,
          durationMs: Date.now() - startedAt,
          logicPass,
          exitPass,
          pass: logicPass && exitPass,
          parsedResult,
          logs: lines,
        });
      },
    );
  });
}

function printSemanticsRunSummary(run) {
  const status = run.pass ? "PASS" : "FAIL";
  console.log(
    `[${status}] ${run.testName} (${path.basename(run.testFile)}) duration=${run.durationMs}ms`,
  );

  if (!run.pass) {
    if (!run.logicPass) {
      const reason = run.parsedResult && (run.parsedResult.error || run.parsedResult.reason);
      console.log(`  logic: FAIL ${reason || ""}`.trim());
    }
    if (!run.exitPass) {
      console.log(`  exit: FAIL code=${run.code} signal=${run.signal || "none"} timeout=${run.timedOut}`);
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

  const testFiles = resolveSemanticsTests(memoryDir, opts.grep);
  if (testFiles.length === 0) {
    console.error("No memory semantics tests found.");
    process.exit(1);
  }

  const allRuns = [];
  for (let pass = 1; pass <= opts.repeat; pass++) {
    console.log(`\n=== Memory Semantics Pass ${pass}/${opts.repeat} ===`);
    for (const testFile of testFiles) {
      const run = await runSemanticsTest({
        nsrPath,
        cwd: repoRoot,
        testFile,
        timeoutMs: opts.timeoutMs,
      });
      run.passIndex = pass;
      allRuns.push(run);
      printSemanticsRunSummary(run);
    }
  }

  const total = allRuns.length;
  const passed = allRuns.filter((run) => run.pass).length;
  const failed = total - passed;

  const report = {
    generatedAt: new Date().toISOString(),
    options: opts,
    suite: "memory-semantics",
    totals: { total, passed, failed },
    runs: allRuns,
  };

  const resultsDir = path.join(repoRoot, "build", "test-results");
  fs.mkdirSync(resultsDir, { recursive: true });
  const reportPath = path.join(resultsDir, "memory-cli-semantics-report.json");
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
  resolveSemanticsTests,
  main,
};
