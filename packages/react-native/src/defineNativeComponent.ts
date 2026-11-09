/** Defines a Fabric component at runtime from a TypeScript hook object. */
// React Native uses this registry under `codegenNativeComponent`. It is also
// the available registration path for component names created at runtime.
// eslint-disable-next-line @typescript-eslint/ban-ts-comment
// @ts-ignore; no .d.ts shipped for this RN-internal module.
import * as NativeComponentRegistry from "react-native/Libraries/NativeComponent/NativeComponentRegistry";
import { findNodeHandle, processColor } from "react-native";
import type { HostComponent, ViewProps } from "react-native";
// eslint-disable-next-line @typescript-eslint/ban-ts-comment
// @ts-ignore; this RN-internal module has no .d.ts file. The bridgeless
// architecture dispatches commands through FabricUIManager.
import { getFabricUIManager } from "react-native/Libraries/ReactNative/FabricUIManager";

import NativeScriptNativeApi from "./NativeScriptNativeApi";
import {
  ensureDispatcherInstalled,
  NativeScriptComponentHook,
  type MountingTransaction,
  type NSComponentContext,
} from "./ui/dispatcher";

export type {
  NSComponentContext,
  MountingTransaction,
  TransactionMutation,
} from "./ui/dispatcher";

declare const require: (id: string) => any;

type NativeScriptHostComponentCache = Map<string, unknown>;

function hostComponentCache(): NativeScriptHostComponentCache {
  const globalObject = globalThis as typeof globalThis & {
    __nativeScriptHostComponentCache?: NativeScriptHostComponentCache;
  };
  globalObject.__nativeScriptHostComponentCache ??= new Map();
  return globalObject.__nativeScriptHostComponentCache;
}

// Load Worklets only when a caller defines a native component. The rest of
// the package can run without react-native-worklets.
let cachedCreateSerializable: ((value: unknown) => object) | undefined;
function requireCreateSerializable(): (value: unknown) => object {
  if (!cachedCreateSerializable) {
    const worklets = require("react-native-worklets");
    if (typeof worklets?.createSerializable !== "function") {
      throw new Error(
        "defineNativeComponent requires react-native-worklets (createSerializable was not found)",
      );
    }
    cachedCreateSerializable = worklets.createSerializable;
  }
  return cachedCreateSerializable!;
}

let cachedIsWorkletFunction: ((value: unknown) => boolean) | undefined;
function requireIsWorkletFunction(): (value: unknown) => boolean {
  if (!cachedIsWorkletFunction) {
    cachedIsWorkletFunction =
      require("react-native-worklets")?.isWorkletFunction;
  }
  return cachedIsWorkletFunction ?? (() => false);
}

// Validate worklets when the component is defined so a missing directive does
// not fail during the first mount.
const NATIVE_COMPONENT_HOOK_NAMES = [
  "create",
  "updateProps",
  "mountChildComponentView",
  "unmountChildComponentView",
  "mountingTransactionWillMount",
  "mountingTransactionDidMount",
  "updateLayoutMetrics",
  "safeAreaInsetsDidChange",
  "finalizeUpdates",
  "prepareForRecycle",
] as const;

// Worklets compiles function declarations into non-hoisted const bindings.
// Walk nested closures and reject an undefined capture before the hook runs.
// Mutually dependent helpers should live on a stable object and call each
// other through property lookup.
function findDeadClosureCapture(
  fn: { __closure?: Record<string, unknown> },
  path: string[],
  visited: Set<unknown>,
): { path: string[]; key: string } | undefined {
  const closure = fn.__closure;
  if (!closure || typeof closure !== "object") {
    return undefined;
  }
  for (const key of Object.keys(closure)) {
    const captured = closure[key];
    if (captured === undefined) {
      return { path, key };
    }
    if (typeof captured === "function" && "__closure" in captured) {
      if (visited.has(captured)) {
        continue; // This shared reference was already checked.
      }
      visited.add(captured);
      const nested = findDeadClosureCapture(
        captured as { __closure?: Record<string, unknown> },
        [...path, key],
        visited,
      );
      if (nested) {
        return nested;
      }
    }
  }
  return undefined;
}

