#!/usr/bin/env node
/*
 * Builds every JS graph the app ships into one directory, for the static
 * binding generator and the metadata seed to read.
 *
 * Why this exists rather than one `react-native bundle` per entry: an app's
 * secondary entry points (workers) are separate module graphs that appear in no
 * other bundle, so each has to be built -- but a CLI invocation per entry pays a
 * fresh Node process, a fresh Metro config load and a cold transform cache every
 * time. Loading the config once and reusing it across entries is what React
 * Native's own tooling does, and what a worker bundler does too.
 *
 * Everything is resolved from the *app's* root, never from this package: the
 * Metro, React Native and community-cli-plugin that build the analysis bundles
 * must be the ones that build the shipping bundles, or the analysis describes a
 * program the device never runs.
 *
 * Usage:
 *   bundle-for-analysis.js --project-root <dir> --out <dir> --entry <file>
 *                          [--worker <id>=<file>]… [--resolver-option k=v]…
 */

const fs = require("fs");
const path = require("path");

function reqFrom(fromDir, request) {
  return require(require.resolve(request, { paths: [fromDir] }));
}

function parseArgs(argv) {
  const args = { workers: [], resolverOptions: [] };
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i];
    if (key === "--project-root") args.projectRoot = argv[++i];
    else if (key === "--out") args.out = argv[++i];
    else if (key === "--entry") args.entry = argv[++i];
    else if (key === "--resolver-option") args.resolverOptions.push(argv[++i]);
    else if (key === "--worker") {
      const value = argv[++i];
      const eq = value.indexOf("=");
      if (eq > 0) args.workers.push({ id: value.slice(0, eq), file: value.slice(eq + 1) });
    }
  }
  return args;
}

async function loadMetroConfig(projectRoot, platform) {
  /* The same path @react-native/community-cli-plugin takes: its own loader when
   * present, else Metro's. Both read the app's metro.config.js. */
  try {
    const { loadConfigAsync } = reqFrom(projectRoot, "@react-native/community-cli-plugin");
    if (typeof loadConfigAsync === "function") {
      return await loadConfigAsync({ root: projectRoot, platform }, { platform });
    }
  } catch (_) {
    /* fall through to Metro's own loader */
  }
  const metro = reqFrom(projectRoot, "metro");
  return await metro.loadConfig({ cwd: projectRoot });
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.projectRoot || !args.out || !args.entry) {
    console.error("bundle-for-analysis: --project-root, --out and --entry are required");
    process.exit(2);
  }

  const { unstable_buildBundleWithConfig } = reqFrom(
    args.projectRoot,
    "@react-native/community-cli-plugin"
  );
  if (typeof unstable_buildBundleWithConfig !== "function") {
    /* Older React Native. The caller falls back to one CLI run per entry, which
     * is slower but produces the same bundles. */
    console.error("bundle-for-analysis: unstable_buildBundleWithConfig is unavailable");
    process.exit(3);
  }

  fs.mkdirSync(args.out, { recursive: true });
  const config = await loadMetroConfig(args.projectRoot, "android");

  const build = async (label, entryFile, outFile, resolverOption) =>
    unstable_buildBundleWithConfig(
      {
        platform: "android",
        dev: false,
        /* Readable output: the analysis reads class and member names out of
         * this, and a minified graph hides most of them. */
        minify: false,
        entryFile,
        bundleOutput: outFile,
        assetsDest: path.join(args.out, "assets"),
        /* Keeps `class X extends Y {}` intact. React Native's Babel preset
         * downlevels classes into a helper call otherwise, and a downlevelled
         * class is not something static analysis can recognise as a subclass. */
        unstableTransformProfile: "hermes-stable",
        ...(resolverOption ? { resolverOption } : {}),
      },
      config
    ).catch((e) => {
      throw new Error(`bundle-for-analysis: ${label} failed: ${e.stack || e.message}`);
    });

  await build("app entry", args.entry, path.join(args.out, "index.android.bundle"), null);

  for (const worker of args.workers) {
    const safe = worker.id.replace(/[^A-Za-z0-9]/g, "_");
    await build(
      `worker ${worker.id}`,
      worker.file,
      path.join(args.out, `worker-${safe}.js`),
      /* Worker graphs resolve `react-native` to the worker shim; without the
       * flag this would analyse a graph the device never runs. */
      args.resolverOptions.length > 0 ? args.resolverOptions : null
    );
  }

  console.error(
    `bundle-for-analysis: built ${1 + args.workers.length} bundle(s) in one Metro session`
  );
}

main().catch((e) => {
  console.error(e.message || e);
  process.exit(1);
});
