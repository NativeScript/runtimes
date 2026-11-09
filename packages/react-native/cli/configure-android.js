const fs = require('fs');
const path = require('path');

const APPLY_LINE =
  'apply from: "../../node_modules/@nativescript/react-native/android/nativescript.gradle"';

const TYPINGS = ['types/android/android.d.ts', 'types/android/android-declarations.d.ts'];

/**
 * Adds the NativeScript Gradle script to the app module.
 *
 * Autolinking brings in the runtime itself, but metadata and typings are
 * generated from the *app's* resolved classpath, so they have to be tasks in
 * the app project rather than in the library.
 */
function ensureGradleScript(projectRoot) {
  const buildGradle = path.join(projectRoot, 'android', 'app', 'build.gradle');
  if (!fs.existsSync(buildGradle)) {
    return { changed: false, reason: 'android/app/build.gradle was not found' };
  }

  let source = fs.readFileSync(buildGradle, 'utf8');
  if (source.includes('@nativescript/react-native/android/nativescript.gradle')) {
    return { changed: false, reason: 'already applied' };
  }

  // After the React plugin, whose applicationVariants the script hooks into.
  const reactPlugin = /apply plugin: ["']com\.facebook\.react["']\s*\n/;
  if (reactPlugin.test(source)) {
    source = source.replace(reactPlugin, (match) => `${match}${APPLY_LINE}\n`);
  } else {
    source = `${APPLY_LINE}\n${source}`;
  }

  fs.writeFileSync(buildGradle, source);
  return { changed: true };
}

/** Points tsconfig at the generated declarations, if they are being generated. */
function ensureTypings(projectRoot) {
  const tsconfigPath = path.join(projectRoot, 'tsconfig.json');
  if (!fs.existsSync(tsconfigPath)) {
    return { changed: false, reason: 'tsconfig.json was not found' };
  }

  const source = fs.readFileSync(tsconfigPath, 'utf8');
  let tsconfig;
  try {
    tsconfig = JSON.parse(source);
  } catch (e) {
    // tsconfig.json permits comments; rewriting it would lose them, and a
    // hand-edited config is likelier to be deliberate than stale.
    return { changed: false, reason: 'tsconfig.json is not plain JSON; add the types manually' };
  }

  const include = Array.isArray(tsconfig.include) ? tsconfig.include.slice() : [];
  const missing = TYPINGS.filter((entry) => !include.includes(entry));
  if (missing.length === 0) {
    return { changed: false, reason: 'already referenced' };
  }

  tsconfig.include = include.concat(missing);
  fs.writeFileSync(tsconfigPath, `${JSON.stringify(tsconfig, null, 2)}\n`);
  return { changed: true };
}

function configureAndroid(projectRoot) {
  const gradle = ensureGradleScript(projectRoot);
  const typings = ensureTypings(projectRoot);

  if (gradle.changed) {
    console.log('Added the NativeScript Gradle script to android/app/build.gradle.');
  } else {
    console.log(`Android Gradle script: ${gradle.reason}.`);
  }

  if (typings.changed) {
    console.log('Added the Android typings to tsconfig.json.');
  } else {
    console.log(`Android typings: ${typings.reason}.`);
  }

  return { gradle, typings };
}

module.exports = {
  APPLY_LINE,
  configureAndroid,
  ensureGradleScript,
  ensureTypings,
};
