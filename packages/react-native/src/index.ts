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
import NativeScriptUIViewNativeComponent from "./NativeScriptUIViewNativeComponent";

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
  isWorkletFunction: (value: unknown) => boolean;
  runOnUIAsync: <Args extends unknown[], ReturnValue>(
    callback: (...args: Args) => ReturnValue | Promise<ReturnValue>,
    ...args: Args
  ) => Promise<ReturnValue>;
};

export type UIKitSizingMode =
  | "fill"
  | "intrinsic"
  | "sizeThatFits"
  | "autoLayout";

export type UIKitLayoutOptions = {
  sizing?: UIKitSizingMode;
  defaultSize?: { width?: number; height?: number };
  minSize?: { width?: number; height?: number };
  maxSize?: { width?: number; height?: number };
};

export type UIKitHostReadyEvent = {
  nativeEvent: {
    hostReadyId: string;
    hostId: string;
    nativeViewHandle: string;
    childrenViewHandle: string;
    controllerHandle: string;
    hasChildren: boolean;
  };
};

export type UIKitViewContext<Props extends object> = {
  readonly name: string;
  readonly tag: number | null;
  readonly props: Readonly<Props>;
  emit<K extends keyof Props>(
    eventName: K,
    payload?: Props[K] extends ((arg: infer Payload) => unknown) | undefined
      ? Payload
      : unknown,
  ): void;
  targetAction(control: unknown, events: unknown, callback: () => void): void;
  gestureAction(gesture: unknown, callback: (gesture: unknown) => void): void;
  actionTarget(callback: (sender: unknown) => void): {
    target: unknown;
    action: string;
  };
  delegate<T extends object>(
    object: unknown,
    protocolRef: unknown,
    implementation: Partial<T>,
  ): T;
  notification(
    name: string,
    object: unknown | null,
    callback: (notification: unknown) => void,
  ): void;
  observe(
    object: unknown,
    keyPath: string,
    callback: (value: unknown, change: unknown) => void,
  ): void;
  retain<T>(value: T): T;
  release(value?: unknown): void;
  dispose(callback: () => void): void;
  invalidateLayout(): void;
  loadImage(
    source: unknown,
    options: NativeScriptImageLoadOptions,
    callback: NativeScriptImageLoadCallback,
  ): boolean;
};

type UIKitCreateArgument<Props extends object> = UIKitViewContext<Props> &
  Readonly<Props>;

export type NativeScriptImageLoadOptions = {
  template?: boolean;
};

export type NativeScriptImageLoadCallback = (
  image: unknown | null,
  error: Error | null,
) => void;

export type UIKitDisposeResult =
  | void
  | {
      removeHostView?: boolean;
    };

export type UIKitViewDefinition<Props extends object, NativeView = unknown> = {
  name?: string;
  debugName?: string;
  displayName?: string;
  layout?: UIKitLayoutOptions;
  create: (ctx: UIKitCreateArgument<Props & ViewProps>) => NativeView;
  update?: (
    view: NativeView,
    props: Readonly<Props & ViewProps>,
    previousProps?: Readonly<Props & ViewProps>,
    ctx?: UIKitViewContext<Props & ViewProps>,
  ) => void;
  mounted?: (
    view: NativeView,
    props: Readonly<Props & ViewProps>,
    ctx?: UIKitViewContext<Props & ViewProps>,
  ) => void;
  dispose?: (
    view: NativeView,
    props: Readonly<Props & ViewProps>,
    ctx?: UIKitViewContext<Props & ViewProps>,
  ) => UIKitDisposeResult;
  nativeProps?: (
    props: Readonly<Props & ViewProps>,
  ) => Partial<ViewProps> | undefined;
};

export type UIKitViewRef<NativeView = unknown> = {
  readonly nativeView: NativeView | null;
  runOnUI: <T>(callback: (view: NativeView) => T) => Promise<T>;
  measureNative: () => Promise<{ width: number; height: number }>;
  invalidateNativeLayout: () => void;
};

export type UIKitHostViewProps = ViewProps & {
  attachController?: boolean;
  attachControllerView?: boolean;
  attachNativeView?: boolean;
  onHostReady?: (event: UIKitHostReadyEvent) => void;
};

export type UIKitViewComponent<
  Props extends object,
  NativeView = unknown,
> = ForwardRefExoticComponent<
  PropsWithoutRef<Props & UIKitHostViewProps> &
    RefAttributes<UIKitViewRef<NativeView>>
>;

export type UIKitContainerResult<RootView = unknown, ChildrenView = unknown> = {
  rootView: RootView;
  childrenView: ChildrenView;
};

export type UIKitContainerDefinition<
  Props extends object,
  RootView = unknown,
  ChildrenView = unknown,
> = Omit<
  UIKitViewDefinition<Props, UIKitContainerResult<RootView, ChildrenView>>,
  "create" | "update" | "mounted" | "dispose"
> & {
  create: (
    ctx: UIKitCreateArgument<Props & ViewProps>,
  ) => UIKitContainerResult<RootView, ChildrenView>;
  update?: (
    view: UIKitContainerResult<RootView, ChildrenView>,
    props: Readonly<Props & ViewProps>,
    previousProps?: Readonly<Props & ViewProps>,
    ctx?: UIKitViewContext<Props & ViewProps>,
  ) => void;
  mounted?: (
    view: UIKitContainerResult<RootView, ChildrenView>,
    props: Readonly<Props & ViewProps>,
    ctx?: UIKitViewContext<Props & ViewProps>,
  ) => void;
  dispose?: (
    view: UIKitContainerResult<RootView, ChildrenView>,
    props: Readonly<Props & ViewProps>,
    ctx?: UIKitViewContext<Props & ViewProps>,
  ) => UIKitDisposeResult;
};

export type UIViewControllerDefinition<
  Props extends object,
  Controller = unknown,
> = Omit<UIKitViewDefinition<Props, Controller>, "create"> & {
  createController: (ctx: UIKitCreateArgument<Props & ViewProps>) => Controller;
  hostView?: (controller: Controller) => unknown;
  childrenView?: (controller: Controller) => unknown;
};

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
    | NativeApiHost
    | undefined;
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

function createNativeRetainer(): NativeRetainer {
  const retained: unknown[] = [];
  return {
    get size() {
      return retained.length;
    },
    retain<T>(value: T): T {
      retained.push(value);
      return value;
    },
    release(value?: unknown) {
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
      retained.length = 0;
    },
  };
}

const defaultNativeRetainer = createNativeRetainer();

export function createRetainer(): NativeRetainer {
  return createNativeRetainer();
}

export function retain<T>(value: T): T {
  return defaultNativeRetainer.retain(value);
}

export function release(value?: unknown): void {
  if (arguments.length === 0) {
    defaultNativeRetainer.dispose();
    return;
  }
  defaultNativeRetainer.release(value);
}

const hostViewPropNames = new Set([
  "accessible",
  "accessibilityActions",
  "accessibilityElementsHidden",
  "accessibilityHint",
  "accessibilityIgnoresInvertColors",
  "accessibilityLabel",
  "accessibilityLanguage",
  "accessibilityLiveRegion",
  "accessibilityRole",
  "accessibilityState",
  "accessibilityValue",
  "accessibilityViewIsModal",
  "children",
  "collapsable",
  "focusable",
  "hitSlop",
  "id",
  "importantForAccessibility",
  "nativeID",
  "needsOffscreenAlphaCompositing",
  "onAccessibilityAction",
  "onAccessibilityEscape",
  "onAccessibilityTap",
  "onHostReady",
  "onLayout",
  "onMagicTap",
  "onMoveShouldSetResponder",
  "onMoveShouldSetResponderCapture",
  "onResponderEnd",
  "onResponderGrant",
  "onResponderMove",
  "onResponderReject",
  "onResponderRelease",
  "onResponderStart",
  "onResponderTerminate",
  "onResponderTerminationRequest",
  "onStartShouldSetResponder",
  "onStartShouldSetResponderCapture",
  "pointerEvents",
  "removeClippedSubviews",
  "renderToHardwareTextureAndroid",
  "shouldRasterizeIOS",
  "style",
  "testID",
]);

