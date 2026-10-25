/// <reference path="../types/ios/index.d.ts" />

import type {
  ForwardRefExoticComponent,
  PropsWithoutRef,
  RefAttributes,
} from "react";
import type { ViewProps } from "react-native";

export type NativeApiHost = {
  runtime?: string;
  backend?: string;
  metadata?: {
    classes?: number;
    functions?: number;
    constants?: number;
    protocols?: number;
    enums?: number;
    structs?: number;
    unions?: number;
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

export type NativeScriptImageLoadOptions = {
  template?: boolean;
};
export type NativeScriptImageLoadCallback = (
  image: unknown | null,
  error: Error | null,
) => void;
export type NativeScriptCallbackThread = "js" | "runtime";
export type NativeScriptInvokedCallback<T extends (...args: any[]) => any> =
  T & {
    readonly __nativeScriptCallbackThread?: NativeScriptCallbackThread;
    readonly __nativeScriptWrappedCallback?: T;
  };
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
export type NativeProtocolReference = string | object | Function;
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

export type UIKitDisposeResult =
  | void
  | {
      removeHostView?: boolean;
    };

export type UIKitViewDefinition<Props extends object, NativeView = unknown> = {
  /**
   * Human-readable name for this UIKit view definition. This names the JS
   * wrapper when displayName is omitted and is forwarded to the shared native
   * host view as a debug name. It does not change the RN host component tag.
   */
  name?: string;
  /**
   * Explicit native debug name for the shared host view. Use this when the
   * native inspector name should differ from the JS wrapper displayName.
   */
  debugName?: string;
  /**
   * React component display name. When name/debugName are omitted, this is also
   * used as the native debug name.
   */
  displayName?: string;
  layout?: UIKitLayoutOptions;
  create: (
    ctx: UIKitViewContext<Props & ViewProps> & Readonly<Props & ViewProps>,
  ) => NativeView;
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
    ctx: UIKitViewContext<Props & ViewProps> & Readonly<Props & ViewProps>,
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
  createController: (
    ctx: UIKitViewContext<Props & ViewProps> & Readonly<Props & ViewProps>,
  ) => Controller;
  hostView?: (controller: Controller) => unknown;
  childrenView?: (controller: Controller) => unknown;
};

export function init(metadataPath?: string, options?: InstallOptions): boolean;
export const install: typeof init;
export function installGlobals(): boolean;
export function isInstalled(): boolean;
export function defaultMetadataPath(): string;
export function getRuntimeBackend(): string;
export function installWorklets(
  worklets?: NativeScriptWorklets,
  metadataPath?: string,
): boolean;
export function runOnUI<Args extends unknown[], ReturnValue>(
  callback: (...args: Args) => ReturnValue | Promise<ReturnValue>,
  ...args: Args
): Promise<ReturnValue>;
export function uiInvoker<T extends (...args: any[]) => any>(
  callback: T,
): never;
export function jsInvoker<T extends (...args: any[]) => any>(
  callback: T,
): NativeScriptInvokedCallback<T>;
export function runtimeInvoker<T extends (...args: any[]) => any>(
  callback: T,
): NativeScriptInvokedCallback<T>;
export function eventBridge<T extends (...args: any[]) => any>(
  callback: T,
  thread?: NativeScriptCallbackThread | "caller",
): T | NativeScriptInvokedCallback<T>;
export const createEventBridge: typeof eventBridge;
export function isMainThread(): boolean;
export function assertUIKitThread(message?: string): void;
export function refreshUIKitHostView(view: unknown): boolean;
export function refreshUIKitHostViewHandle(viewHandle: string): boolean;
export function loadImage(
  source: unknown,
  options: NativeScriptImageLoadOptions,
  callback: NativeScriptImageLoadCallback,
): boolean;
export function warnIfNotUIKitThread(message?: string): boolean;
export function createRetainer(): NativeRetainer;
export function retain<T>(value: T): T;
export function release(value?: unknown): void;
export function getClass<T = unknown>(name: string): T | null;
export function getProtocol<T = unknown>(name: string): T | null;
export function isClassAvailable(name: string): boolean;
export function isFrameworkLoaded(nameOrPath: string): boolean;
export function loadFramework(nameOrPath: string): boolean;
export function createDelegate<T extends object>(
  protocols: NativeProtocolReference | NativeProtocolReference[],
  methods: Partial<T>,
  options?: CreateDelegateOptions,
): T;
export function defineUIKitView<Props extends object, NativeView = unknown>(
  definition: UIKitViewDefinition<Props, NativeView>,
): UIKitViewComponent<Props, NativeView>;
export function defineUIKitContainer<
  Props extends object,
  RootView = unknown,
  ChildrenView = unknown,
>(
  definition: UIKitContainerDefinition<Props, RootView, ChildrenView>,
): UIKitViewComponent<Props, UIKitContainerResult<RootView, ChildrenView>>;
export function defineUIViewController<
  Props extends object,
  Controller = unknown,
>(
  definition: UIViewControllerDefinition<Props, Controller>,
): UIKitViewComponent<Props, Controller>;

declare const NativeScript: {
  init: typeof init;
  install: typeof install;
  installGlobals: typeof installGlobals;
  isInstalled: typeof isInstalled;
  defaultMetadataPath: typeof defaultMetadataPath;
  defineUIKitContainer: typeof defineUIKitContainer;
  defineUIKitView: typeof defineUIKitView;
  defineUIViewController: typeof defineUIViewController;
  getRuntimeBackend: typeof getRuntimeBackend;
  installWorklets: typeof installWorklets;
  assertUIKitThread: typeof assertUIKitThread;
  createDelegate: typeof createDelegate;
  createEventBridge: typeof createEventBridge;
  createRetainer: typeof createRetainer;
  eventBridge: typeof eventBridge;
  getClass: typeof getClass;
  getProtocol: typeof getProtocol;
  isClassAvailable: typeof isClassAvailable;
  isFrameworkLoaded: typeof isFrameworkLoaded;
  isMainThread: typeof isMainThread;
  jsInvoker: typeof jsInvoker;
  loadFramework: typeof loadFramework;
  release: typeof release;
  retain: typeof retain;
  refreshUIKitHostView: typeof refreshUIKitHostView;
  runOnUI: typeof runOnUI;
  runtimeInvoker: typeof runtimeInvoker;
  uiInvoker: typeof uiInvoker;
  warnIfNotUIKitThread: typeof warnIfNotUIKitThread;
};

export default NativeScript;
