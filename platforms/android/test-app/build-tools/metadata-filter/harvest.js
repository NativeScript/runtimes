/*
 * Harvests the Java types a JS program can be seen to use, so the build can
 * generate a metadata whitelist from them.
 *
 * The output is deliberately two-sided. `resolved` is what the analysis could
 * name exactly. `roots` is what it could only place -- a package it is sure the
 * program reaches into but whose class it cannot name, because the access was
 * computed. A filter built from `resolved` alone would be wrong: see
 * docs/metadata-filtering.md for the measurement that says by how much. Every
 * construct this file cannot resolve must widen `roots`, never fail silently;
 * that asymmetry is the whole safety argument.
 */

const fs = require("fs");
const path = require("path");
const parser = require("@babel/parser");
const traverse = require("@babel/traverse").default;

// The package names a Java FQN can start with. Anything rooted elsewhere is a
// JS identifier that merely looks like a namespace.
const JAVA_ROOTS = [
  "java",
  "javax",
  "android",
  "androidx",
  "dalvik",
  "kotlin",
  "kotlinx",
  "com",
  "org",
  "net",
  "edu",
  "io",
];

const PARSER_PLUGINS = [
  "jsx",
  "flow",
  "decorators-legacy",
  "classProperties",
  "classPrivateProperties",
  "classPrivateMethods",
  "objectRestSpread",
  "asyncGenerators",
  "dynamicImport",
  "optionalChaining",
  "nullishCoalescingOperator",
  "numericSeparator",
  "logicalAssignment",
  "topLevelAwait",
  "bigInt",
];

/* A dotted name is a class name if any segment starts with an uppercase
 * letter; the segments before it are the package. Java's own convention is the
 * only signal available, and it is the same one the metadata tree encodes. */
function splitPackageAndClass(segments) {
  for (let i = 0; i < segments.length; i++) {
    const s = segments[i];
    if (s && s[0] >= "A" && s[0] <= "Z") {
      return {
        packageName: segments.slice(0, i).join("."),
        className: segments.slice(i).join("."),
        classIndex: i,
      };
    }
  }
  return null;
}

function isJavaRoot(name) {
  return JAVA_ROOTS.indexOf(name) !== -1;
}

class Harvest {
  constructor() {
    /* fully-qualified name -> Set of reasons, kept for the report only */
    this.resolved = new Map();
    /* package prefix -> Set of reasons; the easing set */
    this.roots = new Map();
    /* things worth a human's attention, with file/line */
    this.unresolved = [];
  }

  addResolved(fqn, reason) {
    if (!this.resolved.has(fqn)) this.resolved.set(fqn, new Set());
    this.resolved.get(fqn).add(reason);
  }

  addRoot(pkg, reason) {
    if (!pkg) return;
    if (!this.roots.has(pkg)) this.roots.set(pkg, new Set());
    this.roots.get(pkg).add(reason);
  }

  addUnresolved(kind, detail, loc) {
    this.unresolved.push({ kind, detail, ...loc });
  }
}

/*
 * Walks a MemberExpression outward from its innermost object, returning the
 * static prefix and whether the chain was cut short by a computed access.
 * `java.lang.System.out` yields ["java","lang","System","out"]; `java[pkg].Foo`
 * yields ["java"] with truncated=true, which is the case that produces a root
 * hint instead of a name.
 */
function staticChain(node) {
  const segments = [];
  let truncated = false;
  let cur = node;

  /* Descend to the root identifier first. */
  const stack = [];
  while (cur && cur.type === "MemberExpression") {
    stack.push(cur);
    cur = cur.object;
  }

  if (!cur || cur.type !== "Identifier") {
    return null;
  }
  segments.push(cur.name);

  for (let i = stack.length - 1; i >= 0; i--) {
    const m = stack[i];
    if (m.computed) {
      /* obj["Literal"] is still static; obj[expr] is not. */
      if (m.property.type === "StringLiteral") {
        segments.push(m.property.value);
        continue;
      }
      truncated = true;
      break;
    }
    if (m.property.type !== "Identifier") {
      truncated = true;
      break;
    }
    segments.push(m.property.name);
  }

  return { segments, truncated };
}

