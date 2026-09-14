#!/usr/bin/env node
"use strict";

const childProcess = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { pathToFileURL } = require("url");

const repoRoot = path.resolve(__dirname, "../../../..");
const benchmarkFile = path.join(__dirname, "objc-dispatch-benchmarks.js");
const marker = "NS_BENCH_RESULT:";
const defaultLegacyRepo = process.env.NS_LEGACY_IOS_REPO || "";
const defaultMetadataPath = path.join(
  repoRoot,
  "build/derived-data/macos-tests/Build/Products/Debug/metadata-arm64.bin"
);
const defaultWorkRoot = path.join(repoRoot, "build/benchmarks/objc-dispatch");

function parseArgs(argv) {
  const args = {
    runtime: "napi-node",
    iterations: 250000,
    warmupIterations: undefined,
    includeGsdOff: false,
    includeLegacyAotOff: false,
    legacyRepo: defaultLegacyRepo,
    metadataPath: process.env.METADATA_PATH || defaultMetadataPath,
    destination: process.env.IOS_DESTINATION || "",
    workRoot: defaultWorkRoot,
    timeoutMs: 120000,
    buildTimeoutMs: 15 * 60 * 1000,
    packageTgz: "",
    variantLabel: "",
    skipBuild: false,
    compareResults: ""
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => argv[++i];

    if (arg === "--runtime") args.runtime = next();
    else if (arg.startsWith("--runtime=")) args.runtime = arg.slice("--runtime=".length);
    else if (arg === "--iterations") args.iterations = Number(next());
    else if (arg.startsWith("--iterations=")) args.iterations = Number(arg.slice("--iterations=".length));
    else if (arg === "--warmup") args.warmupIterations = Number(next());
    else if (arg.startsWith("--warmup=")) args.warmupIterations = Number(arg.slice("--warmup=".length));
    else if (arg === "--legacy-repo") args.legacyRepo = path.resolve(next());
    else if (arg.startsWith("--legacy-repo=")) args.legacyRepo = path.resolve(arg.slice("--legacy-repo=".length));
    else if (arg === "--metadata-path") args.metadataPath = path.resolve(next());
    else if (arg.startsWith("--metadata-path=")) args.metadataPath = path.resolve(arg.slice("--metadata-path=".length));
    else if (arg === "--destination") args.destination = next();
    else if (arg.startsWith("--destination=")) args.destination = arg.slice("--destination=".length);
    else if (arg === "--work-root") args.workRoot = path.resolve(next());
    else if (arg.startsWith("--work-root=")) args.workRoot = path.resolve(arg.slice("--work-root=".length));
    else if (arg === "--timeout-ms") args.timeoutMs = Number(next());
    else if (arg.startsWith("--timeout-ms=")) args.timeoutMs = Number(arg.slice("--timeout-ms=".length));
    else if (arg === "--build-timeout-ms") args.buildTimeoutMs = Number(next());
    else if (arg.startsWith("--build-timeout-ms=")) args.buildTimeoutMs = Number(arg.slice("--build-timeout-ms=".length));
    else if (arg === "--package-tgz") args.packageTgz = path.resolve(next());
    else if (arg.startsWith("--package-tgz=")) args.packageTgz = path.resolve(arg.slice("--package-tgz=".length));
    else if (arg === "--variant-label") args.variantLabel = next();
    else if (arg.startsWith("--variant-label=")) args.variantLabel = arg.slice("--variant-label=".length);
    else if (arg === "--include-gsd-off") args.includeGsdOff = true;
    else if (arg === "--include-legacy-aot-off") args.includeLegacyAotOff = true;
    else if (arg === "--skip-build") args.skipBuild = true;
    else if (arg === "--compare-results") args.compareResults = path.resolve(next());
    else if (arg.startsWith("--compare-results=")) args.compareResults = path.resolve(arg.slice("--compare-results=".length));
    else if (arg === "--help" || arg === "-h") {
      printUsage();
      process.exit(0);
    } else {
      throw new Error(`Unknown argument: ${arg}`);
    }
  }

  if (!Number.isFinite(args.iterations) || args.iterations <= 0) {
    throw new Error("--iterations must be a positive number");
  }
  if (args.warmupIterations !== undefined &&
      (!Number.isFinite(args.warmupIterations) || args.warmupIterations < 0)) {
    throw new Error("--warmup must be a non-negative number");
  }

  return args;
}

