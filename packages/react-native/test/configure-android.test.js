const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const {
  APPLY_LINE,
  ensureGradleScript,
  ensureTypings,
} = require('../cli/configure-android');

function scratchProject() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ns-rn-android-'));
  fs.mkdirSync(path.join(root, 'android', 'app'), { recursive: true });
  return root;
}

function writeBuildGradle(root, contents) {
  fs.writeFileSync(path.join(root, 'android', 'app', 'build.gradle'), contents);
}

function readBuildGradle(root) {
  return fs.readFileSync(path.join(root, 'android', 'app', 'build.gradle'), 'utf8');
}

// The script hooks applicationVariants, so it has to come after the plugin that
// creates them.
{
  const root = scratchProject();
  writeBuildGradle(
    root,
    'apply plugin: "com.android.application"\napply plugin: "com.facebook.react"\n\nandroid {\n}\n',
  );

  const result = ensureGradleScript(root);
  assert.strictEqual(result.changed, true);

  const lines = readBuildGradle(root).split('\n');
  const reactIndex = lines.findIndex((line) => line.includes('com.facebook.react'));
  const applyIndex = lines.findIndex((line) => line.includes('nativescript.gradle'));
  assert.ok(applyIndex > reactIndex, 'apply line must follow the React plugin');
}

// Running configure twice must not add it twice.
{
  const root = scratchProject();
  writeBuildGradle(root, `apply plugin: "com.facebook.react"\n${APPLY_LINE}\n`);

  const result = ensureGradleScript(root);
  assert.strictEqual(result.changed, false);
  const occurrences = readBuildGradle(root).split('nativescript.gradle').length - 1;
  assert.strictEqual(occurrences, 1);
}

// A project that does not apply the React plugin still gets the script.
{
  const root = scratchProject();
  writeBuildGradle(root, 'apply plugin: "com.android.application"\n');

  assert.strictEqual(ensureGradleScript(root).changed, true);
  assert.ok(readBuildGradle(root).includes('nativescript.gradle'));
}

// Typings are added to tsconfig, once.
{
  const root = scratchProject();
  const tsconfigPath = path.join(root, 'tsconfig.json');
  fs.writeFileSync(tsconfigPath, JSON.stringify({ include: ['**/*.ts'] }, null, 2));

  assert.strictEqual(ensureTypings(root).changed, true);
  const include = JSON.parse(fs.readFileSync(tsconfigPath, 'utf8')).include;
  assert.ok(include.includes('types/android/android.d.ts'));
  assert.ok(include.includes('types/android/android-declarations.d.ts'));

  assert.strictEqual(ensureTypings(root).changed, false);
  const after = JSON.parse(fs.readFileSync(tsconfigPath, 'utf8')).include;
  assert.strictEqual(after.length, include.length);
}

// A tsconfig with comments is left alone rather than rewritten and stripped.
{
  const root = scratchProject();
  const tsconfigPath = path.join(root, 'tsconfig.json');
  const original = '{\n  // keep me\n  "include": ["**/*.ts"]\n}\n';
  fs.writeFileSync(tsconfigPath, original);

  const result = ensureTypings(root);
  assert.strictEqual(result.changed, false);
  assert.strictEqual(fs.readFileSync(tsconfigPath, 'utf8'), original);
}

console.log('configure-android tests passed');
