const pkg = require("../package.json");

const DEFAULTS = {
  ios: {
    hermes: true,
    newArchitecture: true,
  },
  babelPlugin: true,
  workletsBundleMode: true,
};

const BABEL_PLUGIN = "@nativescript/react-native/babel-plugin";
const WORKLETS_BABEL_PLUGIN = "react-native-worklets/plugin";
const METADATA_CONFIG_FILE = "nativescript.react-native.json";
const METRO_CONFIG_MARKER = "@nativescript/react-native/metro-config";

function readBoolean(value, fallback) {
  return typeof value === "boolean" ? value : fallback;
}

function normalizeMetadataOptions(options = {}) {
  const metadata = options.metadata || {};
  return {
    includePods: Array.isArray(metadata.includePods)
      ? metadata.includePods.filter((value) => typeof value === "string")
      : [],
    includeSystemFrameworks: Array.isArray(metadata.includeSystemFrameworks)
      ? metadata.includeSystemFrameworks.filter(
          (value) => typeof value === "string",
        )
      : [],
  };
}

function normalizeOptions(options = {}) {
  const ios = options.ios || {};
  return {
    babelPlugin: readBoolean(options.babelPlugin, DEFAULTS.babelPlugin),
    workletsBundleMode: readBoolean(
      options.workletsBundleMode,
      DEFAULTS.workletsBundleMode,
    ),
    metadata: normalizeMetadataOptions(options),
    ios: {
      hermes: readBoolean(
        ios.hermes,
        readBoolean(options.hermes, DEFAULTS.ios.hermes),
      ),
      newArchitecture: readBoolean(
        ios.newArchitecture,
        readBoolean(options.newArchitecture, DEFAULTS.ios.newArchitecture),
      ),
    },
  };
}

function ensureMetadataConfig(projectRoot, metadataOptions) {
  const fs = require("fs");
  const path = require("path");
  const configPath = path.join(projectRoot, METADATA_CONFIG_FILE);
  let existing = {};
  if (fs.existsSync(configPath)) {
    try {
      existing = JSON.parse(fs.readFileSync(configPath, "utf8"));
    } catch (error) {
      existing = {};
    }
  }

  const merged = {
    ...existing,
    reactNative: {
      ...(existing.reactNative || {}),
      metadata: metadataOptions,
    },
  };
  const nextSource = `${JSON.stringify(merged, null, 2)}\n`;
  if (
    !fs.existsSync(configPath) ||
    fs.readFileSync(configPath, "utf8") !== nextSource
  ) {
    fs.writeFileSync(configPath, nextSource);
  }
  return merged;
}

function workletsBabelEntry(bundleMode) {
  return bundleMode
    ? `[${JSON.stringify(WORKLETS_BABEL_PLUGIN)}, { bundleMode: true, strictGlobal: true }]`
    : JSON.stringify(WORKLETS_BABEL_PLUGIN);
}