function printUsage() {
  console.log(`Usage: node platforms/apple/benchmarks/objc-dispatch/run.js [options]

Options:
  --runtime all|napi-node|ios-package|legacy-ios
  --iterations N
  --warmup N
  --legacy-repo PATH          Legacy iOS checkout (or NS_LEGACY_IOS_REPO)
  --metadata-path PATH        Used by napi-node. Default: ${defaultMetadataPath}
  --destination DEST_OR_UDID  iOS simulator destination or UDID
  --package-tgz PATH          @nativescript/ios* package tgz for iOS package benchmarks
  --variant-label LABEL       Report iOS package results as this engine/backend label
  --include-gsd-off           Also run iOS packages with generated signature dispatch disabled
  --include-legacy-aot-off    Also run legacy iOS V8 with AOT disabled
  --skip-build                Reuse existing derived-data app builds
  --compare-results PATH      Print report and comparison tables from a saved result JSON
`);
}

function run(command, args, options = {}) {
  const result = childProcess.spawnSync(command, args, {
    encoding: "utf8",
    maxBuffer: 128 * 1024 * 1024,
    ...options
  });

  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    const details = [result.stdout, result.stderr].filter(Boolean).join("\n");
    throw new Error(`Command failed (${result.status}): ${command} ${args.join(" ")}\n${details}`);
  }
  return result;
}

function runInherited(command, args, options = {}) {
  const result = childProcess.spawnSync(command, args, {
    stdio: "inherit",
    ...options
  });
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(`Command failed (${result.status}): ${command} ${args.join(" ")}`);
  }
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function rmrf(target) {
  fs.rmSync(target, { recursive: true, force: true });
}

function copyDirectoryContents(sourceDir, destDir) {
  rmrf(destDir);
  ensureDir(destDir);
  fs.cpSync(sourceDir, destDir, { recursive: true });
}

function writeJsonRunner(targetPath, runtime, variant, options) {
  const payload = JSON.stringify({
    iterations: options.iterations,
    warmupIterations: options.warmupIterations
  });
  fs.writeFileSync(
    targetPath,
    [
      `global.__NS_BENCHMARK_RUNTIME = ${JSON.stringify(runtime)};`,
      `global.__NS_BENCHMARK_VARIANT = ${JSON.stringify(variant)};`,
      `global.__NS_BENCHMARK_OPTIONS__ = ${payload};`,
      `require("./${path.basename(benchmarkFile)}");`,
      ""
    ].join("\n")
  );
}

function parseBenchmarkOutput(output) {
  let position = 0;
  const results = [];
  const skipped = [];
  let done = null;
  let sawMarker = false;

  while (position < output.length) {
    const index = output.indexOf(marker, position);
    if (index === -1) {
      break;
    }
    sawMarker = true;
    const parsed = parseJsonAfterMarker(output, index);
    position = parsed.nextPosition;
    if (!parsed.value) {
      continue;
    }

    const value = parsed.value;
    if (value.kind === "case") {
      results.push({
        name: value.name,
        iterations: value.iterations,
        ms: value.ms,
        nsPerOp: value.nsPerOp
      });
    } else if (value.kind === "skip") {
      skipped.push({ name: value.name, error: value.error });
    } else if (value.kind === "done") {
      done = value;
    } else if (value.results) {
      return value;
    }
  }

  if (done) {
    return {
      version: done.version,
      runtime: done.runtime,
      variant: done.variant,
      baseIterations: done.baseIterations,
      warmupIterations: done.warmupIterations,
      totalMs: done.totalMs,
      sink: done.sink,
      results,
      skipped
    };
  }

  if (!sawMarker) {
    throw new Error(`Benchmark marker not found in output:\n${output.slice(-4000)}`);
  }

  throw new Error(`Benchmark done marker not found in output:\n${output.slice(-4000)}`);
}

