# @nativescript/react-native

NativeScript's Native API for React Native: call Objective-C and UIKit on iOS,
and Java and the Android SDK on Android, directly from JavaScript.

The package is one small TurboModule. Its `init()` method attaches the
NativeScript Native API host object to `globalThis.__nativeScriptNativeApi` and
installs lazy NativeScript-style globals for native classes, functions,
constants and enums. The host object itself is pure JSI and is shared with the
NativeScript runtime.

## Requirements

- React Native 0.79 or newer.
- iOS 13+, with Hermes and the New Architecture.
- `react-native-worklets` 0.8 or newer. It is required on iOS: the pod links
  `RNWorklets`, and UIKit work runs on the Worklets UI runtime.
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
   writes `nativescript.react-native.json`, applies the Android Gradle script,
   adds the Android typings to `tsconfig.json`, and warns when iOS is not
   configured for Hermes and the New Architecture. It is intentionally
   conservative and does not make destructive native project edits.

   To configure Babel by hand instead:

   ```js
   module.exports = {
     presets: ["module:@react-native/babel-preset"],
     plugins: [
       "@nativescript/react-native/babel-plugin",
       ["react-native-worklets/plugin", { strictGlobal: true }],
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
     `react-native-worklets/plugin` (with `{ strictGlobal: true }`) transforms to
     `babel.config.js`, and passes `{ worklets: false }` to `babel-preset-expo`
     so the preset does not add its own copy of the Worklets plugin. An existing
     `react-native-worklets/plugin` entry that already has options is left
     alone, with a warning if it lacks `strictGlobal: true`.

   Set `{ "babelPlugin": false }` in the plugin options if you prefer to add the
   NativeScript and Worklets Babel plugins manually.

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

Bare projects using `nativescript-rn configure` can put the same `metadata`
object under a `nativeScriptReactNative` key in `package.json`.

Importing the package in a build without its native module is safe:
`NativeScript.isAvailable()` returns `false` there, and `init()` and the other
functions that need the module throw an error saying what to rebuild.

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

Run UIKit work through `NativeScript.runOnUI()` with a Worklets callback:

```ts
await NativeScript.runOnUI(() => {
  "worklet";
  UIApplication.sharedApplication.keyWindow.tintColor = UIColor.systemPinkColor;
});
```

`runOnUI()` only accepts Worklets callbacks and throws when the callback was not
transformed into a Worklets function; running React Native's JS-thread runtime
as a UI-thread shim is not supported. `installWorklets()` is still exported for
custom initialization, but it throws when Worklets is unavailable or
incompatible.

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
`NativeScript.runOnUI()` with a Worklets callback.

The bundled Babel plugin also supports directive-style JS callbacks:

```ts
someNativeApi(() => {
  "use js";
  console.log("back on JS");
});
```

The transform rewrites those callbacks to `NativeScript.jsInvoker(fn)`.
`"use ui"` is rejected in React Native; use a Worklets `"worklet"` callback with
`NativeScript.runOnUI()` instead.

## Defining native UIKit views in JS

Use `defineUIKitView()` to turn a NativeScript-created `UIView` tree into a
normal React Native component. The package owns the RN host view; your
definition owns the UIKit subtree. `create`, `update`, `mounted`, and `dispose`
run through the NativeScript UI dispatcher, so UIKit calls are safe and use the
same globals and iOS SDK types as NativeScript.

```tsx
import NativeScript, { defineUIKitView } from "@nativescript/react-native";
import type { UIKitViewRef } from "@nativescript/react-native";

NativeScript.init();

type BadgeProps = {
  title: string;
  tone?: "blue" | "green";
};

export const NativeBadge = defineUIKitView<BadgeProps, UIView>({
  name: "NativeBadge",
  create() {
    const view = UIView.alloc().initWithFrame(CGRectZero);
    const label = UILabel.alloc().initWithFrame(CGRectZero);
    label.tag = 1;
    label.textAlignment = NSTextAlignment.Center;
    label.textColor = UIColor.whiteColor;
    label.autoresizingMask =
      UIViewAutoresizing.FlexibleWidth | UIViewAutoresizing.FlexibleHeight;
    view.addSubview(label);
    return view;
  },
  update(view, props) {
    view.backgroundColor =
      props.tone === "green"
        ? UIColor.systemGreenColor
        : UIColor.systemBlueColor;
    view.layer.cornerRadius = 12;
    view.clipsToBounds = true;
    const label = view.viewWithTag(1) as UILabel;
    label.text = props.title;
  },
});

<NativeBadge title="UIKit from JS" tone="blue" style={{ height: 48 }} />;
```

Forward a ref when you need imperative access:

```tsx
const badgeRef = useRef<UIKitViewRef<UIView>>(null);

await badgeRef.current?.runOnUI((view) => {
  "worklet";
  view.alpha = 0.8;
});

