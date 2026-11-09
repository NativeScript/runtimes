"use strict";

const fs = require("fs");
const path = require("path");
const readline = require("readline");
const { spawn } = require("child_process");

function parseArgs(argv) {
  const args = argv.slice(2);
  const parsed = {
    repeat: 5,
    scale: 1,
    label: "runtime",
    output: null,
  };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === "--repeat" && args[i + 1]) {
      parsed.repeat = Number(args[++i]);
      continue;
    }
    if (arg === "--label" && args[i + 1]) {
      parsed.label = String(args[++i]);
      continue;
    }
    if (arg === "--scale" && args[i + 1]) {
      parsed.scale = Number(args[++i]);
      continue;
    }
    if (arg === "--output" && args[i + 1]) {
      parsed.output = String(args[++i]);
    }
  }

  if (!Number.isFinite(parsed.repeat) || parsed.repeat <= 0) {
    parsed.repeat = 5;
  }
  if (!Number.isFinite(parsed.scale) || parsed.scale <= 0) {
    parsed.scale = 1;
  }

  return parsed;
}

function average(values) {
  if (!values.length) {
    return 0;
  }
  return values.reduce((acc, x) => acc + x, 0) / values.length;
}

function median(values) {
  if (!values.length) {
    return 0;
  }
  const sorted = values.slice().sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  if ((sorted.length & 1) === 1) {
    return sorted[mid];
  }
  return (sorted[mid - 1] + sorted[mid]) / 2;
}

async function runOnce(binaryPath, benchScriptPath, cwd, extraEnv = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(binaryPath, ["run", benchScriptPath], {
      cwd,
      env: { ...process.env, ...extraEnv },
      stdio: ["ignore", "pipe", "pipe"],
    });

    let parsed = null;
    const logs = [];
    const captureLine = (line) => {
      if (logs.length < 200) {
        logs.push(line);
      }
      const idx = line.indexOf("BENCH_RESULT:");
      if (idx >= 0) {
        const payload = line.slice(idx + "BENCH_RESULT:".length).trim();
        try {
          parsed = JSON.parse(payload);
        } catch (error) {
          reject(new Error(`Invalid BENCH_RESULT JSON from ${binaryPath}: ${error.message}`));
        }
      }
    };

    const stdoutRl = readline.createInterface({ input: child.stdout });
    const stderrRl = readline.createInterface({ input: child.stderr });
    stdoutRl.on("line", captureLine);
    stderrRl.on("line", captureLine);

    child.on("error", reject);
    child.on("close", (code, signal) => {
      stdoutRl.close();
      stderrRl.close();
      if (code !== 0 || signal) {
        reject(
          new Error(
            `Benchmark command failed for ${binaryPath} (code=${code}, signal=${signal || "none"})\n` +
              logs.join("\n"),
          ),
        );
        return;
      }
      if (!parsed) {
        reject(new Error(`Missing BENCH_RESULT output from ${binaryPath}`));
        return;
      }
      resolve(parsed);
    });
  });
}

function aggregateRuns(runs) {
  const byScenario = new Map();
  for (const run of runs) {
    for (const scenario of run.scenarios) {
      const list = byScenario.get(scenario.name) || [];
      list.push(scenario);
      byScenario.set(scenario.name, list);
    }
  }

  const aggregated = [];
  for (const [name, samples] of byScenario.entries()) {
    aggregated.push({
      name,
      iterations: samples[0].iterations,
      avgDurationMs: average(samples.map((s) => s.durationMs)),
      medDurationMs: median(samples.map((s) => s.durationMs)),
      avgPerCallUs: average(samples.map((s) => s.perCallUs)),
      medPerCallUs: median(samples.map((s) => s.perCallUs)),
    });
  }

  aggregated.sort((a, b) => a.name.localeCompare(b.name));
  return aggregated;
}

function formatFixed(value, digits) {
  return Number.isFinite(value) ? value.toFixed(digits) : "n/a";
}