function parseJsonAfterMarker(output, index) {
  const afterMarker = output.slice(index + marker.length);
  const jsonStart = afterMarker.indexOf("{");
  if (jsonStart === -1) {
    return { value: null, nextPosition: index + marker.length };
  }

  let depth = 0;
  let inString = false;
  let escaped = false;
  for (let i = jsonStart; i < afterMarker.length; i++) {
    const ch = afterMarker[i];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (ch === "\\") {
        escaped = true;
      } else if (ch === "\"") {
        inString = false;
      }
      continue;
    }

    if (ch === "\"") {
      inString = true;
    } else if (ch === "{") {
      depth++;
    } else if (ch === "}") {
      depth--;
      if (depth === 0) {
        try {
          return {
            value: JSON.parse(afterMarker.slice(jsonStart, i + 1)),
            nextPosition: index + marker.length + i + 1
          };
        } catch (_) {
          return { value: null, nextPosition: index + marker.length + i + 1 };
        }
      }
    }
  }

  return { value: null, nextPosition: index + marker.length };
}

function printReport(report) {
  console.log(`\n${report.runtime} (${report.variant})`);
  console.log("case".padEnd(40) + "ops".padStart(12) + "ms".padStart(12) + "ns/op".padStart(14));
  for (const result of report.results) {
    console.log(
      result.name.padEnd(40) +
        String(result.iterations).padStart(12) +
        result.ms.toFixed(2).padStart(12) +
        result.nsPerOp.toFixed(1).padStart(14)
    );
  }
  if (report.skipped && report.skipped.length > 0) {
    console.log("skipped: " + report.skipped.map((item) => item.name).join(", "));
  }
}

function reportLabel(report) {
  return `${report.runtime} (${report.variant})`;
}

function labeledPackageVariant(options, variant) {
  return options.variantLabel ? `${options.variantLabel} ${variant}` : variant;
}

function gsdVariantGroup(variant) {
  const match = String(variant).match(/^(?:(.*)\s+)?(gsd-on|gsd-off)$/);
  if (!match) {
    return null;
  }
  return {
    label: match[1] || "",
    kind: match[2]
  };
}

function resultMap(report) {
  return new Map(report.results.map((result) => [result.name, result]));
}

function formatSigned(value, digits = 2) {
  const fixed = Math.abs(value).toFixed(digits);
  if (value > 0) {
    return `+${fixed}`;
  }
  if (value < 0) {
    return `-${fixed}`;
  }
  return fixed;
}

function formatPercent(value) {
  return `${formatSigned(value, 1)}%`;
}

function comparisonWinner(deltaNsPerOp) {
  if (Math.abs(deltaNsPerOp) < 0.05) {
    return "tie";
  }
  return deltaNsPerOp < 0 ? "comparison" : "baseline";
}

function printTotalsComparison(reports, baseline) {
  console.log(`\nTotal comparison, baseline ${reportLabel(baseline)}`);
  console.log("| runtime | total ms | delta ms | ratio | relative |");
  console.log("|---|---:|---:|---:|---:|");
  for (const report of reports) {
    const deltaMs = report.totalMs - baseline.totalMs;
    const ratio = report.totalMs / baseline.totalMs;
    const relative = (baseline.totalMs / report.totalMs) * 100;
    console.log(
      `| ${reportLabel(report)} | ${report.totalMs.toFixed(2)} | ${formatSigned(deltaMs)} | ${ratio.toFixed(2)}x | ${relative.toFixed(1)}% |`
    );
  }
}

function printPairComparison(baseline, comparison) {
  const baselineResults = resultMap(baseline);
  const comparisonResults = resultMap(comparison);
  console.log(`\n${reportLabel(comparison)} vs ${reportLabel(baseline)}`);
  console.log("| case | baseline ms | comparison ms | delta ms | baseline ns/op | comparison ns/op | delta ns/op | delta % | winner |");
  console.log("|---|---:|---:|---:|---:|---:|---:|---:|---|");
  for (const baselineResult of baseline.results) {
    const comparisonResult = comparisonResults.get(baselineResult.name);
    if (!comparisonResult) {
      continue;
    }
    const deltaMs = comparisonResult.ms - baselineResult.ms;
    const deltaNsPerOp = comparisonResult.nsPerOp - baselineResult.nsPerOp;
    const deltaPercent = (deltaNsPerOp / baselineResult.nsPerOp) * 100;
    console.log(
      `| ${baselineResult.name} | ${baselineResult.ms.toFixed(2)} | ${comparisonResult.ms.toFixed(2)} | ${formatSigned(deltaMs)} | ${baselineResult.nsPerOp.toFixed(1)} | ${comparisonResult.nsPerOp.toFixed(1)} | ${formatSigned(deltaNsPerOp, 1)} | ${formatPercent(deltaPercent)} | ${comparisonWinner(deltaNsPerOp)} |`
    );
  }
}

