#!/usr/bin/env node
"use strict";

const childProcess = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const marker = "NS_BENCH_RESULT:";
const benchmarkFile = path.join(__dirname, "objc-dispatch-benchmarks.js");

function parseArgs(argv) {
  const options = {
    baselineBinary: "",
    baselineMetadata: "",
    candidateBinary: "",
    candidateMetadata: "",
    engine: "unknown",
    runs: 7,
    iterations: 250000,
    warmupIterations: 10000,
    aggregateThresholdPercent: 10,
    caseThresholdPercent: 30,
    caseThresholdNs: 100,
    output: "",
    timeoutMs: 120000,
  };

  for (let index = 0; index < argv.length; index++) {
    const arg = argv[index];
    const next = () => argv[++index];

    if (arg === "--baseline-binary") options.baselineBinary = path.resolve(next());
    else if (arg === "--baseline-metadata") options.baselineMetadata = path.resolve(next());
    else if (arg === "--candidate-binary") options.candidateBinary = path.resolve(next());
    else if (arg === "--candidate-metadata") options.candidateMetadata = path.resolve(next());
    else if (arg === "--engine") options.engine = next();
    else if (arg === "--runs") options.runs = Number(next());
    else if (arg === "--iterations") options.iterations = Number(next());
    else if (arg === "--warmup") options.warmupIterations = Number(next());
    else if (arg === "--aggregate-threshold-percent") options.aggregateThresholdPercent = Number(next());
    else if (arg === "--case-threshold-percent") options.caseThresholdPercent = Number(next());
    else if (arg === "--case-threshold-ns") options.caseThresholdNs = Number(next());
    else if (arg === "--output") options.output = path.resolve(next());
    else if (arg === "--timeout-ms") options.timeoutMs = Number(next());
    else if (arg === "--help" || arg === "-h") {
      printUsage();
      process.exit(0);
    } else {
      throw new Error(`Unknown argument: ${arg}`);
    }
  }

  for (const key of ["baselineBinary", "baselineMetadata", "candidateBinary", "candidateMetadata"]) {
    if (!options[key]) {
      throw new Error(`--${key.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)} is required`);
    }
    if (!fs.existsSync(options[key])) {
      throw new Error(`File not found: ${options[key]}`);
    }
  }

  for (const key of ["runs", "iterations", "timeoutMs"]) {
    if (!Number.isFinite(options[key]) || options[key] <= 0) {
      throw new Error(`${key} must be a positive number`);
    }
  }
  for (const key of ["warmupIterations", "aggregateThresholdPercent", "caseThresholdPercent", "caseThresholdNs"]) {
    if (!Number.isFinite(options[key]) || options[key] < 0) {
      throw new Error(`${key} must be a non-negative number`);
    }
  }

  return options;
}

function printUsage() {
  console.log(`Usage: node compare-cli.js [options]

Required:
  --baseline-binary PATH
  --baseline-metadata PATH
  --candidate-binary PATH
  --candidate-metadata PATH

Optional:
  --engine NAME                         Report label
  --runs N                             Paired runs (default: 7)
  --iterations N                       Base iterations per case (default: 250000)
  --warmup N                           Warmup iterations (default: 10000)
  --aggregate-threshold-percent N      Native-dispatch geometric mean limit (default: 10)
  --case-threshold-percent N           Per-case relative limit (default: 30)
  --case-threshold-ns N                Per-case absolute limit (default: 100)
  --output PATH                        Save the JSON report
`);
}

function parseJsonAfterMarker(output, markerIndex) {
  const afterMarker = output.slice(markerIndex + marker.length);
  const jsonStart = afterMarker.indexOf("{");
  if (jsonStart === -1) {
    return null;
  }

  let depth = 0;
  let inString = false;
  let escaped = false;
  for (let index = jsonStart; index < afterMarker.length; index++) {
    const character = afterMarker[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (character === "\\") escaped = true;
      else if (character === "\"") inString = false;
      continue;
    }

    if (character === "\"") inString = true;
    else if (character === "{") depth++;
    else if (character === "}") {
      depth--;
      if (depth === 0) {
        return {
          value: JSON.parse(afterMarker.slice(jsonStart, index + 1)),
          nextIndex: markerIndex + marker.length + index + 1,
        };
      }
    }
  }
  return null;
}

function parseBenchmarkOutput(output) {
  const results = [];
  let done = null;
  let position = 0;

  while (position < output.length) {
    const markerIndex = output.indexOf(marker, position);
    if (markerIndex === -1) break;
    const parsed = parseJsonAfterMarker(output, markerIndex);
    if (!parsed) break;
    position = parsed.nextIndex;
    if (parsed.value.kind === "case") results.push(parsed.value);
    else if (parsed.value.kind === "done") done = parsed.value;
  }

  if (!done || results.length === 0) {
    throw new Error(`Incomplete benchmark output:\n${output.slice(-4000)}`);
  }
  return results;
}

function median(values) {
  const sorted = [...values].sort((left, right) => left - right);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0
    ? (sorted[middle - 1] + sorted[middle]) / 2
    : sorted[middle];
}

function runBenchmark(binary, metadata, runnerPath, timeoutMs) {
  const result = childProcess.spawnSync(binary, ["run", runnerPath], {
    cwd: path.dirname(runnerPath),
    encoding: "utf8",
    timeout: timeoutMs,
    maxBuffer: 64 * 1024 * 1024,
    env: { ...process.env, NS_METADATA_PATH: metadata },
  });

  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(
      `Benchmark failed (${result.status}): ${binary}\n${result.stdout || ""}\n${result.stderr || ""}`,
    );
  }
  return parseBenchmarkOutput(`${result.stdout || ""}\n${result.stderr || ""}`);
}

