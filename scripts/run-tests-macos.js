// Runs macOS TestRunner.app and streams runtime logs directly.
// Optional args:
//  - junit output path (defaults to build/test-results/macos-junit.xml)
//
// Env:
//  - MACOS_TEST_SKIP_BUILD=1 skips xcodebuild app build.
//  - MACOS_TEST_CLEAN_BUILD=1 deletes derived data before build.
//  - MACOS_TEST_ENGINE selects the runtime engine build to use when runtime
//    artifacts need rebuilding. Supported: v8, hermes, quickjs, jsc. Defaults to v8.
//  - MACOS_TEST_FFI_BACKEND selects the FFI backend build to use when runtime
//    artifacts need rebuilding. Supported: auto, napi, v8, hermes, quickjs, jsc.
//    Defaults to auto.
//  - MACOS_TEST_GSD_BACKEND selects generated signature dispatch backend.
//    Supported: auto, v8, jsc, quickjs, hermes, napi, none. Defaults to auto.
//  - MACOS_COMMAND_TIMEOUT_MS overrides timeout for build commands (default: 10 minutes).
//  - MACOS_COMMAND_MAX_BUFFER_BYTES overrides spawnSync maxBuffer for captured command output (default: 64 MiB).
//  - MACOS_TEST_TIMEOUT_MS overrides max test runtime after launch (default: 2 minutes).
//  - MACOS_TEST_INACTIVITY_TIMEOUT_MS overrides max no-log interval after launch (default: 45 seconds).
//  - MACOS_LOG_JUNIT=0 disables streaming TKUnit/JUnit lines to console.
//  - MACOS_TESTS filters test modules (comma-separated substrings passed as -tests).
//  - MACOS_TEST_SPECS filters spec names (comma-separated substrings passed as -specs).
//  - MACOS_TEST_VERBOSE_SPECS=1 prints Jasmine spec start/done markers.

const fs = require("fs");
const path = require("path");
const cp = require("child_process");
const crypto = require("crypto");
const os = require("os");

const projectPath = path.join(__dirname, "../platforms/apple/NativeScriptRuntime.xcodeproj");
const scheme = "TestRunner";
const configuration = "Debug";
const derivedDataPath = path.join(__dirname, "../build", "derived-data", "macos-tests");
const testRunnerAppSourcePath = path.join(__dirname, "../platforms/apple/test/runtime/runner", "app");
const nativeScriptXCFramework = path.join(__dirname, "../dist", "NativeScript.xcframework");
const tkLiveSyncXCFramework = path.join(__dirname, "../dist", "TKLiveSync.xcframework");
const nativeScriptSourceRoot = path.join(__dirname, "../NativeScript");
const metadataGeneratorRoot = path.join(__dirname, "../metadata-generator");
const metadataGeneratorBinary = path.join(
    metadataGeneratorRoot,
    "dist",
    "arm64",
    "bin",
    "objc-metadata-generator"
);
const metadataGeneratorBuildStepScript = path.join(
    metadataGeneratorRoot,
    "dist",
    "arm64",
    "bin",
    "build-step-metadata-generator.py"
);
const metadataGeneratorSymbolAnalyzer = path.join(
    metadataGeneratorRoot,
    "dist",
    "arm64",
    "bin",
    "ns-metadata-symbols"
);
const metadataGeneratorSourceInputs = [
    path.join(metadataGeneratorRoot, "src"),
    path.join(metadataGeneratorRoot, "include"),
    path.join(metadataGeneratorRoot, "tests"),
    path.join(metadataGeneratorRoot, "symbol-analyzer", "Cargo.toml"),
    path.join(metadataGeneratorRoot, "symbol-analyzer", "Cargo.lock"),
    path.join(metadataGeneratorRoot, "symbol-analyzer", "src"),
    path.join(metadataGeneratorRoot, "symbol-analyzer", "tests"),
    path.join(metadataGeneratorRoot, "build-step-metadata-generator.py"),
    path.join(metadataGeneratorRoot, "CMakeLists.txt"),
    path.join(__dirname, "build_metadata_generator.sh")
];
const buildStatePath = path.join(derivedDataPath, ".macos-test-build-state.json");
const macosBuildInputs = [
    path.join(__dirname, "../platforms/apple/NativeScriptRuntime.xcodeproj", "project.pbxproj"),
    path.join(__dirname, "../platforms/apple/test/runtime/runner", "Source Files"),
    path.join(__dirname, "../platforms/apple/test/runtime/runner", "Info.plist"),
    path.join(__dirname, "../platforms/apple/test/runtime/fixtures"),
    path.join(__dirname, "../platforms/apple/TKLiveSync"),
    ...metadataGeneratorSourceInputs,
    nativeScriptXCFramework,
    tkLiveSyncXCFramework
];