function printComparisons(reports) {
  if (!Array.isArray(reports) || reports.length < 2) {
    return;
  }

  const baseline = reports[0];
  printTotalsComparison(reports, baseline);

  const gsdGroups = new Map();
  for (const report of reports) {
    const group = gsdVariantGroup(report.variant);
    if (!group) {
      continue;
    }
    const key = group.label || report.runtime;
    if (!gsdGroups.has(key)) {
      gsdGroups.set(key, new Map());
    }
    gsdGroups.get(key).set(group.kind, report);
  }

  let firstGsdOn = null;
  for (const group of gsdGroups.values()) {
    const gsdOn = group.get("gsd-on");
    const gsdOff = group.get("gsd-off");
    if (gsdOn && !firstGsdOn) {
      firstGsdOn = gsdOn;
    }
    if (gsdOn && gsdOff) {
      printPairComparison(gsdOn, gsdOff);
    }
  }

  const legacyAotOn = reports.find((report) => report.runtime === "legacy-ios" && report.variant === "aot-on");
  if (firstGsdOn && legacyAotOn) {
    printPairComparison(firstGsdOn, legacyAotOn);
  }

  const legacyAotOff = reports.find((report) => report.runtime === "legacy-ios" && report.variant === "aot-off");
  if (firstGsdOn && legacyAotOff) {
    printPairComparison(firstGsdOn, legacyAotOff);
  }
}

function printSavedResultsComparison(resultsPath) {
  const parsed = JSON.parse(fs.readFileSync(resultsPath, "utf8"));
  const reports = parsed.reports || [];
  for (const report of reports) {
    printReport(report);
  }
  printComparisons(reports);
}

function runNapiNode(options, variant) {
  if (!fs.existsSync(options.metadataPath)) {
    throw new Error(`Metadata file not found: ${options.metadataPath}`);
  }

  const runnerDir = path.join(options.workRoot, "node");
  ensureDir(runnerDir);
  fs.copyFileSync(benchmarkFile, path.join(runnerDir, path.basename(benchmarkFile)));

  const runnerPath = path.join(runnerDir, `run-${variant}.cjs`);
  const benchmarkOptions = {
    iterations: options.iterations,
    warmupIterations: options.warmupIterations
  };
  fs.writeFileSync(
    runnerPath,
    [
      `global.__NS_BENCHMARK_RUNTIME = "napi-node";`,
      `global.__NS_BENCHMARK_VARIANT = ${JSON.stringify(variant)};`,
      `global.__NS_BENCHMARK_OPTIONS__ = ${JSON.stringify(benchmarkOptions)};`,
      `import(${JSON.stringify(pathToFileUrl(path.join(repoRoot, "packages/macos-node-api/index.mjs")))}).then(() => {`,
      `  require(${JSON.stringify(path.join(runnerDir, path.basename(benchmarkFile)))});`,
      `}).catch((error) => { console.error(error && error.stack || error); process.exit(1); });`,
      ""
    ].join("\n")
  );

  const env = { ...process.env, METADATA_PATH: options.metadataPath };
  if (variant === "gsd-off") {
    env.NS_DISABLE_GSD = "1";
  } else {
    delete env.NS_DISABLE_GSD;
  }

  const result = run(process.execPath, [runnerPath], { cwd: repoRoot, env, timeout: options.timeoutMs });
  return parseBenchmarkOutput(result.stdout + result.stderr);
}

function pathToFileUrl(filePath) {
  return pathToFileURL(filePath).href;
}

function destinationToUdid(destination) {
  if (!destination) {
    return "";
  }
  const idMatch = destination.match(/id=([0-9A-Fa-f-]{36})/);
  if (idMatch) {
    return idMatch[1];
  }
  if (/^[0-9A-Fa-f-]{36}$/.test(destination)) {
    return destination;
  }
  return "";
}

