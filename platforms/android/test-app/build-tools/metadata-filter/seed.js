/*
 * Turns a harvest of the app's JS into the seed whitelist the metadata
 * generator grows its closure from.
 *
 * The seed is deliberately generous. Everything here is a *lower* bound on what
 * the app needs -- the generator widens it further (supertypes, nested classes,
 * signature types) before anything is dropped. The rules below are the ones
 * that cannot be derived from the class graph and so have to be decided here:
 *
 *   - what the JS names, exactly                     -> resolved harvest
 *   - what the JS reaches but does not name          -> root hints, as pkg:*
 *   - what Android instantiates by name              -> the manifest
 *   - what the runtime itself resolves               -> the keep-list below
 *
 * When in doubt this file keeps more. Over-inclusion costs bytes; the
 * under-inclusion failures are launch crashes.
 */

const fs = require("fs");
const path = require("path");
const { execFileSync } = require("child_process");

/*
 * Classes the runtime resolves from native code or by reflection. No analysis
 * of either the JS or the class graph can see these references, so they are
 * listed by hand -- and they live here, next to the runtime they belong to,
 * rather than in any individual app's config.
 *
 * Derived from the string literals in NativeScript/ffi/jni/jsi and
 * NativeScript/runtime/android/jsi; keep in step when that changes. The
 * Throwable and reflect entries are there because exceptions and members
 * arrive at JS as objects whose type the app never mentions.
 */
const RUNTIME_KEEP = [
  "com.tns:*",
  "java.lang:Object",
  "java.lang:Class",
  "java.lang:String",
  "java.lang:CharSequence",
  "java.lang:Number",
  "java.lang:Boolean",
  "java.lang:Byte",
  "java.lang:Character",
  "java.lang:Short",
  "java.lang:Integer",
  "java.lang:Long",
  "java.lang:Float",
  "java.lang:Double",
  "java.lang:Void",
  "java.lang:Enum",
  "java.lang:Iterable",
  "java.lang:Runnable",
  "java.lang:Thread",
  "java.lang:ClassLoader",
  "java.lang:StackTraceElement",
  /* every Throwable: they reach JS as thrown objects, named nowhere */
  "java.lang:Throwable",
  "java.lang:Exception",
  "java.lang:RuntimeException",
  "java.lang:Error",
  "java.lang:*Exception",
  "java.lang:*Error",
  "java.lang.reflect:*",
  "java.lang.annotation:*",
  "java.io:*Exception",
  "java.util:*Exception",
  "java.nio:ByteBuffer",
  "java.nio:ByteOrder",
  "java.nio:Buffer",
  "dalvik.system:*",
  "android.os:Bundle",
  "android.os:Looper",
  "android.os:Handler",
  "android.os:Process",
  "android.app:Activity",
  "android.app:Application",
  "android.content:Context",
  "org.json:*",
];

function parseManifest(manifestPath) {
  /* android:name on any component -- Android instantiates these by name, so
   * they must exist however the JS is written. Regex rather than an XML
   * parser: the attribute is unambiguous and this avoids a dependency. */
  const names = new Set();
  if (!fs.existsSync(manifestPath)) return names;

  const xml = fs.readFileSync(manifestPath, "utf8");
  const re = /android:name\s*=\s*"([^"]+)"/g;
  let m;
  while ((m = re.exec(xml)) !== null) {
    const v = m[1];
    /* Permissions and features share the attribute; only dotted names that
     * look like classes are of interest. */
    if (v.indexOf(".") > 0 && !v.startsWith("android.permission") && !v.startsWith("android.hardware")) {
      names.add(v.startsWith(".") ? v.slice(1) : v);
    }
  }
  return names;
}

function toEntry(fqn) {
  const segments = fqn.split(".");
  for (let i = 0; i < segments.length; i++) {
    if (segments[i] && segments[i][0] >= "A" && segments[i][0] <= "Z") {
      return segments.slice(0, i).join(".") + ":" + segments.slice(i).join(".");
    }
  }
  return null;
}

