// Runs TestRunner on an iOS simulator and streams runtime logs directly.
// This path intentionally avoids XCTest integration so output comes from the app itself.
//
// Optional args:
//  - junit output path (defaults to build/test-results/ios-junit.xml)
//  - destination (e.g. "platform=iOS Simulator,id=..." or simulator UDID)
//
// Env:
//  - IOS_DESTINATION overrides destination argument.
//  - IOS_TEST_SKIP_BUILD=1 skips auto-build of iOS simulator runtime artifacts.
//  - IOS_TEST_CLEAN_BUILD=1 deletes derived data before building TestRunner.app.
//  - IOS_TEST_ENGINE selects the runtime engine build to use when runtime
//    artifacts need rebuilding. Supported: v8, hermes, quickjs, jsc. Defaults to v8.
//  - IOS_SWIFT_VERSION overrides default Swift version (default: 5.0).
//  - IOS_COMMAND_TIMEOUT_MS overrides timeout for build/install/simctl commands (default: 3 minutes).
//  - IOS_SIMCTL_QUERY_TIMEOUT_MS overrides timeout for polling simctl queries (default: 10 seconds).
//  - IOS_APP_CONTAINER_READY_TIMEOUT_MS overrides how long to wait for simctl to expose app containers (default: 60 seconds).
//  - IOS_BUILD_TIMEOUT_MS overrides timeout for xcodebuild app build (default: IOS_COMMAND_TIMEOUT_MS).
//  - IOS_COMMAND_MAX_BUFFER_BYTES overrides spawnSync maxBuffer for captured command output (default: 64 MiB).
//  - IOS_TEST_TIMEOUT_MS overrides max test runtime (default: 10 minutes).
//  - IOS_LOG_JUNIT=1 enables streaming TKUnit/JUnit lines to console.
//  - IOS_TESTS filters test modules (comma-separated substrings passed to app as -tests).
//  - IOS_SPECS filters individual Jasmine specs by description substring (comma-separated, passed as -specs).
//  - IOS_TEST_INACTIVITY_TIMEOUT_MS overrides max no-log interval (default: 2 minutes).
//  - IOS_TEST_LOG_STREAM=0 disables parallel simulator log stream (enabled by default).
//  - IOS_TEST_VERBOSE_SPECS=1 enables per-spec start/done logs from the app.
//  - IOS_SIM_LOG_LOOKBACK sets log-show window used for post-failure diagnostics (default: 45s).

const fs = require("fs");
const path = require("path");
const cp = require("child_process");
const crypto = require("crypto");

const projectPath = path.join(__dirname, "../platforms/apple/NativeScriptRuntime.xcodeproj");
const scheme = "TestRunner";
const bundleId = "com.descendra.TestRunner";

const resultsDir = path.join(__dirname, "../build", "test-results");
const defaultJunitPath = path.join(resultsDir, "ios-junit.xml");
const derivedDataPath = path.join(__dirname, "../build", "derived-data", "ios-tests");
const testRunnerAppSourcePath = path.join(__dirname, "../platforms/apple/test/runtime/runner", "app");
const buildStatePath = path.join(derivedDataPath, ".ios-test-build-state.json");
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
const nativeScriptSourceRoot = path.join(__dirname, "../NativeScript");

const nativeScriptXCFramework = path.join(__dirname, "../dist", "NativeScript.xcframework");
const tkLiveSyncXCFramework = path.join(__dirname, "../dist", "TKLiveSync.xcframework");
const iosBuildInputs = [
    path.join(__dirname, "../platforms/apple/NativeScriptRuntime.xcodeproj", "project.pbxproj"),
    path.join(__dirname, "../platforms/apple/test/runtime/runner", "Source Files"),
    path.join(__dirname, "../platforms/apple/test/runtime/runner", "Info.plist"),
    path.join(__dirname, "../platforms/apple/test/runtime/fixtures"),
    path.join(__dirname, "../platforms/apple/TKLiveSync"),
    ...metadataGeneratorSourceInputs,
    nativeScriptXCFramework,
    tkLiveSyncXCFramework
];

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

const commandTimeoutMs = parseTimeoutMs("IOS_COMMAND_TIMEOUT_MS", 3 * 60 * 1000);
// Clean CI runners often need substantially longer for the first iOS build than
// for simulator control commands like boot/install/log collection.
const buildTimeoutMs = parseTimeoutMs("IOS_BUILD_TIMEOUT_MS", 10 * 60 * 1000);
const simctlQueryTimeoutMs = parseTimeoutMs(
    "IOS_SIMCTL_QUERY_TIMEOUT_MS",
    Math.min(commandTimeoutMs, 10 * 1000)
);
const appContainerReadyTimeoutMs = parseTimeoutMs(
    "IOS_APP_CONTAINER_READY_TIMEOUT_MS",
    Math.max(60 * 1000, simctlQueryTimeoutMs * 6)
);
const commandMaxBufferBytes = parsePositiveInt("IOS_COMMAND_MAX_BUFFER_BYTES", 64 * 1024 * 1024);
const testTimeoutMs = Number(process.env.IOS_TEST_TIMEOUT_MS || 10 * 60 * 1000);
const inactivityTimeoutMs = Number(process.env.IOS_TEST_INACTIVITY_TIMEOUT_MS || 2 * 60 * 1000);
const emitJunitLogs = process.env.IOS_LOG_JUNIT === "1";
const requestedTests = (process.env.IOS_TESTS || "").trim();
const requestedSpecs = (process.env.IOS_SPECS || "").trim();
const requestedEngine = (process.env.IOS_TEST_ENGINE || "v8").trim().toLowerCase();
const enableLiveLogStream = process.env.IOS_TEST_LOG_STREAM !== "0";
const verboseSpecLogs = process.env.IOS_TEST_VERBOSE_SPECS === "1";
const simulatorLogLookback = process.env.IOS_SIM_LOG_LOOKBACK || "45s";
const consoleLogMarker = "CONSOLE LOG:";