function pickSimulator(destination) {
  const explicit = destinationToUdid(destination);
  if (explicit) {
    return explicit;
  }

  const result = run("xcrun", ["simctl", "list", "devices", "available", "--json"]);
  const parsed = JSON.parse(result.stdout);
  const devices = [];
  for (const runtimeName of Object.keys(parsed.devices || {})) {
    for (const device of parsed.devices[runtimeName]) {
      if (device.isAvailable && /iPhone/.test(device.name)) {
        devices.push(device);
      }
    }
  }

  const booted = devices.find((device) => device.state === "Booted");
  const preferred = booted || devices.find((device) => /Pro/.test(device.name)) || devices[0];
  if (!preferred) {
    throw new Error("No available iPhone simulator found");
  }
  return preferred.udid;
}

function bootSimulator(udid) {
  const boot = childProcess.spawnSync("xcrun", ["simctl", "boot", udid], { encoding: "utf8" });
  if (boot.status !== 0 && !/Unable to boot device in current state: Booted/.test(boot.stderr || "")) {
    throw new Error(`Unable to boot simulator ${udid}:\n${boot.stderr || boot.stdout}`);
  }
  runInherited("xcrun", ["simctl", "bootstatus", udid, "-b"], { timeout: 180000 });
}

function findBuiltApp(derivedDataPath, appName) {
  const productsRoot = path.join(derivedDataPath, "Build/Products");
  const queue = [productsRoot];
  while (queue.length > 0) {
    const current = queue.pop();
    if (!fs.existsSync(current)) {
      continue;
    }
    const stats = fs.statSync(current);
    if (stats.isDirectory() && path.basename(current) === `${appName}.app`) {
      return current;
    }
    if (stats.isDirectory()) {
      for (const entry of fs.readdirSync(current)) {
        queue.push(path.join(current, entry));
      }
    }
  }
  throw new Error(`Built app not found under ${productsRoot}`);
}

function launchAndCollect(udid, bundleId, options, env = {}) {
  return new Promise((resolve, reject) => {
    let output = "";
    let settled = false;
    const children = [];
    const launchEnv = { ...process.env };
    for (const [key, value] of Object.entries(env)) {
      launchEnv[`SIMCTL_CHILD_${key}`] = value;
    }

    const logChild = childProcess.spawn(
      "xcrun",
      [
        "simctl", "spawn", udid,
        "log", "stream",
        "--style", "compact",
        "--level", "debug",
        "--predicate", `eventMessage CONTAINS "${marker}"`
      ],
      { env: process.env }
    );
    children.push(logChild);

    const child = childProcess.spawn(
      "xcrun",
      ["simctl", "launch", "--terminate-running-process", udid, bundleId],
      { env: launchEnv }
    );
    children.push(child);

    const timeout = setTimeout(() => {
      if (settled) {
        return;
      }
      settled = true;
      for (const activeChild of children) {
        activeChild.kill("SIGTERM");
      }
      childProcess.spawnSync("xcrun", ["simctl", "terminate", udid, bundleId], { stdio: "ignore" });
      reject(new Error(`Timed out waiting for benchmark marker from ${bundleId}`));
    }, options.timeoutMs);

    function settleWithReport(report) {
      if (settled) {
        return;
      }
      settled = true;
      clearTimeout(timeout);
      for (const activeChild of children) {
        activeChild.kill("SIGTERM");
      }
      childProcess.spawnSync("xcrun", ["simctl", "terminate", udid, bundleId], { stdio: "ignore" });
      resolve(report);
    }

    function onData(data) {
      const text = data.toString();
      output += text;
      process.stdout.write(text);
      if (!settled && output.includes(marker)) {
        try {
          settleWithReport(parseBenchmarkOutput(output));
        } catch (_) {
          // log stream prints the predicate itself before app logs; wait for the
          // actual console message containing marker JSON.
        }
      }
    }

    logChild.stdout.on("data", onData);
    logChild.stderr.on("data", onData);
    child.stdout.on("data", onData);
    child.stderr.on("data", onData);
    for (const activeChild of children) {
      activeChild.on("error", (error) => {
        if (!settled) {
          settled = true;
          clearTimeout(timeout);
          reject(error);
        }
      });
    }
    child.on("exit", (code, signal) => {
      if (!settled) {
        if (output.includes(marker)) {
          try {
            settleWithReport(parseBenchmarkOutput(output));
            return;
          } catch (_) {
            // The unified log stream may still deliver the actual message after
            // simctl launch exits.
          }
        }
        if (code !== 0 || signal) {
          settled = true;
          clearTimeout(timeout);
          for (const activeChild of children) {
            activeChild.kill("SIGTERM");
          }
          childProcess.spawnSync("xcrun", ["simctl", "terminate", udid, bundleId], { stdio: "ignore" });
          reject(new Error(
            `simctl launch for ${bundleId} exited with code ${code ?? "unknown"}${signal ? ` (signal ${signal})` : ""} before emitting ${marker}.\n${output}`
          ));
        }
      }
    });
  });
}

