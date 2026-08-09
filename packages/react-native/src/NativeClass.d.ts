/**
 * `@NativeClass` — the marker every class that extends a native class carries.
 *
 * Implemented in plain JavaScript (NativeClass.js) so that it is identical
 * under every bundler and needs no NativeScript-specific transform; these are
 * the types for it.
 */

export type NativeClassOptions = {
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
