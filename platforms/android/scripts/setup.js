#!/usr/bin/env node
//
// One-shot developer/CI setup for the android-runtime repo:
//   1. Fetch the QuickJS-NG engine source submodule and reset it
//      to its pinned commit.
//   2. Apply the NativeScript local patch onto the pristine submodule.
//   3. Install the jsparser build-tool dependencies (the previous `npm run setup`).
//
// Idempotent: re-running resets the submodules to the pinned commit (discarding
// an already-applied patch) and re-applies, so it is safe to run repeatedly.
//
// The submodule + patch config mirrors scripts/vendor-engines-as-submodules.sh.

'use strict';

const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

// This script lives in platforms/android/scripts, but the engine submodules are
// registered against the napi-ios repository root, so the two are tracked
// separately: git commands run from REPO_ROOT, the android build tools from
// ANDROID_ROOT.
const ANDROID_ROOT = path.resolve(__dirname, '..');
const REPO_ROOT = path.resolve(__dirname, '../../..');
const NPM = process.platform === 'win32' ? 'npm.cmd' : 'npm';

const NAPI_QJS = 'vendor/quickjs';
const PATCHES = path.join(ANDROID_ROOT, 'tools/patches');
// QuickJS-NG only: the bellard tree was dropped, and QUICKJS is an alias for
// QUICKJS_NG (runtime/build.gradle). The pinned commit fixes the engine's
// BC_VERSION, which the bytecode compiler must be built from as well -- see
// tools/patches/README.md.
const ENGINES = [
  {
    sub: `${NAPI_QJS}/source_ng`,
    patch: path.join(PATCHES, 'quickjs_ng/0001-nativescript-local-changes.patch'),
  },
];

function run(cmd, args, opts = {}) {
  console.log(`==> ${cmd} ${args.join(' ')}`);
  execFileSync(cmd, args, { stdio: 'inherit', cwd: REPO_ROOT, ...opts });
}

function insideGitWorkTree() {
  try {
    return execFileSync('git', ['rev-parse', '--is-inside-work-tree'], { cwd: REPO_ROOT })
      .toString().trim() === 'true';
  } catch (_) {
    return false;
  }
}

function setupEngines() {
  if (!insideGitWorkTree()) {
    console.log('==> Not a git work tree; skipping submodule + patch setup.');
    return;
  }

  // 1. Fetch + reset the submodules to their pinned commit. --force discards any
  //    previously-applied patch so step 2 always starts from a clean checkout.
  run('git', ['submodule', 'update', '--init', '--force', ...ENGINES.map((e) => e.sub)]);

  // 2. Hard-reset each submodule's working tree. `submodule update --force`
  //    above only forces a checkout when the recorded SHA differs; if the
  //    submodule is already at the pinned commit it leaves local modifications
  //    alone, so a re-run would try to apply the patch onto an already-patched
  //    tree and fail. Resetting here is what actually makes this idempotent.
  for (const { sub } of ENGINES) {
    run('git', ['-C', sub, 'reset', '--hard', '--quiet']);
    run('git', ['-C', sub, 'clean', '-qfd']);
  }

  // 3. Apply the local patches onto the pristine submodules.
  for (const { sub, patch } of ENGINES) {
    if (!fs.existsSync(patch)) {
      console.warn(`   ! patch missing, skipping: ${patch}`);
      continue;
    }
    run('git', ['-C', sub, 'apply', '--whitespace=nowarn', patch]);
  }
}

function setupJsParser() {
  const dir = path.join(ANDROID_ROOT, 'test-app/build-tools/jsparser');
  const hasLock = fs.existsSync(path.join(dir, 'package-lock.json'));
  run(NPM, [hasLock ? 'ci' : 'install'], { cwd: dir });
}

setupEngines();
setupJsParser();
console.log('==> Setup complete');