function splitUIKitViewProps<Props extends object>(
  props: Props & UIKitHostViewProps,
  definition: UIKitViewDefinition<Props>,
): {
  nativeProps: ViewProps;
  pluginProps: Props & UIKitHostViewProps;
} {
  const nativeProps: Record<string, unknown> = {};
  const pluginProps: Record<string, unknown> = {};

  for (const [key, value] of Object.entries(props)) {
    if (
      hostViewPropNames.has(key) ||
      key.startsWith("accessibility") ||
      key.startsWith("aria-")
    ) {
      nativeProps[key] = value;
    } else {
      pluginProps[key] = value;
    }
  }

  Object.assign(nativeProps, definition.nativeProps?.(props));

  return {
    nativeProps: nativeProps as ViewProps,
    pluginProps: pluginProps as Props & UIKitHostViewProps,
  };
}

function nativeHandleForUIKitView(view: unknown): string {
  "worklet";

  const interop = (globalThis as Record<string, any>).interop;
  if (!interop || typeof interop.handleof !== "function") {
    throw new Error("NativeScript interop globals are not installed");
  }

  const pointer = interop.handleof(view);
  if (!pointer) {
    throw new Error(
      "UIKit view definition returned a value without a native handle",
    );
  }

  if (typeof pointer.toHexString === "function") {
    const text = pointer.toHexString();
    if (typeof text === "string" && text.length > 0) {
      return text;
    }
  }

  if (typeof pointer.address === "string" && pointer.address.length > 0) {
    return pointer.address;
  }

  if (typeof pointer.address === "number") {
    return String(pointer.address);
  }

  if (typeof pointer.toNumber === "function") {
    return String(pointer.toNumber());
  }

  throw new Error("UIKit view native handle could not be read");
}

function nativeHandleOrUndefined(value: unknown): string | undefined {
  "worklet";

  return value == null ? undefined : nativeHandleForUIKitView(value);
}

function nativeHandleForNSObject(value: unknown): string | undefined {
  "worklet";

  if (value == null) {
    return undefined;
  }
  const interop = (globalThis as Record<string, any>).interop;
  const pointer = interop?.handleof?.(value);
  if (!pointer) {
    return undefined;
  }
  if (typeof pointer.toHexString === "function") {
    return pointer.toHexString();
  }
  if (typeof pointer.address === "string") {
    return pointer.address;
  }
  if (typeof pointer.address === "number") {
    return String(pointer.address);
  }
  if (typeof pointer.toNumber === "function") {
    return String(pointer.toNumber());
  }
  return undefined;
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
          actual.kind === (nativeConstructor as Record<string, unknown>).kind &&
          actual.name ===
            (nativeConstructor as Record<string, unknown>).runtimeName
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
        value: (nativeConstructor as Record<string, unknown>)[key],
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

  if ((nativeFactory as Record<string, unknown>).__nativeScriptConstructable) {
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
    | Record<string, unknown>
    | undefined;
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
      `NativeScript.runOnUI requires ${workletsPackageName}`,
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
      "NativeScript.runOnUI received an incompatible Worklets module",
    );
  }
  return worklets;
}

function installIdleAwareWorkletsFrameLoop(): boolean {
  "worklet";

  const globalObject = globalThis as Record<string, any>;
  if (globalObject.__nativeScriptIdleAwareWorkletsFrameLoop === true) {
    return true;
  }

  const nativeRequestAnimationFrame =
    globalObject.__nativeRequestAnimationFrame;
  const callMicrotasks = globalObject.__callMicrotasks;

  if (
    typeof nativeRequestAnimationFrame !== "function" ||
    typeof callMicrotasks !== "function"
  ) {
    return false;
  }

  globalObject.__nativeScriptIdleAwareWorkletsFrameLoop = true;
  globalObject.__nativeScriptNativeRequestAnimationFrame =
    nativeRequestAnimationFrame;

  let queuedCallbacks: Array<(timestamp: number) => void> = [];
  let queuedCallbacksBegin = 0;
  let queuedCallbacksEnd = 0;
  let flushedCallbacks = queuedCallbacks;
  let flushedCallbacksBegin = 0;
  let flushedCallbacksEnd = 0;
  let queuedFinalizers: Array<() => void> = [];
  let nativeFlushScheduled = false;

  const NSTimerClass = globalObject.NSTimer;
  const NSRunLoopClass = globalObject.NSRunLoop;
  if (
    NSTimerClass == null ||
    NSRunLoopClass == null ||
    NSRunLoopClass.mainRunLoop == null
  ) {
    throw new Error("NativeScript Worklets timers require NSTimer/NSRunLoop");
  }

  type NativeTimer = { invalidate?: () => void };
  const nativeTimers = new Map<number, NativeTimer>();
  let nextNativeTimerHandle = 1;

  function runtimeTimerInvoker<T extends (...args: any[]) => any>(
    callback: T,
  ): T {
    const wrapped = function nativeScriptWorkletTimerCallback(
      this: unknown,
      ...args: unknown[]
    ) {
      return callback.apply(this, args);
    } as T;
    Object.defineProperties(wrapped, {
      __nativeScriptCallbackThread: {
        configurable: false,
        enumerable: false,
        writable: false,
        value: "runtime",
      },
      __nativeScriptWrappedCallback: {
        configurable: false,
        enumerable: false,
        writable: false,
        value: callback,
      },
    });
    return wrapped;
  }

  function normalizeTimerDelay(delay: unknown): number {
    const numericDelay =
      typeof delay === "number" && Number.isFinite(delay) ? delay : 0;
    return Math.max(0.001, numericDelay / 1000);
  }

  function scheduleNativeTimer(
    callback: (...args: unknown[]) => void,
    delay: unknown,
    repeats: boolean,
    args: unknown[],
  ): number {
    if (typeof callback !== "function") {
      throw new TypeError("NativeScript Worklets timer expects a callback");
    }

    const handle = nextNativeTimerHandle++;
    const fireTimer = runtimeTimerInvoker((timer: NativeTimer) => {
      if (!nativeTimers.has(handle)) {
        return;
      }
      if (!repeats) {
        nativeTimers.delete(handle);
      }
      callback(...args);
      callMicrotasks();
      if (!repeats) {
        timer?.invalidate?.();
      }
    });

    const interval = normalizeTimerDelay(delay);
    const timer =
      typeof NSTimerClass.timerWithTimeIntervalRepeatsBlock === "function"
        ? NSTimerClass.timerWithTimeIntervalRepeatsBlock(
            interval,
            repeats,
            fireTimer,
          )
        : NSTimerClass.scheduledTimerWithTimeIntervalRepeatsBlock(
            interval,
            repeats,
            fireTimer,
          );

    nativeTimers.set(handle, timer);
    if (typeof NSTimerClass.timerWithTimeIntervalRepeatsBlock === "function") {
      NSRunLoopClass.mainRunLoop.addTimerForMode(
        timer,
        "kCFRunLoopCommonModes",
      );
    }
    return handle;
  }

  function clearNativeTimer(handle: unknown) {
    if (typeof handle !== "number") {
      return;
    }
    const timer = nativeTimers.get(handle);
    nativeTimers.delete(handle);
    timer?.invalidate?.();
  }

  function hasPendingFrameWork() {
    return queuedCallbacks.length > 0 || queuedFinalizers.length > 0;
  }

  function executeQueue(timestamp: number) {
    flushedCallbacks = queuedCallbacks;
    queuedCallbacks = [];

    flushedCallbacksBegin = queuedCallbacksBegin;
    flushedCallbacksEnd = queuedCallbacksEnd;
    queuedCallbacksBegin = queuedCallbacksEnd;

    for (const callback of flushedCallbacks) {
      callback(timestamp);
    }

    flushedCallbacksBegin = flushedCallbacksEnd;
    callMicrotasks();

    const finalizers = queuedFinalizers;
    queuedFinalizers = [];
    for (const finalizer of finalizers) {
      finalizer();
    }
  }

  function flushQueue(timestamp: number) {
    globalObject.__frameTimestamp = timestamp;
    executeQueue(timestamp);
    globalObject.__frameTimestamp = undefined;
  }

  function nativeFlushQueue(timestamp: number) {
    nativeFlushScheduled = false;
    flushQueue(timestamp);
    if (hasPendingFrameWork()) {
      scheduleNativeFlush();
    }
  }

  function scheduleNativeFlush() {
    if (nativeFlushScheduled) {
      return;
    }
    nativeFlushScheduled = true;
    nativeRequestAnimationFrame(nativeFlushQueue);
  }

  globalObject.requestAnimationFrame = (
    callback: (timestamp: number) => void,
  ): number => {
    const handle = queuedCallbacksEnd;
    queuedCallbacksEnd += 1;
    queuedCallbacks.push(callback);
    scheduleNativeFlush();
    return handle;
  };

  globalObject.cancelAnimationFrame = (handle: number) => {
    if (handle < flushedCallbacksBegin || handle >= queuedCallbacksEnd) {
      return;
    }

    if (handle < flushedCallbacksEnd) {
      flushedCallbacks[handle - flushedCallbacksBegin] = () => undefined;
    } else {
      queuedCallbacks[handle - queuedCallbacksBegin] = () => undefined;
    }
  };

  globalObject.requestAnimationFrameFinalizer = (callback: () => void) => {
    queuedFinalizers.push(callback);
    scheduleNativeFlush();
  };

  globalObject.setTimeout = (
    callback: (...args: unknown[]) => void,
    delay?: unknown,
    ...args: unknown[]
  ) => scheduleNativeTimer(callback, delay, false, args);
  globalObject.clearTimeout = clearNativeTimer;
  globalObject.setInterval = (
    callback: (...args: unknown[]) => void,
    delay?: unknown,
    ...args: unknown[]
  ) => scheduleNativeTimer(callback, delay, true, args);
  globalObject.clearInterval = clearNativeTimer;

  globalObject.__flushAnimationFrame = (eventTimestamp: number) => {
    nativeFlushScheduled = false;
    flushQueue(eventTimestamp);
    if (hasPendingFrameWork()) {
      scheduleNativeFlush();
    }
  };

  // Stop react-native-worklets' startup frame pump. The replacements above
  // schedule the native display link only when worklet callbacks are pending.
  globalObject.__nativeRequestAnimationFrame = () => undefined;

  return true;
}

