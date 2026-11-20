#!/usr/bin/env node

const { spawnSync } = require("node:child_process");
const fs = require("node:fs/promises");
const path = require("node:path");

const archs = ["arm64", "x86_64"];
const buildTargets = [
  {
    dir: "build_iphoneos-arm64",
    sdk: "iphoneos",
    arch: "arm64",
    host: "aarch64-apple-darwin13",
    minVersionFlag: "-miphoneos-version-min=15.0",
  },
  {
    dir: "build_iphonesimulator-x86_64",
    sdk: "iphonesimulator",
    arch: "x86_64",
    host: "x86_64-apple-darwin13",
    minVersionFlag: "-mios-simulator-version-min=15.0",
  },
  {
    dir: "build_iphonesimulator-arm64",
    sdk: "iphonesimulator",
    arch: "arm64",
    host: "aarch64-apple-darwin13",
    minVersionFlag: "-mios-simulator-version-min=15.0",
  },
  {
    dir: "build_macosx-x86_64",
    sdk: "macosx",
    arch: "x86_64",
    host: "x86_64-apple-darwin13",
    minVersionFlag: "-mmacosx-version-min=11.0",
  },
  {
    dir: "build_macosx-arm64",
    sdk: "macosx",
    arch: "arm64",
    host: "aarch64-apple-darwin13",
    minVersionFlag: "-mmacosx-version-min=11.0",
  },
  {
    dir: "build_xros-arm64",
    sdk: "xros",
    arch: "arm64",
    host: "aarch64-apple-darwin13",
    minVersionFlag: "-target arm64-apple-xros2.0",
  },
  {
    dir: "build_xrsimulator-arm64",
    sdk: "xrsimulator",
    arch: "arm64",
    host: "aarch64-apple-darwin13",
    minVersionFlag: "-target arm64-apple-xros2.0-simulator",
  },
];

async function main() {
  const libffiDir = path.resolve(__dirname, "..", "libffi");
  const libffiPath = (...parts) => path.join(libffiDir, ...parts);
  const runtimeLibffiDir = path.resolve(__dirname, "..", "NativeScript", "libffi");
  const env = {
    ...process.env,
    CC: "clang",
    CFLAGS: "-w",
  };

  try {
    await fs.access(libffiPath("configure"));
  } catch {
    run("sh", ["./autogen.sh"], { cwd: libffiDir, env });
  }

  const skipGenerateSource =
    Boolean(process.env.SKIP_GENERATE_SOURCE) ||
    process.argv.includes("--skip-generate-source");

  if (!skipGenerateSource) {
    for (const target of buildTargets) {
      await configureBuildTarget(target, libffiPath, env);
    }
  } else {
    for (const target of buildTargets) {
      const configurePath = libffiPath(target.dir, "config.status");
      try {
        await fs.access(configurePath);
      } catch {
        throw new Error(
          `Missing ${configurePath}. Re-run without --skip-generate-source to configure build directories.`,
        );
      }
      run("make", ["libffi_convenience.la"], {
        cwd: libffiPath(target.dir),
        env: buildTargetEnv(env, target),
      });
    }
  }

  await prepareDir(libffiPath("prebuilt", "iphoneos-arm64", "include"));
  await fs.copyFile(
    libffiPath("build_iphoneos-arm64", "include", "ffi.h"),
    libffiPath("prebuilt", "iphoneos-arm64", "include", "ffi.h"),
  );
  await fs.copyFile(
    libffiPath("build_iphoneos-arm64", "include", "ffitarget.h"),
    libffiPath("prebuilt", "iphoneos-arm64", "include", "ffitarget.h"),
  );
  await fs.copyFile(
    libffiPath("build_iphoneos-arm64", ".libs", "libffi_convenience.a"),
    libffiPath("prebuilt", "iphoneos-arm64", "libffi.a"),
  );

  await prepareDir(libffiPath("prebuilt", "macosx-universal", "include"));
  await combineHeaders("macosx", libffiPath);
  run("lipo", [
    "-create",
    "-output",
    "prebuilt/macosx-universal/libffi.a",
    "build_macosx-x86_64/.libs/libffi_convenience.a",
    "build_macosx-arm64/.libs/libffi_convenience.a",
  ], { cwd: libffiDir, env });

  await prepareDir(
    libffiPath("prebuilt", "iphonesimulator-universal", "include"),
  );
  run("lipo", [
    "-create",
    "-output",
    "prebuilt/iphonesimulator-universal/libffi.a",
    "build_iphonesimulator-x86_64/.libs/libffi_convenience.a",
    "build_iphonesimulator-arm64/.libs/libffi_convenience.a",
  ], { cwd: libffiDir, env });
  await combineHeaders("iphonesimulator", libffiPath);

  await copySingleArchBuild({
    buildDir: "build_xros-arm64",
    outputDir: path.join("prebuilt", "xros-arm64"),
    libffiPath,
  });

  await copySingleArchBuild({
    buildDir: "build_xrsimulator-arm64",
    outputDir: path.join("prebuilt", "xrsimulator-arm64"),
    libffiPath,
  });

  await syncRuntimePrebuilts({
    runtimeLibffiDir,
    sourceLibffiPath: libffiPath,
    targets: [
      "iphoneos-arm64",
      "iphonesimulator-universal",
      "macosx-universal",
      "xros-arm64",
      "xrsimulator-arm64",
    ],
  });
}