function formatPercent(value) {
  const prefix = value > 0 ? "+" : "";
  return `${prefix}${value.toFixed(1)}%`;
}

function main() {
  const options = parseArgs(process.argv.slice(2));
  const workDir = fs.mkdtempSync(path.join(os.tmpdir(), "ns-objc-dispatch-"));

  try {
    fs.copyFileSync(benchmarkFile, path.join(workDir, path.basename(benchmarkFile)));
    const runnerPath = path.join(workDir, "run.js");
    fs.writeFileSync(
      runnerPath,
      [
        `globalThis.__NS_BENCHMARK_RUNTIME = ${JSON.stringify(`cli-${options.engine}`)};`,
        'globalThis.__NS_BENCHMARK_VARIANT = "performance-gate";',
        `globalThis.__NS_BENCHMARK_OPTIONS__ = ${JSON.stringify({
          iterations: options.iterations,
          warmupIterations: options.warmupIterations,
        })};`,
        `require("./${path.basename(benchmarkFile)}");`,
        "",
      ].join("\n"),
    );

    const samples = { baseline: new Map(), candidate: new Map() };
    const append = (variant, results) => {
      for (const result of results) {
        if (!samples[variant].has(result.name)) samples[variant].set(result.name, []);
        samples[variant].get(result.name).push(result.nsPerOp);
      }
    };

    for (let run = 0; run < options.runs; run++) {
      const order = run % 2 === 0 ? ["baseline", "candidate"] : ["candidate", "baseline"];
      for (const variant of order) {
        const binary = options[`${variant}Binary`];
        const metadata = options[`${variant}Metadata`];
        process.stdout.write(`Run ${run + 1}/${options.runs}: ${variant}\n`);
        append(variant, runBenchmark(binary, metadata, runnerPath, options.timeoutMs));
      }
    }

    const baselineNames = [...samples.baseline.keys()];
    const candidateNames = new Set(samples.candidate.keys());
    const missingCases = baselineNames.filter((name) => !candidateNames.has(name));
    if (missingCases.length > 0) {
      throw new Error(`Candidate did not report benchmark cases: ${missingCases.join(", ")}`);
    }

    const results = baselineNames.map((name) => {
      const baselineSamples = samples.baseline.get(name);
      const candidateSamples = samples.candidate.get(name);
      if (baselineSamples.length !== options.runs || candidateSamples.length !== options.runs) {
        throw new Error(`Expected ${options.runs} samples for ${name}`);
      }
      const baselineNsPerOp = median(baselineSamples);
      const candidateNsPerOp = median(candidateSamples);
      return {
        name,
        baselineNsPerOp,
        candidateNsPerOp,
        deltaPercent: ((candidateNsPerOp / baselineNsPerOp) - 1) * 100,
        baselineSamples,
        candidateSamples,
      };
    });

    const nativeResults = results.filter((result) => !result.name.startsWith("js."));
    if (nativeResults.length === 0) {
      throw new Error("No native-dispatch benchmark cases were reported");
    }
    const aggregateRatio = Math.exp(
      nativeResults.reduce(
        (sum, result) => sum + Math.log(result.candidateNsPerOp / result.baselineNsPerOp),
        0,
      ) / nativeResults.length,
    );
    const aggregateDeltaPercent = (aggregateRatio - 1) * 100;
    const regressedCases = nativeResults.filter(
      (result) =>
        result.deltaPercent > options.caseThresholdPercent &&
        result.candidateNsPerOp - result.baselineNsPerOp > options.caseThresholdNs,
    );
    const passed =
      aggregateDeltaPercent <= options.aggregateThresholdPercent &&
      regressedCases.length === 0;

    console.log(`\nObjective-C dispatch performance: ${options.engine}`);
    console.log("| case | baseline ns/op | candidate ns/op | delta |");
    console.log("|---|---:|---:|---:|");
    for (const result of results) {
      console.log(
        `| ${result.name} | ${result.baselineNsPerOp.toFixed(1)} | ${result.candidateNsPerOp.toFixed(1)} | ${formatPercent(result.deltaPercent)} |`,
      );
    }
    console.log(
      `\nNative-dispatch geometric mean: ${formatPercent(aggregateDeltaPercent)} ` +
      `(limit +${options.aggregateThresholdPercent.toFixed(1)}%).`,
    );

    const report = {
      version: 1,
      engine: options.engine,
      runs: options.runs,
      iterations: options.iterations,
      thresholds: {
        aggregatePercent: options.aggregateThresholdPercent,
        casePercent: options.caseThresholdPercent,
        caseNanoseconds: options.caseThresholdNs,
      },
      aggregateDeltaPercent,
      passed,
      results,
    };
    if (options.output) {
      fs.mkdirSync(path.dirname(options.output), { recursive: true });
      fs.writeFileSync(options.output, `${JSON.stringify(report, null, 2)}\n`);
    }

    if (!passed) {
      const details = regressedCases.length > 0
        ? ` Cases over the per-case limit: ${regressedCases.map((result) => result.name).join(", ")}.`
        : "";
      throw new Error(`Objective-C dispatch performance regressed.${details}`);
    }
  } finally {
    fs.rmSync(workDir, { recursive: true, force: true });
  }
}

main();
