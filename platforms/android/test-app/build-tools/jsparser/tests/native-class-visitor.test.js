// Covers the native-class visitor: `class X extends android.view.View {}` with
// its decorators still in the source.
//
// Plain node rather than a jasmine spec in specs/, because the specs there
// shell out to gradle and need `npm install` in this directory first. This
// drives the visitor directly against jsparser's own dependencies, so it runs
// with nothing installed beyond what the parser already needs:
//
//     node tests/native-class-visitor.test.js
const assert = require("assert");
const path = require("path");

const root = path.resolve(__dirname, "..");
const babelParser = require(path.join(root, "node_modules/@babel/parser"));
const traverse = require(path.join(root, "node_modules/@babel/traverse")).default;
const es5 = require(path.join(root, "visitors/es5-visitors"));

function parse(source) {
  const ast = babelParser.parse(source, {
    minify: false,
    plugins: ["decorators-legacy", "objectRestSpread"],
  });

  traverse(ast, {
    enter(nodePath) {
      es5.es5Visitor(nodePath, {
        filePath: "app/subject.js",
        fullPathName: "app_subject",
        extendDecoratorName: "JavaProxy",
        interfacesDecoratorName: "Interfaces",
        logger: null,
      });
    },
  });

  return {
    named: es5.es5Visitor.getProxyExtendInfo(),
    unnamed: es5.es5Visitor.getCommonExtendInfo(),
  };
}

function fields(row) {
  return row.split("*");
}

// The override set has to match CallbackHandlers::CollectMethodOverrideNames in
// the runtime, because a content-keyed class name hashes it. Accessors are on
// the prototype and count; the constructor, statics and instance fields are not
// overrides.
{
  const { unnamed, named } = parse(`
    @NativeClass
    class MyView extends android.view.View {
      constructor() { super(); }
      onClick(v) { return 1; }
      get title() { return "t"; }
      set title(v) {}
      static helper() {}
      handler = () => {};
      onTouch(e) { return false; }
    }
  `);

  assert.strictEqual(named.length, 0);
  assert.strictEqual(unnamed.length, 1);

  const f = fields(unnamed[0]);
  assert.strictEqual(f[0], "android.view.View");
  assert.strictEqual(f[4], "MyView", "declared name must match the runtime's extendName");
  assert.deepStrictEqual(f[5].split(","), ["onClick", "title", "onTouch"]);
  assert.strictEqual(f[6], "", "an unnamed class must not claim a JavaProxy name");
}

// @JavaProxy puts the fully qualified name in the filename field, which is what
// makes hasSpecifiedName true on the generator side.
{
  const { named, unnamed } = parse(`
    @JavaProxy("com.example.MyActivity")
    @Interfaces([android.view.View.OnClickListener, java.lang.Runnable])
    class MyActivity extends android.app.Activity {
      onCreate(b) {}
      run() {}
    }
  `);

  assert.strictEqual(unnamed.length, 0);
  assert.strictEqual(named.length, 1);

  const f = fields(named[0]);
  assert.strictEqual(f[0], "android.app.Activity");
  assert.strictEqual(f[5], "onCreate,run");
  assert.strictEqual(f[6], "com.example.MyActivity");
  assert.strictEqual(f[8], "android.view.View.OnClickListener,java.lang.Runnable");
}

// @NativeClass({ name }) is the same request as @JavaProxy.
{
  const { named } = parse(`
    @NativeClass({ name: "com.example.Ticker" })
    class Ticker extends java.lang.Object {
      toString() { return "tick"; }
    }
  `);

  assert.strictEqual(named.length, 1);
  assert.strictEqual(fields(named[0])[6], "com.example.Ticker");
}

// An undecorated class is not a binding, however native its superclass looks.
// Bundles are full of `class X extends Foo.Bar {}` that has nothing to do with
// Java, which is why the decorator is required rather than inferred.
{
  const { named, unnamed } = parse(`
    class NotNative extends Something.Else {
      whatever() {}
    }
    @NativeClass
    class AlsoNotNative extends PlainIdentifier {
      whatever() {}
    }
  `);

  assert.strictEqual(named.length, 0);
  assert.strictEqual(unnamed.length, 0);
}