function xcodebuild(args, cwd, timeoutMs) {
  runInherited("xcodebuild", args, {
    cwd,
    timeout: timeoutMs,
    env: {
      ...process.env,
      PATH: ["/opt/homebrew/bin", "/usr/local/bin", process.env.PATH || ""].join(":"),
      NSUnbufferedIO: "YES"
    }
  });
}

function installApp(udid, appPath, bundleId) {
  childProcess.spawnSync("xcrun", ["simctl", "terminate", udid, bundleId], { stdio: "ignore" });
  childProcess.spawnSync("xcrun", ["simctl", "uninstall", udid, bundleId], { stdio: "ignore" });
  runInherited("xcrun", ["simctl", "install", udid, appPath], { timeout: 120000 });
}

function readAppBundleId(appPath, fallback) {
  const plistPath = path.join(appPath, "Info.plist");
  if (!fs.existsSync(plistPath)) {
    return fallback;
  }
  const result = childProcess.spawnSync(
    "/usr/libexec/PlistBuddy",
    ["-c", "Print :CFBundleIdentifier", plistPath],
    { encoding: "utf8" }
  );
  if (result.status === 0 && result.stdout.trim()) {
    return result.stdout.trim();
  }
  return fallback;
}

async function runLegacyIOS(options, variant = "aot-on") {
  if (!options.legacyRepo) {
    throw new Error(
      "Legacy iOS benchmarks require --legacy-repo or NS_LEGACY_IOS_REPO"
    );
  }
  const appName = "TestRunner";
  let bundleId = "com.descendra.TestRunner";
  const appDir = path.join(options.legacyRepo, "TestRunner/app");
  const indexPath = path.join(appDir, "index.js");
  const copiedBenchmarkPath = path.join(appDir, path.basename(benchmarkFile));
  const originalIndex = fs.readFileSync(indexPath, "utf8");
  const derivedDataPath = path.join(options.workRoot, "derived-data/legacy-ios");
  const udid = pickSimulator(options.destination);

  ensureDir(path.dirname(copiedBenchmarkPath));
  fs.copyFileSync(benchmarkFile, copiedBenchmarkPath);
  writeJsonRunner(indexPath, "legacy-ios", variant, options);

  try {
    bootSimulator(udid);
    if (!options.skipBuild) {
      xcodebuild([
        "-project", "v8ios.xcodeproj",
        "-scheme", "TestRunner",
        "-configuration", "Release",
        "-destination", `platform=iOS Simulator,id=${udid}`,
        "-derivedDataPath", derivedDataPath,
        "CODE_SIGNING_ALLOWED=NO",
        "CODE_SIGNING_REQUIRED=NO",
        "CLANG_WARN_NULLABLE_TO_NONNULL_CONVERSION=NO",
        "CLANG_WARN_NULLABILITY_COMPLETENESS=NO",
        "OTHER_CPLUSPLUSFLAGS=-fno-rtti -Wall -Werror -Wno-documentation -Wno-deprecated-declarations -Wno-nullability-completeness -Wno-unknown-pragmas -Wno-unreachable-code -Wno-strict-prototypes -fembed-bitcode",
        "OTHER_CFLAGS=-fno-rtti -Wall -Werror -Wno-documentation -Wno-deprecated-declarations -Wno-nullability-completeness -Wno-unknown-pragmas -Wno-unreachable-code -Wno-strict-prototypes -fembed-bitcode",
        "build",
        "-quiet"
      ], options.legacyRepo, options.buildTimeoutMs);
    }
    let appPath;
    try {
      appPath = findBuiltApp(derivedDataPath, appName);
    } catch (_) {
      appPath = path.join(options.legacyRepo, "build/Release-iphonesimulator", `${appName}.app`);
      if (!fs.existsSync(appPath)) {
        throw _;
      }
    }
    if (options.skipBuild) {
      copyDirectoryContents(appDir, path.join(appPath, "app"));
    }
    bundleId = readAppBundleId(appPath, bundleId);
    installApp(udid, appPath, bundleId);
    const launchEnv = variant === "aot-off" ? { NS_DISABLE_AOT: "1" } : {};
    return await launchAndCollect(udid, bundleId, options, launchEnv);
  } finally {
    fs.writeFileSync(indexPath, originalIndex);
    rmrf(copiedBenchmarkPath);
  }
}

