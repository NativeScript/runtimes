# NativeScript TypeScript Native Module Porting Patterns

## Contents

- Mechanical port contract
- UIKit object model
- Navigation stacks
- Tabs
- Header service objects
- Modals
- Gestures and delegates
- Event delivery
- Layout, safe areas, and first paint
- Verification checklist

## Mechanical Port Contract

Port what upstream does, not what the demo needs. Keep the library's JS surface unchanged and move the iOS implementation from Objective-C/Swift into TypeScript worklets.

Required shape:

- Consuming app depends on the fork and `@nativescript/react-native`.
- Consuming app adds no `.m`, `.mm`, `.swift`, podspec patches, or native modules for the fork.
- Forked package owns all UIKit behavior in TS.
- NativeScript runtime exposes only generic capabilities.
- Original and NativeScript demos stay side by side until behavior matches.

## UIKit Object Model

Use `NativeScriptRuntime.defineUIViewController` for native containers that upstream implements as controllers. Use `defineUIKitView` for native views without child controller lifecycle.

Inside worklets:

```ts
const UINavigationController = nativeValue('UINavigationController');
const controller = UINavigationController.alloc().init();
controller.viewControllers = createArray([placeholder]);
```

When upstream implements behavior through Objective-C subclass overrides,
mirror that object model with a TypeScript `NativeClass` subclass. UIKit
window-trait methods such as `childViewControllerForStatusBarStyle`,
`preferredStatusBarUpdateAnimation`, `supportedInterfaceOrientations`, and
`childViewControllerForHomeIndicatorAutoHidden` are not TurboModule calls; they
are selectors UIKit pulls from the controller hierarchy. Allocate the
NativeScript subclass in the same places upstream allocates its native subclass,
and keep traversal rules mechanical, including modal traversal only where
upstream includes modals.

Port lifecycle selectors the same way. If upstream recalculates layout from
`viewDidLayoutSubviews`, override that selector on the TS subclass, call
`super`, then perform the same UIKit measurement/event work. If a native
shadow-tree helper has no TS equivalent, leave a `NATIVESCRIPT_PORT_DEVIATION`
comment explaining the replacement ownership instead of hiding the gap.
If upstream stores hierarchy state on the controller, such as
`isRemovedFromParent`, keep that state on the TS subclass too and use it when
filtering stale native controllers.
When upstream calls a native view's `updateBounds` from a controller layout
selector, refresh the hosted NativeScript/RN view tree from that same selector
instead of waiting for a later React render.
When upstream captures first responder state in
`willMoveToParentViewController` and restores it in `notifyFinishTransitioning`,
mirror that on the TS subclass with a UIKit `subviews` walk and
`becomeFirstResponder`. Finish-transition ownership should stay on the screen
controller, not in an ad hoc stack helper.
If upstream calls lifecycle workarounds such as `hideHeaderIfNecessary` from
`viewWillAppear`, keep the same selector ordering: call `super`, run the
workaround, then refresh window traits or other pull-based UIKit state.
If upstream exposes a selector for a native target callback, such as
`CADisplayLink` calling `handleAnimation`, expose that selector on the TS
`NativeClass` with `ObjCExposedMethods` and use the native factory directly.
Do not add a TurboModule helper for callback target/selector dispatch.

When upstream receives a React Native service handle through Fabric state, keep
that dependency generic. For image props, upstream packages often receive
`RCTImageLoader` through component state and pass resolved image sources into
native helpers. A TS port should synchronously resolve SF Symbols, xcassets, and
local/file `Image.resolveAssetSource` values with UIKit, then call the generic
NativeScript/RN image-loader API for URI or packager-backed sources and mutate
the existing UIKit item on completion. Do not add package-specific image-loading
helpers.

Always configure:

- `edgesForExtendedLayout`
- `extendedLayoutIncludesOpaqueBars`
- `view.backgroundColor`
- `view.autoresizingMask`
- child `view.frame`
- host view refresh when React children are detached from the React tree

## Navigation Stacks

Match `react-native-screens`:

