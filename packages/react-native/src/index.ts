import React, {
  forwardRef,
  useEffect,
  useImperativeHandle,
  useLayoutEffect,
  useRef,
  useState,
} from "react";
import type {
  ForwardRefExoticComponent,
  PropsWithoutRef,
  RefAttributes,
} from "react";
import type { ViewProps } from "react-native";
import NativeScriptNativeApi from "./NativeScriptNativeApi";
import {
  defineNativeComponent,
  dispatchNativeComponentCommand,
} from "./defineNativeComponent";

export {
  defineNativeComponent,
  dispatchNativeComponentCommand,
} from "./defineNativeComponent";
export type {
  NativeComponentSpec,
  NativeComponentProps,
  NativeView,
  MountingTransaction,
  TransactionMutation,
} from "./defineNativeComponent";
export type { NSComponentContext } from "./ui/dispatcher";

declare const require: (id: string) => any;

type NativeApiHost = {
  metadata?: {
    classNames?: () => string[];
    functionNames?: () => string[];
    constantNames?: () => string[];
    protocolNames?: () => string[];
    enumNames?: () => string[];
    structNames?: () => string[];
    unionNames?: () => string[];
  };
  import?: (path: string) => boolean;
  getClass?: (name: string) => unknown;
  getProtocol?: (name: string) => unknown;
  getEnum?: (name: string) => unknown;
  getStruct?: (name: string) => unknown;
  getUnion?: (name: string) => unknown;
  [name: string]: unknown;
};

export type InstallOptions = {
  /**
   * Install Objective-C classes/functions/constants as RN runtime globals.
   * Native UI should run through worklets; React Native defaults this off so
   * UIKit cannot be touched from the RN JavaScript thread by accident.
   */
  globals?: boolean;
};

export type NativeScriptWorklets = {
  getUIRuntimeHolder: () => object;
  // M1 (ARCHITECTURE.md §3.3/§7.1): the UIScheduler holder handshake,
  // installed alongside the WorkletRuntime one so native can route off-main
  // async entries through the sanctioned `worklets::scheduleOnUI`.
  getUISchedulerHolder?: () => object;
  isWorkletFunction: (value: unknown) => boolean;
  runOnUIAsync: <Args extends unknown[], ReturnValue>(
    callback: (...args: Args) => ReturnValue | Promise<ReturnValue>,
    ...args: Args
  ) => Promise<ReturnValue>;
};

export type NativeScriptImageLoadOptions = {
  template?: boolean;
};

export type NativeScriptImageLoadCallback = (
  image: unknown | null,
  error: Error | null,
) => void;

const nativeApiGlobalName = "__nativeScriptNativeApi";
const nativeApiGlobalCacheName = "__nativeScriptNativeApiGlobalCache";
const nativeApiTypeCodeKey = "__nativeApiTypeCode";
const nativeApiCallbackThreadKey = "__nativeScriptCallbackThread";
const nativeApiWrappedCallbackKey = "__nativeScriptWrappedCallback";
const nativeClassWrappers = new WeakMap<object, unknown>();

export type NativeScriptCallbackThread = "js" | "runtime";
type AnyFunction = (...args: any[]) => any;
export type NativeScriptInvokedCallback<T extends AnyFunction> = T & {
  readonly __nativeScriptCallbackThread?: NativeScriptCallbackThread;
  readonly __nativeScriptWrappedCallback?: T;
};

const nativeCallbackMetadataSkipKeys = new Set<PropertyKey>([
  "length",
  "name",
  "prototype",
  "arguments",
  "caller",
]);

export type NativeRetainer = {
  readonly size: number;
  retain<T>(value: T): T;
  release(value?: unknown): void;
  dispose(): void;
};

export type NativeDelegateOwner = {
  retain<T>(value: T): T | void;
  release?(value?: unknown): void;
  dispose?(callback: () => void): void;
};

export type CreateDelegateOptions = {
  name?: string;
  thread?: NativeScriptCallbackThread | "caller";
  retainer?: NativeRetainer;
  owner?: NativeDelegateOwner;
  assignTo?: {
    object: unknown;
    property?: string;
  };
};

export type NativeProtocolReference = string | object | Function;

function nativeApiHost(): NativeApiHost | undefined {
  return (globalThis as Record<string, unknown>)[nativeApiGlobalName] as
    NativeApiHost | undefined;
}

function requireNativeApiHost(): NativeApiHost {
  const api = nativeApiHost();
  if (!api) {
    throw new Error(
      "NativeScript Native API JSI host object was not installed",
    );
  }
  return api;
}

