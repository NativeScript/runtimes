# @nativescript/react-native

NativeScript's Native API for React Native: call Objective-C and UIKit on iOS,
and Java and the Android SDK on Android, directly from JavaScript.

The package is one small TurboModule. Its `init()` method attaches the
NativeScript Native API host object to `globalThis.__nativeScriptNativeApi` and
installs lazy NativeScript-style globals for native classes, functions,
constants and enums. On iOS it also provides `defineNativeComponent`, which
defines Fabric components in TypeScript without codegen or project-specific
native code. The host object itself is pure JSI and is shared with the
NativeScript runtime.

## Requirements

- React Native 0.79 or newer.
- iOS 15.1+, with Hermes and the New Architecture.
- `react-native-worklets` 0.8 or newer. It is required on iOS: the pod links
  `RNWorklets`, and UIKit work and `defineNativeComponent` hooks run on the
  Worklets UI runtime.
- Android works with Hermes or JSC. See [Android](#android).

Expo Go cannot load this package because it contains custom native code. Use an
Expo development build, EAS Build, or `npx expo run:ios` / `npx expo run:android`.

## Installation

### React Native

1. Install the package and Worklets:

   ```sh
   npm install @nativescript/react-native react-native-worklets
   ```

2. Configure the project:

   ```sh
   npx nativescript-rn configure
   ```

   `configure` adds the NativeScript and Worklets Babel plugins when missing,
   wraps `metro.config.js` for Worklets Bundle Mode, writes
   `nativescript.react-native.json`, applies the Android Gradle script, adds the
   Android typings to `tsconfig.json`, and warns when iOS is not configured for
   Hermes and the New Architecture. It is intentionally conservative and does
   not make destructive native project edits.

   To configure Babel and Metro by hand instead:

   ```js
   // babel.config.js
   module.exports = {
     presets: ["module:@react-native/babel-preset"],
     plugins: [
       "@nativescript/react-native/babel-plugin",
       ["react-native-worklets/plugin", { bundleMode: true, strictGlobal: true }],
     ],
   };
   ```

   `strictGlobal: true` is required. Without it the Worklets plugin captures
   every name a worklet does not declare (`UIView`, `UILabel`, ...) into the
   worklet's closure, read on the JavaScript thread when the module defining
   the worklet is evaluated. NativeScript's classes do not exist there, so the
   app fails at startup with `ReferenceError: Property 'UILabel' doesn't exist`.
   With `strictGlobal`, those names are resolved on the runtime the worklet runs
   on. With `babel-preset-expo`, also pass `{ worklets: false }` to the preset
   so it does not add a second copy of the plugin without `strictGlobal`.

   `bundleMode: true` avoids compiling every component worklet separately at
   startup. Wrap the app's Metro config so generated worklet modules are
   preloaded and content-hashed even when Metro has not indexed them yet:

   ```js
   // metro.config.js
   const { getDefaultConfig } = require("@react-native/metro-config");
   const {
     withNativeScriptMetroConfig,
   } = require("@nativescript/react-native/metro-config");

   module.exports = withNativeScriptMetroConfig(getDefaultConfig(__dirname));
   ```

3. Install the pods:

   ```sh
   cd ios
   RCT_NEW_ARCH_ENABLED=1 USE_HERMES=1 pod install
   ```

`npx nativescript-rn generate-metadata --check` prints the iOS metadata
configuration (extra pods and system frameworks) that native builds will use.

### Expo

1. Install the package and Worklets:

   ```sh
   npx expo install @nativescript/react-native react-native-worklets
   ```

2. Add the config plugin to `app.json` or `app.config.js`:

   ```json
   {
     "expo": {
       "plugins": ["@nativescript/react-native"]
     }
   }
   ```

   The plugin:

   - configures iOS for Hermes and the React Native New Architecture, which this
     JSI TurboModule requires;
   - applies the NativeScript Gradle script to `android/app/build.gradle`;
   - adds the `@nativescript/react-native/babel-plugin` and
     `react-native-worklets/plugin` (with `{ bundleMode: true, strictGlobal: true }`)
     transforms to `babel.config.js`, passes `{ worklets: false }` to
     `babel-preset-expo` so the preset does not add its own copy of the Worklets
     plugin, and wraps `metro.config.js` with the matching resolver and
     generated-module SHA fallback. An existing `react-native-worklets/plugin`
     entry that already has options is left alone, with a warning if it lacks
     `strictGlobal: true`.

   Set `{ "babelPlugin": false }` in the plugin options if you prefer to add the
   NativeScript and Worklets Babel plugins manually.

   Set `{ "workletsBundleMode": false }` only when an app deliberately produces
   multiple independent top-level Metro bundles; Worklets currently initializes
   one bundle snapshot per runtime, so separately loaded bundles cannot
   contribute worklets to that snapshot.

3. Prebuild and run a development build:

   ```sh
   npx expo prebuild
   npx expo run:ios
   npx expo run:android
   ```

The plugin also writes `nativescript.react-native.json` so metadata options are
visible to native builds. On iOS you can pass metadata inputs when the app uses
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

For Android release builds, `metadata.android.include` lists classes to keep;
see [Android setup](#setup). Bare projects using `nativescript-rn configure` can
put the same `metadata` object under a `nativeScriptReactNative` key in
`package.json`. An `android` block written into `nativescript.react-native.json`
by hand is kept unless the options set one.

Importing the package in a build without its native module (Expo Go, or a build
made before the package was added) throws an error that says to use a
development build or rebuild the app.

## Usage

Call `init()` once, before using native APIs:

```ts
import NativeScript from "@nativescript/react-native";

NativeScript.init();
```

### Which runtime gets the globals (iOS)

UIKit must only be touched on the main thread, and React Native's JavaScript
runs on its own JS thread. So on iOS, `init()` installs the full set of native
globals (`UIView`, `NSObject`, `NSTextAlignment`, ...) into the
`react-native-worklets` UI runtime, which runs on the main thread. On the
JavaScript thread it installs only `globalThis.__nativeScriptNativeApi`,
`interop`, and protocol globals.

Run UIKit work through `NativeScript.scheduleOnUI()` with a Worklets callback:

```ts
await NativeScript.scheduleOnUI(() => {
  "worklet";
  UIApplication.sharedApplication.keyWindow.tintColor = UIColor.systemPinkColor;
});
```

`scheduleOnUI()` only accepts Worklets callbacks and throws when the callback
was not transformed into a Worklets function; running React Native's JS-thread
runtime as a UI-thread shim is not supported. `installWorklets()` is still
exported for custom initialization, but it throws when Worklets is unavailable
or incompatible.

Native objects the worklet returns, at the top level or inside plain arrays and
objects, come back as objects of the React Native runtime. Each runtime has its
own wrapper for a native object, and using a wrapper from the other runtime
throws. To hand one across yourself, pass `interop.handleof(object).toHexString()`
and turn it back with `interop.object(handle)`.

For non-UI APIs on the JavaScript thread (Foundation, file system, networking),
either look classes up explicitly or opt into JS-thread globals:

```ts
const NSDateClass = NativeScript.getClass<typeof NSDate>("NSDate");

// or
NativeScript.init("", { globals: true });
const now = NSDate.new();
```

Only full C constant names become globals, for example `NSTextAlignmentCenter`
and `NSOrderedAscending`; short enum member names stay on their enum
(`NSTextAlignment.Center`). JS-thread globals make it possible to touch UIKit
off the main thread by accident, which is why they are opt-in. Helpers that
build native classes, such as `createDelegate()`, need them when called on the
JavaScript thread.

On Android, `init()` installs the Java package namespaces (`java`, `android`,
...) on the JavaScript runtime. See [Android](#android).

### Callbacks and threads

Obj-C blocks and JS-backed Obj-C method callbacks, including `NSObject.extend`
subclass overrides and delegates created with `createDelegate()`, should return
to React Native's JS thread for JS work. Use `jsInvoker()` when a callback can be
reached from a native caller thread:

```ts
NativeScript.init("", { globals: true });

const url = NSURL.URLWithString("https://nativescript.org");
NSURLSession.sharedSession
  .dataTaskWithURLCompletionHandler(
    url,
    NativeScript.jsInvoker((data, response, error) => {
      console.log("request finished", error == null);
    }),
  )
  .resume();
```

Delegate, data-source, target/action, and `UIAction` callbacks are JS-side
callbacks. Treat their bodies as JS work. If a callback can be reached from a
background native thread and needs to mutate UIKit, wrap the mutation in
`NativeScript.scheduleOnUI()` with a Worklets callback.

The bundled Babel plugin also supports directive-style JS callbacks:

```ts
someNativeApi(() => {
  "use js";
  console.log("back on JS");
});
```

The transform rewrites those callbacks to `NativeScript.jsInvoker(fn)`.
`"use ui"` is rejected in React Native; use a Worklets `"worklet"` callback with
`NativeScript.scheduleOnUI()` instead.

`@nativescript/react-native/babel-plugin` also adds the `"worklet"` directive to
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

Four React Native mechanisms cross the JS and UI boundary:

1. Fabric sends changed props to `updateProps` during a commit.
2. `ctx.emit(name, payload)` sends events through Fabric's event emitter.
3. `dispatchNativeComponentCommand(ref, name, args)` invokes a component's
   command handler through Fabric.
4. `NativeScript.jsInvoker()` sends a native callback to the JS thread through
   Worklets' `scheduleOnRN`.

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

### `ctx`

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

### Worked example

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
`txn.didMutateChildrenOf(ctx.tag)` to coalesce native reconciliation. For nested
`UIViewController` containment, build the controller in `create`, return its
`.view`, and attach it with `ctx.attachChildViewController` when the component
becomes reachable.

For modal UIKit controllers, find the top visible presenter and guard against
double presentation. Helpers called from a worklet must be worklets too:

```ts
function topVisibleViewController(
  root = UIApplication.sharedApplication.keyWindow?.rootViewController,
) {
  "worklet";
  let current = root;
  while (current?.presentedViewController) {
    current = current.presentedViewController;
  }
  if (current?.selectedViewController) {
    return topVisibleViewController(current.selectedViewController);
  }
  if (current?.visibleViewController) {
    return topVisibleViewController(current.visibleViewController);
  }
  return current;
}

await NativeScript.scheduleOnUI(() => {
  "worklet";
  const presenter = topVisibleViewController();
  if (!presenter || presenter.presentedViewController) {
    return;
  }
  presenter.presentViewControllerAnimatedCompletion(controller, true, null);
});
```

### Hazards

#### Mutually recursive worklets

Worklets' Babel plugin desugars every `"worklet"`-directed function into a
`const NAME = factory(...)` binding, which is not hoisted. Two helpers that call
each other directly crash with `ReferenceError: Cannot access 'X' before
initialization` no matter which is declared first, because whichever is captured
first is captured while the other's binding is still uninitialized.
Self-recursion works through Worklets' `this._recur` mechanism.
`defineNativeComponent` checks each hook's closure when the component is defined
and reports the capture chain. Put mutually dependent helpers on a stable object
and call them through property lookup:

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

#### State on native proxies

Do not store component state as an expando on `ctx.view` or another native
proxy. Use `ctx.instance`. `ctx.instanceForView` uses the Fabric tag for reverse
lookup.

#### Serializing native objects

Do not pass native objects to `JSON.stringify`, use them as plain-object keys,
or send `-description` to live callback arguments. Those operations inspect
bridge state and may crash while the native object is in use.

#### Class lookup from worklets

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

### Availability and heavy UIKit classes

Use availability helpers before touching optional frameworks. Call them from
the JS thread, not a worklet. Simulator and device availability can differ for
frameworks such as VisionKit, QuickLook, and PassKit.

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

Class globals installed through `NativeScript.init("", { globals: true })` are
lazy. Large UIKit classes such as `UITabBarController` inherit many members.
Direct construction and member access remain lazy. `Object.keys`, prototype
introspection, and generated member lists force enumeration and cost more.

### Retention and delegates outside `defineNativeComponent`

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
`@nativescript/react-native/examples`, and includes generated NativeScript
metadata, the libffi xcframework, and generated iOS SDK TypeScript declarations.

## Android

The same package runs on Android. Where iOS gives you Objective-C and UIKit,
Android gives you Java and the Android SDK — plus every library your app
depends on, because the metadata is generated from your app's own classpath.

```ts
import NativeScript from "@nativescript/react-native";

NativeScript.init();

const now = java.lang.System.currentTimeMillis();
const sdk = android.os.Build.VERSION.SDK_INT;
```

Each top-level Java package is a global named after it (`java`, `android`,
`androidx`, `com`, ...). A package whose name is a JavaScript keyword gets a `$`
prefix instead. So does one whose name is already a global when NativeScript
installs, and a warning is logged. Expo defines `expo`, for example, so Expo's
own Java classes are under `$expo`:

```ts
const modules = $expo.modules;
```

### How it works

The runtime does not bring a JavaScript engine. It binds to the
`jsi::Runtime` React Native already created, which is why the same build works
whether your app runs Hermes or JSC. The Android module compiles from source in
your app the way every React Native module with C++ does — `jsi::Runtime` is an
abstract C++ class whose layout changes between React Native versions, so a
prebuilt binary could not be safe across them.

### Setup

`npx nativescript-rn configure` and the Expo config plugin both add the Gradle
script. To do it by hand, in `android/app/build.gradle`, after the React plugin:

```groovy
apply from: "../../node_modules/@nativescript/react-native/android/nativescript.gradle"
```

That adds two things to your build:

* **Metadata** — a description of the classes on the app's resolved classpath,
  generated into the APK's assets on every build. This is what makes
  `android.widget.Toast` and any third-party library resolvable from JavaScript.
  `whitelist.mdg` / `blacklist.mdg` next to `android/app/build.gradle` filter it,
  with the same format the NativeScript CLI uses.

  Debug builds ship the whole classpath. Release builds ship only what the
  JavaScript bundle can reach, plus the supertypes, nested classes and
  signature types those classes need. They also leave out the packages of
  autolinked React Native modules, which are reached over the TurboModule
  bridge rather than the interop. A class your code only reaches dynamically
  (a name built at runtime, for example) can therefore resolve in debug and be
  missing in release. Declare such classes in `nativescript.react-native.json`
  at the project root, as `"pkg.Class"`, `"pkg.*"` or `"pkg:Class"`:

  ```json
  {
    "reactNative": {
      "metadata": {
        "android": {
          "include": ["com.example.sdk.Worker", "com.example.views.*"]
        }
      }
    }
  }
  ```

  With Expo, put the same `android` object under `metadata` in the config
  plugin options. `-PnsFilterMetadata=false` turns the filter off and ships
  the whole classpath.
* **TypeScript declarations** — for the same classpath, so third-party
  libraries are typed too. They are written to `types/android/` next to the app
  on the first build and regenerated only when the classpath changes; commit
  them alongside your code. `-PnsGenerateTypings=false` opts out, and

  ```bash
  cd android && ./gradlew generateNativeScriptTypingsDebug
  ```

  generates them on their own. `configure` adds them to `tsconfig.json`; with
  the Expo config plugin, add them yourself:

  ```json
  {
    "include": ["types/android/android.d.ts", "types/android/android-declarations.d.ts"]
  }
  ```

  The package does not ship Android declarations, and `@nativescript/types-android`
  is not needed: it covers only the Android SDK, and its declarations collide
  with the generated ones.

### Extending Java classes

Every class that extends a native class carries `@NativeClass`:

```ts
import { NativeClass } from "@nativescript/react-native";

@NativeClass()
class Ticker extends java.lang.Object {
  run() {
    console.log("tick");
  }
}
```

`@NativeClass` is a plain runtime decorator. It reads nothing at build time and
behaves identically under Babel, SWC or tsc, in either the legacy or the TC39
decorator dialect. It is not optional: `class X extends java.util.ArrayList {}`
on its own produces a class whose overrides resolve in JavaScript but which Java
still sees as an unmodified `ArrayList`. The decorator is what generates the
real Java subclass, so that Java calling `size()` reaches your implementation.

A JavaScript class has one base, so several interfaces are declared on the
decorator:

```ts
@NativeClass({
  interfaces: [java.lang.Runnable, java.util.concurrent.Callable],
})
class Job extends java.lang.Object {
  run() {}
  call() {
    return new java.lang.String("done");
  }
}
```

Decorators are *syntax*, so your bundler has to parse them — that means
`@babel/plugin-proposal-decorators` (or the SWC equivalent) in your Babel
config. That is a standard plugin, not a NativeScript-specific transform.

#### Naming a class

`@NativeClass()` on its own generates the Java class on the device. The class is
named after what it contains — its base class, its interfaces, and the names it
overrides — rather than after the source position of the call, because a guest
runs inside React Native's bundle and no source position computed here would
match one a build-time tool computed from your source tree.

One consequence is worth knowing: two classes that extend the same type, declare
the same interfaces and override the same method names **share one generated
Java class**. That is safe, because the generated Java is identical in both
cases and only forwards into JavaScript — each instance still runs its own
implementation.

It stops being safe when *Java* constructs the class by name, because then the
class name is all the runtime has to find the JavaScript side with. So:

> **Name any class that Java instantiates for you** — an `Activity`, a
> `Fragment`, a `Service`, a `Worker`, anything reached by reflection or named
> in `AndroidManifest.xml`. Naming makes it unique. Classes you construct
> yourself with `new` never need it.

A class that has to exist *before* the app runs — one named in
`AndroidManifest.xml`, for instance — needs a fixed name instead:

```ts
import { JavaProxy } from "@nativescript/react-native";

@JavaProxy("com.example.Ticker")
@NativeClass()
class Ticker extends java.lang.Object {}
```

A fully-qualified name is taken verbatim by the runtime, so it is reproducible
at build time. The runtime then *looks the class up* rather than generating it,
which means a named class only resolves once the static binding generator has
emitted it — see the note below.

> **Release builds only.** The generator reads a bundle, so it needs one on
> disk. On a release build the Gradle script bundles your JavaScript a second
> time -- separately from the one that ships, and with classes preserved -- and
> pre-generates the Java from that. In debug there is no bundle to read: Metro
> serves it over HTTP. Unnamed `@NativeClass()` classes are generated on the
> device and work in both, but `@JavaProxy` names resolve only in release.

If you would rather not use decorators, the runtime API is always available and
needs nothing from the bundler:

```ts
const Ticker = java.lang.Object.extend("Ticker", { run() {} });
const listener = new android.view.View.OnClickListener({ onClick(v) {} });
```

### Threads

React Native owns the JavaScript thread, and JSI may only be touched there. A
Java callback arriving on another thread is marshalled onto the JS thread for
you. The one thing to avoid is blocking the JS thread while waiting for such a
callback — `thread.join()` immediately after `thread.start()` deadlocks, because
the callback cannot run until the JS thread is free again.

That includes callbacks Android runs on the main thread: a `Runnable` posted to
`new Handler(Looper.getMainLooper())` runs its JavaScript on the JS thread. There
is no Worklets UI runtime on Android yet, so `scheduleOnUI()` and
`defineNativeComponent` are iOS only, and touching an `android.view.View` from
JavaScript throws `CalledFromWrongThreadException`. Every non-UI Android API is
available.

### Reloads

A reload (Fast Refresh's full reload, `DevSettings.reload()`, or
`expo-updates`' `reloadAsync()`) creates a new JavaScript runtime. NativeScript
lets go of the old one and installs its globals into the new one, so
`NativeScript.init()` works the same after a reload.

Java objects that the old runtime created or extended stay bound to it. If one of
them calls back into JavaScript after the reload, for example a delayed `Runnable`
or a listener that was never removed, the call throws a `NativeScriptException`,
the same as a callback into a terminated worker:

```
com.tns.NativeScriptException: Cannot find runtime for instance=com.tns.gen.java.lang.Runnable@708609e
```

If this happens on the main thread, the app crashes. To avoid it, remove
listeners and cancel posted callbacks when the component that registered them
unmounts.

### What a guest does not install

React Native already provides `console`, timers, `queueMicrotask`,
`performance` and a module loader, so the runtime leaves all of them alone and
installs only the metadata globals, the object manager and the interop
callbacks. `Worker` is not installed either: a worker needs its own VM, which a
guest has no way to create.

## Building from source

From the root of the [NativeScript/runtimes](https://github.com/NativeScript/runtimes)
repository:

```sh
npm run build-rn-turbomodule
```

The tarball is written to `packages/react-native/dist/` and copied to
`build/npm-tarballs/`. To verify it inside a generated React Native iOS app:

```sh
npm run test-rn-turbomodule
```