function validateHookIsWorklet(
  spec: Record<string, unknown>,
  hookLabel: string,
  fn: unknown,
): void {
  const isWorkletFunction = requireIsWorkletFunction();
  if (typeof fn !== "function" || !isWorkletFunction(fn)) {
    throw new Error(
      `defineNativeComponent("${String(spec.name)}"): "${hookLabel}" is missing a 'worklet' directive ` +
        `(or the Worklets Babel plugin isn't running on this file). Every defineNativeComponent hook, ` +
        `including entries inside "commands", runs on the UI runtime and must start with 'worklet'.`,
    );
  }
  const visited = new Set<unknown>([fn]);
  const dead = findDeadClosureCapture(
    fn as { __closure?: Record<string, unknown> },
    [hookLabel],
    visited,
  );
  if (dead) {
    const chain = dead.path.join(" -> captures -> ");
    throw new Error(
      `defineNativeComponent("${String(spec.name)}"): "${chain} -> captures -> ${dead.key}" is undefined. ` +
        `A worklet captured "${dead.key}" before it initialized. Worklet declarations are not hoisted. ` +
        `Move mutually dependent helpers onto a stable object, such as globalThis.__moduleHelpers, ` +
        `and call them through property lookup.`,
    );
  }
}

function validateSpecWorklets(spec: Record<string, unknown>): void {
  for (const hook of NATIVE_COMPONENT_HOOK_NAMES) {
    if (spec[hook] !== undefined) {
      validateHookIsWorklet(spec, hook, spec[hook]);
    }
  }
  const commands = spec.commands as Record<string, unknown> | undefined;
  if (commands && typeof commands === "object") {
    for (const commandName of Object.keys(commands)) {
      validateHookIsWorklet(
        spec,
        `commands.${commandName}`,
        commands[commandName],
      );
    }
  }
}

type EventPayloads = Record<string, object>;

type ChildRef<Instance> = { tag: number; view: unknown; instance?: Instance };

type FrameMetrics = { x: number; y: number; width: number; height: number };

export type NativeComponentSpec<
  Props extends object = object,
  Events extends EventPayloads = EventPayloads,
  Instance extends object = Record<string, unknown>,
> = {
  /** The Fabric component name. */
  name: string;
  /** Prop defaults. Keys become validAttributes. */
  props?: Props;
  /**
   * Props whose React Native `ColorValue` inputs must be normalized before
   * they cross the Fabric boundary. This mirrors the processor generated by
   * `codegenNativeComponent` for typed color props.
   */
  colorProps?: (keyof Props & string)[];
  /** Direct event names, typed by Events. */
  events?: (keyof Events & string)[];
  /**
   * When false, Fabric tears this component down through `-invalidate`
   * instead of the recycle pool. `prepareForRecycle` runs on both paths.
   */
  shouldBeRecycled?: boolean;

  // Every hook below is a worklet on the main thread.
  create?(ctx: NSComponentContext<Instance>): unknown | void;
  /**
   * The dispatcher compares Fabric's raw prop snapshots and sends only values
   * that changed. A removed prop is reset to its declared default, or null when
   * the default is undefined. Merge these values into `ctx.instance`.
   */
  updateProps?(
    ctx: NSComponentContext<Instance>,
    next: Partial<Props>,
    prev: Partial<Props>,
  ): void;
  /**
   * Declaring this hook replaces Fabric's default child mounting behavior.
   */
  mountChildComponentView?(
    ctx: NSComponentContext<Instance>,
    child: ChildRef<unknown>,
    index: number,
  ): void;
  /** Symmetric with `mountChildComponentView`; declaring this hook means
   * `super` is never called here either. */
  unmountChildComponentView?(
    ctx: NSComponentContext<Instance>,
    child: ChildRef<unknown>,
    index: number,
  ): void;
  mountingTransactionWillMount?(
    ctx: NSComponentContext<Instance>,
    txn: MountingTransaction,
  ): void;
  mountingTransactionDidMount?(
    ctx: NSComponentContext<Instance>,
    txn: MountingTransaction,
  ): void;
  /** Return false to keep the current frame instead of Fabric's frame. */
  updateLayoutMetrics?(
    ctx: NSComponentContext<Instance>,
    next: FrameMetrics,
    prev: FrameMetrics,
  ): boolean;
  /** Called on the main/UI runtime whenever UIKit recomputes this view's safe area. */
  safeAreaInsetsDidChange?(
    ctx: NSComponentContext<Instance>,
    insets: { top: number; right: number; bottom: number; left: number },
  ): void;
  finalizeUpdates?(ctx: NSComponentContext<Instance>, mask: number): void;
  /** `viaInvalidate` identifies the `-invalidate` teardown path. */
  prepareForRecycle?(
    ctx: NSComponentContext<Instance>,
    viaInvalidate: boolean,
  ): void;
  /** Invoked from JS via `dispatchNativeComponentCommand(ref.current, name, args)`. */
  commands?: Record<
    string,
    (ctx: NSComponentContext<Instance>, args: unknown[]) => void
  >;
};