function nativeApiGlobalCache(): Record<string, unknown> {
  const globalObject = globalThis as Record<string, unknown>;
  const existing = globalObject[nativeApiGlobalCacheName];
  if (existing && typeof existing === "object") {
    return existing as Record<string, unknown>;
  }

  const cache: Record<string, unknown> = Object.create(null);
  Object.defineProperty(globalThis, nativeApiGlobalCacheName, {
    configurable: false,
    enumerable: false,
    writable: false,
    value: cache,
  });
  return cache;
}

function cacheNativeGlobal(name: string, value: unknown): void {
  if (!name || value === undefined) {
    return;
  }
  nativeApiGlobalCache()[name] = value;
}

// M1 review §3/#3 (fix-list item 3, the ACTUAL root cause; see the
// dedicated ctx.createDelegate report section): `createDelegate` (a real
// worklet, "worklet" directive present) closes over the MODULE-LEVEL
// `defaultNativeRetainer` singleton below and calls its `.retain`/`.release`
// methods. Neither this object's methods NOR the top-level `retain`/
// `release`/`createRetainer` wrappers below carried a `'worklet'` directive
//; so the FIRST time `createDelegate` actually ran on the UI runtime and
// materialized its closure, `defaultNativeRetainer` (a plain object) got
// walked by worklets' closure-cloning and each of ITS non-worklet methods
// was individually wrapped as a "remote function" bound to the RN JS
// thread; calling `defaultNativeRetainer.retain(delegate)` on the UI
// runtime then hit exactly `[Worklets] Tried to synchronously call a Remote
// Function. Called "retain" on the UI Runtime.`, BEFORE any delegate method
// ever ran, which is exactly the symptom this fix list flagged as
// "unverified/likely a worklet-shape gap, not a NativeScript.extend() bug".
// Bisected on-sim (scripts/test_react_native_turbomodule_m1.sh's
// DelegateBisectProbe): a `NSObject.extend()` call with methods that do NOT
// close over `ctx` fails identically on an UNRELATED non-worklet capture
// (`NativeScript.getClass`), confirming the mechanism generalizes: ANY
// non-'worklet' function reachable from a worklet's closure breaks the same
// way, not something specific to `ctx`/`.extend()`.
function createNativeRetainer(): NativeRetainer {
  "worklet";
  const retained: unknown[] = [];
  return {
    // NOT 'worklet'-directived: react-native-worklets' Babel plugin does not
    // support the directive on an object GETTER (confirmed on-sim; it
    // throws `Unexpected token, expected "(" ` while re-parsing the
    // extracted snippet). `.size` is a diagnostic convenience, not on
    // `createDelegate`'s call path, so it stays JS-thread-only for now
    // rather than fighting the plugin; reading it from a worklet will hit
    // the same "Remote Function" guard as everything else in this file that
    // isn't marked; a known, narrow, documented gap.
    get size() {
      return retained.length;
    },
    retain<T>(value: T): T {
      "worklet";
      retained.push(value);
      return value;
    },
    release(value?: unknown) {
      "worklet";
      if (arguments.length === 0) {
        retained.length = 0;
        return;
      }
      for (let i = retained.length - 1; i >= 0; i--) {
        if (retained[i] === value) {
          retained.splice(i, 1);
        }
      }
    },
    dispose() {
      "worklet";
      retained.length = 0;
    },
  };
}

const defaultNativeRetainer = createNativeRetainer();

export function createRetainer(): NativeRetainer {
  "worklet";
  return createNativeRetainer();
}

export function retain<T>(value: T): T {
  "worklet";
  return defaultNativeRetainer.retain(value);
}

export function release(value?: unknown): void {
  "worklet";
  if (arguments.length === 0) {
    defaultNativeRetainer.dispose();
    return;
  }
  defaultNativeRetainer.release(value);
}

function ensureNativeScriptInstalled(): void {
  if (!isInstalled()) {
    init();
  }
}

function defineLazyNativeGlobal(
  name: string,
  resolve: (name: string) => unknown,
  force = false,
) {
  if (!name) {
    return;
  }

  if (!force && Object.prototype.hasOwnProperty.call(globalThis, name)) {
    const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
    if (descriptor && "value" in descriptor) {
      cacheNativeGlobal(name, descriptor.value);
    }
    return;
  }

  try {
    Object.defineProperty(globalThis, name, {
      configurable: true,
      enumerable: false,
      get() {
        const value = resolve(name);
        cacheNativeGlobal(name, value);
        Object.defineProperty(globalThis, name, {
          configurable: true,
          enumerable: false,
          writable: false,
          value,
        });
        return value;
      },
    });
  } catch {
    const value = resolve(name);
    if (value !== undefined) {
      cacheNativeGlobal(name, value);
      Object.defineProperty(globalThis, name, {
        configurable: true,
        enumerable: false,
        writable: false,
        value,
      });
    }
  }
}

