# @nativescript/react-native

This package exposes the NativeScript Objective-C bridge to React Native through
a Hermes TurboModule. It also provides `defineNativeComponent`, which defines
Fabric components in TypeScript without codegen or project-specific native code.

## Threading model

React Native runs the React tree and event handlers on its JS thread. When
`react-native-worklets` is installed, it adds a second Hermes VM on the main
thread. This package installs the NativeScript Objective-C bridge in both VMs.

- `NativeScript.init()` installs the bridge on the React Native JS thread.
  Native globals are disabled there unless you call `init({ globals: true })`.
- `NativeScript.scheduleOnUI()` and `defineNativeComponent` use the Worklets UI
  runtime. Native globals are always enabled in that VM, and every component
  hook runs on the main thread.

A component hook is a worklet. Fabric invokes it on the main thread, so it can
call UIKit directly without another queue or batching layer.

Four React Native mechanisms cross the JS and UI boundary:

1. Fabric sends changed props to `updateProps` during a commit.
2. `ctx.emit(name, payload)` sends events through Fabric's event emitter.
3. `dispatchNativeComponentCommand(ref, name, args)` invokes a component's
   command handler through Fabric.
4. `NativeScript.jsInvoker()` sends a native callback to the JS thread through
   Worklets' `scheduleOnRN`.