const resultsDir = path.join(__dirname, "../build", "test-results");
const defaultJunitPath = path.join(resultsDir, "macos-junit.xml");

function parseTimeoutMs(name, fallback) {
    const value = Number(process.env[name] || fallback);
    if (!Number.isFinite(value) || value <= 0) {
        return fallback;
    }

    return value;
}

function parsePositiveInt(name, fallback) {
    const value = Number(process.env[name] || fallback);
    if (!Number.isFinite(value) || value <= 0) {
        return fallback;
    }

    return Math.floor(value);
}

const commandTimeoutMs = parseTimeoutMs("MACOS_COMMAND_TIMEOUT_MS", 10 * 60 * 1000);
const commandMaxBufferBytes = parsePositiveInt("MACOS_COMMAND_MAX_BUFFER_BYTES", 64 * 1024 * 1024);
const testTimeoutMs = parseTimeoutMs("MACOS_TEST_TIMEOUT_MS", 2 * 60 * 1000);
const inactivityTimeoutMs = parseTimeoutMs("MACOS_TEST_INACTIVITY_TIMEOUT_MS", 45 * 1000);
const emitJunitLogs = process.env.MACOS_LOG_JUNIT !== "0";
const requestedTests = (process.env.MACOS_TESTS || "").trim();
const requestedSpecs = (process.env.MACOS_TEST_SPECS || "").trim();
const verboseSpecs = process.env.MACOS_TEST_VERBOSE_SPECS === "1";
const requestedEngine = (process.env.MACOS_TEST_ENGINE || "v8").trim().toLowerCase();
const requestedFfiBackend = (process.env.MACOS_TEST_FFI_BACKEND || "auto").trim().toLowerCase();
const requestedGsdBackend = (process.env.MACOS_TEST_GSD_BACKEND || process.env.NS_GSD_BACKEND || "auto").trim().toLowerCase();

const launchedMarker = "Application Start!";
const junitPrefix = "TKUnit: ";
const junitEndTag = "</testsuites>";
const consoleLogMarker = "CONSOLE LOG:";
const crashReportsDir = path.join(os.homedir(), "Library", "Logs", "DiagnosticReports");
const generatedRuntimeBuildOutputs = new Set([
    path.join(nativeScriptSourceRoot, "ffi", "objc", "shared", "GeneratedSignatureDispatch.inc"),
    path.join(nativeScriptSourceRoot, "ffi", "objc", "shared", "GeneratedSignatureDispatch.inc.stamp"),
    path.join(nativeScriptSourceRoot, "ffi", "objc", "shared", "GeneratedGsdSignatureDispatch.inc"),
    path.join(nativeScriptSourceRoot, "ffi", "objc", "shared", "GeneratedGsdSignatureDispatch.inc.stamp")
]);

function parseArgs() {
    const args = process.argv.slice(2).filter(Boolean);
    if (args.length === 0) {
        return { junitOutPath: defaultJunitPath };
    }

    return { junitOutPath: path.resolve(args[0]) };
}

function run(command, args, options = {}) {
    const effectiveTimeout = options.timeout ?? commandTimeoutMs;
    const result = cp.spawnSync(command, args, {
        encoding: "utf8",
        timeout: effectiveTimeout,
        maxBuffer: commandMaxBufferBytes,
        ...options
    });

    if (result.error) {
        if (result.error.code === "ETIMEDOUT") {
            throw new Error(`Command timed out after ${effectiveTimeout}ms: ${command} ${args.join(" ")}`);
        }

        throw result.error;
    }

    return result;
}