function looksLikeDestination(value) {
    return value.includes("platform=iOS Simulator") || /^[0-9A-Fa-f-]{36}$/.test(value) || value.includes("id=");
}

function parseArgs() {
    const args = process.argv.slice(2).filter(Boolean);

    let junitOutPath = defaultJunitPath;
    let destinationArg;

    for (const arg of args) {
        if (looksLikeDestination(arg) && !destinationArg) {
            destinationArg = arg;
            continue;
        }

        junitOutPath = path.resolve(arg);
    }

    return { junitOutPath, destinationArg };
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

function runAndRequireSuccess(command, args, timeoutMs = commandTimeoutMs) {
    const result = cp.spawnSync(command, args, { stdio: "inherit", timeout: timeoutMs });
    if (result.error && result.error.code === "ETIMEDOUT") {
        console.error(`ERROR: Command timed out after ${timeoutMs}ms: ${command} ${args.join(" ")}`);
        process.exit(1);
    }
    if (result.status !== 0) {
        process.exit(result.status || 1);
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
    const hostArch = process.arch === "x64" ? "x86_64" : process.arch;
    runAndRequireSuccess(
        "env",
        [`METADATA_GENERATOR_ARCHS=${hostArch}`, "npm", "run", "build-metagen"],
        buildTimeoutMs
    );
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

function writeBuildState(nativeFingerprint, swiftVersion) {
    fs.mkdirSync(path.dirname(buildStatePath), { recursive: true });
    fs.writeFileSync(buildStatePath, JSON.stringify({
        nativeFingerprint,
        swiftVersion,
        createdAt: Date.now()
    }));
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

function stripConsoleLogPrefixes(text) {
    return text.replace(/^.*CONSOLE LOG:\s?/gm, "");
}

function parseRuntimeKey(runtimeKey) {
    const match = runtimeKey.match(/iOS[- ](\d+)[- ](\d+)/);
    if (!match) {
        return { major: 0, minor: 0 };
    }

    return {
        major: Number(match[1]),
        minor: Number(match[2])
    };
}

function listAvailableIphoneSimulators() {
    const simctl = run("xcrun", ["simctl", "list", "devices", "--json"]);
    if (simctl.status !== 0) {
        throw new Error(simctl.stderr || "Failed to list simulators");
    }

    const json = JSON.parse(simctl.stdout);
    const devicesByRuntime = json.devices || {};

    const devices = [];
    for (const runtimeKey of Object.keys(devicesByRuntime)) {
        if (!runtimeKey.includes("iOS")) {
            continue;
        }

        for (const device of devicesByRuntime[runtimeKey] || []) {
            if (!device.isAvailable || !device.name.startsWith("iPhone")) {
                continue;
            }

            devices.push({
                runtimeKey,
                runtimeVersion: parseRuntimeKey(runtimeKey),
                udid: device.udid,
                name: device.name,
                state: device.state
            });
        }
    }

    devices.sort((a, b) => {
        if (b.runtimeVersion.major !== a.runtimeVersion.major) {
            return b.runtimeVersion.major - a.runtimeVersion.major;
        }

        if (b.runtimeVersion.minor !== a.runtimeVersion.minor) {
            return b.runtimeVersion.minor - a.runtimeVersion.minor;
        }

        return a.name.localeCompare(b.name);
    });

    return devices;
}

function pickPreferredSimulator(candidates) {
    if (candidates.length === 0) {
        throw new Error("No available iPhone simulator found.");
    }

    const preferredDeviceNames = [
        "iPhone 16e",
        "iPhone 16",
        "iPhone 16 Plus",
        "iPhone 16 Pro",
        "iPhone 16 Pro Max",
        "iPhone 15",
        "iPhone 15 Plus",
        "iPhone 15 Pro",
        "iPhone 15 Pro Max"
    ];

    const score = (device) => {
        const preferredIndex = preferredDeviceNames.indexOf(device.name);
        if (preferredIndex >= 0) {
            return preferredIndex;
        }

        if (device.name.includes("Pro")) {
            return 10_000;
        }

        return 5_000;
    };

    const sorted = [...candidates].sort((a, b) => score(a) - score(b));
    return sorted.find((d) => d.state === "Booted") || sorted[0];
}

function normalizeDestinationArg(destinationArg) {
    if (!destinationArg) {
        return undefined;
    }

    if (/^[0-9A-Fa-f-]{36}$/.test(destinationArg)) {
        return `platform=iOS Simulator,id=${destinationArg}`;
    }

    return destinationArg;
}

function resolveDestinationAndSimulator(destinationArg) {
    const normalized = normalizeDestinationArg(destinationArg);
    const devices = listAvailableIphoneSimulators();

    if (!normalized) {
        const selected = pickPreferredSimulator(devices);
        return {
            destination: `platform=iOS Simulator,id=${selected.udid}`,
            simulator: selected
        };
    }

    const idMatch = normalized.match(/id=([0-9A-Fa-f-]{36})/);
    if (idMatch) {
        const byId = devices.find((d) => d.udid.toLowerCase() === idMatch[1].toLowerCase());
        if (!byId) {
            throw new Error(`Requested simulator not found or unavailable: ${idMatch[1]}`);
        }

        return {
            destination: `platform=iOS Simulator,id=${byId.udid}`,
            simulator: byId
        };
    }

    const nameMatch = normalized.match(/name=([^,]+)/);
    if (nameMatch) {
        const requestedName = nameMatch[1];
        const sameName = devices.filter((d) => d.name === requestedName);
        if (sameName.length === 0) {
            throw new Error(`Requested simulator name not found or unavailable: ${requestedName}`);
        }

        const selected = sameName[0];
        return {
            destination: `platform=iOS Simulator,id=${selected.udid}`,
            simulator: selected
        };
    }

    throw new Error(`Unsupported destination format: ${normalized}`);
}

function bootSimulator(udid) {
    run("xcrun", ["simctl", "boot", udid]);
    const bootstatus = run("xcrun", ["simctl", "bootstatus", udid, "-b"], { stdio: "inherit" });
    if (bootstatus.status !== 0) {
        process.exit(bootstatus.status || 1);
    }
}

function hasSimulatorSlice(xcframeworkPath) {
    if (!fs.existsSync(xcframeworkPath)) {
        return false;
    }

    const entries = fs.readdirSync(xcframeworkPath, { withFileTypes: true });
    return entries.some((entry) => entry.isDirectory() && entry.name.includes("simulator"));
}

function buildTKLiveSyncSimulatorXCFramework() {
    const intermediates = path.join(__dirname, "../dist", "intermediates", "ios-test-tklivesync");
    const sourcePath = path.join(intermediates, "dummy.c");
    const frameworkPath = path.join(intermediates, "TKLiveSync.framework");
    const arm64Binary = path.join(intermediates, "TKLiveSync-arm64");
    const x64Binary = path.join(intermediates, "TKLiveSync-x86_64");
    const fatBinary = path.join(frameworkPath, "TKLiveSync");
    const infoPlistPath = path.join(frameworkPath, "Info.plist");

    fs.rmSync(intermediates, { recursive: true, force: true });
    fs.mkdirSync(intermediates, { recursive: true });
    fs.mkdirSync(frameworkPath, { recursive: true });
    fs.writeFileSync(sourcePath, "void TKLivesyncDummy(void) {}\n");

    console.log("Building minimal TKLiveSync simulator binary for test linking...");
    runAndRequireSuccess("xcrun", [
        "--sdk", "iphonesimulator",
        "clang",
        "-target", "arm64-apple-ios13.0-simulator",
        "-dynamiclib",
        sourcePath,
        "-install_name", "@rpath/TKLiveSync.framework/TKLiveSync",
        "-o", arm64Binary
    ]);

    runAndRequireSuccess("xcrun", [
        "--sdk", "iphonesimulator",
        "clang",
        "-target", "x86_64-apple-ios13.0-simulator",
        "-dynamiclib",
        sourcePath,
        "-install_name", "@rpath/TKLiveSync.framework/TKLiveSync",
        "-o", x64Binary
    ]);

    runAndRequireSuccess("xcrun", [
        "lipo",
        "-create",
        arm64Binary,
        x64Binary,
        "-output",
        fatBinary
    ]);

    fs.writeFileSync(infoPlistPath, `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>org.nativescript.TKLiveSync</string>
  <key>CFBundleExecutable</key><string>TKLiveSync</string>
  <key>CFBundleName</key><string>TKLiveSync</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
</dict>
</plist>
`);

    fs.rmSync(tkLiveSyncXCFramework, { recursive: true, force: true });
    runAndRequireSuccess("xcodebuild", [
        "-create-xcframework",
        "-framework", frameworkPath,
        "-output", tkLiveSyncXCFramework
    ]);
}

function ensureIOSSimulatorArtifacts() {
    const cachePath = path.join(__dirname, "../dist", "intermediates", "ios-sim", "CMakeCache.txt");
    const sourceInputs = [
        nativeScriptSourceRoot,
        path.join(__dirname, "build_nativescript.sh")
    ];
    const sourceMtime = sourceInputs.reduce(
        (latest, inputPath) => Math.max(latest, getPathStats(inputPath).maxMtimeMs),
        0
    );
    const artifactMtime = getPathStats(nativeScriptXCFramework).maxMtimeMs;
    const hasNativeScriptSimulator = hasSimulatorSlice(nativeScriptXCFramework);
    const hasTKLiveSyncSimulator = hasSimulatorSlice(tkLiveSyncXCFramework);
    let configuredEngine = null;

    if (fs.existsSync(cachePath)) {
        try {
            const cache = fs.readFileSync(cachePath, "utf8");
            const match = cache.match(/^TARGET_ENGINE:STRING=(.+)$/m);
            if (match) {
                configuredEngine = match[1].trim().toLowerCase();
            }
        } catch (_) {
            configuredEngine = null;
        }
    }

    const supportedEngines = new Set(["v8", "hermes", "quickjs", "jsc"]);
    if (!supportedEngines.has(requestedEngine)) {
        throw new Error(`Unsupported IOS_TEST_ENGINE: ${requestedEngine}`);
    }

    const needsNativeScriptRebuild =
        artifactMtime === 0 ||
        artifactMtime < sourceMtime ||
        configuredEngine !== requestedEngine;

    if (!hasNativeScriptSimulator || needsNativeScriptRebuild) {
        console.log(
            `NativeScript simulator artifacts are missing, stale, or built for '${configuredEngine ?? "unknown"}'; running ${requestedEngine} build...`
        );
        runAndRequireSuccess(path.join(__dirname, "build_nativescript.sh"), [
            "--no-iphone",
            "--simulator",
            "--no-macos",
            `--${requestedEngine}`
        ], buildTimeoutMs);
    }

    if (!hasTKLiveSyncSimulator) {
        buildTKLiveSyncSimulatorXCFramework();
    }
}

function buildTestRunnerApp(destination, swiftVersion) {
    const appPath = path.join(derivedDataPath, "Build", "Products", "Debug-iphonesimulator", "TestRunner.app");
    const appResourcesPath = path.join(appPath, "app");

    if (process.env.IOS_TEST_CLEAN_BUILD === "1") {
        fs.rmSync(derivedDataPath, { recursive: true, force: true });
    }

    ensureMetadataGeneratorBuilt();

    const nativeFingerprint = `${requestedEngine}:${createBuildFingerprint(iosBuildInputs)}`;
    const existingBuildState = readBuildState();
    const canReuseBuild = process.env.IOS_TEST_CLEAN_BUILD !== "1" &&
        fs.existsSync(appPath) &&
        existingBuildState &&
        existingBuildState.nativeFingerprint === nativeFingerprint &&
        existingBuildState.swiftVersion === swiftVersion;

    console.log("Building...");
    if (!canReuseBuild) {
        const args = [
            "-project", projectPath,
            "-scheme", scheme,
            "-configuration", "Debug",
            "-quiet",
            "-destination", destination,
            "-destination-timeout", "120",
            "-derivedDataPath", derivedDataPath,
            `SWIFT_VERSION=${swiftVersion}`,
            "CODE_SIGN_STYLE=Manual",
            "CODE_SIGNING_ALLOWED=NO",
            "CODE_SIGNING_REQUIRED=NO",
            "CODE_SIGN_IDENTITY=",
            "DEVELOPMENT_TEAM=",
            "build"
        ];
        const result = cp.spawnSync("xcodebuild", args, {
            encoding: "utf8",
            timeout: buildTimeoutMs,
            maxBuffer: commandMaxBufferBytes
        });
        if (result.error && result.error.code === "ETIMEDOUT") {
            if (result.stdout && result.stdout.trim().length > 0) {
                console.error(result.stdout.trimEnd());
            }
            if (result.stderr && result.stderr.trim().length > 0) {
                console.error(result.stderr.trimEnd());
            }
            throw new Error(`xcodebuild timed out after ${buildTimeoutMs}ms while building TestRunner.`);
        }
        if (result.error) {
            throw result.error;
        }
        if (result.status !== 0) {
            if (result.stdout && result.stdout.trim().length > 0) {
                console.error(result.stdout.trimEnd());
            }
            if (result.stderr && result.stderr.trim().length > 0) {
                console.error(result.stderr.trimEnd());
            }
            throw new Error(`xcodebuild failed while building TestRunner (exit ${result.status}).`);
        }
    }

    syncAppResources(appResourcesPath);
    writeBuildState(nativeFingerprint, swiftVersion);

    if (!fs.existsSync(appPath)) {
        throw new Error(`Built app not found at expected path: ${appPath}`);
    }

    return { appPath, reusedBuild: canReuseBuild };
}

const appContainerPathCache = new Map();

function getAppContainerPath(udid, containerType, options = {}) {
    const cacheKey = `${udid}:${containerType}`;
    if (appContainerPathCache.has(cacheKey)) {
        return appContainerPathCache.get(cacheKey);
    }

    let result;
    try {
        result = run("xcrun", ["simctl", "get_app_container", udid, bundleId, containerType], {
            timeout: options.timeout ?? simctlQueryTimeoutMs
        });
    } catch (error) {
        if (!options.quiet) {
            console.warn(`WARNING: Unable to resolve ${containerType} container yet (${error.message}).`);
        }
        return null;
    }

    if (result.status !== 0) {
        if (!options.quiet) {
            const stderr = (result.stderr || "").trim();
            console.warn(
                `WARNING: Unable to resolve ${containerType} container yet (simctl exited ${result.status}${stderr ? `: ${stderr}` : ""}).`
            );
        }
        return null;
    }

    const out = (result.stdout || "").trim();
    if (out) {
        appContainerPathCache.set(cacheKey, out);
    }
    return out || null;
}

function getAppDataContainerPath(udid, options = {}) {
    return getAppContainerPath(udid, "data", options);
}

function getInstalledAppPath(udid, options = {}) {
    return getAppContainerPath(udid, "app", options);
}

async function waitForAppContainerPath(udid, containerType, timeoutMs = appContainerReadyTimeoutMs) {
    const deadline = Date.now() + timeoutMs;
    let attempt = 0;

    while (Date.now() < deadline) {
        const remaining = Math.max(1, deadline - Date.now());
        const containerPath = getAppContainerPath(udid, containerType, {
            quiet: attempt > 0,
            timeout: Math.min(simctlQueryTimeoutMs, remaining)
        });
        if (containerPath) {
            return containerPath;
        }

        attempt += 1;
        await sleep(Math.min(250 * attempt, 2000));
    }

    console.warn(`WARNING: ${containerType} container was not available after ${timeoutMs}ms.`);
    return null;
}

function waitForAppDataContainerPath(udid, timeoutMs) {
    return waitForAppContainerPath(udid, "data", timeoutMs);
}

function removeOldJunitFile(udid, options = {}) {
    const dataContainer = getAppDataContainerPath(udid, options);
    if (!dataContainer) {
        return;
    }

    const junitPath = path.join(dataContainer, "Documents", "junit-result.xml");
    if (fs.existsSync(junitPath)) {
        fs.rmSync(junitPath, { force: true });
    }
}

function startLaunchWithConsole(udid, appArgs = [], useConsolePty = true, captureOutput = true) {
    const args = [
        "simctl",
        "launch",
        "--terminate-running-process",
        udid,
        bundleId
    ];
    if (useConsolePty) {
        args.splice(2, 0, "--console-pty");
    }
    if (appArgs.length > 0) {
        args.push("--args", ...appArgs);
    }

    console.log(`Launching app and streaming logs: xcrun ${args.join(" ")}`);
    return cp.spawn("xcrun", args, {
        stdio: captureOutput ? ["ignore", "pipe", "pipe"] : "ignore"
    });
}

function wireOutput(launchProcess, state) {
    const onStdout = (chunk) => {
        const text = chunk.toString();
        state.lastActivityAt = Date.now();
        state.logs += text;
        if (state.logs.length > 4 * 1024 * 1024) {
            state.logs = state.logs.slice(state.logs.length - 2 * 1024 * 1024);
        }
        process.stdout.write(stripConsoleLogPrefixes(text));
    };

    const onStderr = (chunk) => {
        const text = chunk.toString();
        state.lastActivityAt = Date.now();
        state.logs += text;
        if (state.logs.length > 4 * 1024 * 1024) {
            state.logs = state.logs.slice(state.logs.length - 2 * 1024 * 1024);
        }
        process.stderr.write(stripConsoleLogPrefixes(text));
    };

    launchProcess.stdout.on("data", onStdout);
    launchProcess.stderr.on("data", onStderr);
}

function startSimulatorLogStream(udid) {
    const args = [
        "simctl",
        "spawn",
        udid,
        "log",
        "stream",
        "--style",
        "compact",
        "--predicate",
        'process == "TestRunner"'
    ];

    console.log(`Streaming simulator logs: xcrun ${args.join(" ")}`);
    return cp.spawn("xcrun", args, {
        stdio: ["ignore", "pipe", "pipe"]
    });
}

function stripAnsi(text) {
    return text.replace(/\u001b\[[0-9;]*m/g, "");
}

function wireLogStreamOutput(logStreamProcess, state) {
    const buffers = {
        stdout: "",
        stderr: ""
    };

    const includeLine = (normalized) => {
        return normalized.includes("CONSOLE LOG:") ||
            normalized.includes("SPEC START:") ||
            normalized.includes("SPEC DONE:") ||
            normalized.includes("TKUnit:") ||
            normalized.includes("Application Start!") ||
            normalized.includes("SUCCESS:") ||
            normalized.includes("FAILURE:") ||
            normalized.includes("NativeScriptException") ||
            /Uncaught Exception/i.test(normalized) ||
            /EXC_BAD_ACCESS|EXC_CRASH|Abort trap/i.test(normalized) ||
            /heap corruption|malloc: \*\*\*/i.test(normalized) ||
            normalized.includes("Using metadata from pointer");
    };

    const fatalPattern = /NativeScriptException::OnUncaughtError|Uncaught Exception|heap corruption|EXC_BAD_ACCESS|EXC_CRASH|Abort trap|malloc: \*\*\*/i;

    const flushLine = (line, streamName) => {
        if (!line) {
            return;
        }

        const normalized = stripAnsi(line);
        state.recentRawLines.push(normalized);
        if (state.recentRawLines.length > 400) {
            state.recentRawLines.splice(0, state.recentRawLines.length - 400);
        }
        if (!includeLine(normalized)) {
            return;
        }

        state.logs += normalized + "\n";
        state.lastActivityAt = Date.now();
        if (state.logs.length > 4 * 1024 * 1024) {
            state.logs = state.logs.slice(state.logs.length - 2 * 1024 * 1024);
        }

        if (state.jasmineSummary == null) {
            const summary = parseJasmineSummary(normalized);
            if (summary) {
                state.jasmineSummary = summary;
            }
        }

        if (fatalPattern.test(normalized)) {
            state.fatalDetected = true;
        }

        const displayLine = stripConsoleLogPrefix(line);
        if (streamName === "stderr") {
            process.stderr.write(displayLine + "\n");
        } else {
            process.stdout.write(displayLine + "\n");
        }
    };

    const onChunk = (streamName, chunk) => {
        const key = streamName;
        buffers[key] += chunk.toString();
        const lines = buffers[key].split(/\r?\n/);
        buffers[key] = lines.pop() || "";
        for (const line of lines) {
            flushLine(line, streamName);
        }
    };

    logStreamProcess.stdout.on("data", (chunk) => onChunk("stdout", chunk));
    logStreamProcess.stderr.on("data", (chunk) => onChunk("stderr", chunk));
}

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

function readCompletedJunitFileIfPresent(udid) {
    const dataContainer = getAppDataContainerPath(udid);
    if (!dataContainer) {
        return null;
    }

    const junitPath = path.join(dataContainer, "Documents", "junit-result.xml");
    if (!fs.existsSync(junitPath)) {
        return null;
    }

    const xml = fs.readFileSync(junitPath, "utf8");
    if (!xml.includes("</testsuites>")) {
        return null;
    }

    return { xml, junitPath };
}

function waitForLaunchProcessClose(launchProcess, timeoutMs) {
    if (launchProcess.exitCode !== null && launchProcess.exitCode !== undefined) {
        return Promise.resolve({
            code: launchProcess.exitCode ?? 0,
            signal: launchProcess.signalCode || null
        });
    }

    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            if (launchProcess.exitCode === null || launchProcess.exitCode === undefined) {
                launchProcess.kill("SIGTERM");
            }
            reject(new Error(`Timed out waiting for TestRunner process to exit (${timeoutMs}ms).`));
        }, timeoutMs);

        launchProcess.on("close", (code, signal) => {
            clearTimeout(timer);
            resolve({ code: code ?? 0, signal: signal || null });
        });

        launchProcess.on("error", (error) => {
            clearTimeout(timer);
            reject(error);
        });
    });
}

async function waitForCompletedJunitOrLaunchExit(udid, launchProcess, timeoutMs, state) {
    let launchResult = null;
    launchProcess.on("close", (code, signal) => {
        launchResult = { code: code ?? 0, signal: signal || null };
    });

    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (state.jasmineSummary) {
            return { junitResult: null, launchResult, timedOut: false };
        }
        if (state.fatalDetected) {
            return { junitResult: null, launchResult, timedOut: false };
        }

        const junitResult = readCompletedJunitFileIfPresent(udid);
        if (junitResult) {
            return { junitResult, launchResult, timedOut: false };
        }

        if (Date.now() - state.lastActivityAt >= inactivityTimeoutMs) {
            const launchPid = extractLaunchPid(state.logs);
            const appRunning = isAppProcessRunning(udid, launchPid);
            if (appRunning === false) {
                return { junitResult: null, launchResult, timedOut: true, inactive: true };
            }
        }

        await sleep(250);
    }

    return { junitResult: null, launchResult, timedOut: true, inactive: false };
}

