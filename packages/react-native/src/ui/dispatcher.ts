/**
 * Dispatches Fabric component hooks on the main-thread UI runtime. It owns the
 * tag-indexed instance table and creates one context object per component.
 * Native code registers each materialized spec, then calls the dispatcher for
 * lifecycle hooks and commands.
 */
import {
  scheduleOnUI,
  createDelegate as createDelegateImpl,
  loadImage as loadImageImpl,
} from "../index";

// Keep these values in sync with NativeScriptComponentHook in the gateway.
export const NativeScriptComponentHook = {
  UpdateProps: 1 << 0,
  MountChild: 1 << 1,
  UnmountChild: 1 << 2,
  WillMount: 1 << 3,
  DidMount: 1 << 4,
  UpdateLayoutMetrics: 1 << 5,
  FinalizeUpdates: 1 << 6,
  PrepareForRecycle: 1 << 7,
  Commands: 1 << 8,
  SafeAreaInsetsDidChange: 1 << 9,
} as const;

// The wrapper exposes the live Fabric ComponentView and UIKit methods.
// eslint-disable-next-line @typescript-eslint/no-explicit-any
type NativeView = any;

// Native forwards each relevant Fabric mutation in mounting order.
export type TransactionMutation = {
  type: "insert" | "remove" | "delete";
  tag: number;
  parentTag: number;
  index: number;
};

export type MountingTransaction = {
  readonly mutations: TransactionMutation[];
  /** True when the transaction inserted or removed one of this tag's children. */
  didMutateChildrenOf(tag: number): boolean;
};

function buildMountingTransaction(
  mutations: TransactionMutation[],
): MountingTransaction {
  "worklet";
  return {
    mutations,
    didMutateChildrenOf(tag: number): boolean {
      "worklet";
      return mutations.some(
        (m) =>
          m.parentTag === tag && (m.type === "insert" || m.type === "remove"),
      );
    },
  };
}

export type NSComponentContext<
  Instance extends object = Record<string, unknown>,
> = {
  readonly view: NativeView;
  readonly instance: Instance;
  readonly tag: number;
  emit(name: string, payload?: unknown): void;
  setContentSize(
    size: { width: number; height: number },
    opts?: {
      offsetX?: number;
      offsetY?: number;
      offsetMode?: "frame" | "content";
      authority?: boolean;
      updateMode?: "immediate" | "asynchronous";
    },
  ): void;
  setLayoutInsets(
    insets: { top: number; right: number; bottom: number; left: number },
    edges: { top: boolean; right: boolean; bottom: boolean; left: boolean },
  ): void;
  setContentInsets(
    insets: { top: number; right: number; bottom: number; left: number },
    opts?: { updateMode?: "immediate" | "asynchronous" },
  ): void;
  setWindowOverlay(view: NativeView, enabled: boolean): void;
  setNativeObjectMetadata(
    name: string,
    value: string | readonly string[] | undefined,
  ): void;
  enableChildControllerTraitForwarding(): void;
  invalidateControllerTraits(traits: {
    statusBar?: boolean;
    homeIndicator?: boolean;
    orientations?: boolean;
  }): void;
  attachChildViewController(
    controller: NativeView,
    containerView: NativeView,
  ): boolean;
  scheduleOnMainQueue(fn: () => void): void;
  loadImage: typeof loadImageImpl;
  createDelegate: typeof createDelegateImpl;
  instanceForView(view: NativeView): unknown;
};

type ComponentSpec = {
  props?: Record<string, unknown>;
  create?: (ctx: NSComponentContext) => NativeView | void;
  updateProps?: (ctx: NSComponentContext, next: unknown, prev: unknown) => void;
  mountChildComponentView?: (
    ctx: NSComponentContext,
    child: { tag: number; view: NativeView; instance?: unknown },
    index: number,
  ) => void;
  unmountChildComponentView?: (
    ctx: NSComponentContext,
    child: { tag: number; view: NativeView; instance?: unknown },
    index: number,
  ) => void;
  mountingTransactionWillMount?: (
    ctx: NSComponentContext,
    txn: MountingTransaction,
  ) => void;
  mountingTransactionDidMount?: (
    ctx: NSComponentContext,
    txn: MountingTransaction,
  ) => void;
  updateLayoutMetrics?: (
    ctx: NSComponentContext,
    next: { x: number; y: number; width: number; height: number },
    prev: { x: number; y: number; width: number; height: number },
  ) => boolean;
  safeAreaInsetsDidChange?: (
    ctx: NSComponentContext,
    insets: { top: number; right: number; bottom: number; left: number },
  ) => void;
  finalizeUpdates?: (ctx: NSComponentContext, mask: number) => void;
  prepareForRecycle?: (ctx: NSComponentContext, viaInvalidate: boolean) => void;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  commands?: Record<string, (ctx: NSComponentContext, args: any) => void>;
};