function wrapAggregateConstructor(nativeConstructor: unknown): unknown {
  if (typeof nativeConstructor !== "function") {
    return nativeConstructor;
  }

  const aggregate = function NativeScriptAggregate(initialValue?: unknown) {
    return nativeConstructor(initialValue);
  };

  try {
    const hasInstance = Symbol.hasInstance;
    Object.defineProperty(aggregate, hasInstance, {
      configurable: true,
      enumerable: false,
      value(value: unknown) {
        if (!value || typeof value !== "object") {
          return false;
        }
        const actual = value as Record<string, unknown>;
        return (
          actual.kind ===
            (nativeConstructor as unknown as Record<string, unknown>).kind &&
          actual.name ===
            (nativeConstructor as unknown as Record<string, unknown>)
              .runtimeName
        );
      },
    });
  } catch {
    // Older runtimes can expose Symbol.hasInstance as read-only.
  }

  for (const key of [
    "kind",
    "runtimeName",
    "metadataOffset",
    "sizeof",
    "fields",
    "equals",
  ]) {
    try {
      Object.defineProperty(aggregate, key, {
        configurable: true,
        enumerable: false,
        writable: false,
        value: (nativeConstructor as unknown as Record<string, unknown>)[key],
      });
    } catch {
      // Best effort metadata copy for runtimes with stricter function objects.
    }
  }

  return aggregate;
}

function wrapNativeClass(nativeClass: unknown): unknown {
  if (
    !nativeClass ||
    (typeof nativeClass !== "object" && typeof nativeClass !== "function")
  ) {
    return nativeClass;
  }

  const cached = nativeClassWrappers.get(nativeClass as object);
  if (cached) {
    return cached;
  }

  const constructable = function NativeScriptNativeClass(...args: unknown[]) {
    const cls = nativeClass as Record<string, any>;
    if (args.length > 0 && typeof cls.construct === "function") {
      return cls.construct(...args);
    }
    if (typeof cls.alloc !== "function") {
      throw new Error("Native class cannot be allocated");
    }
    const instance = cls.alloc();
    if (instance && typeof instance.init === "function") {
      return instance.init();
    }
    return instance;
  };

  Object.defineProperty(constructable, "new", {
    configurable: true,
    enumerable: false,
    writable: false,
    value(...args: unknown[]) {
      if (args.length !== 0) {
        throw new Error(
          "new does not take arguments; use invoke for an explicit Objective-C selector.",
        );
      }
      return constructable();
    },
  });

  Object.defineProperty(constructable, "__nativeApiClass", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: nativeClass,
  });
  const cachedNativeFunctions = new Map<PropertyKey, unknown>();

  try {
    const hasInstance = Symbol.hasInstance;
    Object.defineProperty(constructable, hasInstance, {
      configurable: true,
      enumerable: false,
      value(value: unknown) {
        if (!value || typeof value !== "object") {
          return false;
        }

        const cls = nativeClass as Record<string, any>;
        try {
          if (
            typeof (value as Record<string, any>).isKindOfClass === "function"
          ) {
            return Boolean(
              (value as Record<string, any>).isKindOfClass(constructable),
            );
          }
        } catch {
          // Fall through to class-name equality for host objects that cannot
          // dispatch isKindOfClass from this thread.
        }

        const expectedName = cls.runtimeName ?? cls.name;
        const actualName = (value as Record<string, unknown>).className;
        return typeof expectedName === "string" && actualName === expectedName;
      },
    });
  } catch {
    // Older runtimes can expose Symbol.hasInstance as read-only.
  }

  const wrapper = new Proxy(constructable, {
    get(target, property, receiver) {
      if (property in target) {
        return Reflect.get(target, property, receiver);
      }
      if (cachedNativeFunctions.has(property)) {
        return cachedNativeFunctions.get(property);
      }
      const nativeValue = (nativeClass as Record<PropertyKey, unknown>)[
        property
      ];
      if (typeof nativeValue === "function") {
        cachedNativeFunctions.set(property, nativeValue);
        try {
          Object.defineProperty(target, property, {
            configurable: true,
            enumerable: false,
            writable: false,
            value: nativeValue,
          });
        } catch {
          // Host runtimes may reject defining function properties; the map is enough.
        }
      }
      return nativeValue;
    },
    set(_target, property, value) {
      (nativeClass as Record<PropertyKey, unknown>)[property] = value;
      return true;
    },
    has(target, property) {
      return property in target || property in (nativeClass as object);
    },
  });

  nativeClassWrappers.set(nativeClass as object, wrapper);
  return wrapper;
}

