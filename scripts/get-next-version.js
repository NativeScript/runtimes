const child_process = require("node:child_process");
const process = require("node:process");
const fs = require("node:fs");
const semver = require("semver");
const dayjs = require("dayjs");

/** @type {string} */
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

if (!currentVersion) {
  throw new Error("Invalid current version");
}
const currentTag = process.env.NPM_TAG || "next";
const runID = process.env.GITHUB_RUN_ID || 0;

let prPrerelease = "";

if (currentTag === "pr" && process.env.GITHUB_EVENT_PATH) {
  try {
    const ev = JSON.parse(
      fs.readFileSync(process.env.GITHUB_EVENT_PATH, "utf8"),
    );
    const prNum = ev.pull_request.number;
    // add extra PR number to version-pr.PRNUM-....
    prPrerelease = `${prNum}-`;
  } catch {
    // don't add pr prerelease
  }
}

const preRelease = `${currentTag}.${prPrerelease}${dayjs().format(
  "YYYY-MM-DD",
)}-${runID}`;

function normalizeVersionCandidate(candidate) {
  if (!candidate) {
    return null;
  }

  const trimmed = candidate.trim();
  if (!trimmed) {
    return null;
  }

  const parsedDirect = semver.parse(trimmed);
  if (parsedDirect) {
    return parsedDirect.version;
  }

  const withoutLeadingV = trimmed.replace(/^v/, "");
  const parsedWithoutV = semver.parse(withoutLeadingV);
  if (parsedWithoutV) {
    return parsedWithoutV.version;
  }

  // Supports tag formats such as refs/tags/v1.2.3, pkg@1.2.3, etc.
  const extracted = trimmed.match(
    /\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?/,
  );
  if (extracted && semver.parse(extracted[0])) {
    return extracted[0];
  }

  return null;
}

function getLastTagVersion() {
  const explicitTagVersion = normalizeVersionCandidate(
    process.env.LAST_TAGGED_VERSION,
  );
  if (explicitTagVersion) {
    return explicitTagVersion;
  }

  const gitDescribe = child_process.spawnSync("git", [
    "describe",
    "--tags",
    "--abbrev=0",
    "--match=v*",
  ]);

  const describedTag = normalizeVersionCandidate(gitDescribe.stdout.toString());
  if (describedTag) {
    return describedTag;
  }

  // If v* matching tags are unavailable, try any tag name.
  const gitDescribeAnyTag = child_process.spawnSync("git", [
    "describe",
    "--tags",
    "--abbrev=0",
  ]);

  const anyTag = normalizeVersionCandidate(gitDescribeAnyTag.stdout.toString());
  if (anyTag) {
    return anyTag;
  }

  return "0.0.0";
}

const lastTagVersion = getLastTagVersion();

function setPreRelease(version) {
  const parsed = semver.parse(version);
  return semver.parse(
    `${parsed.major}.${parsed.minor}.${parsed.patch}-${preRelease}`,
  );
}

let nextVersion = setPreRelease(currentVersion);

if (!nextVersion) {
  throw new Error("Invalid next version");
}

if (semver.compare(currentVersion, lastTagVersion) <= 0) {
  // next version is older than current version
  nextVersion = setPreRelease(semver.parse(lastTagVersion).inc("patch"));
}

if (!nextVersion) {
  throw new Error("Invalid next version");
}

console.log(nextVersion.format());