// Event keys are the JSX prop names supplied by the component definition.
type DirectEventHandlers<Events extends EventPayloads> = {
  [K in keyof Events & string]?: (event: { nativeEvent: Events[K] }) => void;
};

export type NativeComponentProps<
  Props extends object,
  Events extends EventPayloads,
> = ViewProps & Props & DirectEventHandlers<Events>;

function computeHookMask(
  spec: NativeComponentSpec<object, EventPayloads, object>,
): number {
  let mask = 0;
  if (spec.updateProps) mask |= NativeScriptComponentHook.UpdateProps;
  if (spec.mountChildComponentView)
    mask |= NativeScriptComponentHook.MountChild;
  if (spec.unmountChildComponentView)
    mask |= NativeScriptComponentHook.UnmountChild;
  if (spec.mountingTransactionWillMount)
    mask |= NativeScriptComponentHook.WillMount;
  if (spec.mountingTransactionDidMount)
    mask |= NativeScriptComponentHook.DidMount;
  if (spec.updateLayoutMetrics)
    mask |= NativeScriptComponentHook.UpdateLayoutMetrics;
  if (spec.safeAreaInsetsDidChange)
    mask |= NativeScriptComponentHook.SafeAreaInsetsDidChange;
  if (spec.finalizeUpdates) mask |= NativeScriptComponentHook.FinalizeUpdates;
  if (spec.prepareForRecycle)
    mask |= NativeScriptComponentHook.PrepareForRecycle;
  if (spec.commands && Object.keys(spec.commands).length > 0)
    mask |= NativeScriptComponentHook.Commands;
  return mask;
}

function eventNameToRegistrationName(name: string): string {
  // React Native maps an `onSomething` prop to a `topSomething` direct event.
  if (!/^on[A-Z]/.test(name)) {
    throw new Error(
      `defineNativeComponent: event name "${name}" must start with "on" followed by an uppercase letter, such as "onSomething".`,
    );
  }
  return `top${name.slice(2)}`;
}

