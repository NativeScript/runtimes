const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const SHA_PATCH_MARKER = Symbol.for(
  "@nativescript/react-native/worklets-generated-sha",
);

function isGeneratedWorkletPath(filePath) {
  return (
    typeof filePath === "string" &&
    /(?:^|[\\/])react-native-worklets[\\/]\.worklets[\\/]/.test(filePath)
  );
}

function installGeneratedWorkletShaFallback(projectRoot = process.cwd()) {
  let dependencyGraphPath;
  try {
    dependencyGraphPath = require.resolve(
      "metro/private/node-haste/DependencyGraph",
      { paths: [projectRoot] },
    );
  } catch {
    const metroPackagePath = require.resolve("metro/package.json", {
      paths: [projectRoot],
    });
    dependencyGraphPath = path.join(
      path.dirname(metroPackagePath),
      "src/node-haste/DependencyGraph.js",
    );
  }
  const dependencyGraphModule = require(dependencyGraphPath);
  const DependencyGraph =
    dependencyGraphModule.default || dependencyGraphModule;
  const prototype = DependencyGraph?.prototype;
  if (!prototype || typeof prototype.getOrComputeSha1 !== "function") {
    throw new Error(
      "@nativescript/react-native could not install the Worklets Bundle Mode SHA fallback for this Metro version.",
    );
  }
  if (prototype[SHA_PATCH_MARKER]) {
    return;
  }

  const originalGetOrComputeSha1 = prototype.getOrComputeSha1;
  prototype.getOrComputeSha1 = async function getOrComputeSha1(filePath) {
    if (isGeneratedWorkletPath(filePath)) {
      const content = await fs.promises.readFile(filePath);
      return {
        sha1: crypto.createHash("sha1").update(content).digest("hex"),
        content,
      };
    }
    return originalGetOrComputeSha1.call(this, filePath);
  };
  Object.defineProperty(prototype, SHA_PATCH_MARKER, { value: true });
}

function withNativeScriptMetroConfig(config) {
  if (!config || typeof config !== "object") {
    throw new TypeError("withNativeScriptMetroConfig requires a Metro config");
  }
  installGeneratedWorkletShaFallback(config.projectRoot || process.cwd());
  const {
    getBundleModeMetroConfig,
  } = require("react-native-worklets/bundleMode");
  if (typeof getBundleModeMetroConfig !== "function") {
    throw new Error(
      "react-native-worklets does not expose getBundleModeMetroConfig; install a Bundle Mode-compatible release.",
    );
  }
  return getBundleModeMetroConfig(config);
}

module.exports = {
  installGeneratedWorkletShaFallback,
  isGeneratedWorkletPath,
  withNativeScriptMetroConfig,
};