console.log("native-class visitor tests passed");

// Java types are reached through `global` in a React Native project, and the
// minifier renames that binding, so the prefix has to be dropped by shape
// rather than by matching a literal "global".
{
  const { unnamed } = parse(`
    @NativeClass
    class G extends global.java.util.ArrayList { size() { return 0; } }
  `);
  assert.strictEqual(fields(unnamed[0])[0], "java.util.ArrayList");
}

{
  const { unnamed } = parse(`
    @NativeClass
    class M extends g.java.util.ArrayList { size() { return 0; } }
  `);
  assert.strictEqual(fields(unnamed[0])[0], "java.util.ArrayList",
    "a minified global binding must not end up in the class name");
}

// A name that already starts at its root is untouched.
{
  const { unnamed } = parse(`
    @NativeClass
    class C extends com.example.Widget { go() {} }
  `);
  assert.strictEqual(fields(unnamed[0])[0], "com.example.Widget");
}

console.log("global-prefix tests passed");

// The call form. A project without the Babel decorator plugin writes this, and
// it must produce exactly what the decorator form produces.
{
  const { named } = parse(`
    const Ticker = JavaProxy('com.example.nsrn.Ticker')(
      class Ticker extends global.java.lang.Object {
        toString() { return 'ticked'; }
      }
    );
  `);
  assert.strictEqual(named.length, 1, "JavaProxy(...)(class) must emit a binding");
  const f = fields(named[0]);
  assert.strictEqual(f[0], "java.lang.Object");
  assert.strictEqual(f[5], "toString");
  assert.strictEqual(f[6], "com.example.nsrn.Ticker");
}

// Bare call form, no arguments.
{
  const { unnamed } = parse(`
    const L = NativeClass(class MyList extends global.java.util.ArrayList {
      size() { return 0; }
    });
  `);
  assert.strictEqual(unnamed.length, 1);
  assert.strictEqual(fields(unnamed[0])[0], "java.util.ArrayList");
  assert.strictEqual(fields(unnamed[0])[4], "MyList");
}

// Nested call form: both wrappers have to be seen.
{
  const { named } = parse(`
    const B = JavaProxy('com.example.Both')(
      Interfaces([java.lang.Runnable, java.util.concurrent.Callable])(
        class Both extends global.java.lang.Object { run() {} call() {} }
      )
    );
  `);
  assert.strictEqual(named.length, 1);
  const f = fields(named[0]);
  assert.strictEqual(f[6], "com.example.Both");
  assert.strictEqual(f[8], "java.lang.Runnable,java.util.concurrent.Callable");
}

// An ordinary call taking a class must not be mistaken for a decorator.
{
  const { named, unnamed } = parse(`
    const x = describe(class Plain extends global.java.lang.Object { run() {} });
  `);
  assert.strictEqual(named.length, 0);
  assert.strictEqual(unnamed.length, 0);
}

console.log("call-form decorator tests passed");

// The shape a bundler actually emits for an imported decorator. This is the
// form that reaches the generator in a real Metro build.
{
  const { named } = parse(`
    var Ticker = (0, _$$_REQUIRE(_dependencyMap[5]).JavaProxy)('com.example.nsrn.Ticker')(
      class Ticker extends global.java.lang.Object {
        toString() { return 'ticked'; }
      }
    );
  `);
  assert.strictEqual(named.length, 1, "a bundled JavaProxy import must be recognised");
  const f = fields(named[0]);
  assert.strictEqual(f[0], "java.lang.Object");
  assert.strictEqual(f[5], "toString");
  assert.strictEqual(f[6], "com.example.nsrn.Ticker");
}

// Namespace-imported form.
{
  const { unnamed } = parse(`
    var L = ns.NativeClass(class MyList extends global.java.util.ArrayList { size() {} });
  `);
  assert.strictEqual(unnamed.length, 1);
  assert.strictEqual(fields(unnamed[0])[4], "MyList");
}

console.log("bundled-import decorator tests passed");