function getPathStats(targetPath) {
    if (!fs.existsSync(targetPath)) {
        return { files: 0, bytes: 0, maxMtimeMs: 0 };
    }

    const queue = [targetPath];
    let files = 0;
    let bytes = 0;
    let maxMtimeMs = 0;

    while (queue.length > 0) {
        const currentPath = queue.pop();
        if (generatedRuntimeBuildOutputs.has(currentPath)) {
            continue;
        }
        let stats;
        try {
            stats = fs.lstatSync(currentPath);
        } catch (_) {
            continue;
        }

        if (stats.mtimeMs > maxMtimeMs) {
            maxMtimeMs = stats.mtimeMs;
        }

        if (stats.isDirectory()) {
            const entries = fs.readdirSync(currentPath);
            for (const entry of entries) {
                queue.push(path.join(currentPath, entry));
            }
            continue;
        }

        if (stats.isFile() || stats.isSymbolicLink()) {
            files += 1;
            bytes += stats.size;
        }
    }

    return { files, bytes, maxMtimeMs };
}

function createBuildFingerprint(inputs) {
    const snapshot = inputs.map((inputPath) => ({
        path: inputPath,
        ...getPathStats(inputPath)
    }));

    return crypto.createHash("sha1").update(JSON.stringify(snapshot)).digest("hex");
}

function readBuildState() {
    if (!fs.existsSync(buildStatePath)) {
        return null;
    }

    try {
        return JSON.parse(fs.readFileSync(buildStatePath, "utf8"));
    } catch (_) {
        return null;
    }
}

function writeBuildState(nativeFingerprint) {
    fs.mkdirSync(path.dirname(buildStatePath), { recursive: true });
    fs.writeFileSync(buildStatePath, JSON.stringify({ nativeFingerprint, createdAt: Date.now() }));
}

function syncAppResources(destinationAppResourcesPath) {
    fs.mkdirSync(destinationAppResourcesPath, { recursive: true });
    const result = run("rsync", [
        "-a",
        "--delete",
        `${testRunnerAppSourcePath}/`,
        `${destinationAppResourcesPath}/`
    ]);

    if (result.status !== 0) {
        throw new Error(`Failed to sync TestRunner app resources: ${result.stderr || result.stdout || "unknown rsync error"}`);
    }
}

function stripConsoleLogPrefix(line) {
    const markerIndex = line.indexOf(consoleLogMarker);
    if (markerIndex < 0) {
        return line;
    }

    return line.slice(markerIndex + consoleLogMarker.length).trimStart();
}