- Start with a placeholder controller if needed for UIKit header initialization.
- Skip animated stack updates while `navigationController.transitionCoordinator` exists.
- Skip animated updates until `navigationController.view.window != null`.
- For a single push, normalize the base stack with `setViewControllers(..., animated: false)`, then call `pushViewController(..., animated: true)`.
- For a pop, call the narrow UIKit pop API: `popViewControllerAnimated`, `popToRootViewControllerAnimated`, or `popToViewControllerAnimated`.
- Defer JS state reconciliation until UIKit did-show or transition completion.
- If React state changes while UIKit owns a JS-requested transition, record a pending reconcile and replay it after completion. Do not silently return from reconciliation and drop the update.
- Guard repeated taps with native transition state, not JS debounce alone.
- On iOS 26, port upstream transition interaction gates: incoming screens can become visible/interactable before UIKit has finished the transition. Gate the native container that owns the transition, then re-enable it from the central did-show/completion/cancel path. Do not recursively toggle hosted React child views; React Native owns `pointerEvents`, touch sentinels, and accessibility state below the host.
- Treat cancelled interactive pops as gesture cancels, not completed closes. A closing transition only dismissed a route if UIKit's final shown stack is shorter than React Navigation's active route stack.

Do not replace a push with a full non-animated `setViewControllers` unless upstream does for a non-top change or replacement.

## Tabs

Use `UITabBarController` from TS and create one controller per route. Preserve labels, SF Symbols, selected state, badges, minimize behavior, sidebar adaptivity, and background/material behavior.

Port the native navigation-state machine, not just `selectedIndex`:

- Keep the native-owned `{ selectedScreenKey, provenance }` state and advance provenance for user, implicit, and accepted JS updates the same way upstream does.
- Treat uninitialized navigation state as `nil`, not as a `{ provenance: 0 }`
  record. The first accepted/user/implicit selection creates provenance `0`;
  only later progressions increment. Do not infer initialized state from an old
  numeric `provenance` field.
- Reject stale JS updates when the upstream component supports `rejectStaleNavStateUpdates`; emit the rejected event instead of silently applying or dropping the request.
- Treat repeated programmatic selection as rejected after the initial render, but still allow user repeated tab taps to emit the repeated-selection event and trigger the upstream repeated-tab effect.
- Respect `preventNativeSelection` from both `shouldSelect` and fallback gesture paths, and emit `onTabSelectionPrevented` with the current native state.
- Do not mutate native state when UIKit selects the More controller; emit `onMoreTabSelected` with the current state, then preserve upstream More-controller behavior.

First paint requirements:

- Create tab items before first visible layout.
- Assign `viewControllers` before mounting the host view when possible.
- Refresh hosted React content once the native host has children.
- If the TS port receives the host and tab-screen records across separate
  React/native host mounts, use only a bounded, token-coalesced first-paint
  commit window. Do not install persistent restore intervals or use delayed
  commits to repair navigation state.
- Verify labels and symbols before tapping tabs; first-paint missing labels are a bug.

## Header Service Objects

Some upstream native components are not visible views in the final UIKit
hierarchy; they own UIKit service objects that another native container
installs later. For example, `react-native-screens` `RNSSearchBar` owns a
`UISearchController`, while `RNSScreenStackHeaderConfig` attaches
`searchBar.controller` to `UINavigationItem.searchController`.

Port this mechanically:

- Implement the service owner as a NativeScript UIKit component in the package
  fork. It should allocate the same UIKit object, keep the same default props,
  install the same delegate protocol, and expose the same imperative commands.
- Register the owned native object through the package's existing TS registry or
  context boundary. The runtime should not grow a `setSearchController` or
  header-specific helper.
- Have the consuming native container install the object exactly where upstream
  does. For search bars, set `navigationItem.searchController`,
  `hidesSearchBarWhenScrolling`, iOS 16 `preferredSearchBarPlacement`, and iOS
  26 `searchBarPlacementAllowsToolbarIntegration` with the same stacked
  placement workaround.
- Keep non-iOS paths on the original native component when the port is only
  targeting iOS.
- Test both halves: the owner creates/configures/delegates the UIKit object, and
  the header/container installs the registered object into the target UIKit
  API.

Header image subviews are similar. Upstream may read the rendered native image
view from a header subview and install that `UIImage` into another UIKit object,
such as `UINavigationBarAppearance.backIndicatorImage`. In a TS port, prefer
carrying the resolved `Image.source` through the package registry and resolving
it with the same generic image-loading helper used by bar button items. Avoid
adding a runtime snapshot API unless the upstream behavior truly depends on
rendered pixels rather than the original image source.