function findDefaultIOSPackage() {
  const distDir = path.join(repoRoot, "packages/ios/dist");
  const names = fs.readdirSync(distDir)
    .filter((name) => /^nativescript-ios-.*\.tgz$/.test(name))
    .sort();
  if (names.length === 0) {
    throw new Error(`No @nativescript/ios package tgz found in ${distDir}`);
  }
  return path.join(distDir, names[names.length - 1]);
}

function packageRuntimeLabel(options) {
  return options.variantLabel || "ios-package";
}

function replaceInTextFiles(root, search, replacement) {
  const queue = [root];
  while (queue.length > 0) {
    const current = queue.pop();
    const stats = fs.lstatSync(current);
    if (stats.isDirectory()) {
      for (const entry of fs.readdirSync(current)) {
        queue.push(path.join(current, entry));
      }
      continue;
    }
    if (!stats.isFile()) {
      continue;
    }
    const buffer = fs.readFileSync(current);
    if (buffer.includes(0)) {
      continue;
    }
    const text = buffer.toString("utf8");
    if (text.includes(search)) {
      fs.writeFileSync(current, text.split(search).join(replacement));
    }
  }
}

function renamePlaceholderPaths(root, search, replacement) {
  const entries = [];
  const queue = [root];
  while (queue.length > 0) {
    const current = queue.pop();
    entries.push(current);
    if (fs.lstatSync(current).isDirectory()) {
      for (const entry of fs.readdirSync(current)) {
        queue.push(path.join(current, entry));
      }
    }
  }

  entries.sort((a, b) => b.length - a.length);
  for (const current of entries) {
    const base = path.basename(current);
    if (!base.includes(search) || !fs.existsSync(current)) {
      continue;
    }
    fs.renameSync(current, path.join(path.dirname(current), base.split(search).join(replacement)));
  }
}

function scaffoldIOSPackageApp(options, variant, packageTgz, reportVariant = variant) {
  const appName = "NativeScriptDispatchBench";
  const bundleId = "org.nativescript.bench.dispatch.iospackage";
  const tgz = packageTgz || options.packageTgz || findDefaultIOSPackage();
  const root = path.join(options.workRoot, "apps", `ios-package-${variant}`);
  rmrf(root);
  ensureDir(root);
  run("tar", ["-xzf", tgz, "-C", root]);

  const frameworkRoot = path.join(root, "package/framework");
  const projectPath = path.join(frameworkRoot, `${appName}.xcodeproj`);
  const appSourceRoot = path.join(frameworkRoot, appName);

  fs.renameSync(
    path.join(frameworkRoot, "__PROJECT_NAME__.xcodeproj"),
    projectPath
  );
  fs.renameSync(
    path.join(frameworkRoot, "__PROJECT_NAME__"),
    appSourceRoot
  );

  for (const name of fs.readdirSync(appSourceRoot)) {
    if (name.includes("__PROJECT_NAME__")) {
      fs.renameSync(
        path.join(appSourceRoot, name),
        path.join(appSourceRoot, name.replaceAll("__PROJECT_NAME__", appName))
      );
    }
  }

  renamePlaceholderPaths(frameworkRoot, "__PROJECT_NAME__", appName);
  replaceInTextFiles(frameworkRoot, "__PROJECT_NAME__", appName);
  replaceInTextFiles(frameworkRoot, "config.LogToSystemConsole = isDebug;", "config.LogToSystemConsole = YES;");
  fs.writeFileSync(path.join(frameworkRoot, "plugins-debug.xcconfig"), "\n");
  fs.writeFileSync(path.join(frameworkRoot, "plugins-release.xcconfig"), "\n");
  writeInfoPlist(path.join(appSourceRoot, `${appName}-Info.plist`));

  const appDir = path.join(appSourceRoot, "app");
  ensureDir(appDir);
  fs.writeFileSync(path.join(appDir, "package.json"), JSON.stringify({ main: "index" }, null, 2) + "\n");
  fs.copyFileSync(benchmarkFile, path.join(appDir, path.basename(benchmarkFile)));
  writeJsonRunner(path.join(appDir, "index.js"), packageRuntimeLabel(options), reportVariant, options);

  const zipPath = path.join(frameworkRoot, "internal/XCFrameworks.zip");
  run("unzip", ["-q", "-o", zipPath, "-d", path.join(frameworkRoot, "internal")]);

  return { appName, bundleId, frameworkRoot, projectPath, appDir };
}