function extractLaunchPid(logs) {
    const match = logs.match(/com\.descendra\.TestRunner:\s*(\d+)/);
    return match ? Number(match[1]) : null;
}

function collectRecentSimulatorLogs(udid, pid) {
    const predicate = Number.isInteger(pid)
        ? `processID == ${pid}`
        : 'process == "TestRunner"';

    const result = run("xcrun", [
        "simctl",
        "spawn",
        udid,
        "log",
        "show",
        "--style",
        "compact",
        "--last",
        simulatorLogLookback,
        "--predicate",
        predicate
    ]);

    if (result.status !== 0) {
        return "";
    }

    const text = result.stdout || "";
    let lines = text
        .split(/\r?\n/)
        .filter(Boolean);

    // Keep only the most recent app run in this log window.
    const startIndex = (() => {
        for (let i = lines.length - 1; i >= 0; i--) {
            if (lines[i].includes("Application Start!")) {
                return i;
            }
        }
        return -1;
    })();

    if (startIndex >= 0) {
        lines = lines.slice(startIndex);
    }

    // Keep output manageable while preserving the most recent failures/summary.
    const maxLines = 400;
    if (lines.length > maxLines) {
        lines = lines.slice(lines.length - maxLines);
    }

    return lines.join("\n");
}

function getRecentRawLogTail(state, maxLines = 80) {
    if (!state.recentRawLines || state.recentRawLines.length === 0) {
        return "";
    }

    return state.recentRawLines.slice(-maxLines).join("\n");
}

