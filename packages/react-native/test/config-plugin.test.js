const fs = require('fs');
const os = require('os');
const path = require('path');
const assert = require('assert');
const {
  ensureBabelPlugin,
  ensureMetadataConfig,
  normalizeMetadataOptions,
} = require('../plugin/withNativeScriptReactNative');

function withTempProject(callback) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ns-rn-plugin-'));
  try {
    callback(dir);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
}

withTempProject((projectRoot) => {
  ensureBabelPlugin(projectRoot);
  ensureBabelPlugin(projectRoot);
  const source = fs.readFileSync(path.join(projectRoot, 'babel.config.js'), 'utf8');
  assert.strictEqual(
    (source.match(/@nativescript\/react-native\/babel-plugin/g) || []).length,
    1,
  );
  assert.strictEqual(
    (source.match(/react-native-worklets\/plugin/g) || []).length,
    1,
  );
});

withTempProject((projectRoot) => {
  fs.writeFileSync(
    path.join(projectRoot, 'babel.config.js'),
    [
      'module.exports = function(api) {',
      '  api.cache(true);',
      '  return {',
      "    presets: ['babel-preset-expo'],",
      '  };',
      '};',
      '',
    ].join('\n'),
  );
  ensureBabelPlugin(projectRoot);
  ensureBabelPlugin(projectRoot);
  const source = fs.readFileSync(path.join(projectRoot, 'babel.config.js'), 'utf8');
  assert.strictEqual(
    (source.match(/@nativescript\/react-native\/babel-plugin/g) || []).length,
    1,
  );
  assert.strictEqual(
    (source.match(/react-native-worklets\/plugin/g) || []).length,
    1,
  );
});

withTempProject((projectRoot) => {
  fs.writeFileSync(
    path.join(projectRoot, 'babel.config.js'),
    [
      'module.exports = {',
      "  plugins: ['@nativescript/react-native/babel-plugin'],",
      '};',
      '',
    ].join('\n'),
  );
  ensureBabelPlugin(projectRoot);
  ensureBabelPlugin(projectRoot);
  const source = fs.readFileSync(path.join(projectRoot, 'babel.config.js'), 'utf8');
  assert.strictEqual(
    (source.match(/@nativescript\/react-native\/babel-plugin/g) || []).length,
    1,
  );
  assert.strictEqual(
    (source.match(/react-native-worklets\/plugin/g) || []).length,
    1,
  );
});

function loadBabelConfig(projectRoot) {
  const configPath = path.join(projectRoot, 'babel.config.js');
  delete require.cache[require.resolve(configPath)];
  const exported = require(configPath);
  return typeof exported === 'function' ? exported({cache() {}}) : exported;
}

function findEntry(entries, name) {
  return (entries || []).find(
    (entry) => entry === name || (Array.isArray(entry) && entry[0] === name),
  );
}

withTempProject((projectRoot) => {
  ensureBabelPlugin(projectRoot);
  const config = loadBabelConfig(projectRoot);
  assert.deepStrictEqual(findEntry(config.presets, 'babel-preset-expo'), [
    'babel-preset-expo',
    {worklets: false},
  ]);
  assert.deepStrictEqual(findEntry(config.plugins, 'react-native-worklets/plugin'), [
    'react-native-worklets/plugin',
    {strictGlobal: true},
  ]);
});

withTempProject((projectRoot) => {
  fs.writeFileSync(
    path.join(projectRoot, 'babel.config.js'),
    [
      'module.exports = function(api) {',
      '  api.cache(true);',
      '  return {',
      "    presets: ['babel-preset-expo'],",
      "    plugins: ['@nativescript/react-native/babel-plugin', 'react-native-worklets/plugin'],",
      '  };',
      '};',
      '',
    ].join('\n'),
  );
  ensureBabelPlugin(projectRoot);
  ensureBabelPlugin(projectRoot);
  const config = loadBabelConfig(projectRoot);
  assert.deepStrictEqual(findEntry(config.presets, 'babel-preset-expo'), [
    'babel-preset-expo',
    {worklets: false},
  ]);
  assert.deepStrictEqual(findEntry(config.plugins, 'react-native-worklets/plugin'), [
    'react-native-worklets/plugin',
    {strictGlobal: true},
  ]);
  assert.strictEqual(config.plugins.length, 2);
});

withTempProject((projectRoot) => {
  const source = [
    'module.exports = {',
    "  plugins: [['react-native-worklets/plugin', { processNestedWorklets: true }]],",
    '};',
    '',
  ].join('\n');
  fs.writeFileSync(path.join(projectRoot, 'babel.config.js'), source);
  const warnings = [];
  const warn = console.warn;
  console.warn = (message) => warnings.push(message);
  try {
    ensureBabelPlugin(projectRoot);
  } finally {
    console.warn = warn;
  }
  const config = loadBabelConfig(projectRoot);
  assert.deepStrictEqual(findEntry(config.plugins, 'react-native-worklets/plugin'), [
    'react-native-worklets/plugin',
    {processNestedWorklets: true},
  ]);
  assert.strictEqual(warnings.length, 1);
  assert.match(warnings[0], /strictGlobal/);
});

withTempProject((projectRoot) => {
  const metadata = normalizeMetadataOptions({
    metadata: {
      includePods: ['SomeObjCSDK', 42],
      includeSystemFrameworks: ['UIKit', 'MapKit', null],
    },
  });
  ensureMetadataConfig(projectRoot, metadata);
  ensureMetadataConfig(projectRoot, metadata);
  const config = JSON.parse(
    fs.readFileSync(path.join(projectRoot, 'nativescript.react-native.json'), 'utf8'),
  );
  assert.deepStrictEqual(config.reactNative.metadata, {
    includePods: ['SomeObjCSDK'],
    includeSystemFrameworks: ['UIKit', 'MapKit'],
  });
});

console.log('config plugin tests passed');
