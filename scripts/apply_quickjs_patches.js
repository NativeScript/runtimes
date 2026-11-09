#!/usr/bin/env node
//
// Applies the NativeScript local patches onto the vendored QuickJS-NG source.
//
// The engine source under vendor/ is shared by the Apple and Android builds, so
// this lives at the repo root rather than under platforms/<platform>.
//
// This script does NOT touch submodule registration or the pin -- fetching the
// engine source is a manual step, so nothing here can silently move you off the
// pinned commit:
//
//     git submodule update --init --force vendor/quickjs/source_ng
//
// (never --remote: that moves the submodule to the tip of its branch, silently
// changing the engine and its BC_VERSION -- see vendor/quickjs/patches/README.md)
//
// Idempotent: the submodule work tree is hard-reset to the pinned commit before
// the patch is applied, so re-running is safe. Pass --no-reset to apply on top
// of the current work tree instead (it will fail if the patch is already in).
//
// Flags:
//   --no-reset   don't hard-reset/clean the submodule work tree first
//   --check      verify the patch still applies to the pinned commit, change
//                nothing (runs in a throwaway worktree, so an already-patched
//                checkout doesn't turn into a false failure)

'use strict';

const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const REPO_ROOT = path.resolve(__dirname, '..');

// The pinned commit fixes the engine's BC_VERSION, which the ahead-of-time
// bytecode compiler must be built from as well -- see
// vendor/quickjs/patches/README.md.
const ENGINES = [
  {
    sub: 'vendor/quickjs/source_ng',
    patch: 'vendor/quickjs/patches/source_ng/0001-nativescript-local-changes.patch',
  },
];

const flags = new Set(process.argv.slice(2));
const reset = !flags.has('--no-reset');
const checkOnly = flags.has('--check');

function run(cmd, args, opts = {}) {
  console.log(`==> ${cmd} ${args.join(' ')}`);
  try {
    execFileSync(cmd, args, { stdio: 'inherit', cwd: REPO_ROOT, ...opts });
  } catch (err) {
    // The command already printed its own diagnostics; a node stack trace on top
    // of them just buries the useful part.
    process.exit(typeof err.status === 'number' ? err.status : 1);
  }
}

function fail(msg) {
  console.error(`\nerror: ${msg}`);
  process.exit(1);
}

// Verify the patch against the pinned commit itself rather than the (usually
// already-patched) checkout, in a temporary worktree that is thrown away after.
function checkAgainstPin(sub, patchFile) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'qjs-patch-check-'));
  const work = path.join(tmp, 'src');
  try {
    run('git', ['-C', sub, 'worktree', 'add', '--detach', '--quiet', work, 'HEAD']);
    run('git', ['-C', work, 'apply', '--check', '--whitespace=nowarn', patchFile]);
  } finally {
    try {
      execFileSync('git', ['-C', sub, 'worktree', 'remove', '--force', work],
        { cwd: REPO_ROOT, stdio: 'ignore' });
    } catch (_) { /* best effort */ }
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

for (const { sub, patch } of ENGINES) {
  const subDir = path.join(REPO_ROOT, sub);
  const patchFile = path.join(REPO_ROOT, patch);

  // An unfetched submodule is an empty directory. Say so plainly rather than
  // failing later inside git with something cryptic.
  if (!fs.existsSync(path.join(subDir, '.git'))) {
    fail(
      `${sub} is not checked out. Fetch it first:\n\n` +
      `    git submodule update --init --force ${sub}\n`
    );
  }
  if (!fs.existsSync(patchFile)) fail(`patch missing: ${patch}`);

  if (checkOnly) {
    checkAgainstPin(sub, patchFile);
    console.log(`==> ${patch} applies cleanly to the pinned commit`);
    continue;
  }

  if (reset) {
    run('git', ['-C', sub, 'reset', '--hard', '--quiet']);
    run('git', ['-C', sub, 'clean', '-qfd']);
  }
  run('git', ['-C', sub, 'apply', '--whitespace=nowarn', patchFile]);
}

console.log(checkOnly ? '==> Check complete' : '==> Patches applied');