For `UINavigationItem` source back-button items, preserve UIKit ownership when
upstream does. `react-native-screens` sets `backButtonTitle` and
`backButtonDisplayMode` on the previous item, but only assigns a custom
`RNSBackBarButtonItem` when the menu is disabled or the back-title font is
customized. Do not always create a custom `backBarButtonItem`; that disables
UIKit's automatic shortening/hiding behavior. If UIKit recreated the previous
controller and the item lost its title, fall back to the source screen header
config just like upstream reads `prevScreen.screenView.findHeaderConfig.title`.
When a TS port intentionally clears only a port-owned stale custom item so a
prop update returns to UIKit defaults, mark that as a
`NATIVESCRIPT_PORT_DEVIATION` in code and explain why it is package-local.

## Modals

Use UIKit presentation APIs for modal routes:

- `presentViewControllerAnimatedCompletion`
- `dismissViewControllerAnimatedCompletion`
- `UIAdaptivePresentationControllerDelegate`
- `sheetPresentationController` when upstream configures sheets

A modal route is not a push. It must support swipe-down dismissal, presentation-controller cancellation/prevention, and correct dismissal events. For `react-native-screens`, present the `RNSScreen` controller directly and set its presentation controller delegate on that same screen controller, matching upstream ownership.

For modal headers, preserve the same component/controller nesting upstream uses.
In `react-native-screens`, a modal with a visible header is an outer presented
screen containing an inner stack/screen that owns the header and route content.
Do not collapse that into a single modal route controller whose
`UINavigationItem` owns the header. If the TS host registry requires an id for
the inner screen while upstream's inner native `Screen` has no JS `screenId`,
derive a stable package-owned key from the outer route id and leave a
`NATIVESCRIPT_PORT_DEVIATION` comment explaining that the extra id is only a
registry identity.

When dismissing a presented chain, preserve upstream's common-root logic: dismiss foreign/owned controllers carefully, wait for transition completion, then present the next chain.
Keep a package-owned presented modal list equivalent to `RNSScreenStackView`'s
`_presentedModals`. Compute the common root between the currently presented
modal ids and the next modal ids before issuing UIKit dismiss/present calls, and
reject reshuffles where an already-presented controller reappears above that
root. UIKit modal controllers cannot be safely reordered in place, and treating
the whole modal stack as one opaque wrapper hides this invariant.

Do not replace missing modal presentation selectors with a manually attached
child controller, custom slide animation, or custom pan-to-dismiss gesture. If
upstream uses UIKit presentation, the TS port should require generic
`presentViewControllerAnimatedCompletion` /
`dismissViewControllerAnimatedCompletion` interop and use
`UIAdaptivePresentationControllerDelegate` for native swipe dismissal. A custom
gesture fallback creates double-handling races and is not a mechanical port.

Do not wrap modal screens in a `UINavigationController` as a workaround when
upstream presents `RNSScreen` directly. The wrapper changes child controller
parentage, breaks upstream's `viewDidDisappear` removal condition, and moves
`UIAdaptivePresentationControllerDelegate` ownership away from the object that
actually owns screen events. Close the gap by implementing the missing direct
UIKit presentation/list reconciliation in TS, not by forwarding lifecycle
events from a wrapper.

Also port the modal backdrop tap recognizer where upstream installs one. In
`react-native-screens`, `RNSScreen` adds a `UITapGestureRecognizer` to the
presentation controller container for `modal`, `pageSheet`, and `formSheet`.
The recognizer has `cancelsTouchesInView = NO`, only receives touches when
`preventNativeDismiss` is true, ignores touches inside
`presentationController.presentedView`, recognizes simultaneously, and emits
`onNativeDismissCancelled({ dismissCount: 1 })` on a recognized outside tap.
Implement this with generic NativeScript gesture delegate/action primitives;
do not add a modal/backdrop TurboModule helper.

For `formSheet`, port upstream sheet configuration directly: resolve public JS detent props, create `UISheetPresentationControllerDetent` values from TS, use explicit `interop.Block` signatures for custom detent resolver blocks, and update `fitToContents` detents from the native content-wrapper frame callback. If upstream adds navigation-bar height errata to content height, mirror that in the TS port. Also port native content-wrapper scroll-view correction: when upstream finds a descendant `UIScrollView` inside `RNSScreenContentWrapper`, resize the direct scroll child to the sheet size and resize a second-child scroll view to `sheet height - header height` with the same y-origin correction. Do this from the package's UIKit traversal/state, not through a package-specific runtime helper.
When the upstream screen object is the `UISheetPresentationControllerDelegate`,
keep those delegate selectors on the TS controller subclass too. For
`react-native-screens`, `sheetPresentationControllerDidChangeSelectedDetentIdentifier`
must map UIKit's selected detent identifier back to the JS detent index and
emit `onSheetDetentChanged({ index, isStable: true })`.