function ensureBabelPlugin(projectRoot, options = {}) {
  const fs = require("fs");
  const path = require("path");
  const bundleMode = readBoolean(
    options.bundleMode,
    DEFAULTS.workletsBundleMode,
  );
  const babelPluginEntries = [
    JSON.stringify(BABEL_PLUGIN),
    workletsBabelEntry(bundleMode),
  ];
  const candidates = ["babel.config.js", "babel.config.cjs"];
  let babelConfigPath = candidates
    .map((candidate) => path.join(projectRoot, candidate))
    .find((candidate) => fs.existsSync(candidate));

  if (!babelConfigPath) {
    babelConfigPath = path.join(projectRoot, "babel.config.js");
    fs.writeFileSync(
      babelConfigPath,
      [
        "module.exports = function(api) {",
        "  api.cache(true);",
        "  return {",
        "    presets: ['babel-preset-expo'],",
        `    plugins: [${babelPluginEntries.join(", ")}],`,
        "  };",
        "};",
        "",
      ].join("\n"),
    );
    return;
  }

  let source = fs.readFileSync(babelConfigPath, "utf8");
  if (bundleMode && source.includes(WORKLETS_BABEL_PLUGIN)) {
    if (!source.includes("bundleMode")) {
      const simplePlugin = /(["'])react-native-worklets\/plugin\1/;
      source = source.replace(simplePlugin, workletsBabelEntry(true));
    }
  }

  const missingPlugins = [BABEL_PLUGIN, WORKLETS_BABEL_PLUGIN].filter(
    (plugin) => !source.includes(plugin),
  );
  if (missingPlugins.length === 0) {
    fs.writeFileSync(babelConfigPath, source);
    return;
  }

  const pluginEntry =
    missingPlugins
      .map((plugin) =>
        plugin === WORKLETS_BABEL_PLUGIN
          ? workletsBabelEntry(bundleMode)
          : JSON.stringify(plugin),
      )
      .join(", ") + ", ";
  if (/plugins\s*:\s*\[/.test(source)) {
    source = source.replace(
      /plugins\s*:\s*\[/,
      (match) => `${match}${pluginEntry}`,
    );
  } else if (/return\s*\{/.test(source)) {
    source = source.replace(
      /return\s*\{/,
      (match) => `${match}\n    plugins: [${pluginEntry}],`,
    );
  } else if (/module\.exports\s*=\s*\{/.test(source)) {
    source = source.replace(
      /module\.exports\s*=\s*\{/,
      (match) => `${match}\n  plugins: [${pluginEntry}],`,
    );
  } else if (/export\s+default\s+\{/.test(source)) {
    source = source.replace(
      /export\s+default\s+\{/,
      (match) => `${match}\n  plugins: [${pluginEntry}],`,
    );
  } else {
    source += `\n// @nativescript/react-native: add ${missingPlugins.map((plugin) => `'${plugin}'`).join(" and ")} to your Babel plugins.\n`;
  }

  fs.writeFileSync(babelConfigPath, source);
}

function ensureMetroConfig(projectRoot) {
  const fs = require("fs");
  const path = require("path");
  const candidates = ["metro.config.js", "metro.config.cjs"];
  let metroConfigPath = candidates
    .map((candidate) => path.join(projectRoot, candidate))
    .find((candidate) => fs.existsSync(candidate));

  if (!metroConfigPath) {
    metroConfigPath = path.join(projectRoot, "metro.config.js");
    fs.writeFileSync(
      metroConfigPath,
      [
        "let getDefaultConfig;",
        "try { ({getDefaultConfig} = require('expo/metro-config')); }",
        "catch { ({getDefaultConfig} = require('@react-native/metro-config')); }",
        `const {withNativeScriptMetroConfig} = require('${METRO_CONFIG_MARKER}');`,
        "",
        "module.exports = withNativeScriptMetroConfig(getDefaultConfig(__dirname));",
        "",
      ].join("\n"),
    );
    return;
  }

  let source = fs.readFileSync(metroConfigPath, "utf8");
  if (source.includes(METRO_CONFIG_MARKER)) {
    return;
  }
  source = `${source.trimEnd()}\n\nmodule.exports = require('${METRO_CONFIG_MARKER}').withNativeScriptMetroConfig(module.exports);\n`;
  fs.writeFileSync(metroConfigPath, source);
}

function withNativeScriptTransforms(config, bundleMode) {
  let withDangerousMod;
  try {
    ({ withDangerousMod } = require("expo/config-plugins"));
  } catch (error) {
    return config;
  }

  return withDangerousMod(config, [
    "ios",
    async (modConfig) => {
      ensureBabelPlugin(modConfig.modRequest.projectRoot, { bundleMode });
      if (bundleMode) {
        ensureMetroConfig(modConfig.modRequest.projectRoot);
      }
      return modConfig;
    },
  ]);
}

function withNativeScriptMetadata(config, metadataOptions) {
  let withDangerousMod;
  try {
    ({ withDangerousMod } = require("expo/config-plugins"));
  } catch (error) {
    return config;
  }

  return withDangerousMod(config, [
    "ios",
    async (modConfig) => {
      ensureMetadataConfig(modConfig.modRequest.projectRoot, metadataOptions);
      return modConfig;
    },
  ]);
}

function withNativeScriptReactNative(config, options) {
  const normalized = normalizeOptions(options);

  config.ios = config.ios || {};

  if (normalized.ios.hermes) {
    config.ios.jsEngine = "hermes";
  }

  if (normalized.ios.newArchitecture) {
    // Expo SDKs have accepted both the root and iOS-scoped keys over time.
    // Set both so CNG and existing native projects agree on the required RN mode.
    config.newArchEnabled = true;
    config.ios.newArchEnabled = true;
  }

  if (normalized.babelPlugin) {
    config = withNativeScriptTransforms(config, normalized.workletsBundleMode);
  }
  config = withNativeScriptMetadata(config, normalized.metadata);

  return config;
}

module.exports = withNativeScriptReactNative;
module.exports.default = withNativeScriptReactNative;
module.exports.withNativeScriptReactNative = withNativeScriptReactNative;
module.exports.ensureBabelPlugin = ensureBabelPlugin;
module.exports.ensureMetroConfig = ensureMetroConfig;
module.exports.BABEL_PLUGIN = BABEL_PLUGIN;
module.exports.WORKLETS_BABEL_PLUGIN = WORKLETS_BABEL_PLUGIN;
module.exports.ensureMetadataConfig = ensureMetadataConfig;
module.exports.normalizeMetadataOptions = normalizeMetadataOptions;
module.exports.pkg = pkg;