function wrapInteropFactory(
  nativeFactory: unknown,
  properties: Record<string, unknown>,
): unknown {
  if (typeof nativeFactory !== "function") {
    return nativeFactory;
  }

  if (
    (nativeFactory as unknown as Record<string, unknown>)
      .__nativeScriptConstructable
  ) {
    return nativeFactory;
  }

  const constructable = function NativeScriptInteropValue(...args: unknown[]) {
    return (nativeFactory as (...args: unknown[]) => unknown)(...args);
  };

  try {
    const nativePrototype = (nativeFactory as { prototype?: unknown })
      .prototype;
    if (
      nativePrototype &&
      (typeof nativePrototype === "object" ||
        typeof nativePrototype === "function")
    ) {
      constructable.prototype = nativePrototype;
    }
  } catch {
    // Keep construction working even if the host function exposes a fixed prototype.
  }

  try {
    const hasInstance = Symbol.hasInstance;
    Object.defineProperty(constructable, hasInstance, {
      configurable: true,
      enumerable: false,
      value(value: unknown) {
        return (
          Boolean(value) &&
          typeof value === "object" &&
          (value as Record<string, unknown>).kind === properties.kind
        );
      },
    });
  } catch {
    // Older runtimes can expose Symbol.hasInstance as read-only.
  }

  for (const [key, value] of Object.entries(properties)) {
    try {
      Object.defineProperty(constructable, key, {
        configurable: true,
        enumerable: false,
        writable: false,
        value,
      });
    } catch {
      // Best effort metadata copy for runtimes with stricter function objects.
    }
  }

  Object.defineProperty(constructable, "__nativeScriptConstructable", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: true,
  });

  return constructable;
}

function installInteropConstructors(): void {
  const interop = (globalThis as Record<string, unknown>).interop as
    Record<string, unknown> | undefined;
  if (!interop || typeof interop !== "object") {
    return;
  }

  const sizeof = interop.sizeof;
  const pointerType = (interop.types as Record<string, unknown> | undefined)
    ?.pointer;
  let pointerSize: unknown = undefined;
  if (typeof sizeof === "function" && pointerType !== undefined) {
    try {
      pointerSize = sizeof(pointerType);
    } catch {
      pointerSize = undefined;
    }
  }

  interop.Pointer = wrapInteropFactory(interop.Pointer, {
    kind: "pointer",
    sizeof: pointerSize,
  });
  interop.Reference = wrapInteropFactory(interop.Reference, {
    kind: "reference",
    sizeof: pointerSize,
  });
  interop.Block = wrapInteropFactory(interop.Block, {
    kind: "block",
    sizeof: pointerSize,
  });
  interop.FunctionReference = wrapInteropFactory(interop.FunctionReference, {
    kind: "functionReference",
    sizeof: pointerSize,
  });

  const types = interop.types as Record<string, unknown> | undefined;
  if (types && typeof types === "object") {
    for (const [name, value] of Object.entries(types)) {
      if (typeof value !== "number") {
        continue;
      }
      const boxed = {
        valueOf: () => value,
        toString: () => String(value),
      } as Record<string, unknown>;
      Object.defineProperty(boxed, nativeApiTypeCodeKey, {
        configurable: false,
        enumerable: false,
        writable: false,
        value,
      });
      types[name] = boxed;
    }
  }
}

function defineInlineFunction(name: string, value: Function): void {
  if (Object.prototype.hasOwnProperty.call(globalThis, name)) {
    return;
  }
  Object.defineProperty(globalThis, name, {
    configurable: true,
    enumerable: false,
    writable: true,
    value,
  });
}

function installInlineFunctions(): void {
  const makePoint = (x: number, y: number) => ({ x, y });
  const makeSize = (width: number, height: number) => ({ width, height });
  const makeRect = (x: number, y: number, width: number, height: number) => ({
    origin: { x, y },
    size: { width, height },
  });

  defineInlineFunction("CGPointMake", makePoint);
  defineInlineFunction("NSMakePoint", makePoint);
  defineInlineFunction("CGSizeMake", makeSize);
  defineInlineFunction("NSMakeSize", makeSize);
  defineInlineFunction("CGRectMake", makeRect);
  defineInlineFunction("NSMakeRect", makeRect);
  defineInlineFunction("NSMakeRange", (location: number, length: number) => ({
    location,
    length,
  }));
  defineInlineFunction(
    "UIEdgeInsetsMake",
    (top: number, left: number, bottom: number, right: number) => ({
      top,
      left,
      bottom,
      right,
    }),
  );
}