function readJunitFileState(udid) {
    const dataContainer = getAppDataContainerPath(udid);
    if (!dataContainer) {
        return {
            dataContainer: null,
            junitPath: null,
            exists: false,
            complete: false,
            sizeBytes: 0,
            tail: "",
            documentsEntries: []
        };
    }

    const documentsPath = path.join(dataContainer, "Documents");
    const junitPath = path.join(documentsPath, "junit-result.xml");
    const documentsEntries = fs.existsSync(documentsPath)
        ? fs.readdirSync(documentsPath).sort()
        : [];

    if (!fs.existsSync(junitPath)) {
        return {
            dataContainer,
            junitPath,
            exists: false,
            complete: false,
            sizeBytes: 0,
            tail: "",
            documentsEntries
        };
    }

    const xml = fs.readFileSync(junitPath, "utf8");
    return {
        dataContainer,
        junitPath,
        exists: true,
        complete: xml.includes("</testsuites>"),
        sizeBytes: Buffer.byteLength(xml, "utf8"),
        tail: xml.slice(-2000),
        documentsEntries
    };
}

function collectSimulatorProcessSnapshot(udid) {
    const result = run("xcrun", ["simctl", "spawn", udid, "ps", "-axo", "pid,ppid,stat,etime,command"], {
        timeout: simctlQueryTimeoutMs
    });
    if (result.status !== 0) {
        return null;
    }

    const lines = (result.stdout || "")
        .split(/\r?\n/)
        .filter((line) => /PID|TestRunner|launchd_sim|UIKitApplication/i.test(line));

    return lines.join("\n");
}