## Gestures And Delegates

Use `ctx.delegate` for UIKit delegate protocols and retain through the NativeScript context. Use `ctx.gestureAction` for `UIGestureRecognizer` target/action. Use `ctx.actionTarget` when upstream sets a generic UIKit object's `target`/`action`, such as `UIBarButtonItem.target` and `UIBarButtonItem.action`; assign the returned `target` and selector string directly instead of wrapping the object in a different view.

Common protocols:

- `UINavigationControllerDelegate`
- `UIAdaptivePresentationControllerDelegate`
- `UISheetPresentationControllerDelegate`
- `UIGestureRecognizerDelegate`
- `UITabBarControllerDelegate`

When upstream declares protocol conformance, mirror the full protocol list in
the NativeScript subclass metadata as well as exposing the selector signatures.
Having a TS method named like a delegate callback is weaker than declaring that
the native class conforms to the same delegate protocols UIKit expects.

For iOS 26 stack parity, prefer native `interactiveContentPopGestureRecognizer` when available and fall back only for custom animations that upstream cannot express with the native gesture.

For UIKit tab selection, separate native selection state from hosted-content
touch readiness. A `UITabBarController` selection can be observed and bridged
to JS before the NativeScript/RN hosted subtree has refreshed its hit-test
tree. After actual native selection callbacks, synchronously reconcile the
selected controller view, then repeat the selected-view/frame/host refresh on a
few short UI ticks with a token guard. Do not run this broad schedule from
ordinary host commits, and do not debounce React Navigation presses in JS to
hide the race.

## Event Delivery

Native callback thread policy matters:

- UI worklet callbacks stay on the UI/runtime thread.
- React Navigation and React Native events that must reach JS use event bridges.
- Generic target/action callbacks use `ctx.actionTarget(NativeScriptRuntime.eventBridge(callback, 'js'))` so the native selector can fire on the UI thread while React callbacks are scheduled back onto JS.
- Native completion blocks that need to re-enter the owning worklet use `NativeScriptRuntime.runtimeInvoker(callback)` when metadata already describes the block signature.
- Native block/function-pointer parameters whose metadata is only `@?` need an explicit generic ObjC signature, for example `interop.Block('v@?@', NativeScriptRuntime.runtimeInvoker(complete))` for `void (^)(id context)`. For `UIAction` handlers that must call React JS, have the block invoke a retained `ctx.actionTarget` target rather than directly calling the React callback from the UI runtime.

Native callback worklet symbol order matters too. A selector callback or
top-level worklet used by UIKit should not call a helper declared later in the
module unless the callback is created only after that helper is registered.
Normal JS hoisting is not enough once the function is compiled for the UI
runtime. Move the helper earlier, inline tiny operations, or register a
package-owned global worklet entrypoint after the helper definition and resolve
it from `globalThis` inside the native callback. This is how a TS port avoids
`undefined is not a function` crashes in callbacks such as
`presentationControllerDidDismiss`.

UIKit delegate proxy identity can differ from ObjC pointer identity. Upstream
may compare the exact native controller object in callbacks like
`navigationController:didShowViewController:animated:`. In a TS port, tag
controllers with native identifiers that survive proxy boundaries first. If a
callback still reports an unresolved proxy/placeholder, only update the
package's route key/count bookkeeping from the known native stack length; do
not rebuild `viewControllers` from guessed ids or schedule timed native resets.
Route-change events may be inferred only when UIKit's native stack is actually
shorter after a closing transition.

Transition completion should be UIKit-first. For stack transitions started from
TS, register completion through `transitionCoordinator` using an explicit
`interop.Block` and `runtimeInvoker`. A timeout may exist only as a watchdog
for missed delegate callbacks, and it must verify the transition token and that
the stack is still transitioning before touching native state. Never allow a
watchdog to rewrite controllers after `didShow` has already completed.

Shared navigation-bar appearance is also transition-coordinator state. When
upstream updates a `UINavigationBar` inside
`animateAlongsideTransition:completion:`, the TS port should use the same
generic `transitionCoordinator` plus explicit `interop.Block` path. Resolve the
current/cancelled controller's header config at completion time if ObjC view
identity is not stable through NativeScript proxies. Do not add a
navigation-bar-specific TurboModule helper.