function main() {
  const args = process.argv.slice(2);
  let out = null;
  let manifest = null;
  let bindings = null;
  const targets = [];
  /* Paths whose parse failure is expected -- the runtime test-app ships files
   * that are invalid on purpose, to test how the loader reports them. Nothing
   * an ordinary app should ever need. */
  const allowUnparseable = [];

  for (let i = 0; i < args.length; i++) {
    if (args[i] === "--out") out = args[++i];
    else if (args[i] === "--manifest") manifest = args[++i];
    else if (args[i] === "--sbg-bindings") bindings = args[++i];
    else if (args[i] === "--allow-unparseable") allowUnparseable.push(args[++i]);
    else targets.push(args[i]);
  }

  if (!out || targets.length === 0) {
    console.error(
      "usage: seed.js <js-dir-or-file>… --out whitelist.mdg [--manifest AndroidManifest.xml] [--sbg-bindings sbg-bindings.txt]"
    );
    process.exit(2);
  }

  /* Run the harvester in-process so the two cannot disagree about grammar. */
  const harvestJson = path.join(path.dirname(out), ".metadata-harvest.json");
  execFileSync(
    process.execPath,
    [path.join(__dirname, "harvest.js"), ...targets, "--json", harvestJson],
    { stdio: ["ignore", "ignore", "inherit"] }
  );
  const harvest = JSON.parse(fs.readFileSync(harvestJson, "utf8"));

  /* A file we could not parse is a file whose Java usage is entirely unknown.
   * Filtering on a partial view of the program is how apps break in the field,
   * so refuse to emit a seed at all -- the generator then ships everything. */
  const parseErrors = harvest.unresolved
    .filter((u) => u.kind === "parse-error")
    .filter((u) => !allowUnparseable.some((p) => u.file.indexOf(p) !== -1));
  if (parseErrors.length > 0) {
    console.error(
      `seed: ${parseErrors.length} file(s) could not be parsed; metadata filtering disabled for this build:`
    );
    for (const e of parseErrors.slice(0, 10)) console.error(`  ${e.file}: ${e.detail}`);
    if (fs.existsSync(out)) fs.unlinkSync(out);
    process.exit(0);
  }

  const entries = new Set(RUNTIME_KEEP);

  for (const fqn of harvest.resolved) {
    const e = toEntry(fqn);
    if (e) entries.add(e);
  }

  /* The easing rule. A computed access or a package alias tells us which
   * package the program reaches into but not which class; keep the package
   * whole rather than guess. This is also what preserves feature-detection
   * guards like `if (android.support.design && …)`, which throw when an
   * intermediate package node disappears rather than merely being empty. */
  for (const root of harvest.roots) {
    entries.add(root + ":*");
  }

  for (const name of parseManifest(manifest)) {
    const e = toEntry(name);
    if (e) entries.add(e);
  }

  /* SBG bindings: the base class and interfaces of everything the app extends.
   * Field 0 is the base, field 8 the interface list. */
  if (bindings && fs.existsSync(bindings)) {
    for (const line of fs.readFileSync(bindings, "utf8").split("\n")) {
      if (!line.trim()) continue;
      const f = line.split("*");
      for (const candidate of [f[0], ...(f[8] ? f[8].split(",") : [])]) {
        const name = (candidate || "").trim().replace(/\//g, ".");
        if (!name) continue;
        const e = toEntry(name);
        if (e) entries.add(e);
      }
    }
  }

  const sorted = Array.from(entries).sort();
  fs.writeFileSync(
    out,
    "# GENERATED from the app's JS bundle. Do not edit.\n" +
      "# Seed only -- the metadata generator computes the closure over\n" +
      "# supertypes, nested classes and signature types before filtering.\n" +
      "# See docs/metadata-filtering.md.\n" +
      sorted.join("\n") +
      "\n"
  );

  console.error(
    `seed: ${sorted.length} entries ` +
      `(${harvest.resolved.length} named, ${harvest.roots.length} eased to whole packages)`
  );
}

main();