function buildViewConfig(
  spec: NativeComponentSpec<object, EventPayloads, object>,
) {
  // Do not add `style` here. The base view config provides React Native's
  // style descriptor. Replacing it with `true` prevents Yoga properties from
  // reaching the shadow node.
  const propKeys = Object.keys(spec.props ?? {});
  const colorProps = new Set<string>(
    (spec.colorProps ?? []) as readonly string[],
  );
  for (const key of colorProps) {
    if (!propKeys.includes(key)) {
      throw new Error(
        `defineNativeComponent("${spec.name}"): color prop "${key}" must also be declared in "props".`,
      );
    }
  }
  const validAttributes: Record<
    string,
    true | { process: typeof processColor }
  > = {};
  for (const key of propKeys) {
    validAttributes[key] = colorProps.has(key)
      ? { process: processColor }
      : true;
  }

  // The map key is Fabric's internal event name. `registrationName` is the
  // JSX prop name.
  const directEventTypes: Record<string, { registrationName: string }> = {};
  for (const eventName of spec.events ?? []) {
    directEventTypes[eventNameToRegistrationName(eventName)] = {
      registrationName: eventName,
    };
  }

  return {
    uiViewClassName: spec.name,
    validAttributes,
    directEventTypes,
    bubblingEventTypes: {},
  };
}

/**
 * Registers `spec` as a Fabric-native component and returns a typed React
 * host component (`<MyThing tint="red" onSomething={...} />`).
 *
 * The function serializes the worklet handlers, registers the native Fabric
 * class, and builds the JS view config before returning the component.
 */
export function defineNativeComponent<
  Props extends object = object,
  Events extends EventPayloads = EventPayloads,
  Instance extends object = Record<string, unknown>,
>(
  spec: NativeComponentSpec<Props, Events, Instance>,
): HostComponent<NativeComponentProps<Props, Events>> {
  if (!spec || typeof spec.name !== "string" || spec.name.length === 0) {
    throw new Error("defineNativeComponent requires a non-empty `name`");
  }

  // Start installing the dispatcher before native registration completes.
  ensureDispatcherInstalled();

  validateSpecWorklets(spec as unknown as Record<string, unknown>);

  const genericSpec = spec as unknown as NativeComponentSpec<
    object,
    EventPayloads,
    object
  >;
  const hookMask = computeHookMask(genericSpec);
  // Native registration expects the SerializableJSRef marker produced by
  // Worklets' `createSerializable`.
  const serializableSpec = requireCreateSerializable()(spec);
  const shouldBeRecycledTriState =
    spec.shouldBeRecycled === undefined ? -1 : spec.shouldBeRecycled ? 1 : 0;
  const registered = NativeScriptNativeApi.registerComponent(
    spec.name,
    serializableSpec as object,
    hookMask,
    shouldBeRecycledTriState,
  );
  if (!registered) {
    throw new Error(
      `defineNativeComponent("${spec.name}") failed to register with NativeScript`,
    );
  }

  const viewConfig = buildViewConfig(genericSpec);
  const cached = hostComponentCache().get(spec.name);
  if (cached) {
    return cached as HostComponent<NativeComponentProps<Props, Events>>;
  }
  const component = NativeComponentRegistry.get(
    spec.name,
    () => viewConfig,
  ) as HostComponent<NativeComponentProps<Props, Events>>;
  hostComponentCache().set(spec.name, component);
  return component;
}

/** Dispatches a Fabric command to a mounted component. */
export function dispatchNativeComponentCommand(
  componentRef: unknown,
  commandName: string,
  args: unknown[] = [],
): void {
  const handle = findNodeHandle(componentRef as never);
  if (handle == null) {
    throw new Error(
      `dispatchNativeComponentCommand("${commandName}"): findNodeHandle(componentRef) returned null. ` +
        "pass a mounted defineNativeComponent instance's ref.current.",
    );
  }
  const fabricUIManager = getFabricUIManager();
  if (fabricUIManager == null) {
    throw new Error(
      `dispatchNativeComponentCommand("${commandName}"): not running on Fabric`,
    );
  }
  const shadowNode = fabricUIManager.findShadowNodeByTag_DEPRECATED(handle);
  if (shadowNode == null) {
    throw new Error(
      `dispatchNativeComponentCommand("${commandName}"): no shadow node for tag ${handle} (unmounted?)`,
    );
  }
  fabricUIManager.dispatchCommand(shadowNode, commandName, args);
}

// A component may wrap any UIKit view, so this layer cannot provide one
// static view type.
export type NativeView = unknown;