Prevented native pop cancellation must also follow UIKit's delegate sequence.
In `react-native-screens`, a prevented header/native-back pop returns an
animator, creates an `RNSPercentDrivenInteractiveTransition` in
`navigationController:interactionControllerForAnimationController:`, cancels it
on the next main-queue tick, then runs `updateContainer` and emits
`onNativeDismissCancelled` with the computed dismiss count. If NativeScript can
read the stable `from`/`to` controllers in the animation delegate more
reliably than through `transitionCoordinator viewForKey:`, cache that pair for
the next delegate callback and document the proxy-boundary reason. Do not add a
package-specific TurboModule transition-coordinator helper.

Post-transition host layout settles are different from navigation watchdogs.
Upstream queues stack updates after React child layout with `dispatch_async`.
When NativeScript owns the native controller and React owns hosted child
content, a short settle window can defer reconciles until `onHostReady` /
`refreshUIKitHostView` has published final frames. That settle path should only
gate/replay reconciliation and refresh hosted views; it should never call
`setViewControllers` directly.

Never use a library-specific completion helper. If a new native callback pattern is needed, implement a generic callback policy and test it across engines.

If a TS port bypasses an upstream JS wrapper that normally wires native events,
recreate that wrapper behavior in the port. For `react-native-screens`
native-stack, bypassing `Screen.tsx` means the port must create the same
`Animated.Value`s, install the same `Animated.event` for
`onTransitionProgress`, and provide the same `TransitionProgressContext`.
The native event payload stays mechanical: `{ progress, closing: 0 | 1,
goingForward: 0 | 1 }`.

For animated transition progress, mirror RNSScreen's UIKit sampling model:
`viewWillAppear` / `viewWillDisappear` emit progress `0`, compute
`goingForward` from UIKit's `isBeingPresented` / `isMovingToParentViewController`
and `isBeingDismissed` / `isMovingFromParentViewController`, then register a
transition-coordinator animation block. Add a fake `UIView` to the coordinator
container, drive its alpha to `1`, create a `CADisplayLink` targetting the TS
controller selector, and read `fakeView.layer.presentationLayer.opacity` on
each frame. This is ordinary NativeScript ObjC interop, not a runtime gap.
Keep transition progress owned by the screen lifecycle. `viewDidAppear` /
`viewDidDisappear` should emit final progress `1` and reset the upstream-style
swipe bookkeeping (`_isSwiping` / `_shouldNotify` equivalents). Do not also
emit synthetic progress from stack reconciliation helpers; those helpers may
still emit stack transition events for React state, but progress belongs to the
screen that UIKit is transitioning.
Keep route lifecycle events on the same controller selectors too:
`viewWillAppear` emits `onWillAppear`, `viewDidAppear` emits `onAppear` or
`onGestureCancel` for a cancelled interactive pop, `viewWillDisappear` emits
`onWillDisappear`, and `viewDidDisappear` emits `onDisappear`. Stack delegates
may still reconcile React state, retained children, native stack changes, and
finish-transition notifications, but they should not duplicate route lifecycle
callbacks that upstream sends from `RNSScreen`.
For custom stack transitions, port the upstream animator mechanics instead of
introducing a visual-only fallback. If upstream defines a reusable ObjC
animator class, define a reusable NativeScript ObjC subclass too; for
`react-native-screens`, that means an `NSObject` subclass conforming to
`UIViewControllerAnimatedTransitioning` and a
`UIPercentDrivenInteractiveTransition` subclass for interactive gestures. Keep
transition state on those native proxies, not in ad hoc closure-only delegates.
If upstream uses `UIViewPropertyAnimator` so an interactive transition can set
`fractionComplete`, the TS port should require that UIKit class/initializer
metadata and fail loudly when it is missing. A fallback `UIView.animate`
completion path hides a runtime gap and does not provide mechanical parity for
gestures.
For native dismiss counts, mirror upstream's `viewWillDisappear` calculation on
the screen controller. RNSScreen compares the screen's index in React subviews
with the target top screen's index; in a TS NativeScript port, use the stack's
active screen id order as the mechanical equivalent. Keep this state on the
controller so later `viewDidDisappear` dismissal events can use the same count
instead of recalculating from stack delegate fallbacks.
When UIKit owns a pop or dismiss and React unmounts the route before the native
animation finishes, mirror `RNSScreen.setViewToSnapshot`: call
`snapshotViewAfterScreenUpdates:` on the live controller view, replace the
controller's `view` with the snapshot in the same superview, and let UIKit
animate that stable view. Do this on the screen controller subclass. Do not add
a runtime snapshot helper; the required operations are standard UIKit
selectors. JS-owned closes may still retain an inactive React element until the
transition completes because React owns that element lifetime in the TS port.