Do not add JSON transport, string handles, a separate batching queue, or
method swizzling for this boundary. See [Design limits](#design-limits).

## Setup

```ts
import NativeScript from "@nativescript/react-native";

NativeScript.init();

const object = NSObject.new();
```

`NativeScript.init()` also installs the Native API into the
`react-native-worklets` UI runtime. `NativeScript.scheduleOnUI()` only accepts
Worklets callbacks; running React Native's JS-thread runtime as a UI-thread
shim is not supported.

```ts
await NativeScript.scheduleOnUI(() => {
  "worklet";
  UIApplication.sharedApplication.keyWindow.tintColor = UIColor.systemPinkColor;
});
```

Install `react-native-worklets`, add its Babel plugin, and run `pod install` so
the `RNWorklets` pod is linked:

```sh
npm install react-native-worklets
```

```js
module.exports = {
  presets: ["module:@react-native/babel-preset"],
  plugins: [
    "@nativescript/react-native/babel-plugin",
    ["react-native-worklets/plugin", { bundleMode: true, strictGlobal: true }],
  ],
};
```

Bundle Mode avoids compiling every component worklet separately at startup.
Wrap the app's Metro config so generated worklet modules are preloaded and
content-hashed even when Metro has not indexed them yet:

```js
const { getDefaultConfig } = require("@react-native/metro-config");
const {
  withNativeScriptMetroConfig,
} = require("@nativescript/react-native/metro-config");

module.exports = withNativeScriptMetroConfig(getDefaultConfig(__dirname));
```

`npx nativescript-rn configure` applies both transformations idempotently.

`installWorklets()` is still exported for custom initialization, but it throws
when Worklets is unavailable or incompatible. `scheduleOnUI()` throws when the
callback was not transformed into a Worklets function.

Obj-C blocks and JS-backed Obj-C method callbacks, including `NSObject.extend`
subclass overrides and delegates created with `createDelegate()`, should return
to React Native's JS thread for JS work. Use `jsInvoker()` when a callback can be
reached from a native caller thread:

```ts
UIView.animateWithDurationAnimationsCompletion(
  0.25,
  null,
  NativeScript.jsInvoker((finished) => {
    console.log("animation finished", finished);
  }),
);
```

The package also includes a Babel plugin for directive-style JS callbacks:

```ts
someNativeApi(() => {
  "use js";
  console.log("back on JS");
});
```

The transform rewrites those callbacks to `NativeScript.jsInvoker(fn)`.
`"use ui"` is rejected in React Native; use a Worklets `"worklet"` callback with
`NativeScript.scheduleOnUI()` instead.

`@nativescript/react-native/babel-plugin` adds the `"worklet"` directive to
Fabric hooks and command handlers inside a `defineNativeComponent` spec.
Examples still include the directive because extracted helper functions need
their own directive.

## `defineNativeComponent`

```ts
import { defineNativeComponent } from "@nativescript/react-native";
```

One call defines a component name, prop defaults, events, and lifecycle hooks.
It returns a typed `HostComponent`. The package builds its view config at
runtime through `NativeComponentRegistry.get`.

```ts
type NativeComponentSpec<Props, Events, Instance> = {
  name: string;
  props?: Props; // defaults; keys become validAttributes
  colorProps?: (keyof Props & string)[]; // apply React Native's ColorValue processor
  events?: (keyof Events & string)[]; // "onXxx" -> Fabric's "topXxx"
  shouldBeRecycled?: boolean; // default: recycled like any Fabric view

  create?(ctx): unknown | void;
  updateProps?(ctx, next: Partial<Props>, prev: Partial<Props>): void;
  mountChildComponentView?(ctx, child, index): void;
  unmountChildComponentView?(ctx, child, index): void;
  mountingTransactionWillMount?(ctx, txn): void;
  mountingTransactionDidMount?(ctx, txn): void;
  updateLayoutMetrics?(ctx, next, prev): boolean;
  finalizeUpdates?(ctx, mask: number): void;
  prepareForRecycle?(ctx, viaInvalidate: boolean): void;
  commands?: Record<string, (ctx, args: unknown[]) => void>;
};
```

- `name` is the Fabric component name.
- `props` supplies defaults. Its keys become the component's
  `validAttributes`; the `Props` generic defines their types. Do not add
  `style`. The inherited view config already contains React Native's style
  descriptor, and replacing it prevents Yoga props from reaching the shadow
  node.
- `colorProps` names declared props whose values use React Native's
  `ColorValue` contract. The generated view config applies `processColor`
  before Fabric receives them, including CSS color syntax, `PlatformColor`,
  and `DynamicColorIOS`. This matches `codegenNativeComponent`; UIKit hooks
  should consume the resulting processed number or native-color object.
- `events` contains names such as `onAppear`. The function rejects names that
  do not start with `on` followed by an uppercase letter.
- `shouldBeRecycled: false` makes Fabric dispose of the view through
  `-invalidate` instead of its recycle pool. `prepareForRecycle` runs on both
  paths and receives the chosen path in `viaInvalidate`.
- Every hook is a worklet on the main thread. If the Babel plugin does not add
  the directive, write `"worklet"` at the start of the hook.

### The hooks

- `create(ctx)` runs first and once per instance. Store component state on
  `ctx.instance`. A returned `UIView` becomes the component's `contentView`.
  With no return value, `ctx.view` remains the content view.
- `updateProps(ctx, next, prev)` receives partial updates. Check whether each
  key is present and merge it into `ctx.instance`.
- `mountChildComponentView` and `unmountChildComponentView` replace Fabric's
  default behavior independently. Define both when the component owns child
  mounting. `child` contains `{ tag, view, instance }`.
- `mountingTransactionWillMount` and `mountingTransactionDidMount` run before
  and after a transaction that touches the tag's tree. Use
  `txn.didMutateChildrenOf(tag)` to check whether the transaction changed its
  children. Defer UIKit containment changes with `ctx.scheduleOnMainQueue`.
- `updateLayoutMetrics` can return `false` when the component owns its frame.
- `finalizeUpdates` runs after the other update hooks in a commit. Its `mask`
  argument is React Native's `RNComponentViewUpdateMask` value.
- `prepareForRecycle` is the final hook. Release retained helpers there.
- `commands` maps names to worklet handlers. Invoke them with
  `dispatchNativeComponentCommand(ref.current, "name", args)`.

## `ctx`

Every hook receives an `NSComponentContext<Instance>`:

| Member                                             | What it does                                                                                                                                                                                                                                                                                                                                                           | Safe from                              |
| -------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------- |
| `ctx.view`                                         | This instance's `NativeView` (the Fabric `ComponentView`, or the raw view for a `child` argument).                                                                                                                                                                                                                                                                     | every hook                             |
| `ctx.tag`                                          | The Fabric react tag.                                                                                                                                                                                                                                                                                                                                                  | every hook                             |
| `ctx.instance`                                     | Your mutable per-tag object (`Instance`). Empty at `create`; the same object every other hook for this tag gets back. State lives here, never as an expando on `ctx.view`.                                                                                                                                                                                             | every hook after `create` populates it |
| `ctx.emit(name, payload?)`                         | Dispatches a declared event. Fired-before-mounted events are buffered natively and flushed once Fabric's own emitter attaches.                                                                                                                                                                                                                                         | every hook, including `create`         |
| `ctx.setContentSize(size, opts?)`                  | Writes a UIKit-measured size into shadow state. `offsetX` and `offsetY` move either the component frame or its content origin, selected with `offsetMode`. `authority` controls whether the native size overrides Yoga measurement. `updateMode: "immediate"` opts into Fabric's synchronous event-queue update. Calls made before Fabric supplies state are buffered. | every hook, including `create`         |
| `ctx.setLayoutInsets(insets, edges)`               | Adds native margins on the selected Yoga edges. Safe-area hosts use this for edge-specific layout.                                                                                                                                                                                                                                                                     | every hook                             |
| `ctx.setContentInsets(insets, opts?)`              | Adds native padding to the component's declared Yoga padding. UIKit-measured header content uses this without replacing app styles. `updateMode` has the same meaning as in `setContentSize`.                                                                                                                                                                          | every hook                             |
| `ctx.setWindowOverlay(view, enabled)`              | Configures a detached content view as a touch-enabled, frontmost `UIWindow` overlay. Disable it during teardown.                                                                                                                                                                                                                                                       | every hook                             |
| `ctx.setNativeObjectMetadata(name, value)`         | Exposes a string or string-array value through an exact zero-argument Objective-C selector on this component's per-name native class. Existing native methods are never replaced.                                                                                                                                                                                      | every hook                             |
| `ctx.enableChildControllerTraitForwarding()`       | Lazily makes root UIKit controllers honor nested child controllers. Controllers with `nativeScriptProvidesWindowTraits()` route status-bar and home-indicator queries; controllers with `nativeScriptEvaluateOrientationMask()` route orientation, where `0` inherits the app's supported orientations.                                                                | every hook                             |
| `ctx.invalidateControllerTraits(traits)`           | Asks the owning window's root controller to re-evaluate status-bar, home-indicator, or supported-orientation traits. Orientation invalidation uses scene geometry updates on iOS 16 and newer.                                                                                                                                                                         | every hook                             |
| `ctx.scheduleOnMainQueue(fn)`                      | Defers `fn` by one main run-loop turn with `dispatch_async`. Use it before changing UIKit containment during a Fabric mounting transaction.                                                                                                                                                                                                                            | every hook                             |
| `ctx.loadImage(source, options, callback)`         | Loads an image through React Native's image pipeline and returns the resulting native image to the UI runtime. Set `options.template` to request template rendering mode.                                                                                                                                                                                              | every hook                             |
| `ctx.createDelegate(protocols, methods, options?)` | Same function as top-level `NativeScript.createDelegate`, forwarded so a hook doesn't need a second import.                                                                                                                                                                                                                                                            | every hook                             |
| `ctx.instanceForView(view)`                        | Looks up a tracked `Instance` from a `NativeView` and its Fabric tag.                                                                                                                                                                                                                                                                                                  | every hook                             |

## Worked example

A minimal component: one prop, one command, no children.

```ts
import {
  defineNativeComponent,
  dispatchNativeComponentCommand,
} from "@nativescript/react-native";

type BadgeProps = { text: string };
type BadgeInstance = { label: any };

export const NativeBadge = defineNativeComponent<
  BadgeProps,
  Record<string, never>,
  BadgeInstance
>({
  name: "NativeBadge",
  props: { text: "" },
  shouldBeRecycled: false,

  create(ctx) {
    "worklet";
    const g = globalThis as any;
    const label = g.UILabel.alloc().initWithFrame(g.CGRectZero);
    label.textAlignment = g.NSTextAlignment.Center;
    label.textColor = g.UIColor.whiteColor;
    label.backgroundColor = g.UIColor.systemBlueColor;
    label.layer.cornerRadius = 8;
    label.clipsToBounds = true;
    ctx.instance.label = label;
    return label; // installed as this component's contentView
  },

  updateProps(ctx, next) {
    "worklet";
    if (next.text !== undefined) ctx.instance.label.text = next.text;
  },

  commands: {
    setTone(ctx, args) {
      "worklet";
      const g = globalThis as any;
      const tone = args[0] as string;
      ctx.instance.label.backgroundColor =
        tone === "green"
          ? g.UIColor.systemGreenColor
          : g.UIColor.systemBlueColor;
    },
  },
});

// <NativeBadge ref={badgeRef} text="Saved" style={{ height: 32, width: 96 }} />
dispatchNativeComponentCommand(badgeRef.current, "setTone", ["green"]);
```

Container components can take ownership of child mounting and defer UIKit
updates until Fabric finishes a mounting transaction. Return a controller's
view from `create`, implement `mountChildComponentView` and
`unmountChildComponentView`, then use `mountingTransactionDidMount` with
`txn.didMutateChildrenOf(ctx.tag)` to coalesce native reconciliation.

## Hazards

### Mutually recursive worklets

Worklets' Babel plugin
desugars every `"worklet"`-directed function into a `const NAME =
factory(...)` binding, which is not hoisted. Two helpers that call each other
directly crash with `ReferenceError: Cannot access 'X' before initialization` no
matter which is declared first, because whichever is captured first is
captured while the other's binding is still uninitialized. Self-recursion
works through Worklets' `this._recur` mechanism. `defineNativeComponent`
checks each hook's closure when the component is defined and reports the
capture chain. Put mutually dependent helpers on a stable object and call them
through property lookup:

```ts
function ensureReconcileHelpersInstalled() {
  "worklet";
  const g = globalThis as any;
  if (g.__nsComponentHelpers) return;
  g.__nsComponentHelpers = {
    updateParent(ctx) {
      "worklet";
      // ... calls g.__nsComponentHelpers.updateChild(ctx, inst) by lookup
    },
    updateChild(ctx, inst) {
      "worklet";
      // ... calls g.__nsComponentHelpers.updateParent(ctx) by lookup
    },
  };
}
```

### Protocol completion blocks

A method whose completion parameter is declared only on a protocol may throw
`Error: Native callback metadata is unavailable` when passed a plain JS
closure. The bridge cannot infer the block signature from the concrete class.
Supply the signature with `interop.Block`:

```ts
const retry = (globalThis as any).interop.Block(() => {
  "worklet";
  // ...
}, "v@?@"); // void return, block-self, one object argument
nav.transitionCoordinator.animateAlongsideTransitionCompletion(null, retry);
```

Class-declared completion parameters do not need this wrapper. PR #71 adds
protocol lookup to the runtime, which will also remove this requirement.

### State on native proxies

Do not store component state as an expando on `ctx.view` or another native
proxy. Use `ctx.instance`. `ctx.instanceForView` uses the Fabric tag for reverse
lookup.

### Serializing native objects

Do not pass native objects to `JSON.stringify`, use them as plain-object keys,
or send `-description` to live callback arguments. Those operations inspect
bridge state and may crash while the native object is in use.

### Class lookup from worklets

`NativeScript.getClass` and `NativeScript.getProtocol` are JS-thread functions.
Calling either from a worklet throws `Tried to synchronously call a Remote
Function`. Native globals are already installed in the UI runtime, so access
the class through `globalThis`:

```ts
create(ctx) {
  "worklet";
  const vc = (globalThis as any).UIViewController.alloc().init(); // not NativeScript.getClass("UIViewController")
},
```

## Design limits

The package does not add a batching layer, a second marshalling protocol, or
method swizzling. Fabric carries props, events, and commands. Hooks call UIKit
on the main thread. If a component needs JSON transport or string handles to
cross this boundary, report the missing bridge behavior instead of adding a
parallel transport.

For nested `UIViewController` containment, build the controller in `create`,
return its `.view`, and attach it through the responder chain when the
component becomes reachable.

## Availability and heavy UIKit classes

Use availability helpers before touching optional frameworks. Call them from
the JS thread, not a worklet. Simulator and device availability can differ for
such as VisionKit, QuickLook, and PassKit.

```ts
if (
  NativeScript.loadFramework("VisionKit") &&
  NativeScript.isClassAvailable("VNDocumentCameraViewController")
) {
  const CameraController = NativeScript.getClass<
    typeof VNDocumentCameraViewController
  >("VNDocumentCameraViewController");
  const controller = CameraController?.new();
}
```

`NativeScript.isFrameworkLoaded(nameOrPath)` checks an `NSBundle`;
`NativeScript.loadFramework(nameOrPath)` loads a system framework by name or a
specific `.framework` path; `NativeScript.getClass(name)` and
`NativeScript.getProtocol(name)` return dynamically available native references.

Class globals installed through `NativeScript.init({ globals: true })` are
lazy. Large UIKit classes such as `UITabBarController` inherit many members.
Direct construction and member access remain lazy. `Object.keys`, prototype
introspection, and generated member lists force enumeration and cost more.

## Retention and delegates outside `defineNativeComponent`

UIKit often retains delegates and actions weakly or outlives the JavaScript
closure that created them. Retain those helper objects explicitly:

```ts
const retainer = NativeScript.createRetainer();

const delegate = NativeScript.createDelegate<UIScrollViewDelegate>(
  UIScrollViewDelegate,
  {
    scrollViewDidScroll(scrollView) {
      NativeScript.scheduleOnUI(() => {
        "worklet";
        scrollView.indicatorStyle = UIScrollViewIndicatorStyle.White;
      });
    },
  },
  { retainer },
);

scrollView.delegate = delegate;

// Later, when the owner is done:
scrollView.delegate = null;
retainer.dispose();
```

`createDelegate(protocols, methods, options)` accepts protocol objects or
names. Use `NativeScript.retain(value)` / `NativeScript.release(value)` only
for process-lifetime helpers; prefer `createRetainer()` (or, inside a
`defineNativeComponent` hook, `ctx.createDelegate`'s own retention options)
for anything scoped to one component instance.

Objective-C exceptions thrown while dispatching through the bridge are converted
to JS errors where Objective-C can catch them. Process-level failures such as
`abort()`, fatal assertions, memory corruption, and some framework precondition
violations are not catchable; use availability checks and presentation guards
instead of relying on exceptions as control flow.

The package ships example native-API usage under
`@nativescript/react-native/examples`.

The published package includes generated NativeScript metadata, the libffi
xcframework, and generated iOS SDK TypeScript declarations. Build it from the
repository root with:

```sh
npm run build-rn-turbomodule
```

The tarball is written to `packages/react-native/dist/` and copied to
`build/npm-tarballs/`.

To verify it inside a generated React Native iOS app:

```sh
npm run test-rn-turbomodule
```

## Using the package in a React Native app

1. Build or download the package tarball.
2. Install it in an RN app that has Hermes and the New Architecture enabled:

   ```sh
   npm install /path/to/nativescript-react-native-0.0.1.tgz react-native-worklets
   cd ios
   RCT_NEW_ARCH_ENABLED=1 USE_HERMES=1 pod install
   ```

3. Initialize it before using native APIs:

   ```ts
   import NativeScript from "@nativescript/react-native";

   NativeScript.init();

   await NativeScript.scheduleOnUI(() => {
     "worklet";
     UIApplication.sharedApplication.keyWindow.tintColor =
       UIColor.systemPinkColor;
   });
   ```

4. Add the bundled NativeScript Babel plugin and the Worklets Babel plugin:

   ```js
   module.exports = {
     presets: ["module:@react-native/babel-preset"],
     plugins: [
       "@nativescript/react-native/babel-plugin",
       [
         "react-native-worklets/plugin",
         { bundleMode: true, strictGlobal: true },
       ],
     ],
   };
   ```

## Using the package in an Expo app

Expo Go cannot load this package because it contains custom native code. Use an
Expo development build, EAS Build, or `npx expo run:ios`.

1. Install the package:

   ```sh
   npx expo install @nativescript/react-native react-native-worklets
   ```

   When testing a local tarball:

   ```sh
   npm install /path/to/nativescript-react-native-0.0.1.tgz
   ```

2. Add the config plugin to `app.json` or `app.config.js`:

   ```json
   {
     "expo": {
       "plugins": ["@nativescript/react-native"]
     }
   }
   ```

   The plugin configures iOS for Hermes and the React Native New Architecture,
   which are required by this JSI TurboModule. It also adds the
   `@nativescript/react-native/babel-plugin` and `react-native-worklets/plugin`
   transforms to `babel.config.js`, enables Worklets Bundle Mode, and wraps
   `metro.config.js` with the matching resolver and generated-module SHA
   fallback.

3. Prebuild and run the iOS development build:

   ```sh
   npx expo prebuild --platform ios
   npx expo run:ios
   ```

4. Initialize NativeScript in app code before using native APIs, then define
   native components as shown in [`defineNativeComponent`](#definenativecomponent)
   above:

   ```tsx
   import NativeScript from "@nativescript/react-native";

   NativeScript.init();
   ```

Set `{ "babelPlugin": false }` in the config plugin options if you prefer to add
the NativeScript and Worklets Babel plugins manually.

Set `{ "workletsBundleMode": false }` only when an app deliberately produces
multiple independent top-level Metro bundles; Worklets currently initializes
one bundle snapshot per runtime, so separately loaded bundles cannot contribute
worklets to that snapshot.

The plugin also writes `nativescript.react-native.json` so metadata options are
visible to native builds. You can pass metadata inputs when the app uses
Objective-C-visible pods or extra system frameworks:

```json
{
  "expo": {
    "plugins": [
      [
        "@nativescript/react-native",
        {
          "metadata": {
            "includePods": ["SomeObjCSDK"],
            "includeSystemFrameworks": ["UIKit", "MapKit", "WebKit"]
          }
        }
      ]
    ]
  }
}
```

## Bare React Native setup helper

The tarball includes a small CLI for bare RN projects:

```sh
npx nativescript-rn configure
npx nativescript-rn generate-metadata --check
cd ios
RCT_NEW_ARCH_ENABLED=1 USE_HERMES=1 pod install
```

`configure` adds the bundled NativeScript and Worklets Babel plugins when
missing, writes
`nativescript.react-native.json`, and warns when the app is not configured for
Hermes and the New Architecture. The command is intentionally conservative and
does not make destructive native project edits.