export function installGlobals(): boolean {
  const api = nativeApiHost();
  if (!api) {
    return false;
  }

  const classNames = api.metadata?.classNames?.() ?? [];
  for (const name of classNames) {
    defineLazyNativeGlobal(name, (className) =>
      wrapNativeClass(api[className]),
    );
  }

  const functionNames = api.metadata?.functionNames?.() ?? [];
  for (const name of functionNames) {
    defineLazyNativeGlobal(name, (functionName) => api[functionName]);
  }

  const constantNames = api.metadata?.constantNames?.() ?? [];
  for (const name of constantNames) {
    defineLazyNativeGlobal(name, (constantName) => api[constantName]);
  }

  const protocolNames = api.metadata?.protocolNames?.() ?? [];
  for (const name of protocolNames) {
    defineLazyNativeGlobal(
      name,
      (protocolName) => api.getProtocol?.(protocolName) ?? api[protocolName],
    );
  }

  const enumNames = api.metadata?.enumNames?.() ?? [];
  for (const name of enumNames) {
    const resolveEnum = (enumName: string) =>
      api.getEnum?.(enumName) ?? api[enumName];
    defineLazyNativeGlobal(name, resolveEnum);

    const enumValue = resolveEnum(name);
    if (!enumValue || typeof enumValue !== "object") {
      continue;
    }
    for (const memberName of Object.keys(enumValue)) {
      if (/^-?\d+$/.test(memberName)) {
        continue;
      }
      defineLazyNativeGlobal(
        memberName,
        () => (enumValue as Record<string, unknown>)[memberName],
      );
    }
  }

  const structNames = api.metadata?.structNames?.() ?? [];
  for (const name of structNames) {
    defineLazyNativeGlobal(
      name,
      (structName) =>
        wrapAggregateConstructor(
          api.getStruct?.(structName) ?? api[structName],
        ),
      true,
    );
  }

  const unionNames = api.metadata?.unionNames?.() ?? [];
  for (const name of unionNames) {
    defineLazyNativeGlobal(
      name,
      (unionName) =>
        wrapAggregateConstructor(api.getUnion?.(unionName) ?? api[unionName]),
      true,
    );
  }

  return true;
}

export function init(metadataPath = "", options: InstallOptions = {}): boolean {
  const installed =
    NativeScriptNativeApi.isInstalled() ||
    NativeScriptNativeApi.install(metadataPath);
  if (installed) {
    installInteropConstructors();
    installInlineFunctions();
  }
  if (installed && options.globals === true) {
    installGlobals();
  }
  if (installed) {
    ensureWorkletsInstalled(metadataPath);
  }
  return installed;
}

export const install = init;

export function isInstalled(): boolean {
  return NativeScriptNativeApi.isInstalled();
}

export function defaultMetadataPath(): string {
  return NativeScriptNativeApi.defaultMetadataPath();
}

export function getRuntimeBackend(): string {
  return NativeScriptNativeApi.getRuntimeBackend();
}

let workletsAdapter: NativeScriptWorklets | undefined;
const workletsPackageName = "react-native-worklets";

function workletsSetupError(reason: string): Error {
  return new Error(
    `${reason}. Install ${workletsPackageName}, add ${workletsPackageName}/plugin to your Babel plugins, and run pod install so RNWorklets is linked.`,
  );
}

function requireReactNativeWorklets(): NativeScriptWorklets {
  try {
    return require(workletsPackageName) as NativeScriptWorklets;
  } catch (error) {
    throw workletsSetupError(
      `NativeScript.scheduleOnUI requires ${workletsPackageName}`,
    );
  }
}

function validateWorkletsModule(
  worklets: NativeScriptWorklets,
): NativeScriptWorklets {
  if (
    worklets == null ||
    typeof worklets.getUIRuntimeHolder !== "function" ||
    typeof worklets.isWorkletFunction !== "function" ||
    typeof worklets.runOnUIAsync !== "function"
  ) {
    throw workletsSetupError(
      "NativeScript.scheduleOnUI received an incompatible Worklets module",
    );
  }
  return worklets;
}

function ensureWorkletsInstalled(metadataPath = ""): NativeScriptWorklets {
  if (workletsAdapter) {
    return workletsAdapter;
  }
  installWorklets(requireReactNativeWorklets(), metadataPath);
  return workletsAdapter as unknown as NativeScriptWorklets;
}