Emit native dismissal events from `viewDidDisappear` using the same UIKit
removal condition as upstream: the screen controller has no parent and no
presenter, or the upstream-equivalent `isRemovedFromParent` flag says the
controller has been detached. If `preventNativeDismiss` is set, emit
`onNativeDismissCancelled`; otherwise emit `onDismissed`. For an actual
prevented native removal, first restore the stack through the package's
upstream-equivalent `updateContainer`/reconcile entrypoint, then emit the
cancellation event. In NativeScript this may be a registry/global worklet
entrypoint rather than `screenView.reactSuperview`, but preserve the ordering
and leave a `NATIVESCRIPT_PORT_DEVIATION` comment explaining the object-graph
difference. Do not synthesize `onDismissed` from stack-change reconciliation or
wrapper modal cleanup helpers. The stack delegate still reconciles native/React
state and presentation-controller delegates still emit attempted-but-prevented
modal cancellation, because `viewDidDisappear` does not run when UIKit refuses
that dismissal.

Avoid adding a separate repeated "stack changed" event to repair JS state.
Native-driven dismissals should be represented by the same transition/lifecycle
events that upstream screens emit; if JS needs to retain or drop route content,
derive that from the transition end/dismissal event, not from several delayed
copies of a native stack snapshot.

## Layout, Safe Areas, And First Paint

Rules:

- Let scroll views go under translucent native headers/tab bars when upstream does.
- Use native content inset adjustment rather than hard-coded padding.
- Avoid capped-height host views: fixed-format native containers need stable fill sizing and autoresizing masks.
- If upstream has a native wrapper component, port that wrapper as a NativeScript UIKit container instead of substituting a plain RN `View`; otherwise route content can keep stale hosted-wrapper frames after native transitions even when the outer UIKit controller is correctly sized.
- If upstream has a native wrapper view around a UIKit controller, keep the wrapper view and controller view separate. Use `defineUIViewController({ hostView(controller) { ... } })` when the React Native host view must be the wrapper but the attached child controller must keep its own `controller.view`. For `react-native-screens`, `RNSScreenStackView` wraps `UINavigationController.view` and overrides `hitTest:withEvent:` so oversized left/right header subviews can still receive touches; the TS port should express that as a NativeScript `UIView` subclass wrapper, not by replacing `UINavigationController.view` or adding per-button tap repairs.
- Refresh detached host children after native stack layout.
- Wait for `onHostReady` only when the upstream behavior truly requires rendered content before transition.
- If upstream mutates a descendant UIKit control found from the native view tree, port the traversal too. For example, `react-native-screens` iOS 26 `scrollEdgeEffects` finds the first descendant `UIScrollView` and configures its `topEdgeEffect`, `bottomEdgeEffect`, `leftEdgeEffect`, and `rightEdgeEffect`; the TS port should do that with UIKit interop rather than dropping the prop or adding a TurboModule helper.
- If upstream creates UIKit menu trees, port the same `UIMenu`/`UIAction` object graph in TS. Retain `interop.Block` handlers and native actions through the UIKit context, and keep iOS-specific properties such as bar-button badges/shared backgrounds on `UIBarButtonItem` itself instead of substituting custom RN or UIKit wrapper views.

## Verification Checklist

Run both original and NativeScript demos:

- First paint: no blank tab bar, no missing labels/icons, no half-height scroll views.
- Navigation: one tap pushes, push animates, pop animates, back button is minimal, edge/back gesture works.
- Gesture recovery: the revealed screen remains interactive after an interactive pop.
- Gesture cancellation: a short edge drag leaves the detail route active, and the next full edge gesture still pops exactly once.
- Modal: presented as modal, no back button unless upstream shows one, swipe-down dismisses, prevention events work.
- Insets: content can scroll under translucent header and tab bar.
- Repeated taps: no duplicate pushes, no ignored first taps.
- Transition overlap: an intentional tap during push/pop/modal animation must not leave the next settled tap ignored or create duplicate routes.
- Logs: no TypeError, ReferenceError, NativeScript callback thread errors, redboxes, or UIKit warnings relevant to the module.
- Tests: package unit/type tests plus simulator flows on the required runtime/device.