function isAppProcessRunning(udid, pid) {
    const snapshot = collectSimulatorProcessSnapshot(udid);
    if (snapshot == null) {
        return null;
    }
    if (!snapshot) {
        return false;
    }

    const lines = snapshot.split(/\r?\n/).filter(Boolean);
    if (Number.isInteger(pid)) {
        const pidPattern = new RegExp(`\\b${pid}\\b`);
        return lines.some((line) => pidPattern.test(line) && /TestRunner|UIKitApplication/i.test(line));
    }

    return lines.some((line) => /TestRunner|com\\.descendra\\.TestRunner|UIKitApplication:com\\.descendra\\.TestRunner/i.test(line));
}

function formatInactivityDiagnostics(udid, state, pid) {
    const sections = [];
    const rawTail = getRecentRawLogTail(state);
    if (rawTail) {
        sections.push(`--- Recent raw simulator log lines ---\n${rawTail}`);
    }

    const simulatorLogs = collectRecentSimulatorLogs(udid, pid);
    if (simulatorLogs) {
        sections.push(`--- Recent TestRunner simulator logs ---\n${simulatorLogs}`);
    }

    const junitState = readJunitFileState(udid);
    const junitSummaryLines = [
        `data container: ${junitState.dataContainer || "<missing>"}`,
        `junit path: ${junitState.junitPath || "<missing>"}`,
        `junit exists: ${junitState.exists}`,
        `junit complete: ${junitState.complete}`,
        `junit size bytes: ${junitState.sizeBytes}`,
        `documents entries: ${junitState.documentsEntries.length > 0 ? junitState.documentsEntries.join(", ") : "<empty>"}`
    ];
    if (junitState.tail) {
        junitSummaryLines.push("junit tail:");
        junitSummaryLines.push(junitState.tail);
    }
    sections.push(`--- App container state ---\n${junitSummaryLines.join("\n")}`);

    const processSnapshot = collectSimulatorProcessSnapshot(udid);
    if (processSnapshot) {
        sections.push(`--- Simulator process snapshot ---\n${processSnapshot}`);
    }

    return sections.join("\n\n");
}