function writeInfoPlist(plistPath) {
  fs.writeFileSync(plistPath, `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleExecutable</key>
  <string>$(EXECUTABLE_NAME)</string>
  <key>CFBundleIdentifier</key>
  <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>$(PRODUCT_NAME)</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>LSRequiresIPhoneOS</key>
  <true/>
  <key>UILaunchStoryboardName</key>
  <string></string>
  <key>UISupportedInterfaceOrientations</key>
  <array>
    <string>UIInterfaceOrientationPortrait</string>
  </array>
</dict>
</plist>
`);
}

async function runIOSPackage(options, variant, packageTgz, reportVariant = variant) {
  const app = scaffoldIOSPackageApp(options, variant, packageTgz, reportVariant);
  const derivedDataPath = path.join(options.workRoot, `derived-data/ios-package-${variant}`);
  const udid = pickSimulator(options.destination);

  bootSimulator(udid);
  if (!options.skipBuild) {
    xcodebuild([
      "-project", app.projectPath,
      "-scheme", app.appName,
      "-configuration", "Release",
      "-destination", `platform=iOS Simulator,id=${udid}`,
      "-derivedDataPath", derivedDataPath,
      "CODE_SIGNING_ALLOWED=NO",
      "CODE_SIGNING_REQUIRED=NO",
      `PRODUCT_BUNDLE_IDENTIFIER=${app.bundleId}`,
      "ARCHS=arm64",
      "ONLY_ACTIVE_ARCH=YES",
      "EXCLUDED_ARCHS=",
      "build",
      "-quiet"
    ], app.frameworkRoot, options.buildTimeoutMs);
  }

  const appPath = findBuiltApp(derivedDataPath, app.appName);
  if (options.skipBuild) {
    copyDirectoryContents(app.appDir, path.join(appPath, "app"));
  }
  installApp(udid, appPath, app.bundleId);
  const launchEnv = variant === "gsd-off" ? { NS_DISABLE_GSD: "1" } : {};
  return await launchAndCollect(udid, app.bundleId, options, launchEnv);
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.compareResults) {
    printSavedResultsComparison(options.compareResults);
    return;
  }

  ensureDir(options.workRoot);

  const reports = [];
  const runtimes = options.runtime === "all"
    ? ["napi-node", "ios-package", "legacy-ios"]
    : options.runtime.split(",").map((item) => item.trim()).filter(Boolean);

  for (const runtime of runtimes) {
    if (runtime === "napi-node") {
      reports.push(runNapiNode(options, "gsd-on"));
      if (options.includeGsdOff) {
        reports.push(runNapiNode(options, "gsd-off"));
      }
    } else if (runtime === "ios-package") {
      reports.push(await runIOSPackage(options, "gsd-on", undefined, labeledPackageVariant(options, "gsd-on")));
      if (options.includeGsdOff) {
        reports.push(await runIOSPackage(options, "gsd-off", undefined, labeledPackageVariant(options, "gsd-off")));
      }
    } else if (runtime === "legacy-ios") {
      reports.push(await runLegacyIOS(options, "aot-on"));
      if (options.includeLegacyAotOff) {
        reports.push(await runLegacyIOS({ ...options, skipBuild: true }, "aot-off"));
      }
    } else {
      throw new Error(`Unknown runtime: ${runtime}`);
    }
  }

  for (const report of reports) {
    printReport(report);
  }
  printComparisons(reports);

  const outPath = path.join(options.workRoot, `results-${new Date().toISOString().replace(/[:.]/g, "-")}.json`);
  fs.writeFileSync(outPath, JSON.stringify({ createdAt: new Date().toISOString(), reports }, null, 2) + "\n");
  console.log(`\nWrote ${outPath}`);
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
