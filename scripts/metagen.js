#!/usr/bin/env node

const { spawnSync } = require("node:child_process");
const fsp = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");

const COMMON_FRAMEWORKS = [
  "Foundation",
  "CoreFoundation",
  "CoreGraphics",
  "CoreText",
  "QuartzCore",
  "WebKit",
  "Metal",
  "MetalKit",
  "MetalPerformanceShaders",
  "SpriteKit",
  "SceneKit",
  "ModelIO",
  "GameController",
  "GameKit",
  "GameplayKit",
  "CloudKit",
  "Intents",
  "Contacts",
  "CoreSpotlight",
  "JavaScriptCore",
  "UserNotifications",
  "CoreHaptics",
  "EventKit",
  "AddressBook",
  "MapKit",
  "CoreServices",
  "CoreMedia",
  "CoreVideo",
  "CoreImage",
  "CoreData",
  "CoreMIDI",
  "CoreML",
  "CoreBluetooth",
  "CoreLocation",
  "CoreMotion",
  "MLCompute",
  "AudioToolbox",
  "AudioUnit",
  "AVFoundation",
  "NaturalLanguage",
  "Symbols",
];

const MACOS_FRAMEWORKS = ["AppKit", "CoreAudio", "ScreenCaptureKit"];
const IOS_FRAMEWORKS = ["UIKit"];

function getSDKPath(platform) {
  const output = spawnSync("xcrun", ["--sdk", platform, "--show-sdk-path"], {
    stdio: ["ignore", "pipe", "inherit"],
    encoding: "utf8",
  });

  if (output.status !== 0) {
    throw new Error(`Failed to get SDK path for ${platform}`);
  }

  return output.stdout.trim();
}

const sdkVersionCache = new Map();

