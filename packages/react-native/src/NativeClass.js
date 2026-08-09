/**
 * `@NativeClass` — the marker every class that extends a native class carries.
 *
 * This is a plain runtime decorator. It needs no NativeScript-specific
 * transformer, reads nothing at build time, and behaves the same under Babel,
 * SWC, tsc or anything else: all it ever receives is the class object the
 * language hands a decorator, and all it ever does is annotate it before
 * handing it back.
 *
 * Both decorator dialects are supported, because which one a project gets
 * depends on its toolchain rather than on its code:
 *
 *   legacy   (TypeScript `experimentalDecorators`, Babel legacy)
 *              -> called as (target)
 *   standard (TC39 stage 3, Babel/SWC 2023-05, TypeScript 5)
 *              -> called as (value, context)
 *
 * and both the bare and the called form:
 *
 *   @NativeClass
 *   class Foo extends java.lang.Object {}
 *
 *   @NativeClass()
 *   class Foo extends java.lang.Object {}
 *
 *   @NativeClass({ interfaces: [java.lang.Runnable, java.util.concurrent.Callable] })
 *   class Foo extends java.lang.Object {}
 *
 * The interfaces list is the part that carries weight: a JavaScript class has
 * one base, so implementing several Java interfaces cannot be expressed in the
 * `extends` clause. The runtime reads `prototype.interfaces` when it generates
 * the proxy class, which is exactly what this sets.
 */

/** Set on every decorated class, so the runtime and tooling can recognise one. */
const NATIVE_CLASS = "__nsNativeClass";

/** A native base carries the brand the runtime stamps on its own extend(). */
function nativeExtendOf(base) {
  const extend = base && base.extend;
  return typeof extend === "function" && extend.__isNativeExtend__ === true
    ? extend
    : undefined;
}

function annotate(target, options) {
  if (typeof target !== "function") {
    throw new TypeError(
      "@NativeClass can only be applied to a class declaration.",
    );
  }

  Object.defineProperty(target, NATIVE_CLASS, {
    value: true,
    enumerable: false,
    configurable: true,
  });

  const interfaces = options.interfaces;
  if (interfaces && interfaces.length > 0) {
    // On the prototype: the prototype is the implementation object the runtime
    // is handed when it generates the proxy, and it reads `interfaces` off it.
    Object.defineProperty(target.prototype, "interfaces", {
      value: interfaces,
      enumerable: false,
      configurable: true,
      writable: true,
    });
  }

  const explicitName = options.name;
  if (explicitName !== undefined && explicitName.indexOf(".") < 0) {
    throw new Error(
      `@NativeClass name "${explicitName}" must be fully qualified, e.g. ` +
        `"com.example.${explicitName}". The name is the generated Java class's ` +
        `name, and the static binding generator matches on it.`,
    );
  }

  const base = Object.getPrototypeOf(target);
  const extend = nativeExtendOf(base);
  if (!extend) {
    // Not extending a native class. Nothing to wire up, and no reason to
    // complain: @NativeClass on a plain class is harmless.
    return target;
  }

  // Generate the Java class now.
  //
  // This is the whole reason the decorator exists as more than a marker. A
  // downlevelled class calls the runtime's __extends helper, which does this
  // lazily; a real `class X extends Native {}` does not call any helper at all,
  // so without this the JS side works -- overrides resolve through the
  // prototype chain -- while Java sees an unmodified base class and never
  // dispatches an override back into JS.
  //
  // Passing the prototype as the implementation object and `true` for the
  // TypeScript-extend form is exactly what ts_helpers does.
  // A fully-qualified name is taken verbatim by the runtime, with no call-site
  // location mixed in. That is what makes it reproducible at build time: the
  // static binding generator can emit a Java class under exactly this name and
  // the runtime will find it instead of generating one on the device.
  //
  // Without a name the generated class is keyed on the call site, which under a
  // bundler is a position in the bundle rather than in your source -- so nothing
  // ahead of time can predict it, and the class is generated at runtime.
  const extended = explicitName
    ? extend.call(base, explicitName, target.prototype)
    : extend.call(base, target.name, target.prototype, true);

  // Re-point the class at the generated one. `super()` resolves through the
  // constructor's own prototype, so this is what makes construction go via the
  // runtime -- and because it is still `target` that is being constructed, the
  // class's constructor body and field initialisers run on the object the
  // runtime returns, exactly as ES semantics require.
  //
  // Only the constructor. The instance chain is already right: the runtime uses
  // the implementation object it was handed as the generated class's prototype,
  // so extended.prototype is target.prototype, and re-pointing that at itself is
  // a prototype cycle.
  Object.setPrototypeOf(target, extended);

  return target;
}

/**
 * True when `args` is a standard-decorator invocation, i.e. (value, context)
 * with a class context, rather than a legacy (target) one or an options object.
 */
function isStandardDecoratorCall(args) {
  return (
    args.length >= 2 &&
    typeof args[0] === "function" &&
    args[1] != null &&
    typeof args[1] === "object" &&
    args[1].kind === "class"
  );
}

function NativeClass(...args) {
  // @NativeClass  (standard decorators)
  if (isStandardDecoratorCall(args)) {
    return annotate(args[0], {});
  }

  // @NativeClass  (legacy decorators)
  if (args.length === 1 && typeof args[0] === "function") {
    return annotate(args[0], {});
  }

  // @NativeClass(options) / @NativeClass() -- both dialects land here, and the
  // returned decorator has to accept either shape in turn.
  const options = args[0] || {};
  return function decorate(target, _context) {
    return annotate(target, options);
  };
}

/**
 * Declares the Java interfaces a class implements.
 *
 * Equivalent to `@NativeClass({ interfaces })`, kept as a separate decorator
 * because that is how NativeScript has always spelled it.
 */
function Interfaces(interfaces) {
  return function decorate(target, _context) {
    return annotate(target, { interfaces });
  };
}

/**
 * Names the generated Java class.
 *
 * `@JavaProxy("com.example.Ticker")` is `@NativeClass({ name })`, spelled the way
 * NativeScript has always spelled it. Naming a class is what lets it be
 * generated at build time rather than on the device, and it is required for a
 * class that has to exist before the app runs -- one referenced from
 * AndroidManifest.xml, say.
 */
function JavaProxy(name) {
  return function decorate(target, _context) {
    return annotate(target, { name });
  };
}

module.exports = { NativeClass, Interfaces, JavaProxy, NATIVE_CLASS };
