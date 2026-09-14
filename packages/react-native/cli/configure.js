#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const {
  ensureBabelPlugin,
  ensureMetroConfig,
  ensureMetadataConfig,
  normalizeMetadataOptions,
} = require("../plugin/withNativeScriptReactNative");

function readJson(filePath) {
  if (!fs.existsSync(filePath)) {
    return undefined;
  }
  return JSON.parse(fs.readFileSync(filePath, "utf8"));
}

function projectRoot() {
  return process.cwd();
}

function printNextSteps() {
  console.log("NativeScript React Native configuration checked.");
  console.log("Next steps:");
  console.log("  cd ios");
  console.log("  RCT_NEW_ARCH_ENABLED=1 USE_HERMES=1 pod install");
}

function configure(argv = process.argv.slice(2)) {
  const command = argv[0];
  if (command === "generate-metadata") {
    require("./generate-metadata").generateMetadata(argv.slice(1));
    return;
  }
  if (command && command !== "configure") {
    console.error(`Unknown command: ${command}`);
    process.exitCode = 1;
    return;
  }

  const root = projectRoot();
  const packageJson = readJson(path.join(root, "package.json"));
  if (!packageJson) {
    console.error("package.json was not found in the current directory.");
    process.exitCode = 1;
    return;
  }

  const nativeScriptOptions = packageJson.nativeScriptReactNative || {};
  const bundleMode = nativeScriptOptions.workletsBundleMode !== false;
  ensureBabelPlugin(root, { bundleMode });
  if (bundleMode) {
    ensureMetroConfig(root);
  }
  ensureMetadataConfig(
    root,
    normalizeMetadataOptions(packageJson.nativeScriptReactNative || {}),
  );

  const dependencies = {
    ...(packageJson.dependencies || {}),
    ...(packageJson.devDependencies || {}),
  };
  if (!dependencies["@nativescript/react-native"]) {
    console.warn(
      "Warning: @nativescript/react-native is not listed in package.json dependencies.",
    );
  }

  const iosPropertiesPath = path.join(root, "ios", "Podfile.properties.json");
  const iosProperties = readJson(iosPropertiesPath);
  if (iosProperties) {
    if (
      iosProperties["expo.jsEngine"] &&
      iosProperties["expo.jsEngine"] !== "hermes"
    ) {
      console.warn("Warning: iOS jsEngine is not Hermes.");
    }
    if (
      iosProperties.newArchEnabled !== "true" &&
      iosProperties.newArchEnabled !== true
    ) {
      console.warn(
        "Warning: React Native New Architecture is not enabled for iOS.",
      );
    }
  }

  printNextSteps();
}

if (require.main === module) {
  configure();
}

module.exports = {
  configure,
};