function getSDKVersion(platform) {
  const cached = sdkVersionCache.get(platform);
  if (cached) {
    return cached;
  }

  const output = spawnSync("xcrun", ["--sdk", platform, "--show-sdk-version"], {
    stdio: ["ignore", "pipe", "inherit"],
    encoding: "utf8",
  });

  if (output.status !== 0) {
    throw new Error(`Failed to get SDK version for ${platform}`);
  }

  const version = output.stdout.trim();
  sdkVersionCache.set(platform, version);
  return version;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function findLLDB() {
  const xcrun = spawnSync("xcrun", ["--find", "lldb"], {
    stdio: ["ignore", "pipe", "ignore"],
    encoding: "utf8",
  });

  if (xcrun.status === 0) {
    const candidate = xcrun.stdout.trim();
    if (candidate) {
      return candidate;
    }
  }

  return "lldb";
}

function formatLLDBOutput(output) {
  return [output.stdout, output.stderr].filter(Boolean).join("");
}

function runLLDBBacktrace(exec, args) {
  const lldb = findLLDB();
  const output = spawnSync(
    lldb,
    [
      "--batch",
      "--no-lldbinit",
      "-o",
      "run",
      "-k",
      "bt",
      "-k",
      "thread backtrace all",
      "--",
      exec,
      ...args,
    ],
    {
      stdio: ["ignore", "pipe", "pipe"],
      encoding: "utf8",
      timeout: 120000,
    },
  );

  return {
    output,
  };
}

function formatCrashReport(reportPath, content) {
  try {
    const lines = content.split(/\r?\n/);
    const header = JSON.parse(lines[0]);
    const body = JSON.parse(lines.slice(1).join("\n"));
    const faultingThreadIndex = body.faultingThread ?? 0;
    const thread = body.threads?.[faultingThreadIndex];
    const usedImages = body.usedImages ?? [];

    const imageNames = new Map();
    for (const image of usedImages) {
      if (typeof image.imageIndex === "number" && image.name) {
        imageNames.set(image.imageIndex, image.name);
      }
    }

    const linesOut = [
      `Crash report: ${reportPath}`,
      `Process: ${body.procName ?? header.app_name ?? "unknown"}`,
      `Timestamp: ${header.timestamp ?? body.captureTime ?? "unknown"}`,
      `Exception: ${body.exception?.type ?? "unknown"} ${body.exception?.signal ?? ""}`.trim(),
      `Termination: ${body.termination?.indicator ?? "unknown"}`,
      `Faulting thread: ${faultingThreadIndex}`,
    ];

    const frames = thread?.frames ?? [];
    if (frames.length > 0) {
      linesOut.push("Faulting thread frames:");
      for (const [index, frame] of frames.entries()) {
        const image = imageNames.get(frame.imageIndex) ?? `image#${frame.imageIndex ?? "?"}`;
        const symbol = frame.symbol ?? "<unknown>";
        const offset =
          typeof frame.symbolLocation === "number"
            ? ` + ${frame.symbolLocation}`
            : typeof frame.imageOffset === "number"
              ? ` @ ${frame.imageOffset}`
              : "";
        linesOut.push(`  #${index} ${image} ${symbol}${offset}`);
      }
    }

    return linesOut.join("\n");
  } catch (error) {
    return [
      `Crash report: ${reportPath}`,
      "Unable to parse crash report as JSON; showing raw excerpt instead.",
      content.slice(0, 4000),
    ].join("\n");
  }
}

async function findRecentCrashReport(exec, startedAtMs) {
  const reportsDir = path.join(os.homedir(), "Library", "Logs", "DiagnosticReports");
  const execName = path.basename(exec);
  const deadline = Date.now() + 10000;

  while (Date.now() < deadline) {
    let entries = [];
    try {
      entries = await fsp.readdir(reportsDir, { withFileTypes: true });
    } catch {
      return null;
    }

    const candidates = [];
    for (const entry of entries) {
      if (!entry.isFile() || !entry.name.endsWith(".ips")) {
        continue;
      }
      if (!entry.name.startsWith(execName)) {
        continue;
      }

      const reportPath = path.join(reportsDir, entry.name);
      try {
        const stat = await fsp.stat(reportPath);
        if (stat.mtimeMs + 1000 < startedAtMs) {
          continue;
        }
        candidates.push({ reportPath, mtimeMs: stat.mtimeMs });
      } catch {
        // Ignore reports that disappear while polling.
      }
    }

    candidates.sort((a, b) => b.mtimeMs - a.mtimeMs);
    if (candidates.length > 0) {
      const content = await fsp.readFile(candidates[0].reportPath, "utf8");
      return {
        reportPath: candidates[0].reportPath,
        content,
      };
    }

    await sleep(1000);
  }

  return null;
}

async function emitCrashDiagnostics(exec, args, startedAtMs) {
  console.error("Attempting crash diagnostics...");

  const lldbResult = runLLDBBacktrace(exec, args);
  const lldbOutput = formatLLDBOutput(lldbResult.output);

  if (lldbOutput.trim()) {
    console.error("LLDB output:");
    console.error(lldbOutput.trimEnd());
  } else {
    console.error("LLDB produced no output.");
  }

  const attachDenied =
    lldbOutput.includes("Not allowed to attach to process") ||
    lldbOutput.includes("attach failed");
  if (lldbResult.output.error) {
    console.error(`LLDB error: ${lldbResult.output.error.message}`);
  }
  if (attachDenied) {
    console.error("LLDB attach was denied by macOS; falling back to DiagnosticReports.");
  }

  const crashReport = await findRecentCrashReport(exec, startedAtMs);
  if (!crashReport) {
    console.error("No recent macOS crash report found in ~/Library/Logs/DiagnosticReports.");
    return;
  }

  console.error(formatCrashReport(crashReport.reportPath, crashReport.content));
}

const sdks = {
  macos: {
    path: getSDKPath("macosx"),
    frameworks: [...COMMON_FRAMEWORKS, ...MACOS_FRAMEWORKS],
    targets: {
      x86_64: "x86_64-apple-macos11.0",
      arm64: "arm64-apple-macos11.0",
    },
  },
  ios: {
    path: getSDKPath("iphoneos"),
    frameworks: [...COMMON_FRAMEWORKS, ...IOS_FRAMEWORKS],
    targets: {
      arm64: `arm64-apple-ios${getSDKVersion("iphoneos")}`,
    },
    tnsTarget: "ios-arm64",
  },
  "ios-sim": {
    path: getSDKPath("iphonesimulator"),
    frameworks: [...COMMON_FRAMEWORKS, ...IOS_FRAMEWORKS],
    targets: {
      x86_64: `x86_64-apple-ios${getSDKVersion("iphonesimulator")}-simulator`,
      arm64: `arm64-apple-ios${getSDKVersion("iphonesimulator")}-simulator`,
    },
    tnsTarget: "ios-arm64_x86_64-simulator",
  },
  catalyst: {
    path: getSDKPath("iphoneos"),
    frameworks: [...COMMON_FRAMEWORKS, ...MACOS_FRAMEWORKS, ...IOS_FRAMEWORKS],
    targets: {
      x86_64: `x86_64-apple-ios${getSDKVersion("iphoneos")}-macabi`,
      arm64: `arm64-apple-ios${getSDKVersion("iphoneos")}-macabi`,
    },
    tnsTarget: "ios-arm64_x86_64-maccatalyst",
  },
  visionos: {
    path: getSDKPath("xros"),
    frameworks: [...COMMON_FRAMEWORKS],
    targets: {
      arm64: "arm64-apple-xros26.0",
    },
    tnsTarget: "xros-arm64",
  },
  "visionos-sim": {
    path: getSDKPath("xrsimulator"),
    frameworks: [...COMMON_FRAMEWORKS],
    targets: {
      arm64: "arm64-apple-xros26.0-simulator",
    },
    tnsTarget: "xros-arm64_x86_64-simulator",
  },
};

async function main() {
  const sdkName = process.argv[2] ?? "macos";
  const sdk = sdks[sdkName];

  if (!sdk) {
    throw new Error(`Invalid platform: ${sdkName}`);
  }

  const requestedArchs = new Set(
    (process.env.METAGEN_ARCHS || "")
      .split(",")
      .map((arch) => arch.trim())
      .filter(Boolean),
  );
  const targetArchs = Object.keys(sdk.targets).filter((arch) => {
    return requestedArchs.size === 0 || requestedArchs.has(arch);
  });
  if (targetArchs.length === 0) {
    throw new Error(
      `No matching metadata architectures for ${sdkName}; requested ${Array.from(requestedArchs).join(",")}`,
    );
  }

  const typesDir = path.resolve(__dirname, "..", "packages", sdkName, "types");
  const metadataJsonDir = path.resolve(
    __dirname,
    "..",
    "packages",
    sdkName,
    "metadata-json",
  );
  const metadataDir = path.resolve(__dirname, "..", "metadata-generator", "metadata");
  const signatureBindingsPath =
    process.env.NS_SIGNATURE_BINDINGS_CPP_PATH ||
    process.env.TNS_SIGNATURE_BINDINGS_CPP_PATH ||
    path.resolve(__dirname, "..", "NativeScript", "ffi", "objc", "shared", "GeneratedSignatureDispatch.inc");
  await fsp.rm(typesDir, { recursive: true, force: true });
  await fsp.mkdir(typesDir, { recursive: true });
  await fsp.rm(metadataJsonDir, { recursive: true, force: true });
  await fsp.mkdir(metadataDir, { recursive: true });
  await fsp.mkdir(path.dirname(signatureBindingsPath), { recursive: true });

  for (const arch of targetArchs) {
    // Use the matching executable when available. The generator process may
    // use the host architecture while Clang emits metadata for another target.
    const preferredArch = arch;
    const preferredExec = path.resolve(
      __dirname,
      "..",
      "metadata-generator",
      "dist",
      preferredArch,
      "bin",
      "objc-metadata-generator",
    );
    const fallbackExec = path.resolve(
      __dirname,
      "..",
      "metadata-generator",
      "dist",
      process.arch === "x64" ? "x86_64" : process.arch,
      "bin",
      "objc-metadata-generator",
    );

    let exec;
    try {
      await fsp.access(preferredExec);
      exec = preferredExec;
    } catch {
      exec = fallbackExec;
    }

    const args = [
      `types=${typesDir}`,
      `json=${metadataJsonDir}`,
      ...(sdkName === "macos"
        ? [
            "ts-index-mode=frameworks-list",
            "ts-index-frameworks=Foundation,AppKit",
          ]
        : ["ts-index-mode=all"]),
      "-verbose",
      "-output-bin",
      path.resolve(
        metadataDir,
        `metadata.${sdkName}.${arch}.nsmd`,
      ),
      "-output-umbrella",
      path.resolve(
        metadataDir,
        `metadata.${sdkName}.${arch}.h`,
      ),
      "-output-signature-bindings-cpp",
      signatureBindingsPath,
      "Xclang",
      "-isysroot",
      sdk.path,
      "-std=gnu99",
      "-target",
      sdk.targets[arch],
    ];

    // for (const framework of sdk.frameworks) {
    //   args.push(`framework=${framework}`);
    // }

    console.log(`$ MetadataGenerator ${args.join(" ")}`);

    const startedAtMs = Date.now();
    const output = spawnSync(exec, args, {
      stdio: "inherit",
    });

    if (output.status !== 0) {
      if (process.platform === "darwin" && output.signal) {
        await emitCrashDiagnostics(exec, args, startedAtMs);
      }
      console.error(`Failed to generate metadata for ${sdkName} ${arch}`);
      console.error(`Command: ${exec} ${args.join(" ")}`);
      console.error(`Exit code: ${output.status}`);
      if (output.signal) {
        console.error(`Killed by signal: ${output.signal}`);
      }
      if (output.error) {
        console.error(`Spawn error: ${output.error.message}`);
      }
      throw new Error("Failed to generate metadata");
    }
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