const measured = await badgeRef.current?.measureNative();
badgeRef.current?.invalidateNativeLayout();
```

React Native view props such as `style`, `testID`, accessibility props, responder
props, and `pointerEvents` go to the host component. Your own props go to the
UIKit definition; add a `nativeProps(props)` function to the definition when a
plugin prop should also affect the RN host. The `name` option is forwarded to the
shared native host view as a debug name, so native view descriptions can show
`NativeScriptUIView` with your definition name. It does not dynamically change
the registered RN host component tag.

### Lifecycle and context

`create`, `update`, `mounted`, and `dispose` run through the UIKit path. You do
not need to wrap UIKit work in `runOnUI()` inside those callbacks.

The first argument to `create` is also the current props object, so existing
`create(props)` definitions keep working. New code can use the context helpers:

```tsx
export const NativeSwitch = NativeScript.defineUIKitView<
  { value: boolean; onValueChange?: (value: boolean) => void },
  UISwitch
>({
  name: "NativeSwitch",
  layout: { sizing: "intrinsic" },
  create(ctx) {
    const view = UISwitch.new();
    ctx.targetAction(view, UIControlEvents.ValueChanged, () => {
      ctx.emit("onValueChange", view.on);
    });
    return view;
  },
  update(view, props) {
    if (view.on !== props.value) {
      view.setOnAnimated(props.value, false);
    }
  },
});
```

Context helpers cover common native view-manager patterns:

- `ctx.emit(name, payload)` asynchronously calls the matching React prop.
- `ctx.targetAction(control, events, callback)` retains and removes a target/action helper.
- `ctx.delegate(object, protocol, implementation)` creates, assigns, and retains a delegate.
- `ctx.notification(name, object, callback)` observes and removes notifications.
- `ctx.observe(object, keyPath, callback)` observes and removes KVO.
- `ctx.retain(value)` keeps native helper objects alive for the component lifetime.
- `ctx.release(value)` releases a retained helper before component disposal.
- `ctx.dispose(callback)` runs cleanup once, in reverse registration order.
- `ctx.invalidateLayout()` schedules a fresh native measurement.

### State, delegates, and retention

Native proxies support JavaScript expando properties for local state. Native
property setters still win first, and unsupported names fall back to JS state:

```ts
NativeScript.runOnUI(() => {
  "worklet";
  const view = UIView.new();
  view.ownerState = { selected: false };
  view.tag = 42; // still calls UIKit's native tag setter
});
```

Use `WeakMap`, React state, or another external object when you want state that
is not tied to the lifetime of a specific native proxy.

UIKit often retains delegates and actions weakly or outlives the JavaScript
closure that created them. Retain those helper objects explicitly. Use
`ctx.retain()` inside `defineUIKitView()`, or a standalone retainer elsewhere:

```ts
const retainer = NativeScript.createRetainer();

