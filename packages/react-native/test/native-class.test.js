const assert = require('assert');

// Plain JavaScript on purpose: the decorator has to behave identically under
// every bundler, so it is shipped untranspiled and tested as shipped.
const {
  NativeClass,
  Interfaces,
  JavaProxy,
  NATIVE_CLASS,
} = require('../src/NativeClass');

/** Stands in for a metadata-backed native class. */
function makeNativeBase() {
  const calls = [];
  function Base() {}
  Base.prototype.baseMethod = function () {
    return 'base';
  };

  function extend(name, implementationObject, isTypeScriptExtend) {
    calls.push({ name, implementationObject, isTypeScriptExtend });
    function Extended() {}
    // The runtime uses the implementation object it is handed as the generated
    // class's prototype; the decorator relies on that.
    Extended.prototype = implementationObject;
    return Extended;
  }
  extend.__isNativeExtend__ = true;
  Base.extend = extend;

  return { Base, calls };
}

// A plain class is annotated and returned untouched.
{
  class Plain {}
  const result = NativeClass(Plain);
  assert.strictEqual(result, Plain);
  assert.strictEqual(Plain[NATIVE_CLASS], true);
}

// Called form, no options.
{
  class Plain {}
  const result = NativeClass()(Plain);
  assert.strictEqual(result, Plain);
  assert.strictEqual(Plain[NATIVE_CLASS], true);
}

// Standard (TC39) decorators pass (value, context).
{
  class Plain {}
  const result = NativeClass(Plain, { kind: 'class', name: 'Plain' });
  assert.strictEqual(result, Plain);
  assert.strictEqual(Plain[NATIVE_CLASS], true);
}

// Extending a native base generates the Java class and re-points the
// constructor, so that super() goes through the runtime.
{
  const { Base, calls } = makeNativeBase();
  class Child extends Base {
    size() {
      return 5;
    }
  }

  const result = NativeClass(Child);

  assert.strictEqual(result, Child);
  assert.strictEqual(calls.length, 1, 'extend() must be called exactly once');
  assert.strictEqual(calls[0].name, 'Child');
  assert.strictEqual(calls[0].implementationObject, Child.prototype);
  assert.strictEqual(calls[0].isTypeScriptExtend, true);
  assert.notStrictEqual(
    Object.getPrototypeOf(Child),
    Base,
    'the constructor must be re-pointed at the generated class',
  );
  // The instance chain has to stay intact -- re-pointing it too is a cycle.
  assert.strictEqual(Child.prototype.size.call(null), 5);
}

// Interfaces land on the prototype, which is what the runtime reads.
{
  const { Base, calls } = makeNativeBase();
  const IRunnable = {};
  const ICallable = {};

  class Child extends Base {}
  NativeClass({ interfaces: [IRunnable, ICallable] })(Child);

  assert.deepStrictEqual(Child.prototype.interfaces, [IRunnable, ICallable]);
  // Set before extend() runs, or the generated class would not declare them.
  assert.deepStrictEqual(calls[0].implementationObject.interfaces, [
    IRunnable,
    ICallable,
  ]);
}

// The separate Interfaces decorator does the same thing.
{
  const { Base } = makeNativeBase();
  const IRunnable = {};
  class Child extends Base {}
  Interfaces([IRunnable])(Child);
  assert.deepStrictEqual(Child.prototype.interfaces, [IRunnable]);
}

// A base with a plain (non-native) static extend must not be mistaken for one.
{
  class NotNative {
    static extend() {
      throw new Error('must not be called');
    }
  }
  class Child extends NotNative {}
  assert.doesNotThrow(() => NativeClass(Child));
}

// Applying it to something that is not a class is a mistake worth reporting.
{
  assert.throws(() => NativeClass()(42), /class declaration/);
}

// A fully-qualified name goes to the runtime verbatim, with no
// isTypeScriptExtend flag -- that is the form the runtime takes as-is, and the
// only one the static binding generator can predict.
{
  const { Base, calls } = makeNativeBase();
  class Ticker extends Base {}
  JavaProxy('com.example.Ticker')(Ticker);

  assert.strictEqual(calls.length, 1);
  assert.strictEqual(calls[0].name, 'com.example.Ticker');
  assert.strictEqual(calls[0].isTypeScriptExtend, undefined);
}

// @NativeClass({ name }) is the same thing.
{
  const { Base, calls } = makeNativeBase();
  class Ticker extends Base {}
  NativeClass({ name: 'com.example.Ticker' })(Ticker);
  assert.strictEqual(calls[0].name, 'com.example.Ticker');
}

// An unqualified name would collide across packages and cannot be generated
// ahead of time; say so rather than producing a class nobody can find.
{
  const { Base } = makeNativeBase();
  class Ticker extends Base {}
  assert.throws(
    () => JavaProxy('Ticker')(Ticker),
    /fully qualified/,
  );
}

console.log('native-class tests passed');