function quoteForLLDB(arg) {
    return `"${String(arg).replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
}

function getProcessExitStatus(code, signal) {
    if (typeof code === "number") {
        return { code, display: String(code) };
    }

    if (signal) {
        const signalNumber = os.constants.signals[signal];
        if (typeof signalNumber === "number") {
            const mappedCode = 128 + signalNumber;
            return { code: mappedCode, display: `${mappedCode} (signal ${signal})` };
        }

        return { code: 1, display: `signal ${signal}` };
    }

    return { code: 1, display: "unknown" };
}

function isLikelyCrash(code, signal) {
    if (signal) {
        return true;
    }

    return code === 134 || code === 139;
}

function readRecentCrashReportForPid(pid, launchedAtMs) {
    if (!pid || !fs.existsSync(crashReportsDir)) {
        return null;
    }

    const candidates = fs.readdirSync(crashReportsDir)
        .filter((name) => name.startsWith("TestRunner") && (name.endsWith(".ips") || name.endsWith(".crash")))
        .map((name) => {
            const fullPath = path.join(crashReportsDir, name);
            let stats;
            try {
                stats = fs.statSync(fullPath);
            } catch (_) {
                return null;
            }

            return {
                fullPath,
                mtimeMs: stats.mtimeMs
            };
        })
        .filter(Boolean)
        .filter((item) => item.mtimeMs >= (launchedAtMs - 5000))
        .sort((a, b) => b.mtimeMs - a.mtimeMs);

    const pidMatchers = [
        `"pid" : ${pid}`,
        `"pid":${pid}`,
        `Process:               TestRunner [${pid}]`,
        `Process:         TestRunner [${pid}]`
    ];

    for (const candidate of candidates) {
        let content;
        try {
            content = fs.readFileSync(candidate.fullPath, "utf8");
        } catch (_) {
            continue;
        }

        if (pidMatchers.some((matcher) => content.includes(matcher))) {
            return { path: candidate.fullPath, content };
        }
    }

    return null;
}

function formatBacktraceFromIPS(ipsContent) {
    const firstNewline = ipsContent.indexOf("\n");
    if (firstNewline < 0) {
        return null;
    }

    let report;
    try {
        report = JSON.parse(ipsContent.slice(firstNewline + 1).trim());
    } catch (_) {
        return null;
    }

    const threads = report.threads || [];
    if (!Array.isArray(threads) || threads.length === 0) {
        return null;
    }

    const faultingThread = Number.isInteger(report.faultingThread) ? report.faultingThread : 0;
    const images = report.usedImages || [];
    const lines = [];
    const exceptionType = report.exception && report.exception.type ? report.exception.type : "unknown";
    const exceptionSignal = report.exception && report.exception.signal ? report.exception.signal : "unknown";
    lines.push(`Exception: ${exceptionType} (${exceptionSignal})`);
    lines.push(`Faulting thread: ${faultingThread}`);

    for (let threadIndex = 0; threadIndex < threads.length; threadIndex++) {
        const thread = threads[threadIndex];
        if (!thread || !Array.isArray(thread.frames)) {
            continue;
        }

        const threadHeader = threadIndex === faultingThread
            ? `Thread ${threadIndex} Crashed${thread.queue ? ` (${thread.queue})` : ""}:`
            : `Thread ${threadIndex}${thread.queue ? ` (${thread.queue})` : ""}:`;
        lines.push(threadHeader);

        for (let frameIndex = 0; frameIndex < thread.frames.length; frameIndex++) {
            const frame = thread.frames[frameIndex];
            const imageName = (typeof frame.imageIndex === "number" && images[frame.imageIndex] && images[frame.imageIndex].name)
                ? images[frame.imageIndex].name
                : `image[${frame.imageIndex ?? "?"}]`;
            const symbol = frame.symbol || (typeof frame.imageOffset === "number" ? `0x${frame.imageOffset.toString(16)}` : "<unknown>");
            const symbolLocation = typeof frame.symbolLocation === "number" ? ` + ${frame.symbolLocation}` : "";
            const sourceLocation = frame.sourceFile
                ? ` (${path.basename(frame.sourceFile)}${typeof frame.sourceLine === "number" ? `:${frame.sourceLine}` : ""})`
                : "";
            lines.push(`${String(frameIndex).padStart(3, " ")} ${imageName} ${symbol}${symbolLocation}${sourceLocation}`);
        }
    }

    return lines.join("\n");
}

function formatBacktraceFromCrashText(crashContent) {
    const match = crashContent.match(/Thread\s+\d+\s+Crashed:[\s\S]*?(?=\n\nThread\s+\d+|\nBinary Images:|$)/);
    return match ? match[0] : null;
}

function emitLLDBBacktrace(appBinaryPath, runArgs) {
    const runCommand = runArgs.length > 0
        ? `run ${runArgs.map(quoteForLLDB).join(" ")}`
        : "run";
    const args = [
        "lldb",
        "--batch",
        "--one-line", "process handle -p true -s false -n false SIGSEGV SIGBUS SIGABRT SIGILL SIGTRAP",
        "--one-line", runCommand,
        "--one-line", "thread backtrace all",
        "--",
        appBinaryPath
    ];

    const result = cp.spawnSync("xcrun", args, {
        encoding: "utf8",
        timeout: commandTimeoutMs,
        maxBuffer: commandMaxBufferBytes
    });

    if (result.error) {
        console.error(`ERROR: Unable to collect LLDB backtrace: ${result.error.message}`);
        return;
    }

    const output = `${result.stdout || ""}${result.stderr || ""}`.trim();
    if (output.length === 0) {
        console.error("ERROR: LLDB produced no backtrace output.");
        return;
    }

    console.error("\n--- Crash Backtrace (LLDB) ---");
    console.error(output);
}

async function emitCrashBacktrace(appBinaryPath, runArgs, launchedAtMs, pid) {
    const deadline = Date.now() + 5000;

    while (Date.now() < deadline) {
        const report = readRecentCrashReportForPid(pid, launchedAtMs);
        if (report) {
            const formatted = report.path.endsWith(".ips")
                ? formatBacktraceFromIPS(report.content)
                : formatBacktraceFromCrashText(report.content);

            if (formatted) {
                console.error(`\n--- Crash Backtrace (${report.path}) ---`);
                console.error(formatted);
                return;
            }

            break;
        }

        await new Promise((resolve) => setTimeout(resolve, 200));
    }

    emitLLDBBacktrace(appBinaryPath, runArgs);
}

function runBuildAndRequireSuccess(command, args, timeoutMs = commandTimeoutMs) {
    const result = cp.spawnSync(command, args, {
        encoding: "utf8",
        timeout: timeoutMs,
        maxBuffer: commandMaxBufferBytes
    });

    if (result.error && result.error.code === "ETIMEDOUT") {
        console.error(`ERROR: Command timed out after ${timeoutMs}ms: ${command} ${args.join(" ")}`);
        if (result.stdout && result.stdout.trim().length > 0) {
            console.error(result.stdout.trimEnd());
        }
        if (result.stderr && result.stderr.trim().length > 0) {
            console.error(result.stderr.trimEnd());
        }
        process.exit(1);
    }

    if (result.error) {
        throw result.error;
    }

    if (result.status !== 0) {
        console.error(`ERROR: Build command failed: ${command} ${args.join(" ")}`);
        if (result.stdout && result.stdout.trim().length > 0) {
            console.error(result.stdout.trimEnd());
        }
        if (result.stderr && result.stderr.trim().length > 0) {
            console.error(result.stderr.trimEnd());
        }
        process.exit(1);
    }
}

function ensureMetadataGeneratorBuilt() {
    const sourceMtime = metadataGeneratorSourceInputs.reduce(
        (latest, inputPath) => Math.max(latest, getPathStats(inputPath).maxMtimeMs),
        0
    );
    const artifactMtime = Math.min(
        getPathStats(metadataGeneratorBinary).maxMtimeMs,
        getPathStats(metadataGeneratorSymbolAnalyzer).maxMtimeMs,
        getPathStats(metadataGeneratorBuildStepScript).maxMtimeMs
    );

    if (artifactMtime > 0 && artifactMtime >= sourceMtime) {
        return;
    }

    console.log("Metadata generator is missing or stale; running build-metagen...");
    const hostArch = os.arch() === "x64" ? "x86_64" : os.arch();
    runBuildAndRequireSuccess(
        "env",
        [`METADATA_GENERATOR_ARCHS=${hostArch}`, "npm", "run", "build-metagen"],
        commandTimeoutMs
    );
}

function ensureMacOSRuntimeArtifactsBuilt() {
    const cachePath = path.join(__dirname, "../dist", "intermediates", "macos", "CMakeCache.txt");
    const sourceInputs = [
        nativeScriptSourceRoot,
        ...metadataGeneratorSourceInputs,
        path.join(__dirname, "build_nativescript.sh")
    ];

    const sourceMtime = sourceInputs.reduce(
        (latest, inputPath) => Math.max(latest, getPathStats(inputPath).maxMtimeMs),
        0
    );
    const artifactMtime = getPathStats(nativeScriptXCFramework).maxMtimeMs;
    let configuredEngine = null;
    let configuredFfiBackend = null;
    let configuredGsdBackend = null;

    if (fs.existsSync(cachePath)) {
        try {
            const cache = fs.readFileSync(cachePath, "utf8");
            const engineMatch = cache.match(/^TARGET_ENGINE:STRING=(.+)$/m);
            if (engineMatch) {
                configuredEngine = engineMatch[1].trim().toLowerCase();
            }
            const ffiBackendMatch = cache.match(/^NS_FFI_BACKEND:STRING=(.+)$/m);
            if (ffiBackendMatch) {
                configuredFfiBackend = ffiBackendMatch[1].trim().toLowerCase();
            }
            const gsdBackendMatch = cache.match(/^NS_GSD_BACKEND:STRING=(.+)$/m);
            if (gsdBackendMatch) {
                configuredGsdBackend = gsdBackendMatch[1].trim().toLowerCase();
            }
        } catch (_) {
            configuredEngine = null;
            configuredFfiBackend = null;
        }
    }

    if (artifactMtime > 0 &&
        artifactMtime >= sourceMtime &&
        configuredEngine === requestedEngine &&
        configuredFfiBackend === requestedFfiBackend &&
        configuredGsdBackend === requestedGsdBackend) {
        return;
    }

    const supportedEngines = new Set(["v8", "hermes", "quickjs", "jsc"]);
    if (!supportedEngines.has(requestedEngine)) {
        throw new Error(`Unsupported MACOS_TEST_ENGINE: ${requestedEngine}`);
    }

    const supportedFfiBackends = new Set(["auto", "napi", "v8", "hermes", "quickjs", "jsc"]);
    if (!supportedFfiBackends.has(requestedFfiBackend)) {
        throw new Error(`Unsupported MACOS_TEST_FFI_BACKEND: ${requestedFfiBackend}`);
    }

    const supportedGsdBackends = new Set(["auto", "v8", "jsc", "quickjs", "hermes", "napi", "none"]);
    if (!supportedGsdBackends.has(requestedGsdBackend)) {
        throw new Error(`Unsupported MACOS_TEST_GSD_BACKEND: ${requestedGsdBackend}`);
    }

    console.log(`NativeScript macOS artifacts are missing, stale, or built for '${configuredEngine ?? "unknown"}/${configuredFfiBackend ?? "unknown"}/${configuredGsdBackend ?? "unknown"}'; running ${requestedEngine}/${requestedFfiBackend}/${requestedGsdBackend} build...`);
    runBuildAndRequireSuccess(
        path.join(__dirname, "build_nativescript.sh"),
        ["--macos", "--no-iphone", "--no-simulator", `--${requestedEngine}`, `--ffi-backend=${requestedFfiBackend}`, `--gsd-backend=${requestedGsdBackend}`],
        commandTimeoutMs
    );
}

function buildTestRunnerApp() {
    const appBundlePath = path.join(
        derivedDataPath,
        "Build",
        "Products",
        configuration,
        "TestRunner.app"
    );
    const appResourcesPath = path.join(appBundlePath, "Contents", "Resources", "app");
    const appPath = path.join(appBundlePath, "Contents", "MacOS", "TestRunner");

    if (process.env.MACOS_TEST_CLEAN_BUILD === "1") {
        fs.rmSync(derivedDataPath, { recursive: true, force: true });
    }

    ensureMetadataGeneratorBuilt();
    ensureMacOSRuntimeArtifactsBuilt();

    const nativeFingerprint = `${requestedEngine}:${requestedFfiBackend}:${requestedGsdBackend}:${createBuildFingerprint(macosBuildInputs)}`;
    const existingBuildState = readBuildState();
    const canReuseBuild = process.env.MACOS_TEST_CLEAN_BUILD !== "1" &&
        fs.existsSync(appPath) &&
        existingBuildState &&
        existingBuildState.nativeFingerprint === nativeFingerprint;

    console.log("Building...");
    if (!canReuseBuild) {
        const args = [
            "-project", projectPath,
            "-scheme", scheme,
            "-configuration", configuration,
            "-destination", "platform=macOS,arch=arm64",
            "-derivedDataPath", derivedDataPath,
            "CODE_SIGN_STYLE=Manual",
            "CODE_SIGNING_ALLOWED=NO",
            "CODE_SIGNING_REQUIRED=NO",
            "CODE_SIGN_IDENTITY=",
            "DEVELOPMENT_TEAM=",
            "build"
        ];
        runBuildAndRequireSuccess("xcodebuild", args, commandTimeoutMs);
    }

    syncAppResources(appResourcesPath);
    writeBuildState(nativeFingerprint);

    if (!fs.existsSync(appPath)) {
        console.error(`ERROR: Built app executable not found at expected path: ${appPath}`);
        process.exit(1);
    }

    return appPath;
}

function extractCount(xml, attribute) {
    const regex = new RegExp(`${attribute}="(\\d+)"`, "g");
    let total = 0;
    let match;
    while ((match = regex.exec(xml)) !== null) {
        total += Number(match[1]);
    }

    return total;
}

function main() {
    const { junitOutPath } = parseArgs();

    fs.mkdirSync(path.dirname(junitOutPath), { recursive: true });
    fs.mkdirSync(resultsDir, { recursive: true });

    const skipBuild = process.env.MACOS_TEST_SKIP_BUILD === "1";
    const appBinaryPath = skipBuild
        ? path.join(
            derivedDataPath,
            "Build",
            "Products",
            configuration,
            "TestRunner.app",
            "Contents",
            "MacOS",
            "TestRunner"
        )
        : buildTestRunnerApp();

    if (!fs.existsSync(appBinaryPath)) {
        console.error(`ERROR: App executable not found: ${appBinaryPath}`);
        process.exit(1);
    }

    const runArgs = ["-logjunit"];
    if (verboseSpecs) {
        runArgs.push("-verbose-specs");
    }
    if (requestedTests.length > 0) {
        runArgs.push("-tests", requestedTests);
    }
    if (requestedSpecs.length > 0) {
        runArgs.push("-specs", requestedSpecs);
    }

    console.log(`Launching app and streaming logs: ${appBinaryPath} ${runArgs.join(" ")}`);

    const results = fs.createWriteStream(junitOutPath);
    let completedSuccessfully = false;
    let completionKillIssued = false;
    let junitBuffer = "";
    let appLaunched = false;
    let timeoutTimer = null;
    let inactivityTimer = null;
    let childPid = null;
    let launchedAtMs = Date.now();

    function clearTimers() {
        if (timeoutTimer) {
            clearTimeout(timeoutTimer);
            timeoutTimer = null;
        }
        if (inactivityTimer) {
            clearTimeout(inactivityTimer);
            inactivityTimer = null;
        }
    }

    function failAndExit(message, processHandle) {
        console.error(message);
        clearTimers();
        results.end();
        if (processHandle && !processHandle.killed) {
            processHandle.kill("SIGKILL");
        }
        process.exit(1);
    }

    function scheduleInactivityTimeout(processHandle) {
        if (inactivityTimer) {
            clearTimeout(inactivityTimer);
        }

        inactivityTimer = setTimeout(() => {
            failAndExit(`ERROR: No logs received for ${inactivityTimeoutMs}ms after launch.`, processHandle);
        }, inactivityTimeoutMs);
    }

    timeoutTimer = setTimeout(() => {
        failAndExit(`ERROR: Launch timeout after ${testTimeoutMs}ms.`, null);
    }, testTimeoutMs);

    const child = cp.spawn(appBinaryPath, runArgs, {
        stdio: ["ignore", "pipe", "pipe"]
    });
    childPid = child.pid;
    launchedAtMs = Date.now();

    function createChunkHandler() {
        let leftover = "";

        return function handleChunk(chunk) {
            const text = chunk.toString();
            const chunks = leftover + text;
            const lines = chunks.split(/\r?\n/);

            for (let i = 0; i < lines.length - 1; i++) {
                const line = lines[i];
                process.stdout.write(`${stripConsoleLogPrefix(line)}\n`);

                if (appLaunched) {
                    scheduleInactivityTimeout(child);
                }

                if (!appLaunched && line.indexOf(launchedMarker) !== -1) {
                    appLaunched = true;
                    if (timeoutTimer) {
                        clearTimeout(timeoutTimer);
                    }
                    timeoutTimer = setTimeout(() => {
                        failAndExit(`ERROR: Tests run timeout after ${testTimeoutMs}ms.`, child);
                    }, testTimeoutMs);
                    scheduleInactivityTimeout(child);
                    console.log("Application launched. Start test run timeout.");
                }

                const prefixIndex = line.indexOf(junitPrefix);
                if (prefixIndex < 0) {
                    continue;
                }

                const data = line.slice(prefixIndex + junitPrefix.length);
                results.write(data + "\n");
                junitBuffer += data + "\n";

                if (emitJunitLogs) {
                    // Already printed via process.stdout.write(text), keep behavior toggled for parity.
                }

                if (data.indexOf(junitEndTag) !== -1) {
                    completedSuccessfully = true;
                    clearTimers();
                    if (!completionKillIssued && child && !child.killed) {
                        completionKillIssued = true;
                        child.kill("SIGKILL");
                    }
                }
            }

            leftover = lines[lines.length - 1];
        };
    }

    child.stdout.on("data", createChunkHandler());
    child.stderr.on("data", createChunkHandler());

    child.on("error", (error) => {
        failAndExit(`ERROR: Failed to start TestRunner process: ${error.message}`, child);
    });

    child.on("close", async (code, signal) => {
        clearTimers();
        results.end();
        const exitStatus = getProcessExitStatus(code, signal);

        if (!completedSuccessfully) {
            if (code !== 0 || signal) {
                await emitCrashBacktrace(appBinaryPath, runArgs, launchedAtMs, childPid);
            }

            console.error(`ERROR: Test run failed before JUnit completion. Exit code: ${exitStatus.display}`);
            process.exit(exitStatus.code);
            return;
        }

        const tests = extractCount(junitBuffer, "tests");
        const failures = extractCount(junitBuffer, "failures");
        const errors = extractCount(junitBuffer, "errors");
        const skipped = extractCount(junitBuffer, "skipped");

        console.log(`Summary: tests=${tests}, failures=${failures}, errors=${errors}, skipped=${skipped}`);
        console.log(`Test run finished. JUnit written to: ${junitOutPath}`);
        process.exit(failures > 0 || errors > 0 ? 1 : 0);
    });
}

main();