let dispatcherInstallStarted = false;

/** Installs the dispatcher once for the current UI runtime. */
export function ensureDispatcherInstalled(): void {
  if (dispatcherInstallStarted) {
    return;
  }
  dispatcherInstallStarted = true;

  scheduleOnUI(() => {
    "worklet";

    const globalObject = globalThis as Record<string, any>;
    if (
      typeof globalObject.__nativeScriptDispatchComponentHook === "function"
    ) {
      return;
    }

    // A Worklets reload creates a new runtime and a new instance table.
    const instances = new Map<
      number,
      {
        ctx: NSComponentContext;
        instance: object;
        effectiveProps: Record<string, unknown>;
      }
    >();
    // Native registers each materialized component spec once per runtime.
    const specs = new Map<string, ComponentSpec>();

    function changedProps(
      spec: ComponentSpec | undefined,
      patchValue: unknown,
      effectiveProps: Record<string, unknown>,
    ): Record<string, unknown> {
      "worklet";
      const defaults = spec?.props ?? {};
      const patch =
        patchValue && typeof patchValue === "object"
          ? (patchValue as Record<string, unknown>)
          : {};
      const changed: Record<string, unknown> = {};
      for (const key of Object.keys(defaults)) {
        if (!Object.prototype.hasOwnProperty.call(patch, key)) continue;
        const hadPrevious = Object.prototype.hasOwnProperty.call(
          effectiveProps,
          key,
        );
        const previousProp = hadPrevious ? effectiveProps[key] : defaults[key];
        const rawNextProp = patch[key];
        const nextProp = rawNextProp ?? defaults[key] ?? null;
        const sameReferenceOrPrimitive = Object.is(nextProp, previousProp);
        const bothObjects =
          nextProp !== null &&
          previousProp !== null &&
          typeof nextProp === "object" &&
          typeof previousProp === "object";
        if (
          !sameReferenceOrPrimitive &&
          (!bothObjects ||
            JSON.stringify(nextProp) !== JSON.stringify(previousProp))
        ) {
          changed[key] = nextProp;
        }
        effectiveProps[key] = nextProp;
      }
      return changed;
    }

    globalObject.__nativeScriptRegisterMaterializedSpec = (
      name: string,
      spec: ComponentSpec,
    ) => {
      specs.set(name, spec);
    };

    function buildCtx(tag: number, view: NativeView): NSComponentContext {
      const instance: Record<string, unknown> = {};
      const ctx: NSComponentContext = {
        view,
        instance,
        tag,
        emit(name_, payload) {
          "worklet";
          globalObject.__nativeScriptComponentEmit(
            view,
            name_,
            payload ?? null,
          );
        },
        setContentSize(size, opts) {
          "worklet";
          globalObject.__nativeScriptComponentSetContentSize(
            view,
            size.width,
            size.height,
            opts?.offsetX ?? 0,
            opts?.offsetY ?? 0,
            opts?.authority ?? true,
            opts?.offsetX === undefined && opts?.offsetY === undefined
              ? 0
              : opts?.offsetMode === "content"
                ? 2
                : 1,
            opts?.updateMode === "immediate",
          );
        },
        setLayoutInsets(insets, edges) {
          "worklet";
          globalObject.__nativeScriptComponentSetLayoutInsets(
            view,
            insets.top,
            insets.right,
            insets.bottom,
            insets.left,
            edges.top,
            edges.right,
            edges.bottom,
            edges.left,
          );
        },
        setContentInsets(insets, opts) {
          "worklet";
          globalObject.__nativeScriptComponentSetContentInsets(
            view,
            insets.top,
            insets.right,
            insets.bottom,
            insets.left,
            opts?.updateMode === "immediate",
          );
        },
        setWindowOverlay(overlayView, enabled) {
          "worklet";
          globalObject.__nativeScriptComponentSetWindowOverlay(
            view,
            overlayView,
            enabled,
          );
        },
        setNativeObjectMetadata(name, value) {
          "worklet";
          globalObject.__nativeScriptComponentSetObjectMetadata(
            view,
            name,
            value,
          );
        },
        enableChildControllerTraitForwarding() {
          "worklet";
          globalObject.__nativeScriptComponentEnableChildControllerTraitForwarding();
        },
        invalidateControllerTraits(traits) {
          "worklet";
          globalObject.__nativeScriptComponentInvalidateControllerTraits(
            view,
            traits.statusBar === true,
            traits.homeIndicator === true,
            traits.orientations === true,
          );
        },
        attachChildViewController(controller, containerView) {
          "worklet";
          return globalObject.__nativeScriptComponentAttachChildViewController(
            view,
            controller,
            containerView,
          );
        },
        scheduleOnMainQueue(fn) {
          "worklet";
          globalObject.__nativeScriptComponentScheduleOnMainQueue(view, fn);
        },
        loadImage: (source, options, callback) =>
          loadImageImpl(source, options, callback),
        createDelegate: createDelegateImpl,
        instanceForView(childView) {
          "worklet";
          // Fabric writes the React tag to UIView.tag before lifecycle hooks.
          const childTag = (childView as { tag?: number } | null)?.tag;
          return typeof childTag === "number"
            ? instances.get(childTag)?.instance
            : undefined;
        },
      };
      return ctx;
    }

    globalObject.__nativeScriptDispatchComponentHook = (
      name: string,
      tag: number,
      hookName: string,
      view: NativeView,
      a: unknown,
      b: unknown,
      c: unknown,
    ): unknown => {
      "worklet";
      const spec = specs.get(name);

      if (hookName === "create") {
        const ctx = buildCtx(tag, view);
        instances.set(tag, {
          ctx,
          instance: ctx.instance,
          effectiveProps: {},
        });
        return spec?.create ? spec.create(ctx) : undefined;
      }

      let entry = instances.get(tag);
      if (!entry) {
        // Native normally sends create first. Keep a fallback for teardown
        // races during development reloads.
        const ctx = buildCtx(tag, view);
        entry = { ctx, instance: ctx.instance, effectiveProps: {} };
        instances.set(tag, entry);
      }
      const { ctx } = entry;

      switch (hookName) {
        case "updateProps": {
          if (!spec?.updateProps) return undefined;
          const previousProps = { ...entry.effectiveProps };
          return spec.updateProps(
            ctx,
            changedProps(spec, a, entry.effectiveProps),
            previousProps,
          );
        }
        case "mountChildComponentView": {
          const childTag = b as number;
          const childInstance = instances.get(childTag)?.instance;
          return spec?.mountChildComponentView
            ? spec.mountChildComponentView(
                ctx,
                { tag: childTag, view: a, instance: childInstance },
                c as number,
              )
            : undefined;
        }
        case "unmountChildComponentView": {
          const childTag = b as number;
          const childInstance = instances.get(childTag)?.instance;
          return spec?.unmountChildComponentView
            ? spec.unmountChildComponentView(
                ctx,
                { tag: childTag, view: a, instance: childInstance },
                c as number,
              )
            : undefined;
        }
        case "mountingTransactionWillMount":
          return spec?.mountingTransactionWillMount
            ? spec.mountingTransactionWillMount(
                ctx,
                buildMountingTransaction(a as TransactionMutation[]),
              )
            : undefined;
        case "mountingTransactionDidMount":
          return spec?.mountingTransactionDidMount
            ? spec.mountingTransactionDidMount(
                ctx,
                buildMountingTransaction(a as TransactionMutation[]),
              )
            : undefined;
        case "updateLayoutMetrics":
          return spec?.updateLayoutMetrics
            ? spec.updateLayoutMetrics(
                ctx,
                a as { x: number; y: number; width: number; height: number },
                b as { x: number; y: number; width: number; height: number },
              )
            : true;
        case "safeAreaInsetsDidChange":
          return spec?.safeAreaInsetsDidChange
            ? spec.safeAreaInsetsDidChange(
                ctx,
                a as {
                  top: number;
                  right: number;
                  bottom: number;
                  left: number;
                },
              )
            : undefined;
        case "finalizeUpdates":
          return spec?.finalizeUpdates
            ? spec.finalizeUpdates(ctx, a as number)
            : undefined;
        case "prepareForRecycle": {
          const result = spec?.prepareForRecycle
            ? spec.prepareForRecycle(ctx, a as boolean)
            : undefined;
          instances.delete(tag);
          return result;
        }
        case "handleCommand": {
          const commandFn = spec?.commands?.[a as string];
          return commandFn ? commandFn(ctx, b) : undefined;
        }
        default:
          return undefined;
      }
    };
    // Component modules import this dispatcher before React can mount them.
  }).catch(() => undefined);
}