const delegate = NativeScript.createDelegate<UIScrollViewDelegate>(
  UIScrollViewDelegate,
  {
    scrollViewDidScroll(scrollView) {
      NativeScript.runOnUI(() => {
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

`createDelegate(protocols, methods, options)` accepts protocol objects or names.
If metadata was generated before a framework was loaded, use strings with
`NativeScript.loadFramework()` and `NativeScript.getProtocol()`. Passing
`{ owner: ctx }` from a definition's `create(ctx)` ties the delegate to the
component's lifetime:

```ts
NativeScript.loadFramework("QuickLook");

const dataSource = NativeScript.createDelegate(
  "QLPreviewControllerDataSource",
  {
    numberOfPreviewItemsInPreviewController() {
      return 1;
    },
    previewControllerPreviewItemAtIndex() {
      return NSURL.fileURLWithPath(ctx.props.path);
    },
  },
  { owner: ctx },
);
```

Use `NativeScript.retain(value)` and `NativeScript.release(value)` only for
process-lifetime helpers. Prefer `createRetainer()` or `ctx.retain()` for
component-scoped objects.

### Layout

React Native owns placement through Yoga. UIKit owns native behavior inside the
placed rectangle. Use `layout.sizing` to opt into native measurement:

- `fill`: fill the RN host bounds.
- `intrinsic`: use `intrinsicContentSize`.
- `sizeThatFits`: use `sizeThatFits` with style constraints.
- `autoLayout`: use `systemLayoutSizeFittingSize`.

Use `defaultSize`, `minSize`, and `maxSize` when a native view can report zero
or needs bounds during the first layout pass.

```tsx
const NativeTitle = NativeScript.defineUIKitView<{ text: string }, UILabel>({
  name: "NativeTitle",
  layout: {
    sizing: "intrinsic",
    defaultSize: { width: 1, height: 1 },
  },
  create() {
    return UILabel.new();
  },
  update(label, props, _previous, ctx) {
    label.text = props.text;
    ctx?.invalidateLayout();
  },
});
```

### Containers and view controllers

Use `defineUIKitContainer()` when React Native children should mount inside a
UIKit-owned content view:

```tsx
export const BlurCard = NativeScript.defineUIKitContainer({
  name: "BlurCard",
  create() {
    const rootView = UIVisualEffectView.alloc().initWithEffect(
      UIBlurEffect.effectWithStyle(UIBlurEffectStyle.SystemMaterial),
    );
    return {
      rootView,
      childrenView: rootView.contentView,
    };
  },
});

<BlurCard style={{ padding: 16 }}>
  <Text>React Native child content</Text>
</BlurCard>;
```

Use `defineUIViewController()` for APIs that require real child view-controller
containment:

```tsx
export const NativePageHost = NativeScript.defineUIViewController({
  name: "NativePageHost",
  createController() {
    return UIViewController.new();
  },
  update(controller) {
    controller.view.backgroundColor = UIColor.systemBackgroundColor;
  },
});
```

### Building app-specific native UI

This package is intentionally low-level. It installs NativeScript's Native API
inside React Native and gives you lifecycle helpers; it does not ship opinionated
wrappers for tabs, maps, cameras, pickers, or other app components. Build those
as local components in your app or library:

- Use `defineUIKitView()` for one native `UIView`.
- Use `defineUIKitContainer()` when React Native children should mount inside a
  native `UIView`.
- Use `defineUIViewController()` when UIKit expects view-controller containment,
  such as tabs, navigation controllers, split views, document browsers, preview
  controllers, and presentation flows.
- Use `ctx.delegate()`, `ctx.targetAction()`, `ctx.retain()`, and
  `ctx.dispose()` for native callbacks and weakly-held helper objects.
- Use `NativeScript.isClassAvailable()` before touching SDK-new APIs.

For example, build native tabs with `UITabBarController` instead of measuring a
standalone `UITabBar` as a leaf RN view:

```tsx
type NativeTabsProps = {
  selectedIndex: number;
  onSelectedIndexChange?: (index: number) => void;
};

export const NativeTabs = NativeScript.defineUIViewController<
  NativeTabsProps,
  UITabBarController
>({
  name: "NativeTabs",
  createController(ctx) {
    const controller = UITabBarController.new();
    const viewControllers = TAB_ITEMS.map((item, index) => {
      const child = UIViewController.new();
      child.view.backgroundColor = UIColor.systemBackgroundColor;
      child.tabBarItem = UITabBarItem.alloc().initWithTitleImageSelectedImage(
        item.title,
        UIImage.systemImageNamed(item.symbol),
        UIImage.systemImageNamed(item.selectedSymbol),
      );
      child.tabBarItem.tag = index;
      return child;
    });

    controller.viewControllers = NSArray.arrayWithArray(viewControllers);
    ctx.delegate(controller, UITabBarControllerDelegate, {
      tabBarControllerDidSelectViewController(tabBarController) {
        ctx.emit("onSelectedIndexChange", tabBarController.selectedIndex);
      },
    });
    return controller;
  },
  update(controller, props) {
    controller.selectedIndex = props.selectedIndex;
  },
});

<NativeTabs
  selectedIndex={selectedIndex}
  onSelectedIndexChange={setSelectedIndex}
  style={{ flex: 1 }}
/>;
```

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

await NativeScript.runOnUI(() => {
  "worklet";
  const presenter = topVisibleViewController();
  if (!presenter || presenter.presentedViewController) {
    return;
  }
  presenter.presentViewControllerAnimatedCompletion(controller, true, null);
});
```

### Availability and heavy UIKit classes

Use availability helpers before touching optional frameworks. Simulator and
device availability can differ for frameworks such as VisionKit, QuickLook, and
PassKit.

```ts
if (
  NativeScript.loadFramework("VisionKit") &&
  NativeScript.isClassAvailable("VNDocumentCameraViewController")
) {
  const CameraController = NativeScript.getClass<
    typeof VNDocumentCameraViewController
  >("VNDocumentCameraViewController");
}
```

`NativeScript.isFrameworkLoaded(nameOrPath)` checks an `NSBundle`;
`NativeScript.loadFramework(nameOrPath)` loads a system framework by name or a
specific `.framework` path; `NativeScript.getClass(name)` and
`NativeScript.getProtocol(name)` return dynamically available native references.

Class globals are lazy. Large UIKit classes such as `UITabBarController` can
have a wide inherited surface, so avoid forcing member enumeration with broad
reflection in hot paths. Constructing and direct property/method access stay
lazy; `Object.keys`, prototype introspection, and generated member lists are the
expensive path.

Objective-C exceptions thrown while dispatching through the bridge are converted
to JS errors where Objective-C can catch them. Process-level failures such as
`abort()`, fatal assertions, memory corruption, and some framework precondition
violations are not catchable; use availability checks and presentation guards
instead of relying on exceptions as control flow.

More example definitions ship in the package's `examples/` folder, for example
`@nativescript/react-native/examples/UIKitSwitch`.

The package includes generated NativeScript metadata, the libffi xcframework,
and generated iOS SDK TypeScript declarations.

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
  signature types those classes need. A class your code only reaches
  dynamically (a name built at runtime, for example) can therefore resolve in
  debug and be missing in release. In a release build, `whitelist.mdg` adds to
  that set rather than replacing it, so list such classes there.
  `-PnsFilterMetadata=false` turns the filter off and ships the whole
  classpath.
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
is no Worklets UI runtime on Android yet, so `runOnUI()` and the UIKit view
helpers are iOS only, and touching an `android.view.View` from JavaScript throws
`CalledFromWrongThreadException`. Every non-UI Android API is available.

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