async function configureBuildTarget(target, libffiPath, baseEnv) {
  const targetDir = libffiPath(target.dir);
  await fs.rm(targetDir, { recursive: true, force: true });
  await fs.mkdir(targetDir, { recursive: true });

  const env = buildTargetEnv(baseEnv, target);

  run("../configure", ["--disable-docs", "--disable-shared", "--host", target.host], {
    cwd: targetDir,
    env,
  });
  run("make", ["libffi_convenience.la"], { cwd: targetDir, env });
}

function buildTargetEnv(baseEnv, target) {
  return {
    ...baseEnv,
    CC: `xcrun -sdk ${target.sdk} clang -arch ${target.arch}`,
    LD: `xcrun -sdk ${target.sdk} ld -arch ${target.arch}`,
    CFLAGS: `${target.minVersionFlag} -w`,
    // Old libffi asm trips Apple clang's CFI handling on modern toolchains.
    // Force configure to skip CFI pseudo-op usage in generated config headers.
    gcc_cv_as_cfi_pseudo_op: "no",
  };
}

function run(command, args, options = {}) {
  console.log(`$ ${command} ${args.join(" ")}`);
  const result = spawnSync(command, args, {
    stdio: "inherit",
    env: options.env ?? process.env,
    cwd: options.cwd,
  });

  if (result.status !== 0) {
    throw new Error(
      `Command failed: ${command} ${args.join(" ")} (exit code ${result.status})${options.cwd ? ` in ${options.cwd}` : ""}`,
    );
  }
}

async function prepareDir(includePath) {
  const root = path.dirname(includePath);
  await fs.rm(root, { recursive: true, force: true });
  await fs.mkdir(includePath, { recursive: true });
}

async function syncRuntimePrebuilts({ runtimeLibffiDir, sourceLibffiPath, targets }) {
  for (const target of targets) {
    const destination = path.join(runtimeLibffiDir, target);
    await fs.rm(destination, { recursive: true, force: true });
    await fs.mkdir(path.dirname(destination), { recursive: true });
    await fs.cp(sourceLibffiPath("prebuilt", target), destination, {
      recursive: true,
    });
  }
}

async function copySingleArchBuild({ buildDir, outputDir, libffiPath }) {
  await prepareDir(libffiPath(outputDir, "include"));
  await fs.copyFile(
    libffiPath(buildDir, "include", "ffi.h"),
    libffiPath(outputDir, "include", "ffi.h"),
  );
  await fs.copyFile(
    libffiPath(buildDir, "include", "ffitarget.h"),
    libffiPath(outputDir, "include", "ffitarget.h"),
  );
  await fs.copyFile(
    libffiPath(buildDir, ".libs", "libffi_convenience.a"),
    libffiPath(outputDir, "libffi.a"),
  );
}

async function combineHeaders(target, libffiPath) {
  const ffi_h_arm64 = await fs.readFile(
    libffiPath(`build_${target}-${archs[0]}`, "include", "ffi.h"),
    "utf8",
  );
  const ffi_h_x86_64 = await fs.readFile(
    libffiPath(`build_${target}-${archs[1]}`, "include", "ffi.h"),
    "utf8",
  );
  const ffitarget_h_arm64 = await fs.readFile(
    libffiPath(`build_${target}-${archs[0]}`, "include", "ffitarget.h"),
    "utf8",
  );
  const ffitarget_h_x86_64 = await fs.readFile(
    libffiPath(`build_${target}-${archs[1]}`, "include", "ffitarget.h"),
    "utf8",
  );

  const ffi_h_universal = `// This file is generated by scripts/build_libffi.js
  // Merged from build_${target}-${archs[0]}/include/ffi.h and build_${target}-${archs[1]}/include/ffi.h
  
  #if defined(__aarch64__)
  ${ffi_h_arm64}
  #elif defined(__x86_64__)
  ${ffi_h_x86_64}
  #else
  #error "Unsupported architecture"
  #endif
  `;

  const ffitarget_h_universal = `// This file is generated by scripts/build_libffi.js
  // Merged from build_${target}-${archs[0]}/include/ffitarget.h and build_${target}-${archs[1]}/include/ffitarget.h
  
  #if defined(__aarch64__)
  ${ffitarget_h_arm64}
  #elif defined(__x86_64__)
  ${ffitarget_h_x86_64}
  #else
  #error "Unsupported architecture"
  #endif
  `;

  await fs.writeFile(
    libffiPath("prebuilt", `${target}-universal`, "include", "ffi.h"),
    ffi_h_universal,
  );
  await fs.writeFile(
    libffiPath("prebuilt", `${target}-universal`, "include", "ffitarget.h"),
    ffitarget_h_universal,
  );
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
