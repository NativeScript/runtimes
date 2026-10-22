const pkg = require('../package.json');

const DEFAULTS = {
  ios: {
    hermes: true,
    newArchitecture: true,
  },
  babelPlugin: true,
};

const BABEL_PLUGIN = '@nativescript/react-native/babel-plugin';
const WORKLETS_BABEL_PLUGIN = 'react-native-worklets/plugin';
const BABEL_PLUGINS = [BABEL_PLUGIN, WORKLETS_BABEL_PLUGIN];
const EXPO_BABEL_PRESET = 'babel-preset-expo';
// Without strictGlobal the worklets plugin captures every undeclared name into
// the worklet's closure on the JS thread when the worklet is created, where
// NativeScript's classes (UIView, UILabel, ...) do not exist yet.
const WORKLETS_BABEL_PLUGIN_ENTRY = `['${WORKLETS_BABEL_PLUGIN}', { strictGlobal: true }]`;
// babel-preset-expo adds its own worklets plugin, without strictGlobal.
const EXPO_BABEL_PRESET_ENTRY = `['${EXPO_BABEL_PRESET}', { worklets: false }]`;
const METADATA_CONFIG_FILE = 'nativescript.react-native.json';

function readBoolean(value, fallback) {
  return typeof value === 'boolean' ? value : fallback;
}

function stringList(value) {
  return Array.isArray(value) ? value.filter((item) => typeof item === 'string') : undefined;
}

function normalizeMetadataOptions(options = {}) {
  const metadata = options.metadata || {};
  const normalized = {
    includePods: stringList(metadata.includePods) || [],
    includeSystemFrameworks: stringList(metadata.includeSystemFrameworks) || [],
  };
  // Omitted unless given, so ensureMetadataConfig keeps an `android` block
  // written by hand into nativescript.react-native.json.
  const androidInclude = stringList(metadata.android && metadata.android.include);
  if (androidInclude) {
    normalized.android = {include: androidInclude};
  }
  return normalized;
}

function normalizeOptions(options = {}) {
  const ios = options.ios || {};
  return {
    babelPlugin: readBoolean(options.babelPlugin, DEFAULTS.babelPlugin),
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
  const fs = require('fs');
  const path = require('path');
  const configPath = path.join(projectRoot, METADATA_CONFIG_FILE);
  let existing = {};
  if (fs.existsSync(configPath)) {
    try {
      existing = JSON.parse(fs.readFileSync(configPath, 'utf8'));
    } catch (error) {
      existing = {};
    }
  }

  const existingReactNative = existing.reactNative || {};
  const existingMetadata =
    existingReactNative.metadata && typeof existingReactNative.metadata === 'object'
      ? existingReactNative.metadata
      : {};
  const merged = {
    ...existing,
    reactNative: {
      ...existingReactNative,
      metadata: {...existingMetadata, ...metadataOptions},
    },
  };
  const nextSource = `${JSON.stringify(merged, null, 2)}\n`;
  if (!fs.existsSync(configPath) || fs.readFileSync(configPath, 'utf8') !== nextSource) {
    fs.writeFileSync(configPath, nextSource);
  }
  return merged;
}

function escapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\/]/g, '\\$&');
}

// Replaces `'name'` written as a bare array element with `entry`. Elements
// already given options (`['name', {...}]`) are left alone.
function replaceBareEntry(source, name, entry) {
  const bare = new RegExp(
    `([\\[,]\\s*)(['"])${escapeRegExp(name)}\\2(?=\\s*(?:,(?!\\s*\\{)|\\]))`,
  );
  return source.replace(bare, (match, prefix) => `${prefix}${entry}`);
}

