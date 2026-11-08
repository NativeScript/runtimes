const process = require("node:process");
const semver = require("semver");

let currentVersion = process.env.NPM_VERSION;
if (!currentVersion) {
  const cmdArgs = process.argv.slice(2);
  const target = cmdArgs[0];
  const packageJsonByTarget = {
    ios: "../packages/ios/package.json",
    "ios-v8": "../packages/ios-v8/package.json",
    "ios-hermes": "../packages/ios-hermes/package.json",
    "ios-jsc": "../packages/ios-jsc/package.json",
    "ios-quickjs": "../packages/ios-quickjs/package.json",
    macos: "../packages/macos/package.json",
    visionos: "../packages/visionos/package.json",
    "ios-node-api": "../packages/ios-node-api/package.json",
    "macos-node-api": "../packages/macos-node-api/package.json",
    "objc-node-api": "../packages/objc-node-api/package.json",
    "react-native": "../packages/react-native/package.json",
    android: "../platforms/android/package.json",
    "android-v8": "../packages/android-v8/package.json",
    "android-hermes": "../packages/android-hermes/package.json",
    "android-jsc": "../packages/android-jsc/package.json",
    "android-quickjs-ng": "../packages/android-quickjs-ng/package.json",
    "android-primjs": "../packages/android-primjs/package.json",
  };

  if (!packageJsonByTarget[target]) {
    throw new Error(
      `Unknown target "${target}". Expected one of ${Object.keys(packageJsonByTarget)
        .map((name) => `"${name}"`)
        .join(", ")}.`,
    );
  }

  currentVersion = require(packageJsonByTarget[target]).version;
}

function validateNpmTag(version) {
  const parsed = semver.parse(version);
  return (
    parsed.prerelease.length === 0 ||
    (typeof parsed.prerelease[0] === "string" &&
      /^[a-zA-Z][0-9A-Za-z-]*$/.test(parsed.prerelease[0]))
  );
}

function getNpmTag(version) {
  if (!validateNpmTag(version)) {
    throw new Error(`Invalid npm tag "${version}"`);
  }
  const parsed = semver.parse(version);
  return parsed.prerelease[0] || "latest";
}

console.log(getNpmTag(currentVersion));
