/// <reference path="../types/ios/index.d.ts" />

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
export function scheduleOnUI<Args extends unknown[], ReturnValue>(
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

declare const NativeScript: {
  init: typeof init;
  install: typeof install;
  installGlobals: typeof installGlobals;
  isInstalled: typeof isInstalled;
  defaultMetadataPath: typeof defaultMetadataPath;
  defineNativeComponent: typeof import("./defineNativeComponent").defineNativeComponent;
  dispatchNativeComponentCommand: typeof import("./defineNativeComponent").dispatchNativeComponentCommand;
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
  loadImage: typeof loadImage;
  loadFramework: typeof loadFramework;
  release: typeof release;
  retain: typeof retain;
  scheduleOnUI: typeof scheduleOnUI;
  runtimeInvoker: typeof runtimeInvoker;
  uiInvoker: typeof uiInvoker;
  warnIfNotUIKitThread: typeof warnIfNotUIKitThread;
};

export default NativeScript;