function ensureWorkletsInstalled(metadataPath = ""): NativeScriptWorklets {
  if (workletsAdapter) {
    return workletsAdapter;
  }
  installWorklets(requireReactNativeWorklets(), metadataPath);
  return workletsAdapter as NativeScriptWorklets;
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
      "NativeScript.runOnUI could not resolve a Worklets UI runtime",
    );
  }
  const installRuntime = NativeScriptNativeApi.installWorkletRuntime;
  if (typeof installRuntime !== "function") {
    throw workletsSetupError(
      "NativeScript Native API was built without RNWorklets runtime support",
    );
  }
  const installed = installRuntime(holder, metadataPath);
  if (!installed) {
    throw workletsSetupError(
      "NativeScript Native API could not install into the Worklets UI runtime",
    );
  }
  validWorklets
    .runOnUIAsync(installIdleAwareWorkletsFrameLoop)
    .catch(() => undefined);
  workletsAdapter = validWorklets;
  return true;
}

export function runOnUI<Args extends unknown[], ReturnValue>(
  callback: (...args: Args) => ReturnValue | Promise<ReturnValue>,
  ...args: Args
): Promise<ReturnValue> {
  if (typeof callback !== "function") {
    throw new TypeError("NativeScript.runOnUI expects a Worklets callback");
  }

  ensureNativeScriptInstalled();
  const worklets = ensureWorkletsInstalled();
  if (worklets.isWorkletFunction(callback) !== true) {
    throw workletsSetupError(
      "NativeScript.runOnUI requires a worklet callback",
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
    'NativeScript.uiInvoker is not supported in React Native. Use a Worklets "worklet" callback with NativeScript.runOnUI().',
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

  const thread = (callback as Record<string, unknown>)[
    nativeApiCallbackThreadKey
  ];
  return thread === "js" || thread === "runtime" ? thread : undefined;
}

function nativeScriptWrappedCallback(callback: AnyFunction): AnyFunction {
  "worklet";

  const wrapped = (callback as Record<string, unknown>)[
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
  message = "UIKit native APIs must be called through NativeScript.runOnUI",
): void {
  "worklet";

  if (!isMainThread()) {
    throw new Error(message);
  }
}

export function refreshUIKitHostView(view: unknown): boolean {
  "worklet";

  const refresh = (globalThis as Record<string, any>)
    .__nativeScriptRefreshUIKitHostView;
  if (typeof refresh !== "function") {
    return false;
  }

  return refresh(nativeHandleForUIKitView(view)) === true;
}

export function refreshUIKitHostViewHandle(viewHandle: string): boolean {
  "worklet";

  const refresh = (globalThis as Record<string, any>)
    .__nativeScriptRefreshUIKitHostView;
  if (typeof refresh !== "function") {
    return false;
  }

  return refresh(viewHandle) === true;
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
            ? interop?.object?.(interop.Pointer(handle)) ?? null
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
  message = "UIKit native APIs should be mutated through NativeScript.runOnUI",
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
    | Record<string, unknown>
    | undefined;
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

type UIKitRuntimeContext<Props extends object> = UIKitViewContext<Props> & {
  createArgument(): UIKitCreateArgument<Props>;
  disposeResources(): void;
  isDisposed(): boolean;
};

type UIKitHostInstance<NativeView> = {
  hostView: unknown;
  lifecycleValue: NativeView;
  childrenView?: unknown;
  controller?: unknown;
};

type RegisteredUIKitHost<NativeView> = {
  context: UIKitRuntimeContext<any>;
  dispose?: (props: Readonly<any>) => UIKitDisposeResult;
  hostInstance: UIKitHostInstance<NativeView>;
  hasMounted?: boolean;
  mounted?: (props: Readonly<any>) => void;
  nativeView: NativeView;
  previousProps?: Readonly<any>;
  propsRef: { current: Readonly<any> };
  update?: (
    props: Readonly<any>,
    previousProps: Readonly<any> | undefined,
  ) => void;
};

type PendingUIKitHost<Props extends object, NativeView> = {
  debugName: string;
  mountHost: () => RegisteredUIKitHost<NativeView>;
  propsRef: { current: Readonly<Props & ViewProps> };
};

type UIKitHostHandles = {
  nativeViewHandle?: string;
  childrenViewHandle?: string;
  controllerHandle?: string;
};

type UIKitAdapterDefinition<
  Props extends object,
  NativeView,
> = UIKitViewDefinition<Props, NativeView> & {
  resolveHostInstance?: (created: NativeView) => UIKitHostInstance<NativeView>;
};

const uikitHostRegistryGlobalName = "__nativeScriptUIKitHostRegistry";
const pendingUIKitHostRegistryGlobalName =
  "__nativeScriptPendingUIKitHostRegistry";
const createUIKitHostFromNativeGlobalName =
  "__nativeScriptCreateUIKitHostFromNative";
const runUIKitHostLifecycleFromNativeGlobalName =
  "__nativeScriptRunUIKitHostLifecycleFromNative";
let nextUIKitHostId = 1;

function createUIKitHostId(debugName: string): string {
  return `${debugName}:${nextUIKitHostId++}`;
}

function uikitHostRegistry(): Map<string, RegisteredUIKitHost<unknown>> {
  "worklet";

  const globalObject = globalThis as Record<string, unknown>;
  const existing = globalObject[uikitHostRegistryGlobalName];
  if (existing instanceof Map) {
    return existing as Map<string, RegisteredUIKitHost<unknown>>;
  }

  const registry = new Map<string, RegisteredUIKitHost<unknown>>();
  Object.defineProperty(globalThis, uikitHostRegistryGlobalName, {
    configurable: true,
    enumerable: false,
    writable: false,
    value: registry,
  });
  return registry;
}

function pendingUIKitHostRegistry(): Map<
  string,
  PendingUIKitHost<any, unknown>
> {
  "worklet";

  const globalObject = globalThis as Record<string, unknown>;
  const existing = globalObject[pendingUIKitHostRegistryGlobalName];
  if (existing instanceof Map) {
    return existing as Map<string, PendingUIKitHost<any, unknown>>;
  }

  const registry = new Map<string, PendingUIKitHost<any, unknown>>();
  Object.defineProperty(globalThis, pendingUIKitHostRegistryGlobalName, {
    configurable: true,
    enumerable: false,
    writable: false,
    value: registry,
  });
  return registry;
}

function uikitHostHandles(
  host: RegisteredUIKitHost<unknown>,
): UIKitHostHandles {
  "worklet";

  return {
    nativeViewHandle: nativeHandleOrUndefined(host.hostInstance.hostView),
    childrenViewHandle: nativeHandleOrUndefined(host.hostInstance.childrenView),
    controllerHandle: nativeHandleForNSObject(host.hostInstance.controller),
  };
}

function getRegisteredUIKitHost<NativeView>(
  hostId: string,
): RegisteredUIKitHost<NativeView> {
  "worklet";

  const host = uikitHostRegistry().get(hostId);
  if (!host) {
    throw new Error(`UIKit host ${hostId} has not been created`);
  }
  return host as RegisteredUIKitHost<NativeView>;
}

function registerUIKitHost<NativeView>(
  hostId: string,
  host: RegisteredUIKitHost<NativeView>,
): void {
  "worklet";

  uikitHostRegistry().set(hostId, host as RegisteredUIKitHost<unknown>);
}

function createRegisteredUIKitHostFromNative(
  hostId: string,
): UIKitHostHandles | null {
  "worklet";

  const existingHost = uikitHostRegistry().get(hostId);
  if (existingHost) {
    return uikitHostHandles(existingHost);
  }

  const pending = pendingUIKitHostRegistry().get(hostId);
  if (!pending) {
    return null;
  }

  const host = pending.mountHost();
  registerUIKitHost(hostId, host);
  return uikitHostHandles(host);
}

function ensureRegisteredUIKitHost<NativeView>(
  hostId: string,
): RegisteredUIKitHost<NativeView> | null {
  "worklet";

  const existingHost = uikitHostRegistry().get(hostId);
  if (existingHost) {
    return existingHost as RegisteredUIKitHost<NativeView>;
  }

  if (createRegisteredUIKitHostFromNative(hostId) == null) {
    return null;
  }

  const createdHost = uikitHostRegistry().get(hostId);
  return (createdHost ?? null) as RegisteredUIKitHost<NativeView> | null;
}

function disposeRegisteredUIKitHost<NativeView>(
  hostId: string,
  props: Readonly<any>,
): void {
  "worklet";

  pendingUIKitHostRegistry().delete(hostId);
  const registry = uikitHostRegistry();
  const host = registry.get(hostId) as
    | RegisteredUIKitHost<NativeView>
    | undefined;
  if (!host) {
    return;
  }
  registry.delete(hostId);
  host.propsRef.current = props;
  const disposeResult = host.dispose?.(props);
  host.context.disposeResources();
  const maybeView = host.hostInstance.hostView as
    | Record<string, unknown>
    | undefined;
  if (
    disposeResult?.removeHostView !== false &&
    typeof maybeView?.removeFromSuperview === "function"
  ) {
    maybeView.removeFromSuperview();
  }
}

function syncUIKitHostPropsFromReact<Props extends object>(
  hostId: string,
  props: Readonly<Props & ViewProps>,
): void {
  "worklet";

  const pending = pendingUIKitHostRegistry().get(hostId);
  if (pending) {
    pending.propsRef.current = props;
  }

  const host = uikitHostRegistry().get(hostId);
  if (host) {
    host.propsRef.current = props;
  }
}

function runUIKitHostLifecycleFromNative(
  hostId: string,
  phase: string,
): UIKitHostHandles | null {
  "worklet";

  if (phase === "dispose") {
    const host = uikitHostRegistry().get(hostId);
    const pending = pendingUIKitHostRegistry().get(hostId);
    disposeRegisteredUIKitHost(
      hostId,
      host?.propsRef.current ?? pending?.propsRef.current ?? {},
    );
    return null;
  }

  const handles = createRegisteredUIKitHostFromNative(hostId);
  if (handles == null) {
    return null;
  }

  const host = getRegisteredUIKitHost<unknown>(hostId);
  const nextProps = host.propsRef.current;
  if (phase === "update") {
    if (host.previousProps !== nextProps) {
      host.update?.(nextProps, host.previousProps);
      host.previousProps = nextProps;
    }
  } else if (phase === "mounted" && !host.hasMounted) {
    host.hasMounted = true;
    host.mounted?.(nextProps);
  }

  return uikitHostHandles(host);
}

function installUIKitNativeMountBridge(): void {
  "worklet";

  const globalObject = globalThis as Record<string, unknown>;
  if (typeof globalObject[createUIKitHostFromNativeGlobalName] !== "function") {
    Object.defineProperty(globalThis, createUIKitHostFromNativeGlobalName, {
      configurable: true,
      enumerable: false,
      writable: false,
      value: createRegisteredUIKitHostFromNative,
    });
  }
  if (
    typeof globalObject[runUIKitHostLifecycleFromNativeGlobalName] !==
    "function"
  ) {
    Object.defineProperty(
      globalThis,
      runUIKitHostLifecycleFromNativeGlobalName,
      {
        configurable: true,
        enumerable: false,
        writable: false,
        value: runUIKitHostLifecycleFromNative,
      },
    );
  }
}

function ignoreUIKitLayoutInvalidation(): void {
  "worklet";
}

const targetActionClassGlobalName = "__nativeScriptUIKitTargetActionClass";
const observerClassGlobalName = "__nativeScriptUIKitObserverClass";
const targetActionCallbacksGlobalName =
  "__nativeScriptUIKitTargetActionCallbacks";
const observerCallbacksGlobalName = "__nativeScriptUIKitObserverCallbacks";

function objcInteropTypes(): any {
  "worklet";

  return (globalThis as Record<string, any>).interop?.types;
}

function runtimeGlobalMap<T>(name: string): Map<string, T> {
  "worklet";

  const globalObject = globalThis as Record<string, unknown>;
  const existing = globalObject[name];
  if (existing instanceof Map) {
    return existing as Map<string, T>;
  }

  const map = new Map<string, T>();
  Object.defineProperty(globalThis, name, {
    configurable: true,
    enumerable: false,
    writable: false,
    value: map,
  });
  return map;
}

function targetActionCallbacksForRuntime(): Map<
  string,
  (sender: unknown) => void
> {
  "worklet";

  return runtimeGlobalMap<(sender: unknown) => void>(
    targetActionCallbacksGlobalName,
  );
}

function observerCallbacksForRuntime(): Map<
  string,
  (keyPath: string, object: unknown, change: unknown) => void
> {
  "worklet";

  return runtimeGlobalMap<
    (keyPath: string, object: unknown, change: unknown) => void
  >(observerCallbacksGlobalName);
}

function nativeCallbackKey(value: unknown): string {
  "worklet";

  const handleof = (globalThis as Record<string, any>).interop?.handleof;
  if (value != null && typeof handleof === "function") {
    const handle = handleof(value);
    if (handle != null) {
      if (typeof handle.toHexString === "function") {
        return handle.toHexString();
      }
      return String(handle);
    }
  }
  return String(value);
}

function getTargetActionClass(): any {
  "worklet";

  const globalObject = globalThis as Record<string, any>;
  const cached = globalObject[targetActionClassGlobalName];
  if (cached) {
    return cached;
  }
  const types = objcInteropTypes();
  const NSObject = requireNSObject();
  const targetActionClass = NSObject.extend(
    {
      nativeScriptHandleAction(sender: unknown) {
        const callback = targetActionCallbacksForRuntime().get(
          nativeCallbackKey(this),
        );
        if (typeof callback === "function") {
          callback(sender);
        }
      },
    },
    {
      exposedMethods: {
        "nativeScriptHandleAction:": {
          returns: types?.void,
          params: [NSObject],
        },
      },
    },
  );
  Object.defineProperty(globalThis, targetActionClassGlobalName, {
    configurable: true,
    enumerable: false,
    writable: false,
    value: targetActionClass,
  });
  return targetActionClass;
}

function getObserverClass(): any {
  "worklet";

  const globalObject = globalThis as Record<string, any>;
  const cached = globalObject[observerClassGlobalName];
  if (cached) {
    return cached;
  }
  const types = objcInteropTypes();
  const NSObject = requireNSObject();
  const NSString = (globalThis as Record<string, any>).NSString;
  const NSDictionary = (globalThis as Record<string, any>).NSDictionary;
  const Pointer =
    (globalThis as Record<string, any>).interop?.Pointer ?? types?.id;

  const observerClass = NSObject.extend(
    {
      "observeValueForKeyPath:ofObject:change:context:"(
        keyPath: string,
        object: unknown,
        change: unknown,
      ) {
        const callback = observerCallbacksForRuntime().get(
          nativeCallbackKey(this),
        );
        if (typeof callback === "function") {
          callback(keyPath, object, change);
        }
      },
    },
    {
      exposedMethods: {
        "observeValueForKeyPath:ofObject:change:context:": {
          returns: types?.void,
          params: [
            NSString ?? NSObject,
            NSObject,
            NSDictionary ?? NSObject,
            Pointer,
          ],
        },
      },
    },
  );
  Object.defineProperty(globalThis, observerClassGlobalName, {
    configurable: true,
    enumerable: false,
    writable: false,
    value: observerClass,
  });
  return observerClass;
}

function createUIKitContext<Props extends object>(
  name: string,
  propsRef: { current: Props },
  invalidateLayout: () => void,
): UIKitRuntimeContext<Props> {
  "worklet";

  const retained: unknown[] = [];
  const cleanupCallbacks: Array<() => void> = [];
  let disposed = false;

  const context: UIKitRuntimeContext<Props> = {
    get name() {
      return name;
    },
    get tag() {
      return null;
    },
    get props() {
      return propsRef.current;
    },
    emit(eventName, payload) {
      if (disposed) {
        return;
      }
      const handler = (propsRef.current as Record<PropertyKey, unknown>)[
        eventName as PropertyKey
      ];
      if (typeof handler !== "function") {
        return;
      }
      const workletsProxy = (globalThis as Record<string, any>)
        .__workletsModuleProxy;
      const serializer = (globalThis as Record<string, any>).__serializer;
      if (
        workletsProxy &&
        typeof workletsProxy.scheduleOnRN === "function" &&
        typeof serializer === "function"
      ) {
        workletsProxy.scheduleOnRN(handler, serializer([payload]));
      } else {
        setTimeout(() => {
          if (!disposed) {
            (handler as Function)(payload);
          }
        }, 0);
      }
    },
    targetAction(control, events, callback) {
      if (control == null || typeof callback !== "function") {
        return;
      }
      const target = getTargetActionClass().alloc().init();
      const targetKey = nativeCallbackKey(target);
      targetActionCallbacksForRuntime().set(targetKey, () => {
        if (!disposed) {
          invokeNativeScriptCallback(callback, [], () => disposed);
        }
      });
      const selector = "nativeScriptHandleAction:";
      const nativeControl = control as Record<string, Function>;
      if (typeof nativeControl.addTargetActionForControlEvents !== "function") {
        throw new Error("targetAction expects a UIControl-compatible object");
      }
      nativeControl.addTargetActionForControlEvents(target, selector, events);
      context.retain(target);
      context.dispose(() => {
        if (
          typeof nativeControl.removeTargetActionForControlEvents === "function"
        ) {
          nativeControl.removeTargetActionForControlEvents(
            target,
            selector,
            events,
          );
        }
        targetActionCallbacksForRuntime().delete(targetKey);
      });
    },
    gestureAction(gesture, callback) {
      if (gesture == null || typeof callback !== "function") {
        return;
      }
      const target = getTargetActionClass().alloc().init();
      const targetKey = nativeCallbackKey(target);
      targetActionCallbacksForRuntime().set(targetKey, (sender) => {
        if (!disposed) {
          callback(sender ?? gesture);
        }
      });
      const selector = "nativeScriptHandleAction:";
      const nativeGesture = gesture as Record<string, Function>;
      if (typeof nativeGesture.addTargetAction !== "function") {
        throw new Error(
          "gestureAction expects a UIGestureRecognizer-compatible object",
        );
      }
      nativeGesture.addTargetAction(target, selector);
      context.retain(target);
      context.dispose(() => {
        if (typeof nativeGesture.removeTargetAction === "function") {
          nativeGesture.removeTargetAction(target, selector);
        }
        targetActionCallbacksForRuntime().delete(targetKey);
      });
    },
    actionTarget(callback) {
      if (typeof callback !== "function") {
        throw new Error("actionTarget expects a callback");
      }

      const target = getTargetActionClass().alloc().init();
      const targetKey = nativeCallbackKey(target);
      targetActionCallbacksForRuntime().set(targetKey, (sender) => {
        if (!disposed) {
          invokeNativeScriptCallback(callback, [sender], () => disposed);
        }
      });
      context.retain(target);
      context.dispose(() => {
        targetActionCallbacksForRuntime().delete(targetKey);
      });

      return {
        target,
        action: "nativeScriptHandleAction:",
      };
    },
    delegate(object, protocolRef, implementation) {
      const protocolList = [protocolRef as NativeProtocolReference]
        .map(resolveProtocolReference)
        .filter(Boolean);
      if (protocolList.length === 0) {
        throw new Error("NativeScript UIKit delegate requires a protocol");
      }

      const nativeObject = object as Record<string, unknown>;
      const assignedObject =
        nativeObject && "delegate" in nativeObject ? nativeObject : undefined;
      const DelegateClass = requireNSObject().extend(
        wrapDelegateMethods(implementation, "caller"),
        {
          protocols: protocolList,
        },
      );
      const delegate = DelegateClass.alloc().init() as T;
      context.retain(delegate);
      if (assignedObject) {
        assignedObject.delegate = delegate;
      }
      context.dispose(() => {
        if (assignedObject && assignedObject.delegate === delegate) {
          assignedObject.delegate = null;
        }
        context.release(delegate);
      });
      return delegate;
    },
    notification(name, object, callback) {
      const center = (globalThis as Record<string, any>).NSNotificationCenter
        ?.defaultCenter;
      if (!center) {
        throw new Error("NSNotificationCenter.defaultCenter is not available");
      }
      const observer = center.addObserverForNameObjectQueueUsingBlock(
        name,
        object ?? null,
        null,
        (notification: unknown) => {
          if (!disposed) {
            callback(notification);
          }
        },
      );
      context.retain(observer);
      context.dispose(() => {
        center.removeObserver(observer);
      });
    },
    observe(object, keyPath, callback) {
      const nativeObject = object as Record<string, Function>;
      if (
        object == null ||
        typeof nativeObject.addObserverForKeyPathOptionsContext !== "function"
      ) {
        throw new Error("observe expects a KVO-compatible NSObject");
      }
      const observer = getObserverClass().alloc().init();
      const observerKey = nativeCallbackKey(observer);
      observerCallbacksForRuntime().set(
        observerKey,
        (
          observedKeyPath: string,
          _observedObject: unknown,
          change: unknown,
        ) => {
          if (disposed || String(observedKeyPath) !== keyPath) {
            return;
          }
          const newKey = (globalThis as Record<string, any>)
            .NSKeyValueChangeNewKey;
          const value =
            change &&
            typeof (change as Record<string, Function>).objectForKey ===
              "function"
              ? (change as Record<string, Function>).objectForKey(newKey)
              : undefined;
          callback(value, change);
        },
      );
      const options = (globalThis as Record<string, any>)
        .NSKeyValueObservingOptions;
      const optionNew =
        typeof options?.New === "number"
          ? options.New
          : ((globalThis as Record<string, any>).NSKeyValueObservingOptionNew ??
            1);
      nativeObject.addObserverForKeyPathOptionsContext(
        observer,
        keyPath,
        optionNew,
        null,
      );
      context.retain(observer);
      context.dispose(() => {
        try {
          if (typeof nativeObject.removeObserverForKeyPath === "function") {
            nativeObject.removeObserverForKeyPath(observer, keyPath);
          }
        } finally {
          observerCallbacksForRuntime().delete(observerKey);
        }
      });
    },
    retain(value) {
      retained.push(value);
      return value;
    },
	    release(value?: unknown) {
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
	    dispose(callback) {
	      cleanupCallbacks.push(callback);
	    },
	    invalidateLayout,
	    loadImage: (source, options, callback) =>
	      loadImage(source, options, callback),
	    createArgument() {
	      return Object.assign(Object.create(context), propsRef.current);
	    },
    disposeResources() {
      if (disposed) {
        return;
      }
      disposed = true;
      for (let i = cleanupCallbacks.length - 1; i >= 0; i--) {
        cleanupCallbacks[i]();
      }
      cleanupCallbacks.length = 0;
      retained.length = 0;
    },
    isDisposed() {
      return disposed;
    },
  };

  return context;
}

function constrainedSize(
  size: { width: number; height: number },
  layout?: UIKitLayoutOptions,
): { width: number; height: number } {
  "worklet";

  const defaultSize = layout?.defaultSize ?? {};
  let width =
    Number.isFinite(size.width) && size.width >= 0
      ? size.width
      : (defaultSize.width ?? 0);
  let height =
    Number.isFinite(size.height) && size.height >= 0
      ? size.height
      : (defaultSize.height ?? 0);

  if (layout?.minSize?.width != null) {
    width = Math.max(width, layout.minSize.width);
  }
  if (layout?.minSize?.height != null) {
    height = Math.max(height, layout.minSize.height);
  }
  if (layout?.maxSize?.width != null) {
    width = Math.min(width, layout.maxSize.width);
  }
  if (layout?.maxSize?.height != null) {
    height = Math.min(height, layout.maxSize.height);
  }
  return { width, height };
}

function flattenedStyleSize(style: ViewProps["style"]) {
  "worklet";

  const flat: Record<string, unknown> = {};
  const applyStyle = (value: unknown) => {
    if (Array.isArray(value)) {
      for (const item of value) {
        applyStyle(item);
      }
      return;
    }
    if (!value || typeof value !== "object") {
      return;
    }
    const record = value as Record<string, unknown>;
    if (typeof record.width === "number") {
      flat.width = record.width;
    }
    if (typeof record.height === "number") {
      flat.height = record.height;
    }
  };
  applyStyle(style);
  return {
    width: typeof flat.width === "number" ? flat.width : undefined,
    height: typeof flat.height === "number" ? flat.height : undefined,
  };
}

function makeCGSize(width: number, height: number) {
  "worklet";

  const CGSizeMake = (globalThis as Record<string, any>).CGSizeMake;
  if (typeof CGSizeMake === "function") {
    return CGSizeMake(width, height);
  }
  return { width, height };
}

function readNativeSize(size: unknown): { width: number; height: number } {
  "worklet";

  const nativeSize = size as { width?: unknown; height?: unknown };
  return {
    width: Number(nativeSize?.width ?? 0),
    height: Number(nativeSize?.height ?? 0),
  };
}

function measureUIKitView(
  view: unknown,
  layout: UIKitLayoutOptions | undefined,
  style: ViewProps["style"],
): { width: number; height: number } {
  "worklet";

  const mode = layout?.sizing ?? "fill";
  if (mode === "fill") {
    return constrainedSize(
      layout?.defaultSize ?? { width: 0, height: 0 },
      layout,
    );
  }

  const styleSize = flattenedStyleSize(style);
  const nativeView = view as Record<string, any>;
  let measured = layout?.defaultSize ?? { width: 0, height: 0 };

  if (mode === "intrinsic") {
    measured = readNativeSize(nativeView.intrinsicContentSize);
  } else if (
    mode === "sizeThatFits" &&
    typeof nativeView.sizeThatFits === "function"
  ) {
    measured = readNativeSize(
      nativeView.sizeThatFits(
        makeCGSize(
          styleSize.width ?? Number.MAX_SAFE_INTEGER,
          styleSize.height ?? Number.MAX_SAFE_INTEGER,
        ),
      ),
    );
  } else if (
    mode === "autoLayout" &&
    typeof nativeView.systemLayoutSizeFittingSize === "function"
  ) {
    const fittingSize =
      (globalThis as Record<string, any>).UIView?.layoutFittingCompressedSize ??
      makeCGSize(styleSize.width ?? 0, styleSize.height ?? 0);
    measured = readNativeSize(
      nativeView.systemLayoutSizeFittingSize(fittingSize),
    );
  }

  return constrainedSize(
    {
      width: styleSize.width ?? measured.width,
      height: styleSize.height ?? measured.height,
    },
    layout,
  );
}

function defineUIKitHost<Props extends object, NativeView = unknown>(
  definition: UIKitAdapterDefinition<Props, NativeView>,
): UIKitViewComponent<Props, NativeView> {
  const debugName =
    definition.debugName ||
    definition.name ||
    definition.displayName ||
    "NativeScriptUIKitView";

  const Component = forwardRef<
    UIKitViewRef<NativeView>,
    Props & UIKitHostViewProps
  >(function NativeScriptUIKitView(props, ref) {
    const { nativeProps, pluginProps } = splitUIKitViewProps(props, definition);
    const createHost = definition.create;
    const updateHost = definition.update;
    const mountedHost = definition.mounted;
    const disposeHost = definition.dispose;
    const resolveHostInstance = definition.resolveHostInstance;
    const layout = definition.layout;
    const layoutSizing = layout?.sizing ?? "fill";
    const hostIdRef = useRef<string | null>(null);
    if (hostIdRef.current == null) {
      hostIdRef.current = createUIKitHostId(debugName);
    }
    const hostId = hostIdRef.current;
    const propsRef = useRef(pluginProps);
    const previousPropsRef = useRef<Readonly<Props & ViewProps> | undefined>();
    const mountedRef = useRef(false);
    const disposedRef = useRef(false);
    const updateMeasuredSizeRef = useRef<() => void>(() => {});
    const [nativeHostRevision, setNativeHostRevision] = useState(0);
    const attachController = props.attachController !== false;
    const attachControllerView = props.attachControllerView !== false;
    const attachNativeView = props.attachNativeView !== false;
    const mountThroughNativeHost = attachController;

    const invalidateLayout = () => {
      updateMeasuredSizeRef.current();
    };

    const [nativeViewHandle, setNativeViewHandle] = useState<
      string | undefined
    >();
    const [childrenViewHandle, setChildrenViewHandle] = useState<
      string | undefined
    >();
    const [controllerHandle, setControllerHandle] = useState<
      string | undefined
    >();
    const [measuredSize, setMeasuredSize] = useState<
      { width: number; height: number } | undefined
    >(() =>
      layoutSizing === "fill"
        ? undefined
        : layout?.defaultSize
          ? {
              width: layout.defaultSize.width ?? 0,
              height: layout.defaultSize.height ?? 0,
            }
          : undefined,
    );
    const [error, setError] = useState<Error | null>(null);

    propsRef.current = pluginProps;

    const applyHostHandles = (handles: UIKitHostHandles | null | undefined) => {
      if (handles == null) {
        return;
      }

      setNativeViewHandle((previous) =>
        previous === handles.nativeViewHandle
          ? previous
          : handles.nativeViewHandle,
      );
      setChildrenViewHandle((previous) =>
        previous === handles.childrenViewHandle
          ? previous
          : handles.childrenViewHandle,
      );
      setControllerHandle((previous) =>
        previous === handles.controllerHandle
          ? previous
          : handles.controllerHandle,
      );
    };

    const updateMeasuredSize = () => {
      if (nativeViewHandle == null || layoutSizing === "fill") {
        return;
      }
      runOnUI(() => {
        const host = getRegisteredUIKitHost<NativeView>(hostId);
        return measureUIKitView(
          host.hostInstance.hostView,
          layout,
          nativeProps.style,
        );
      })
        .then((nextSize) => {
          setMeasuredSize((previous) =>
            previous &&
            previous.width === nextSize.width &&
            previous.height === nextSize.height
              ? previous
              : nextSize,
          );
        })
        .catch((reason) => {
          setError(
            reason instanceof Error ? reason : new Error(String(reason)),
          );
        });
    };
    updateMeasuredSizeRef.current = updateMeasuredSize;

    useImperativeHandle(
      ref,
      () => ({
        get nativeView() {
          return null;
        },
        runOnUI(callback) {
          return runOnUI(() => {
            const host = getRegisteredUIKitHost<NativeView>(hostId);
            return callback(host.nativeView);
          });
        },
        measureNative() {
          return runOnUI(() => {
            const host = getRegisteredUIKitHost<NativeView>(hostId);
            return measureUIKitView(
              host.hostInstance.hostView,
              layout,
              nativeProps.style,
            );
          });
        },
        invalidateNativeLayout() {
          updateMeasuredSize();
        },
      }),
      [hostId, layout, nativeProps.style],
    );

    useLayoutEffect(() => {
      disposedRef.current = false;
      let cancelled = false;

      ensureNativeScriptInstalled();

      if (mountThroughNativeHost) {
        const effectProps = propsRef.current;
        runOnUI((currentProps) => {
          installUIKitNativeMountBridge();

          const existingHost = uikitHostRegistry().get(hostId);
          if (existingHost) {
            existingHost.propsRef.current = currentProps;
            return uikitHostHandles(existingHost);
          }

          const registry = pendingUIKitHostRegistry();
          const pending = registry.get(hostId) as
            | PendingUIKitHost<Props, NativeView>
            | undefined;
          const pendingPropsRef = pending?.propsRef ?? {
            current: currentProps,
          };
          pendingPropsRef.current = currentProps;

          const mountHost = () => {
            const nextProps = pendingPropsRef.current;
            const context = createUIKitContext(
              debugName,
              pendingPropsRef,
              ignoreUIKitLayoutInvalidation,
            );
            const created = createHost(context.createArgument());
            const hostInstance = resolveHostInstance
              ? resolveHostInstance(created)
              : { hostView: created, lifecycleValue: created };
            const nativeView = hostInstance.lifecycleValue;
            updateHost?.(nativeView, nextProps, undefined, context);
            return {
              context,
              dispose(disposeProps: Readonly<Props & ViewProps>) {
                return disposeHost?.(nativeView, disposeProps, context);
              },
              mounted(mountedProps: Readonly<Props & ViewProps>) {
                mountedHost?.(nativeView, mountedProps, context);
              },
              hostInstance,
              nativeView,
              previousProps: nextProps,
              propsRef: pendingPropsRef,
              update(
                updateProps: Readonly<Props & ViewProps>,
                previousProps: Readonly<Props & ViewProps> | undefined,
              ) {
                updateHost?.(nativeView, updateProps, previousProps, context);
              },
            };
          };

          registry.set(hostId, {
            debugName,
            mountHost,
            propsRef: pendingPropsRef,
          });

          return null;
        }, effectProps)
          .then((handles) => {
            if (cancelled || disposedRef.current) {
              return;
            }
            previousPropsRef.current = propsRef.current;
            applyHostHandles(handles);
            setNativeHostRevision((revision) => revision + 1);
            updateMeasuredSize();
          })
          .catch((reason) => {
            setError(
              reason instanceof Error ? reason : new Error(String(reason)),
            );
          });

        return () => {
          cancelled = true;
          disposedRef.current = true;
          mountedRef.current = false;
          runOnUI(() => {
            if (!uikitHostRegistry().has(hostId)) {
              pendingUIKitHostRegistry().delete(hostId);
            }
          }).catch((reason) => {
            setError(
              reason instanceof Error ? reason : new Error(String(reason)),
            );
          });
        };
      }

      const effectProps = propsRef.current;
      runOnUI((currentProps) => {
        installUIKitNativeMountBridge();

        const existingHost = uikitHostRegistry().get(hostId);
        if (existingHost) {
          existingHost.propsRef.current = currentProps;
          return uikitHostHandles(existingHost);
        }

        const registry = pendingUIKitHostRegistry();
        const pending = registry.get(hostId) as
          | PendingUIKitHost<Props, NativeView>
          | undefined;
        const pendingPropsRef = pending?.propsRef ?? { current: currentProps };
        pendingPropsRef.current = currentProps;

        const mountHost = () => {
          const nextProps = pendingPropsRef.current;
          const context = createUIKitContext(
            debugName,
            pendingPropsRef,
            ignoreUIKitLayoutInvalidation,
          );
          const created = createHost(context.createArgument());
          const hostInstance = resolveHostInstance
            ? resolveHostInstance(created)
            : { hostView: created, lifecycleValue: created };
          const nativeView = hostInstance.lifecycleValue;
          updateHost?.(nativeView, nextProps, undefined, context);
          return {
            context,
            dispose(disposeProps: Readonly<Props & ViewProps>) {
              return disposeHost?.(nativeView, disposeProps, context);
            },
            mounted(mountedProps: Readonly<Props & ViewProps>) {
              mountedHost?.(nativeView, mountedProps, context);
            },
            hostInstance,
            nativeView,
            previousProps: nextProps,
            propsRef: pendingPropsRef,
            update(
              updateProps: Readonly<Props & ViewProps>,
              previousProps: Readonly<Props & ViewProps> | undefined,
            ) {
              updateHost?.(nativeView, updateProps, previousProps, context);
            },
          };
        };

        registry.set(hostId, {
          debugName,
          mountHost,
          propsRef: pendingPropsRef,
        });
        return createRegisteredUIKitHostFromNative(hostId);
      }, effectProps)
        .then((handles) => {
          if (handles == null) {
            throw new Error(`UIKit host ${hostId} was not created`);
          }
          if (cancelled || disposedRef.current) {
            const disposeProps = propsRef.current;
            runOnUI((currentProps) => {
              disposeRegisteredUIKitHost(hostId, currentProps);
            }, disposeProps).catch((reason) => {
              setError(
                reason instanceof Error ? reason : new Error(String(reason)),
              );
            });
            return;
          }
          previousPropsRef.current = propsRef.current;
          applyHostHandles(handles);
          updateMeasuredSize();
        })
        .catch((reason) => {
          setError(
            reason instanceof Error ? reason : new Error(String(reason)),
          );
        });

      return () => {
        cancelled = true;
        disposedRef.current = true;
        mountedRef.current = false;
        const disposeProps = propsRef.current;
        runOnUI((currentProps) => {
          disposeRegisteredUIKitHost(hostId, currentProps);
        }, disposeProps).catch((reason) => {
          setError(
            reason instanceof Error ? reason : new Error(String(reason)),
          );
        });
      };
    }, [
      createHost,
      debugName,
      disposeHost,
      hostId,
      mountedHost,
      mountThroughNativeHost,
      resolveHostInstance,
      updateHost,
    ]);

    useEffect(() => {
      if (nativeViewHandle == null && !mountThroughNativeHost) {
        return;
      }

      const currentProps = propsRef.current;
      const previousProps = previousPropsRef.current;
      previousPropsRef.current = currentProps;

      if (mountThroughNativeHost) {
        runOnUI(
          (nextProps, fallbackPreviousProps) => {
            syncUIKitHostPropsFromReact(hostId, nextProps);
            const host = ensureRegisteredUIKitHost<NativeView>(hostId);
            if (!host) {
              return null;
            }
            host.propsRef.current = nextProps;
            updateHost?.(
              host.nativeView,
              nextProps,
              host.previousProps ?? fallbackPreviousProps,
              host.context,
            );
            host.previousProps = nextProps;
            return uikitHostHandles(host);
          },
          currentProps,
          previousProps,
        )
          .then(applyHostHandles)
          .catch((reason) => {
            setError(
              reason instanceof Error ? reason : new Error(String(reason)),
            );
          });
        updateMeasuredSize();
        return;
      }

      runOnUI(
        (nextProps, fallbackPreviousProps) => {
          const host = ensureRegisteredUIKitHost<NativeView>(hostId);
          if (!host) {
            return;
          }
          host.propsRef.current = nextProps;
          updateHost?.(
            host.nativeView,
            nextProps,
            host.previousProps ?? fallbackPreviousProps,
            host.context,
          );
          host.previousProps = nextProps;
        },
        currentProps,
        previousProps,
      ).catch((reason) => {
        setError(reason instanceof Error ? reason : new Error(String(reason)));
      });
      updateMeasuredSize();
    }, [
      hostId,
      mountThroughNativeHost,
      nativeViewHandle,
      pluginProps,
      updateHost,
    ]);

    useEffect(() => {
      if (
        mountedRef.current ||
        (nativeViewHandle == null && !mountThroughNativeHost)
      ) {
        return;
      }

      if (mountThroughNativeHost) {
        mountedRef.current = true;
        return;
      }

      mountedRef.current = true;
      const currentProps = propsRef.current;
      const isDisposed = disposedRef.current;
      runOnUI(
        (nextProps, shouldSkipMounted) => {
          if (!shouldSkipMounted) {
            const host = ensureRegisteredUIKitHost<NativeView>(hostId);
            if (!host) {
              return;
            }
            host.propsRef.current = nextProps;
            mountedHost?.(host.nativeView, nextProps, host.context);
          }
        },
        currentProps,
        isDisposed,
      ).catch((reason) => {
        setError(reason instanceof Error ? reason : new Error(String(reason)));
      });
    }, [hostId, mountedHost, mountThroughNativeHost, nativeViewHandle]);

    if (error) {
      throw error;
    }

    const layoutStyle =
      measuredSize && layoutSizing !== "fill"
        ? {
            width: measuredSize.width,
            height: measuredSize.height,
          }
        : undefined;
    const { children, ...nativePropsWithoutChildren } =
      nativeProps as ViewProps & { children?: React.ReactNode };

    return React.createElement(NativeScriptUIViewNativeComponent, {
      ...nativePropsWithoutChildren,
      collapsable: false,
      children,
      childrenViewHandle,
      controllerHandle: attachController ? controllerHandle : undefined,
      detachControllerView:
        attachController && !attachControllerView ? true : undefined,
      debugName,
      hostReadyId: hostId,
      hostId: mountThroughNativeHost ? hostId : undefined,
      mountedRevision:
        mountThroughNativeHost && mountedHost != null && nativeHostRevision > 0
          ? nativeHostRevision
          : undefined,
      nativeViewHandle: attachNativeView ? nativeViewHandle : undefined,
      style: layoutStyle ? [nativeProps.style, layoutStyle] : nativeProps.style,
      updateRevision:
        mountThroughNativeHost && nativeHostRevision > 0
          ? nativeHostRevision
          : undefined,
    });
  });

  Component.displayName =
    definition.displayName || definition.name || debugName;
  return Component;
}

export function defineUIKitView<Props extends object, NativeView = unknown>(
  definition: UIKitViewDefinition<Props, NativeView>,
): UIKitViewComponent<Props, NativeView> {
  return defineUIKitHost(definition);
}

export function defineUIKitContainer<
  Props extends object,
  RootView = unknown,
  ChildrenView = unknown,
>(
  definition: UIKitContainerDefinition<Props, RootView, ChildrenView>,
): UIKitViewComponent<Props, UIKitContainerResult<RootView, ChildrenView>> {
  return defineUIKitHost({
    ...definition,
    resolveHostInstance(created) {
      "worklet";

      return {
        hostView: created.rootView,
        lifecycleValue: created,
        childrenView: created.childrenView,
      };
    },
  } as UIKitAdapterDefinition<
    Props,
    UIKitContainerResult<RootView, ChildrenView>
  >);
}

export function defineUIViewController<
  Props extends object,
  Controller = unknown,
>(
  definition: UIViewControllerDefinition<Props, Controller>,
): UIKitViewComponent<Props, Controller> {
  return defineUIKitHost({
    ...definition,
    create: definition.createController,
    resolveHostInstance(controller) {
      "worklet";

      const controllerRecord = controller as Record<string, unknown>;
      return {
        hostView: definition.hostView?.(controller) ?? controllerRecord.view,
        lifecycleValue: controller,
        childrenView: definition.childrenView?.(controller),
        controller,
      };
    },
  } as UIKitAdapterDefinition<Props, Controller>);
}

const NativeScript = {
  init,
  install,
  installGlobals,
  isInstalled,
  defaultMetadataPath,
  defineUIKitContainer,
  defineUIKitView,
  defineUIViewController,
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
  loadFramework,
  release,
  retain,
  refreshUIKitHostView,
  runOnUI,
  runtimeInvoker,
  uiInvoker,
  warnIfNotUIKitThread,
};

export default NativeScript;

// The marker every class extending a native class carries. A runtime decorator,
// so it works the same regardless of bundler or decorator dialect.
export { NativeClass, Interfaces, JavaProxy, NATIVE_CLASS } from "./NativeClass";
export type { NativeClassOptions } from "./NativeClass";