export function installWorklets(
  worklets: NativeScriptWorklets = requireReactNativeWorklets(),
  metadataPath = "",
): boolean {
  if (!NativeScriptNativeApi.isInstalled()) {
    const installed = NativeScriptNativeApi.install(metadataPath);
    if (!installed) {
      throw new Error(
        "NativeScript Native API JSI host object was not installed",
      );
    }
    installInteropConstructors();
    installInlineFunctions();
  }

  const validWorklets = validateWorkletsModule(worklets);
  const holder = validWorklets.getUIRuntimeHolder();
  if (holder == null || typeof holder !== "object") {
    throw workletsSetupError(
      "NativeScript.scheduleOnUI could not resolve a Worklets UI runtime",
    );
  }
  // Best-effort: an older/incompatible Worklets module without
  // getUISchedulerHolder still installs fine; the gateway falls back to a
  // plain dispatch_async(main) when no scheduler is available.
  const schedulerHolder =
    typeof validWorklets.getUISchedulerHolder === "function"
      ? validWorklets.getUISchedulerHolder()
      : {};
  const installRuntime = NativeScriptNativeApi.installUIRuntime;
  if (typeof installRuntime !== "function") {
    throw workletsSetupError(
      "NativeScript Native API was built without RNWorklets runtime support",
    );
  }
  const installed = installRuntime(
    holder,
    schedulerHolder as object,
    metadataPath,
  );
  if (!installed) {
    throw workletsSetupError(
      "NativeScript Native API could not install into the Worklets UI runtime",
    );
  }
  workletsAdapter = validWorklets;
  return true;
}

export function scheduleOnUI<Args extends unknown[], ReturnValue>(
  callback: (...args: Args) => ReturnValue | Promise<ReturnValue>,
  ...args: Args
): Promise<ReturnValue> {
  if (typeof callback !== "function") {
    throw new TypeError(
      "NativeScript.scheduleOnUI expects a Worklets callback",
    );
  }

  ensureNativeScriptInstalled();
  const worklets = ensureWorkletsInstalled();
  if (worklets.isWorkletFunction(callback) !== true) {
    throw workletsSetupError(
      "NativeScript.scheduleOnUI requires a worklet callback",
    );
  }
  return worklets.runOnUIAsync(callback, ...args);
}

function callbackInvoker<T extends AnyFunction>(
  thread: NativeScriptCallbackThread,
  callback: T,
): NativeScriptInvokedCallback<T> {
  "worklet";

  if (typeof callback !== "function") {
    throw new TypeError("NativeScript callback invoker expects a function");
  }

  const existingPolicy = (callback as Record<string, unknown>)[
    nativeApiCallbackThreadKey
  ];
  if (existingPolicy === thread) {
    return callback as NativeScriptInvokedCallback<T>;
  }

  const wrapped = function nativeScriptInvokedCallback(
    this: unknown,
    ...args: unknown[]
  ) {
    return callback.apply(this, args);
  } as NativeScriptInvokedCallback<T>;

  for (const key of [
    ...Object.getOwnPropertyNames(callback),
    ...Object.getOwnPropertySymbols(callback),
  ]) {
    if (nativeCallbackMetadataSkipKeys.has(key)) {
      continue;
    }

    const descriptor = Object.getOwnPropertyDescriptor(callback, key);
    if (!descriptor) {
      continue;
    }

    try {
      Object.defineProperty(wrapped, key, descriptor);
    } catch {
      // Metadata preservation is best-effort for runtimes with fixed function
      // internals; the callback policy markers below are still applied.
    }
  }

  Object.defineProperties(wrapped, {
    [nativeApiCallbackThreadKey]: {
      configurable: false,
      enumerable: false,
      writable: false,
      value: thread,
    },
    [nativeApiWrappedCallbackKey]: {
      configurable: false,
      enumerable: false,
      writable: false,
      value: callback,
    },
  });
  return wrapped;
}

export function uiInvoker<T extends AnyFunction>(_callback: T): never {
  throw new Error(
    'NativeScript.uiInvoker is not supported in React Native. Use a Worklets "worklet" callback with NativeScript.scheduleOnUI().',
  );
}

export function jsInvoker<T extends AnyFunction>(
  callback: T,
): NativeScriptInvokedCallback<T> {
  "worklet";

  return callbackInvoker("js", callback);
}

export function runtimeInvoker<T extends AnyFunction>(
  callback: T,
): NativeScriptInvokedCallback<T> {
  "worklet";

  return callbackInvoker("runtime", callback);
}

function nativeScriptCallbackThread(
  callback: AnyFunction,
): NativeScriptCallbackThread | undefined {
  "worklet";

  const thread = (callback as unknown as Record<string, unknown>)[
    nativeApiCallbackThreadKey
  ];
  return thread === "js" || thread === "runtime" ? thread : undefined;
}

function nativeScriptWrappedCallback(callback: AnyFunction): AnyFunction {
  "worklet";

  const wrapped = (callback as unknown as Record<string, unknown>)[
    nativeApiWrappedCallbackKey
  ];
  return typeof wrapped === "function" ? (wrapped as AnyFunction) : callback;
}