function printComparison(labelA, dataA, labelB, dataB) {
  const mapA = new Map(dataA.map((s) => [s.name, s]));
  const mapB = new Map(dataB.map((s) => [s.name, s]));

  const names = new Set([...mapA.keys(), ...mapB.keys()]);
  const rows = Array.from(names).sort().map((name) => {
    const a = mapA.get(name);
    const b = mapB.get(name);
    const aUs = a ? a.medPerCallUs : NaN;
    const bUs = b ? b.medPerCallUs : NaN;
    const deltaPct = Number.isFinite(aUs) && Number.isFinite(bUs) && bUs > 0
      ? ((aUs - bUs) / bUs) * 100
      : NaN;
    return {
      name,
      aUs,
      bUs,
      deltaPct,
    };
  });

  console.log("\nPer-scenario median per-call latency (microseconds):");
  console.log(`  ${labelA} vs ${labelB}`);
  console.log("");
  console.log("scenario | " + labelA + " us | " + labelB + " us | delta% (" + labelA + " vs " + labelB + ")");
  console.log("---------|-------------|-------------|------------------------------");
  for (const row of rows) {
    console.log(
      `${row.name} | ${formatFixed(row.aUs, 3)} | ${formatFixed(row.bUs, 3)} | ${formatFixed(row.deltaPct, 2)}`,
    );
  }

  const validRows = rows.filter((row) => Number.isFinite(row.deltaPct));
  const avgDelta = average(validRows.map((row) => row.deltaPct));
  const medDelta = median(validRows.map((row) => row.deltaPct));

  console.log("");
  console.log(`aggregate delta (${labelA} vs ${labelB}): avg=${formatFixed(avgDelta, 2)}% median=${formatFixed(medDelta, 2)}%`);
}

async function main() {
  const opts = parseArgs(process.argv);
  const repoRoot = path.resolve(__dirname, "..", "..", "..", "..", "..");
  const benchScriptPath = path.resolve(__dirname, "foundation_calls.js");
  const runtimePath = path.join(repoRoot, "dist", "nsr");

  if (!fs.existsSync(runtimePath)) {
    throw new Error(`Missing runtime: ${runtimePath}`);
  }

  const gsdRuns = [];
  const nonGsdRuns = [];
  const benchmarkEnv = { NS_BENCH_SCALE: String(opts.scale) };

  for (let i = 0; i < opts.repeat; i++) {
    const iteration = i + 1;
    const runGsdFirst = (i & 1) === 0;
    if (runGsdFirst) {
      console.log(`Running iteration ${iteration}/${opts.repeat} with GSD runtime...`);
      gsdRuns.push(await runOnce(runtimePath, benchScriptPath, repoRoot, benchmarkEnv));
      console.log(`Running iteration ${iteration}/${opts.repeat} with non-GSD runtime...`);
      nonGsdRuns.push(
        await runOnce(runtimePath, benchScriptPath, repoRoot, {
          ...benchmarkEnv,
          NS_DISABLE_GSD: "1",
        }),
      );
    } else {
      console.log(`Running iteration ${iteration}/${opts.repeat} with non-GSD runtime...`);
      nonGsdRuns.push(
        await runOnce(runtimePath, benchScriptPath, repoRoot, {
          ...benchmarkEnv,
          NS_DISABLE_GSD: "1",
        }),
      );
      console.log(`Running iteration ${iteration}/${opts.repeat} with GSD runtime...`);
      gsdRuns.push(await runOnce(runtimePath, benchScriptPath, repoRoot, benchmarkEnv));
    }
  }

  const gsdAgg = aggregateRuns(gsdRuns);
  const nonGsdAgg = aggregateRuns(nonGsdRuns);

  printComparison("GSD", gsdAgg, "NO_GSD", nonGsdAgg);

  const report = {
    generatedAt: new Date().toISOString(),
    label: opts.label,
    repeat: opts.repeat,
    scale: opts.scale,
    gsd: gsdAgg,
    noGsd: nonGsdAgg,
  };
  console.log(`BENCH_SUMMARY:${JSON.stringify(report)}`);
  if (opts.output) {
    const outputPath = path.resolve(repoRoot, opts.output);
    fs.mkdirSync(path.dirname(outputPath), { recursive: true });
    fs.writeFileSync(outputPath, JSON.stringify(report, null, 2));
    console.log(`Report: ${outputPath}`);
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
