/**
 * `@NativeClass` — the marker every class that extends a native class carries.
 *
 * Implemented in plain JavaScript (NativeClass.js) so that it is identical
 * under every bundler and needs no NativeScript-specific transform; these are
 * the types for it.
 */

export type NativeClassOptions = {
  /**
   * Fully-qualified name for the generated Java class, e.g.
   * "com.example.Ticker". Taken verbatim by the runtime, which is what allows
   * the static binding generator to emit the class at build time. Without it
   * the class is keyed on the call site and generated on the device.
   */
  name?: string;

  /**
   * Java interfaces the generated class should implement, in addition to
   * whatever it extends. A JavaScript class has one base, so this is the only
   * way to express "implements Runnable and Callable".
   */
  interfaces?: unknown[];
};

type AnyClass = { new (...args: any[]): any; prototype: any };

/** Set on every decorated class. */
export declare const NATIVE_CLASS: "__nsNativeClass";

export declare function NativeClass<T extends AnyClass>(target: T): T;
export declare function NativeClass<T extends AnyClass>(
  target: T,
  context: unknown,
): T;
export declare function NativeClass(
  options?: NativeClassOptions,
): <T extends AnyClass>(target: T, context?: unknown) => T;

/** Equivalent to `@NativeClass({ interfaces })`. */
export declare function Interfaces(
  interfaces: unknown[],
): <T extends AnyClass>(target: T, context?: unknown) => T;

/** `@JavaProxy(name)` — equivalent to `@NativeClass({ name })`. */
export declare function JavaProxy(
  name: string,
): <T extends AnyClass>(target: T, context?: unknown) => T;