function invokeNativeScriptCallback(
  callback: AnyFunction,
  args: unknown[],
  isDisposed?: () => boolean,
): void {
  "worklet";

  if (nativeScriptCallbackThread(callback) !== "js") {
    callback(...args);
    return;
  }

  const handler = nativeScriptWrappedCallback(callback);
  const workletsProxy = (globalThis as Record<string, any>)
    .__workletsModuleProxy;
  const serializer = (globalThis as Record<string, any>).__serializer;

  if (
    workletsProxy &&
    typeof workletsProxy.scheduleOnRN === "function" &&
    typeof serializer === "function"
  ) {
    workletsProxy.scheduleOnRN(handler, serializer(args));
    return;
  }

  setTimeout(() => {
    if (!isDisposed?.()) {
      handler(...args);
    }
  }, 0);
}

export function eventBridge<T extends AnyFunction>(
  callback: T,
  thread: NativeScriptCallbackThread | "caller" = "js",
): T | NativeScriptInvokedCallback<T> {
  "worklet";

  if (thread === "js") {
    return jsInvoker(callback);
  }
  if (thread === "runtime") {
    return runtimeInvoker(callback);
  }
  return callback;
}

export const createEventBridge = eventBridge;

export function isMainThread(): boolean {
  "worklet";

  const NSThread = (globalThis as Record<string, any>).NSThread;
  return NSThread?.isMainThread === true;
}

export function assertUIKitThread(
  message = "UIKit native APIs must be called through NativeScript.scheduleOnUI",
): void {
  "worklet";

  if (!isMainThread()) {
    throw new Error(message);
  }
}

export function loadImage(
  source: unknown,
  options: NativeScriptImageLoadOptions = {},
  callback: NativeScriptImageLoadCallback,
): boolean {
  "worklet";

  const loadReactImage = (globalThis as Record<string, any>)
    .__nativeScriptLoadReactImage;
  if (typeof loadReactImage !== "function" || typeof callback !== "function") {
    return false;
  }

  return (
    loadReactImage(
      source,
      options.template === true,
      (handle: unknown, errorMessage: unknown) => {
        "worklet";

        const interop = (globalThis as Record<string, any>).interop;
        const image =
          typeof handle === "string" && handle.length > 0
            ? (interop?.object?.(interop.Pointer(handle)) ?? null)
            : null;
        const error =
          typeof errorMessage === "string" && errorMessage.length > 0
            ? new Error(errorMessage)
            : null;
        callback(image, error);
      },
    ) === true
  );
}

export function warnIfNotUIKitThread(
  message = "UIKit native APIs should be mutated through NativeScript.scheduleOnUI",
): boolean {
  "worklet";

  if (isMainThread()) {
    return false;
  }
  if (typeof console !== "undefined" && typeof console.warn === "function") {
    console.warn(message);
  }
  return true;
}

function systemFrameworkPath(nameOrPath: string): string {
  if (!nameOrPath) {
    return "";
  }
  if (nameOrPath.includes("/")) {
    return nameOrPath;
  }
  const frameworkName = nameOrPath.endsWith(".framework")
    ? nameOrPath
    : `${nameOrPath}.framework`;
  return `/System/Library/Frameworks/${frameworkName}`;
}

export function getClass<T = unknown>(name: string): T | null {
  if (!name) {
    return null;
  }
  const api = requireNativeApiHost();
  const nativeClass = api.getClass?.(name) ?? api[name];
  if (nativeClass == null) {
    return null;
  }
  const wrapped = wrapNativeClass(nativeClass);
  return wrapped == null ? null : (wrapped as T);
}

export function getProtocol<T = unknown>(name: string): T | null {
  if (!name) {
    return null;
  }
  const api = requireNativeApiHost();
  const protocol = api.getProtocol?.(name) ?? api[name];
  return protocol == null ? null : (protocol as T);
}

function requireNSObject(): any {
  "worklet";

  const nsObject = (globalThis as Record<string, any>).NSObject;
  if (!nsObject || typeof nsObject.extend !== "function") {
    throw new Error("NSObject.extend is not available");
  }
  return nsObject;
}

export function isClassAvailable(name: string): boolean {
  const nativeClass = getClass<Record<string, unknown>>(name);
  if (!nativeClass) {
    return false;
  }
  if (typeof nativeClass.available === "boolean") {
    return nativeClass.available;
  }
  return true;
}

function frameworkBundle(nameOrPath: string): any | null {
  const NSBundle = getClass<any>("NSBundle");
  if (!NSBundle || typeof NSBundle.bundleWithPath !== "function") {
    return null;
  }
  const path = systemFrameworkPath(nameOrPath);
  if (!path) {
    return null;
  }
  return NSBundle.bundleWithPath(path) ?? null;
}

const frameworkSentinelClasses: Record<string, string> = {
  Foundation: "NSObject",
  UIKit: "UIView",
  QuickLook: "QLPreviewController",
  VisionKit: "VNDocumentCameraViewController",
  PassKit: "PKPass",
};