function harvestFile(filePath, source, harvest) {
  let ast;
  const attempt = (sourceType) =>
    parser.parse(source, {
      sourceType,
      allowReturnOutsideFunction: true,
      allowAwaitOutsideFunction: true,
      allowSuperOutsideMethod: true,
      errorRecovery: true,
      plugins: PARSER_PLUGINS,
    });

  try {
    ast = attempt("unambiguous");
  } catch (e) {
    try {
      ast = attempt("script");
    } catch (e2) {
      /* A file that cannot be parsed is a file whose Java usage is entirely
       * invisible. That must widen the filter to everything, not narrow it. */
      harvest.addUnresolved("parse-error", `${e2.message}`, { file: filePath });
      return false;
    }
  }

  const rel = filePath;

  traverse(ast, {
    MemberExpression(p) {
      /* Only consider the outermost member expression of a chain. */
      if (p.parent && p.parent.type === "MemberExpression" && p.parent.object === p.node) {
        return;
      }

      const chain = staticChain(p.node);
      if (!chain || !isJavaRoot(chain.segments[0])) return;

      const split = splitPackageAndClass(chain.segments);

      if (chain.truncated) {
        /* We know the program reaches into this package but not which class.
         * Record the deepest all-lowercase prefix as a root hint. */
        const pkgSegments = [];
        for (const s of chain.segments) {
          if (s && s[0] >= "A" && s[0] <= "Z") break;
          pkgSegments.push(s);
        }
        harvest.addRoot(pkgSegments.join("."), `computed access in ${rel}`);
        harvest.addUnresolved("computed-member", chain.segments.join(".") + "[…]", {
          file: rel,
          line: p.node.loc && p.node.loc.start.line,
        });
        return;
      }

      if (!split) {
        /* All-lowercase chain: a package object being passed around, e.g.
         * `var l = java.lang`. Everything under it is reachable. */
        harvest.addRoot(chain.segments.join("."), `package alias in ${rel}`);
        harvest.addUnresolved("package-alias", chain.segments.join("."), {
          file: rel,
          line: p.node.loc && p.node.loc.start.line,
        });
        return;
      }

      /* Trailing segments after the class name are members/nested classes.
       * Keep the class and, for a nested name, its outer class too. */
      const classSegments = split.className.split(".");
      const outer = split.packageName
        ? split.packageName + "." + classSegments[0]
        : classSegments[0];
      harvest.addResolved(outer, `member access in ${rel}`);

      /* android.R.id.foo and friends: the second uppercase segment is a nested
       * class, not a field. Emitting both is harmless -- the filter is a
       * union -- and missing one is not. */
      if (classSegments.length > 1 && classSegments[1][0] >= "A" && classSegments[1][0] <= "Z") {
        harvest.addResolved(outer + "." + classSegments[1], `nested access in ${rel}`);
      }
    },

    StringLiteral(p) {
      /* Class.forName("…"), loadClass("…"), and every other name-by-string
       * path. Cheap to include and impossible to see any other way. */
      const v = p.node.value;
      if (v.length < 3 || v.indexOf(".") === -1 || /\s/.test(v)) return;
      const segments = v.split(".");
      if (!isJavaRoot(segments[0])) return;
      if (!splitPackageAndClass(segments)) return;
      harvest.addResolved(v, `string literal in ${rel}`);
    },
  });

  return true;
}

function collectFiles(target, out) {
  const stat = fs.statSync(target);
  if (stat.isFile()) {
    out.push(target);
    return out;
  }
  for (const entry of fs.readdirSync(target)) {
    if (entry === "node_modules" || entry.startsWith(".")) continue;
    const full = path.join(target, entry);
    const st = fs.statSync(full);
    if (st.isDirectory()) collectFiles(full, out);
    else if (/\.(js|mjs|cjs|jsx|bundle|jsbundle)$/.test(entry)) out.push(full);
  }
  return out;
}

function main() {
  const args = process.argv.slice(2);
  if (args.length < 1) {
    console.error("usage: harvest.js <file-or-dir> [more…] [--json out.json]");
    process.exit(2);
  }

  let jsonOut = null;
  const targets = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === "--json") {
      jsonOut = args[++i];
    } else {
      targets.push(args[i]);
    }
  }

  const harvest = new Harvest();
  const files = [];
  for (const t of targets) collectFiles(t, files);

  let parsed = 0;
  for (const f of files) {
    const source = fs.readFileSync(f, "utf8");
    if (harvestFile(f, source, harvest)) parsed++;
  }

  const result = {
    filesSeen: files.length,
    filesParsed: parsed,
    resolved: Array.from(harvest.resolved.keys()).sort(),
    roots: Array.from(harvest.roots.keys()).sort(),
    unresolved: harvest.unresolved,
  };

  if (jsonOut) {
    fs.writeFileSync(jsonOut, JSON.stringify(result, null, 2));
  }

  console.error(
    `harvest: ${parsed}/${files.length} files parsed, ` +
      `${result.resolved.length} resolved names, ${result.roots.length} root hints, ` +
      `${result.unresolved.length} unresolved sites`
  );

  if (!jsonOut) {
    for (const n of result.resolved) console.log(n);
  }
}

main();