function parseJasmineSummary(logText) {
    const re = /(SUCCESS|FAILURE):\s+(\d+)\s+specs,\s+(\d+)\s+failure(?:s)?\,\s+(\d+)\s+skipped,\s+(\d+)\s+disabled/i;
    const match = logText.match(re);
    if (!match) {
        return null;
    }

    return {
        status: match[1].toUpperCase(),
        specs: Number(match[2]),
        failures: Number(match[3]),
        skipped: Number(match[4]),
        disabled: Number(match[5])
    };
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

function stopAppAndLaunchProcess(udid, launchProcess) {
    run("xcrun", ["simctl", "terminate", udid, bundleId]);

    if (launchProcess && launchProcess.exitCode === null) {
        launchProcess.kill("SIGTERM");
    }
}

function stopLogStream(logStreamProcess) {
    if (logStreamProcess && logStreamProcess.exitCode === null) {
        logStreamProcess.kill("SIGTERM");
    }
}

async function main() {
    const { junitOutPath, destinationArg } = parseArgs();

    if (!fs.existsSync(projectPath)) {
        console.error(`ERROR: Project not found: ${projectPath}`);
        process.exit(1);
    }

    fs.mkdirSync(resultsDir, { recursive: true });
    fs.mkdirSync(path.dirname(junitOutPath), { recursive: true });

    if (process.env.IOS_TEST_SKIP_BUILD !== "1") {
        ensureIOSSimulatorArtifacts();
    }

    const explicitDestination = process.env.IOS_DESTINATION || destinationArg;
    const resolved = resolveDestinationAndSimulator(explicitDestination);
    const destination = resolved.destination;
    const udid = resolved.simulator.udid;
    const swiftVersion = process.env.IOS_SWIFT_VERSION || "5.0";

    console.log(`Using destination: ${destination} (${resolved.simulator.name})`);
    bootSimulator(udid);

    const builtApp = buildTestRunnerApp(destination, swiftVersion);
    const appPath = builtApp.appPath;

    removeOldJunitFile(udid, { quiet: true });

    let shouldInstallApp = true;
    if (builtApp.reusedBuild && process.env.IOS_TEST_FORCE_INSTALL !== "1") {
        const installedAppPath = getInstalledAppPath(udid, { quiet: true });
        if (installedAppPath && fs.existsSync(installedAppPath)) {
            try {
                syncAppResources(path.join(installedAppPath, "app"));
                shouldInstallApp = false;
            } catch (error) {
                console.log(`Installed app sync failed (${error.message}); reinstalling app.`);
            }
        }
    }

    if (shouldInstallApp) {
        console.log(`Installing app: ${appPath}`);
        runAndRequireSuccess("xcrun", ["simctl", "install", udid, appPath]);
    } else {
        console.log("Installing app: skipped (reusing installed TestRunner).");
    }

    // Ensure stale result does not get picked up if a previous run already created the container after install.
    await waitForAppDataContainerPath(udid);
    removeOldJunitFile(udid);

    let launchProcess;
    let logStreamProcess;
    let exitCode = 0;
    const launchState = {
        logs: "",
        jasmineSummary: null,
        fatalDetected: false,
        lastActivityAt: Date.now(),
        recentRawLines: []
    };

    try {
        if (enableLiveLogStream) {
            logStreamProcess = startSimulatorLogStream(udid);
            wireLogStreamOutput(logStreamProcess, launchState);

            logStreamProcess.on("error", (error) => {
                console.error(`ERROR: Failed to start simulator log stream: ${error.message}`);
            });
        }

        const launchArgs = emitJunitLogs ? ["-logjunit"] : [];
        launchArgs.push("-skip-application-main");
        if (verboseSpecLogs) {
            launchArgs.push("-verbose-specs");
        }
        if (requestedTests.length > 0) {
            launchArgs.push("-tests", requestedTests);
        }
        if (requestedSpecs.length > 0) {
            launchArgs.push("-specs", requestedSpecs);
        }
        const useConsolePty = !enableLiveLogStream;
        const captureLaunchOutput = !enableLiveLogStream;
        launchProcess = startLaunchWithConsole(udid, launchArgs, useConsolePty, captureLaunchOutput);
        if (captureLaunchOutput) {
            wireOutput(launchProcess, launchState);
        }

        launchProcess.on("error", (error) => {
            console.error(`ERROR: Failed to launch app via simctl: ${error.message}`);
            process.exit(1);
        });

        console.log("Streaming TestRunner logs...");
        const { junitResult, launchResult, timedOut, inactive } = await waitForCompletedJunitOrLaunchExit(
            udid,
            launchProcess,
            testTimeoutMs,
            launchState
        );
        if (junitResult) {
            fs.writeFileSync(junitOutPath, junitResult.xml);

            const tests = extractCount(junitResult.xml, "tests");
            const failures = extractCount(junitResult.xml, "failures");
            const errors = extractCount(junitResult.xml, "errors");
            const skipped = extractCount(junitResult.xml, "skipped");

            console.log(`JUnit source: ${junitResult.junitPath}`);
            console.log(`JUnit copied to: ${junitOutPath}`);
            console.log(`Summary: tests=${tests}, failures=${failures}, errors=${errors}, skipped=${skipped}`);

            if (failures > 0 || errors > 0) {
                exitCode = 1;
            }
        } else {
            const closeResult = launchResult || { code: 0, signal: null };
            const launchPid = extractLaunchPid(launchState.logs);
            const inactivityDiagnostics = formatInactivityDiagnostics(udid, launchState, launchPid);
            if (inactivityDiagnostics) {
                launchState.logs += `\n${inactivityDiagnostics}`;
                console.log(`\n${inactivityDiagnostics}`);
            }

            const jasmineSummary = launchState.jasmineSummary || parseJasmineSummary(launchState.logs);
            const fatalPatterns = [
                /NativeScriptException::OnUncaughtError/,
                /Uncaught Exception/i,
                /Heap corruption detected/i,
                /EXC_CRASH|EXC_BAD_ACCESS|Abort trap/i
            ];
            const hasFatal = fatalPatterns.some((pattern) => pattern.test(launchState.logs));
            if (jasmineSummary) {
                console.log(
                    `Jasmine summary: ${jasmineSummary.status}: specs=${jasmineSummary.specs}, failures=${jasmineSummary.failures}, skipped=${jasmineSummary.skipped}, disabled=${jasmineSummary.disabled}`
                );
                if (jasmineSummary.failures > 0 || jasmineSummary.status === "FAILURE") {
                    exitCode = 1;
                }
            }

            if (timedOut && !jasmineSummary) {
                if (inactive) {
                    console.error(`ERROR: No test logs for ${inactivityTimeoutMs}ms; aborting as hung run.`);
                } else {
                    console.error(`ERROR: Timed out waiting for test completion (${testTimeoutMs}ms).`);
                }
                exitCode = 1;
            }

            if (!jasmineSummary) {
                console.error("ERROR: TestRunner exited without Jasmine summary or completed junit-result.xml.");
                exitCode = 1;
            }

            if (hasFatal || closeResult.code !== 0) {
                exitCode = 1;
            }

            console.log(`simctl launch command ended (code=${closeResult.code}${closeResult.signal ? `, signal=${closeResult.signal}` : ""}).`);
            if (!jasmineSummary) {
                console.log("No completed junit-result.xml found; relying on runtime logs.");
            }
        }
    } catch (error) {
        console.error(`ERROR: ${error.message}`);
        exitCode = 1;
    } finally {
        if (launchProcess) {
            stopAppAndLaunchProcess(udid, launchProcess);
            try {
                await waitForLaunchProcessClose(launchProcess, 5000);
            } catch (_) {
                // Ignore close timeout during cleanup.
            }
        }
        stopLogStream(logStreamProcess);
    }

    process.exit(exitCode);
}

main();
