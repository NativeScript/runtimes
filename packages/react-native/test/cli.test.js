const fs = require("fs");
const os = require("os");
const path = require("path");
const assert = require("assert");
const { configure } = require("../cli/configure");
const { generateMetadata } = require("../cli/generate-metadata");

const projectRoot = fs.mkdtempSync(path.join(os.tmpdir(), "ns-rn-cli-"));
const previousCwd = process.cwd();

try {
  process.chdir(projectRoot);
  fs.writeFileSync(
    path.join(projectRoot, "package.json"),
    JSON.stringify(
      {
        dependencies: {
          "@nativescript/react-native": "0.0.1",
        },
        nativeScriptReactNative: {
          metadata: {
            includePods: ["SomeObjCSDK"],
            includeSystemFrameworks: ["UIKit"],
          },
        },
      },
      null,
      2,
    ),
  );
  fs.mkdirSync(path.join(projectRoot, "ios"));
  fs.writeFileSync(
    path.join(projectRoot, "ios", "Podfile.properties.json"),
    JSON.stringify({ newArchEnabled: "true", "expo.jsEngine": "hermes" }),
  );

  configure(["configure"]);
  const babelOnce = fs.readFileSync(
    path.join(projectRoot, "babel.config.js"),
    "utf8",
  );
  configure(["configure"]);
  const babelTwice = fs.readFileSync(
    path.join(projectRoot, "babel.config.js"),
    "utf8",
  );
  assert.strictEqual(babelTwice, babelOnce);
  assert.match(babelTwice, /bundleMode:\s*true/);
  assert.ok(fs.existsSync(path.join(projectRoot, "metro.config.js")));

  generateMetadata(["--check"]);
  assert.ok(
    fs.existsSync(path.join(projectRoot, "nativescript.react-native.json")),
  );
  console.log("cli tests passed");
} finally {
  process.chdir(previousCwd);
  fs.rmSync(projectRoot, { recursive: true, force: true });
}