function frameworkName(nameOrPath: string): string {
  const match = /([^/]+)\.framework(?:\/)?$/.exec(nameOrPath);
  if (match) {
    return match[1];
  }
  return nameOrPath.replace(/\.framework$/, "");
}

export function isFrameworkLoaded(nameOrPath: string): boolean {
  const sentinelClass = frameworkSentinelClasses[frameworkName(nameOrPath)];
  if (sentinelClass && isClassAvailable(sentinelClass)) {
    return true;
  }
  const bundle = frameworkBundle(nameOrPath);
  if (!bundle) {
    return false;
  }
  if (typeof bundle.loaded === "boolean") {
    return bundle.loaded;
  }
  if (typeof bundle.isLoaded === "function") {
    return Boolean(bundle.isLoaded());
  }
  return false;
}

export function loadFramework(nameOrPath: string): boolean {
  if (!nameOrPath) {
    return false;
  }
  if (isFrameworkLoaded(nameOrPath)) {
    return true;
  }
  const api = requireNativeApiHost();
  try {
    if (typeof api.import === "function") {
      api.import(nameOrPath);
      return true;
    }
  } catch {
    // Fall through to NSBundle below so callers get a false availability result.
  }
  const bundle = frameworkBundle(nameOrPath);
  if (!bundle || typeof bundle.load !== "function") {
    return false;
  }
  try {
    return Boolean(bundle.load());
  } catch {
    return false;
  }
}

function resolveProtocolReference(
  protocolRef: NativeProtocolReference,
): unknown {
  "worklet";

  if (typeof protocolRef !== "string") {
    return protocolRef;
  }
  return (
    (globalThis as Record<string, unknown>)[protocolRef] ??
    getProtocol(protocolRef)
  );
}

function wrapDelegateMethods<T extends object>(
  methods: T,
  thread: CreateDelegateOptions["thread"],
): T {
  "worklet";

  if (!thread || thread === "caller") {
    return methods;
  }

  const wrapped = Object.create(Object.getPrototypeOf(methods));
  for (const key of Reflect.ownKeys(methods)) {
    const descriptor = Object.getOwnPropertyDescriptor(methods, key);
    if (!descriptor) {
      continue;
    }
    if ("value" in descriptor && typeof descriptor.value === "function") {
      descriptor.value = eventBridge(descriptor.value, thread);
    }
    Object.defineProperty(wrapped, key, descriptor);
  }
  return wrapped;
}

export function createDelegate<T extends object>(
  protocols: NativeProtocolReference | NativeProtocolReference[],
  methods: Partial<T>,
  options: CreateDelegateOptions = {},
): T {
  "worklet";

  const protocolList = (Array.isArray(protocols) ? protocols : [protocols])
    .map(resolveProtocolReference)
    .filter(Boolean);
  if (protocolList.length === 0) {
    throw new Error(
      "NativeScript.createDelegate requires at least one protocol",
    );
  }

  const DelegateClass = requireNSObject().extend(
    wrapDelegateMethods(methods, options.thread),
    {
      protocols: protocolList,
      name: options.name,
    },
  );
  const delegate = DelegateClass.alloc().init() as T;
  if (options.retainer) {
    options.retainer.retain(delegate);
  } else if (options.owner) {
    options.owner.retain(delegate);
  } else {
    defaultNativeRetainer.retain(delegate);
  }

  const assignedObject = options.assignTo?.object as
    Record<string, unknown> | undefined;
  const assignedProperty = options.assignTo?.property ?? "delegate";
  if (assignedObject) {
    assignedObject[assignedProperty] = delegate;
  }

  options.owner?.dispose?.(() => {
    if (assignedObject && assignedObject[assignedProperty] === delegate) {
      assignedObject[assignedProperty] = null;
    }
    options.owner?.release?.(delegate);
    options.retainer?.release(delegate);
    if (!options.retainer && !options.owner) {
      defaultNativeRetainer.release(delegate);
    }
  });

  return delegate;
}

const NativeScript = {
  init,
  install,
  installGlobals,
  isInstalled,
  defaultMetadataPath,
  defineNativeComponent,
  dispatchNativeComponentCommand,
  getRuntimeBackend,
  installWorklets,
  assertUIKitThread,
  createDelegate,
  createEventBridge,
  createRetainer,
  eventBridge,
  getClass,
  getProtocol,
  isClassAvailable,
  isFrameworkLoaded,
  isMainThread,
  jsInvoker,
  loadImage,
  loadFramework,
  release,
  retain,
  scheduleOnUI,
  runtimeInvoker,
  uiInvoker,
  warnIfNotUIKitThread,
};

export default NativeScript;
