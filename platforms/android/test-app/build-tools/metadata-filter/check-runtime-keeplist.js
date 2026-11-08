/*
 * Checks that RUNTIME_KEEP in seed.js still covers every class the runtime
 * resolves by name from native code.
 *
 * That list was written by reading string literals out of the C++. Nothing kept
 * the two in step, so a class added to the runtime later would simply go
 * missing from filtered metadata -- and the failure would surface as a
 * TypeError in an app, not as anything the build could see. This turns that
 * into a test.
 *
 * usage: node check-runtime-keeplist.js <runtime-source-dir>…
 */

const fs = require("fs");
const path = require("path");

/* Mirrors PatternMatcherImpl in the metadata generator: '*' spans anything,
 * '?' one character. Iterative, because the generator's own matcher is a
 * recursive one and this runs over a lot of candidates. */
function wildcardMatch(pattern, input) {
  let p = 0, i = 0, star = -1, mark = 0;
  while (i < input.length) {
    if (p < pattern.length && (pattern[p] === "?" || pattern[p] === input[i])) {
      p++; i++;
    } else if (p < pattern.length && pattern[p] === "*") {
      star = p++; mark = i;
    } else if (star !== -1) {
      p = star + 1; i = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.length && pattern[p] === "*") p++;
  return p === pattern.length;
}

function loadKeepList(seedPath) {
  const src = fs.readFileSync(seedPath, "utf8");
  const start = src.indexOf("const RUNTIME_KEEP = [");
  if (start === -1) throw new Error("RUNTIME_KEEP not found in " + seedPath);
  const end = src.indexOf("];", start);
  const body = src.slice(src.indexOf("[", start), end + 1);

  return eval(body).map((entry) => {
    const idx = entry.indexOf(":");
    return { pkg: entry.slice(0, idx), cls: entry.slice(idx + 1) };
  });
}

function covered(keep, fqn) {
  const idx = fqn.lastIndexOf(".");
  if (idx < 0) return true;
  const pkg = fqn.slice(0, idx);
  const cls = fqn.slice(idx + 1);
  return keep.some((k) => wildcardMatch(k.pkg, pkg) && wildcardMatch(k.cls, cls));
}

function collectSources(dir, out) {
  for (const entry of fs.readdirSync(dir)) {
    const full = path.join(dir, entry);
    const st = fs.statSync(full);
    if (st.isDirectory()) collectSources(full, out);
    else if (/\.(cpp|h|hpp|cc)$/.test(entry)) out.push(full);
  }
  return out;
}

function main() {
  const dirs = process.argv.slice(2);
  if (dirs.length === 0) {
    console.error("usage: check-runtime-keeplist.js <runtime-source-dir>…");
    process.exit(2);
  }

  const seedPath = path.join(__dirname, "seed.js");
  const keep = loadKeepList(seedPath);

  /* Java class names as the runtime writes them: "java/lang/Object" for JNI
   * FindClass, or dotted for the reflective paths. */
  const literal = /"((?:java|javax|android|androidx|dalvik|kotlin|org|com|net)[./][A-Za-z0-9_./$]*)"/g;

  const missing = new Map();
  const files = [];
  for (const dir of dirs) {
    if (fs.existsSync(dir)) collectSources(dir, files);
  }

  for (const file of files) {
    const src = fs.readFileSync(file, "utf8");
    let m;
    while ((m = literal.exec(src)) !== null) {
      const fqn = m[1].replace(/\//g, ".");

      /* Package prefixes and signature fragments are not class names. */
      if (fqn.endsWith(".")) continue;
      const last = fqn.slice(fqn.lastIndexOf(".") + 1);
      if (!last || last[0] < "A" || last[0] > "Z") continue;

      if (!covered(keep, fqn)) {
        if (!missing.has(fqn)) missing.set(fqn, file);
      }
    }
  }

  console.error(`checked ${files.length} runtime source files against ${keep.length} keep patterns`);

  if (missing.size > 0) {
    console.error(
      `\nRUNTIME_KEEP in ${seedPath} does not cover ${missing.size} class(es) the runtime ` +
        `names directly. Filtered metadata would omit them:\n`
    );
    for (const [fqn, file] of missing) {
      console.error(`  ${fqn}\n      ${file}`);
    }
    process.exit(1);
  }

  console.error("RUNTIME_KEEP covers every class name the runtime resolves.");
}

main();
