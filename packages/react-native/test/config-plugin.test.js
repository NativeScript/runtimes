const fs = require("fs");
const os = require("os");
const path = require("path");
const assert = require("assert");
const {
  ensureBabelPlugin,
  ensureMetroConfig,
  ensureMetadataConfig,
  normalizeMetadataOptions,
} = require("../plugin/withNativeScriptReactNative");

function withTempProject(callback) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ns-rn-plugin-"));
  try {
    callback(dir);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

withTempProject((projectRoot) => {
  ensureBabelPlugin(projectRoot);
  ensureBabelPlugin(projectRoot);
  ensureMetroConfig(projectRoot);
  ensureMetroConfig(projectRoot);
  const source = fs.readFileSync(
    path.join(projectRoot, "babel.config.js"),
    "utf8",
  );
  assert.strictEqual(
    (source.match(/@nativescript\/react-native\/babel-plugin/g) || []).length,
    1,
  );
  assert.strictEqual(
    (source.match(/react-native-worklets\/plugin/g) || []).length,
    1,
  );
  assert.match(source, /bundleMode:\s*true/);
  assert.match(source, /strictGlobal:\s*true/);
  const metroSource = fs.readFileSync(
    path.join(projectRoot, "metro.config.js"),
    "utf8",
  );
  assert.match(metroSource, /withNativeScriptMetroConfig/);
});

withTempProject((projectRoot) => {
  fs.writeFileSync(
    path.join(projectRoot, "babel.config.js"),
    [
      "module.exports = function(api) {",
      "  api.cache(true);",
      "  return {",
      "    presets: ['babel-preset-expo'],",
      "  };",
      "};",
      "",
    ].join("\n"),
  );
  ensureBabelPlugin(projectRoot);
  ensureBabelPlugin(projectRoot);
  const source = fs.readFileSync(
    path.join(projectRoot, "babel.config.js"),
    "utf8",
  );
  assert.strictEqual(
    (source.match(/@nativescript\/react-native\/babel-plugin/g) || []).length,
    1,
  );
  assert.strictEqual(
    (source.match(/react-native-worklets\/plugin/g) || []).length,
    1,
  );
  assert.match(source, /bundleMode:\s*true/);
});

withTempProject((projectRoot) => {
  fs.writeFileSync(
    path.join(projectRoot, "babel.config.js"),
    [
      "module.exports = {",
      "  plugins: ['@nativescript/react-native/babel-plugin'],",
      "};",
      "",
    ].join("\n"),
  );
  ensureBabelPlugin(projectRoot);
  ensureBabelPlugin(projectRoot);
  const source = fs.readFileSync(
    path.join(projectRoot, "babel.config.js"),
    "utf8",
  );
  assert.strictEqual(
    (source.match(/@nativescript\/react-native\/babel-plugin/g) || []).length,
    1,
  );
  assert.strictEqual(
    (source.match(/react-native-worklets\/plugin/g) || []).length,
    1,
  );
  assert.match(source, /bundleMode:\s*true/);
});

withTempProject((projectRoot) => {
  const metroPath = path.join(projectRoot, "metro.config.js");
  fs.writeFileSync(metroPath, "module.exports = {resolver: {}};\n");
  ensureMetroConfig(projectRoot);
  ensureMetroConfig(projectRoot);
  const source = fs.readFileSync(metroPath, "utf8");
  assert.strictEqual(
    (source.match(/@nativescript\/react-native\/metro-config/g) || []).length,
    1,
  );
});

withTempProject((projectRoot) => {
  const metadata = normalizeMetadataOptions({
    metadata: {
      includePods: ["SomeObjCSDK", 42],
      includeSystemFrameworks: ["UIKit", "MapKit", null],
    },
  });
  ensureMetadataConfig(projectRoot, metadata);
  ensureMetadataConfig(projectRoot, metadata);
  const config = JSON.parse(
    fs.readFileSync(
      path.join(projectRoot, "nativescript.react-native.json"),
      "utf8",
    ),
  );
  assert.deepStrictEqual(config.reactNative.metadata, {
    includePods: ["SomeObjCSDK"],
    includeSystemFrameworks: ["UIKit", "MapKit"],
  });
});

console.log("config plugin tests passed");