function ensureBabelPlugin(projectRoot) {
  const fs = require('fs');
  const path = require('path');
  const candidates = ['babel.config.js', 'babel.config.cjs'];
  let babelConfigPath = candidates
    .map((candidate) => path.join(projectRoot, candidate))
    .find((candidate) => fs.existsSync(candidate));

  if (!babelConfigPath) {
    babelConfigPath = path.join(projectRoot, 'babel.config.js');
    fs.writeFileSync(
      babelConfigPath,
      [
        'module.exports = function(api) {',
        '  api.cache(true);',
        '  return {',
        `    presets: [${EXPO_BABEL_PRESET_ENTRY}],`,
        `    plugins: ['${BABEL_PLUGIN}', ${WORKLETS_BABEL_PLUGIN_ENTRY}],`,
        '  };',
        '};',
        '',
      ].join('\n'),
    );
    return;
  }

  const original = fs.readFileSync(babelConfigPath, 'utf8');
  let source = original;
  const missingPlugins = BABEL_PLUGINS.filter((plugin) => !source.includes(plugin));
  if (missingPlugins.length > 0) {
    const pluginEntry = missingPlugins
      .map((plugin) =>
        plugin === WORKLETS_BABEL_PLUGIN ? WORKLETS_BABEL_PLUGIN_ENTRY : `'${plugin}'`,
      )
      .join(', ') + ', ';
    if (/plugins\s*:\s*\[/.test(source)) {
      source = source.replace(/plugins\s*:\s*\[/, (match) => `${match}${pluginEntry}`);
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
      source += `\n// @nativescript/react-native: add ${missingPlugins.map((plugin) => `'${plugin}'`).join(' and ')} to your Babel plugins.\n`;
    }
  }

  source = replaceBareEntry(source, WORKLETS_BABEL_PLUGIN, WORKLETS_BABEL_PLUGIN_ENTRY);
  source = replaceBareEntry(source, EXPO_BABEL_PRESET, EXPO_BABEL_PRESET_ENTRY);

  if (source.includes(WORKLETS_BABEL_PLUGIN) && !/strictGlobal\s*:\s*true/.test(source)) {
    console.warn(
      `@nativescript/react-native: pass { strictGlobal: true } to '${WORKLETS_BABEL_PLUGIN}' ` +
        `in ${path.basename(babelConfigPath)}, or worklets that name native classes fail as their module loads.`,
    );
  }

  if (source !== original) {
    fs.writeFileSync(babelConfigPath, source);
  }
}

function withNativeScriptBabelPlugin(config) {
  let withDangerousMod;
  try {
    ({withDangerousMod} = require('expo/config-plugins'));
  } catch (error) {
    return config;
  }

  const mod = async (modConfig) => {
    ensureBabelPlugin(modConfig.modRequest.projectRoot);
    return modConfig;
  };
  return withDangerousMod(withDangerousMod(config, ['ios', mod]), ['android', mod]);
}

function withNativeScriptMetadata(config, metadataOptions) {
  let withDangerousMod;
  try {
    ({withDangerousMod} = require('expo/config-plugins'));
  } catch (error) {
    return config;
  }

  const mod = async (modConfig) => {
    ensureMetadataConfig(modConfig.modRequest.projectRoot, metadataOptions);
    return modConfig;
  };
  return withDangerousMod(withDangerousMod(config, ['ios', mod]), ['android', mod]);
}

/**
 * Applies the NativeScript Gradle script to the app module under prebuild.
 *
 * Autolinking already brings in the runtime; what this adds are the metadata and
 * typings tasks, which read the app's own resolved classpath and so have to live
 * in the app project.
 */
function withNativeScriptAndroidGradle(config) {
  let withAppBuildGradle;
  try {
    ({withAppBuildGradle} = require('expo/config-plugins'));
  } catch (error) {
    return config;
  }

  return withAppBuildGradle(config, (modConfig) => {
    const {APPLY_LINE} = require('../cli/configure-android');
    let contents = modConfig.modResults.contents;

    if (!contents.includes('@nativescript/react-native/android/nativescript.gradle')) {
      const reactPlugin = /apply plugin: ["']com\.facebook\.react["']\s*\n/;
      contents = reactPlugin.test(contents)
        ? contents.replace(reactPlugin, (match) => `${match}${APPLY_LINE}\n`)
        : `${APPLY_LINE}\n${contents}`;
      modConfig.modResults.contents = contents;
    }

    return modConfig;
  });
}

function withNativeScriptReactNative(config, options) {
  const normalized = normalizeOptions(options);

  config.ios = config.ios || {};

  if (normalized.ios.hermes) {
    config.ios.jsEngine = 'hermes';
  }

  if (normalized.ios.newArchitecture) {
    // Expo SDKs have accepted both the root and iOS-scoped keys over time.
    // Set both so CNG and existing native projects agree on the required RN mode.
    config.newArchEnabled = true;
    config.ios.newArchEnabled = true;
  }

  if (normalized.babelPlugin) {
    config = withNativeScriptBabelPlugin(config);
  }
  config = withNativeScriptMetadata(config, normalized.metadata);
  config = withNativeScriptAndroidGradle(config);

  return config;
}

module.exports = withNativeScriptReactNative;
module.exports.default = withNativeScriptReactNative;
module.exports.withNativeScriptReactNative = withNativeScriptReactNative;
module.exports.ensureBabelPlugin = ensureBabelPlugin;
module.exports.BABEL_PLUGIN = BABEL_PLUGIN;
module.exports.WORKLETS_BABEL_PLUGIN = WORKLETS_BABEL_PLUGIN;
module.exports.ensureMetadataConfig = ensureMetadataConfig;
module.exports.withNativeScriptAndroidGradle = withNativeScriptAndroidGradle;
module.exports.normalizeMetadataOptions = normalizeMetadataOptions;
module.exports.pkg = pkg;
