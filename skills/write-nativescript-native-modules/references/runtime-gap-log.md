# NativeScript Runtime Gap Log

## Contents

- Rule for adding runtime APIs
- Generic APIs introduced during navigation/tabs ports
- APIs rejected
- Tests required for runtime changes
- Open gap audit template

## Rule For Adding Runtime APIs

Add a NativeScript runtime/TurboModule API only when it enables a class of native module behavior. The API name and semantics must not mention the package being ported.

Good:

- callback thread policy
- runtime/worklet callback invoker
- delegate creation
- target/action bridge
- UIKit host refresh
- selector metadata coverage
- structured value serialization

Bad:

- React Navigation transition helper
- Expo tabs badge helper
- RNN screen presenter
- one-off modal repair API

## Generic APIs Introduced During Navigation/Tabs Ports

`runtimeInvoker(callback)`:

- Purpose: mark a native callback so it runs back on the owning UI/worklet runtime.
- Used for UIKit completion blocks whose metadata already includes the callable signature.
- Replaces any package-specific `afterUIKitTransition` helper.
- Must work through shared FFI callback policy, not a single engine.

`interop.Block(encoding, callback)` / `interop.FunctionReference(encoding, callback)`:

- Purpose: construct a native callback from an explicit Objective-C encoding when runtime metadata only says `@?` or `^?`.
- Used for UIKit transition-coordinator completion blocks such as `v@?@`.
- Keeps the API generic: the caller supplies the ObjC signature; the runtime only parses the signature and creates the FFI callback.
- Compose with `runtimeInvoker(callback)` when the native callback must re-enter the owning UI/worklet runtime.
- Must be implemented in shared FFI callback conversion for both NativeScript and the React Native vendored bridge copy.

`eventBridge(callback, 'runtime')` / callback thread policy:

- Purpose: allow native callbacks to specify caller, JS, or runtime/worklet delivery.
- Requires engine parity in shared FFI plus V8/JSC/QuickJS/Hermes metadata paths.

`refreshUIKitHostView(view)`:

- Purpose: ask a generic NativeScript UIKit host to lay out detached React children and report whether visible children exist.
- Used for first-paint and transition readiness.
- Must remain a host-view primitive, not a navigation API.

`onHostReady`:

- Purpose: notify TS when a generic NativeScript host has real rendered children.
- Used to avoid first-paint blank native containers.

Delegate and target/action helpers:

- Purpose: construct Objective-C protocol delegates and target/action objects from TS worklets.
- Must retain through context lifetime and release on dispose.

Hosted React child passthrough:

- Purpose: preserve UIKit hit-testing, touch delivery, and accessibility traversal when React children are reparented into a native `childrenView`.
- Used by UIKit containers whose native controller/view owns layout while React still renders route content.
- Must stay generic to `defineUIKitView` / `defineUIViewController`; do not add package-specific tap repair or navigation test hooks.
- Verification should include native automation snapshots and one-tap Pressable delivery, not only visual screenshots.

Existing `NativeClass` subclassing:

- Purpose: mirror upstream Objective-C/Swift subclasses when UIKit discovers behavior by selector override.
- Used for controller/window-trait behavior such as status bar style/hidden, status bar animation, supported orientations, home-indicator auto-hide, `viewWillAppear` lifecycle workarounds, `viewDidLayoutSubviews` layout refresh, first-responder restoration, and finish-transition cleanup.
- Runtime API needed: none. NativeScript already supports Objective-C subclassing through `NativeClass`; the library port should allocate the subclass instead of plain UIKit classes.
- Do not add package-specific TurboModule helpers for pull-based UIKit traits. If UIKit asks a controller for a selector upstream overrides, implement that selector on the TS subclass.
- Verification should include source/unit guards that subclass allocation is used, plus simulator flows for header/status/orientation/modal behavior when those props are exposed by the demo.

Transition progress event parity:

- Upstream native behavior: `RNSScreen` emits `onTransitionProgress` with `progress`, `closing`, and `goingForward`, while `Screen.tsx` wires those events into `Animated.Value`s and `TransitionProgressContext`.
- Current NativeScript requirement: if the port bypasses `Screen.tsx`, it must recreate the same JS wrapper wiring and emit the same native event payload from the TS UIKit transition lifecycle.
- Runtime API needed: none. This is wrapper parity plus ordinary event delivery through the existing NativeScript host context.
- Verification should include unit/source guards for the provider/event wiring and simulator navigation flows that use native-stack transitions.

Transition progress display-link sampling:

- Upstream native behavior: `RNSScreen` attaches a fake `UIView` to the UIKit transition coordinator container, animates its alpha, and samples `fakeView.layer.presentationLayer.opacity` from a `CADisplayLink` selector to emit intermediate transition progress.
- Current NativeScript requirement: expose the TS controller's `handleAnimation` selector with `NativeClass` `ObjCExposedMethods`, create `CADisplayLink.displayLinkWithTargetSelector(controller, 'handleAnimation')`, add it to `NSRunLoop.currentRunLoop` with `NSDefaultRunLoopMode`, and tear it down from the coordinator completion block. The same controller lifecycle owns final progress: `viewDidAppear` / `viewDidDisappear` emit progress `1` and reset the upstream-style `_isSwiping` / `_shouldNotify` equivalents.
- Runtime API needed: none. NativeScript already supports selector-driven native callbacks generically through `NativeClass` metadata; the port should use that instead of a package-specific TurboModule transition helper.
- Verification should include source/unit guards for display-link registration, presentation-layer sampling, transition-coordinator cleanup, and absence of duplicate stack-level progress emitters, plus simulator navigation flows that exercise animated push/pop/modal transitions.

Screen finish-transition lifecycle parity:

- Upstream native behavior: `notifyFinishTransitioning` restores the first responder captured during parent removal and refreshes window traits after the correct screen is visible.
- Current NativeScript requirement: implement `notifyFinishTransitioning`, `willMoveToParentViewController`, and the first-responder tree walk on the TS `NativeClass` subclass. Stack code should call the controller method, not duplicate a one-off helper.
- Runtime API needed: none. UIKit responder APIs and subclass selectors are already expressible from TypeScript worklets.
- Verification should include source/unit guards plus push/pop/gesture stress so first taps after navigation remain interactive.

Screen route lifecycle event parity:

- Upstream native behavior: `RNSScreen` sends `notifyWillAppear`, `notifyAppear`, `notifyWillDisappear`, `notifyDisappear`, and cancelled-swipe `notifyGestureCancel` from `viewWillAppear`, `viewDidAppear`, `viewWillDisappear`, and `viewDidDisappear`.
- Current NativeScript requirement: emit the matching host events from the TS `NativeClass` controller selectors, preserving `_isSwiping` / `_shouldNotify` semantics. Stack transition reconciliation may keep emitting stack-level transition and finish-transition events, but must not directly invoke route lifecycle props.
- Runtime API needed: none. This is NativeScript host event delivery from a controller subclass, using the existing screen context.
- Verification should include source guards that lifecycle events are emitted from the controller and absent from the stack transition callback, plus simulator push/pop/gesture stress.

Dismiss-count lifecycle parity:

- Upstream native behavior: `RNSScreen` computes `_dismissCount` in `viewWillDisappear` by comparing its React subview index with the target top screen's React subview index, falling back to `1` for interactive transitions, modals, forward navigation, and JS-driven back.
- Current NativeScript requirement: compute and store the dismiss count on the TS screen controller from active screen id order, which is the port's equivalent of RNSScreen's React subview order.
- Runtime API needed: none. The stack registry already tracks active screen order and controller-to-screen identity.
- Verification should include source/unit guards before moving dismissed/cancelled-dismissed event ownership into `viewDidDisappear`.

Dismiss event lifecycle parity:

- Upstream native behavior: `RNSScreen` emits `notifyDismissedWithCount` or `notifyDismissCancelledWithDismissCount` from `viewDidDisappear` when the controller has no parent and no presenter. For prevented native removal, upstream first calls the stack view's `updateContainer` to restore the JS navigation stack, then emits `notifyDismissCancelledWithDismissCount`. `UIAdaptivePresentationControllerDelegate` separately emits attempted-dismiss cancellation for prevented modals because the screen never disappears in that path.
- Current NativeScript requirement: keep `onDismissed` and actual-removal `onNativeDismissCancelled` on the TS screen controller's `viewDidDisappear`; use the mirrored `isRemovedFromParent` flag as the reliable detached-controller marker when direct parent state is not updated at selector time. For prevented native removal, invoke the package's reconcile entrypoint before emitting `onNativeDismissCancelled`; if this uses a registry/global worklet instead of `screenView.reactSuperview`, document that as an object-graph deviation. Modal routes should be presented as their `RNSScreen` controller directly, so UIKit's presentation delegate and screen lifecycle see the same object graph as upstream. Stack did-show paths should reconcile state only and must not synthesize duplicate screen dismissal events.
- Runtime API needed: none. This is ordinary controller lifecycle/event ownership through existing NativeScript host contexts.
- Verification should include source guards that prevented removal restores before emitting `onNativeDismissCancelled`, stack-change/did-show paths do not call `onDismissed`, no extra repeated stack-change event exists, the `UIAdaptivePresentationControllerDelegate` lives on the screen controller, and simulator native-back, header-back, JS pop, and modal dismiss flows all settle with one visible/interactive route.

Modal presentation interop parity:

- Upstream native behavior: modal routes use UIKit `presentViewController:animated:completion:`, `dismissViewControllerAnimatedCompletion`, and `UIAdaptivePresentationControllerDelegate`; UIKit owns swipe-down dismissal and cancellation.
- Current NativeScript requirement: require the same generic UIKit selectors from TS and present the `RNSScreen` controller directly, not a wrapper navigation controller unless upstream does. Do not substitute a manually attached child controller, custom slide animation, or custom pan-to-dismiss fallback when metadata is missing; that adds a second gesture owner and can race UIKit.
- Presented modal chain parity: keep an upstream-style `_presentedModals` equivalent in package state. Reconciliation must compute the last common presented modal before dismissing/presenting and reject reshuffles above that common root, because UIKit cannot safely reorder already-presented modal controllers.
- Modal header parity: when upstream renders an outer presented screen containing an inner stack/screen for the modal header, keep that UIKit object graph in TS. Flattening the header onto the presented route changes controller ownership and makes the later direct-present port harder. A generated inner screen id is acceptable only as a NativeScript registry key; document it as a package deviation, not a runtime gap.
- Backdrop tap parity: for `modal`, `pageSheet`, and `formSheet`, add the same outside-tap recognizer to `presentationController.containerView`. It should be simultaneous, non-cancelling, disabled unless `preventNativeDismiss` is true, ignored for touches inside `presentedView`, and emit the same dismiss-cancel event with count `1`.
- Runtime API needed: none if the Objective-C selector metadata is exposed. If an engine cannot call those methods, fix generic selector metadata/dispatch in the runtime and add runtime tests.
- Verification should include source guards against custom modal pan fallback plus simulator present/dismiss/swipe-dismiss flows.

UIKit controller host-view separation:

- Upstream native behavior: some native components are wrapper views that own a child UIKit controller, not the controller view itself. `RNSScreenStackView` is the React Native view, embeds `UINavigationController.view`, and overrides `hitTest:withEvent:` to forward taps in the navigation-bar frame into left/right header subviews that UIKit would otherwise clip out of the native hit area.
- Current NativeScript requirement: use a generic `defineUIViewController` `hostView(controller)` resolver when a pure-TS port needs the RN host view to be a separate native wrapper while still attaching the UIKit controller for lifecycle. Keep the controller's own `view` intact and add it as a child of the wrapper. The native host must honor an explicit native view handle instead of overwriting it with `controller.view` when the controller handle arrives.
- Runtime API needed: `UIViewControllerDefinition.hostView` plus `NativeScriptUIView` honoring explicit native host handles during controller attachment. This is generic UIKit hosting support; do not add package-specific header-button tap helpers or navigation-stack TurboModule APIs.
- Verification should include runtime source guards for `hostView`, package source guards for the wrapper `hitTest:withEvent:` port, and simulator header/menu/custom-header tap stress.

Native callback worklet entrypoint ordering:

- Upstream native behavior: Objective-C selector callbacks call already-compiled methods on the native class; helper symbol order is not observable.
- Current NativeScript requirement: native callback worklets and `NativeClass` delegate methods must not close over helper functions declared later in the TS module unless the callback class/function is created after that helper is registered. Move tiny helpers above the callback, inline them, or register a package-owned global worklet entrypoint after definition and resolve it from `globalThis` inside the native callback.
- Runtime API needed: none. This is a TS worklet authoring rule; adding a package-specific TurboModule helper would hide the ordering bug instead of fixing the port.
- Verification should include simulator native callback paths, especially UIKit modal `presentationControllerDidDismiss`, because source/type tests will not catch a UI-runtime `undefined is not a function` closure capture.

UIKit delegate proxy identity:

- Upstream native behavior: `UINavigationControllerDelegate` receives the exact `RNSScreen` controller pointer in `didShow`, so route identity comes from ObjC object identity.
- Current NativeScript requirement: first try native identity that survives proxy boundaries, such as controller restoration identifiers and hosted view identifiers. If UIKit still reports an unresolved proxy/placeholder during `didShow`, the port may update only route key/count bookkeeping from the known active/native stack length so React does not replay an already-completed push. It must not reapply `viewControllers` from inferred ids or emit route changes unless UIKit has actually shortened the native stack.
- Runtime API needed: ideally a generic associated-object or stable native identity primitive would remove this deviation. Until then, keep the workaround package-local, documented, and covered by simulator first-tap/gesture stress.
- Verification should include first tap after tab switch, first push after interactive back, duplicate push/pop guards, and a source guard against timed `setViewControllers` reapplication from inferred ids.

Transition completion watchdog:

- Upstream native behavior: stack transition completion is driven by `UINavigationControllerDelegate didShow`, and updates that arrive during a transition are replayed through `transitionCoordinator` completion.
- Current NativeScript requirement: install a generic `transitionCoordinator` completion for TS-started push/pop/replace transitions before using any timer. A timer is allowed only as a watchdog for the unresolved-proxy case where the delegate callback is missed; it must check the transition token and `stackTransitioning` flag so it cannot rewrite controllers after a real `didShow`.
- Runtime API needed: none if `transitionCoordinator` and explicit `interop.Block` callbacks are available. If coordinator completion cannot be installed on an engine, fix generic selector/block interop rather than adding a package-specific navigation helper.
- Verification should include source guards for coordinator-first scheduling and simulator stress for push/pop, interactive back, duplicate taps, and modal transitions.

Navigation-bar appearance during transitions:

- Upstream native behavior: `RNSScreenStackHeaderConfig` applies shared `UINavigationBarAppearance` changes through `transitionCoordinator animateAlongsideTransition:completion:` and restores the `fromVC` header config when an interactive transition is cancelled.
- Current NativeScript requirement: keep the initial synchronous appearance write, then register a generic coordinator completion through `interop.Block`. On completion, resolve the current top controller, or the transition context's `from` controller when cancelled, back to the package's header config registry before reapplying appearance.
- Runtime API needed: none beyond generic selector access, `transitionCoordinator`, transition-context `viewControllerForKey:`, and explicit `interop.Block` callbacks. If an engine cannot expose these, close that in the generic interop/runtime layer, not with a screens-specific helper.
- Verification should include source guards for the coordinator completion and simulator/header stress when header appearance, custom headers, or interactive cancellation are changed.

Presentation delegate protocol conformance:

- Upstream native behavior: `RNSScreenView` conforms to both `UIAdaptivePresentationControllerDelegate` and `UISheetPresentationControllerDelegate`; UIKit dispatches modal prevention/dismissal callbacks through the former and sheet detent changes through the latter.
- Current NativeScript requirement: register the same protocol list on the NativeScript controller subclass, not only the selector metadata. The TS method and exposed selector for `sheetPresentationControllerDidChangeSelectedDetentIdentifier:` must be paired with `UISheetPresentationControllerDelegate` in both NativeClass and `UIViewController.extend` creation paths.
- Runtime API needed: none if generic `nativeProtocol(...)` lookup and subclass protocol metadata are available.
- Verification should include a source guard for every upstream delegate protocol the subclass implements.

Prevented native pop cancellation:

- Upstream native behavior: when a native/header back pop crosses a screen with `preventNativeDismiss`, `RNSScreenStackView` returns an animator, creates an `RNSPercentDrivenInteractiveTransition` from `navigationController:interactionControllerForAnimationController:`, cancels it after one main-queue tick, calls `updateContainer`, and emits `notifyDismissCancelledWithDismissCount` from the original `fromView`.
- Current NativeScript requirement: preserve the same animator plus percent-driven interaction-controller cancellation path. If `transitionCoordinator viewForKey:` is not reliable across NativeScript delegate proxy boundaries, cache the stable `fromViewController`/`toViewController` pair from the immediately preceding animation delegate callback and consume it in the interaction-controller delegate. Guard the scheduled cancel against stale cached pairs.
- Runtime API needed: none. `ctx.delegate`, `UIPercentDrivenInteractiveTransition`, `setTimeout`/main-queue scheduling, and package-local stack reconciliation are enough; a transition-coordinator-specific TurboModule helper would be the wrong abstraction.
- Verification should include source guards for the cached pair, scheduled `cancelInteractiveTransition`, reconcile-before-cancel-event order, and simulator native/header-back prevention followed by one-tap push/pop recovery.

Post-transition hosted layout settle:

- Upstream native behavior: `RNSScreenStackView` queues `updateContainer` with `dispatch_async` after React child layout/mount mutations so controller updates see laid-out native children.
- Current NativeScript requirement: after UIKit finishes a controller transition, hold React-driven reconciles for one short host-layout settle window while NativeScript/RN hosted children publish final frames through `onHostReady` and `refreshUIKitHostView`. This timer may only defer and replay reconciliation; it must not rewrite UIKit controller arrays directly.
- Runtime API needed: none. This is bridging NativeScript host-view readiness to the upstream “layout is enqueued on the UI queue” behavior.
- Verification should include source guards that the settle path does not call `setViewControllers`, plus simulator stress for first tap after transition and lower-half modal touches.

Transition interaction parity:

- Upstream native behavior: `react-native-screens` gates hosted screen interactions during iOS 26 stack transitions because incoming screens can become visible before UIKit has completed the transition.
- Current NativeScript requirement: implement the gate inside the TS port using UIKit lifecycle/transition state, gating only the native transition container. Do not recursively set `userInteractionEnabled` on hosted React children; that overrides RN `pointerEvents` and can enable NativeScript detached-child touch sentinels that must remain disabled.
- Runtime API needed: none. This is a mechanical porting responsibility, not a TurboModule helper.
- Verification should include transition-overlap taps, double-tap duplicate-route checks, and post-transition one-tap navigation.

Tab selection hosted-content readiness:

- Upstream native behavior: `UITabBarController` owns the selected controller's view hierarchy, and tab selection, child appearance, layout, and hit-test readiness settle in UIKit's native run loop.
- Current NativeScript requirement: after actual native tab selection callbacks, synchronously reconcile the selected controller view and repeat only that selected-view/host refresh across a few short UI ticks with a token guard. This closes the window where React Navigation sees the tab as selected and exposes route labels before the hosted subtree accepts the first tap. Ordinary host commits should stay synchronous so first paint is not blanked by broad repeated commits.
- Runtime API needed: none. Use existing UIKit view layout plus `NativeScriptRuntime.refreshUIKitHostView`; do not introduce JS debounces or tab-specific TurboModule helpers.
- Verification should include first push immediately after tab switch at small delays such as `0,25,50,75,100,150,200,300ms`, plus a clean app relaunch to ensure first paint still mounts tab content.

Custom stack animator parity:

- Upstream native behavior: `RNSScreenStackAnimator` uses `UIViewPropertyAnimator`, and `RNSPercentDrivenInteractiveTransition` drives the animator's `fractionComplete` during interactive gestures.
- Current NativeScript requirement: mirror the upstream object model with named NativeScript Objective-C subclasses: an `NSObject` subclass conforming to `UIViewControllerAnimatedTransitioning` for the animator, and a `UIPercentDrivenInteractiveTransition` subclass for the gesture controller. Store per-transition state on the native proxy just as upstream stores it on ObjC ivars/properties.
- Also require `UIViewPropertyAnimator` metadata in the runtime and allocate/use the UIKit animator from TS. Do not add a fallback `UIView.animate` path for custom stack transitions; it may visually complete but cannot be scrubbed interactively, so it is not equivalent native-module behavior.
- Runtime API needed: none if protocol lookup, Objective-C subclassing, and `UIViewPropertyAnimator` metadata are present. If an engine/runtime cannot create the protocol-conforming subclass or see class/initializer metadata, fix the generic Objective-C metadata/interop layer and add runtime tests there.
- Verification should include source guards that the named classes are allocated, no non-interactive fallback exists, plus simulator custom-animation/gesture flows.

Hosted interaction ownership:

- Upstream native behavior: native containers may temporarily gate interaction during transitions, but React Native owns hit-testing state inside hosted route content.
- Current NativeScript requirement: layout/refresh native hosts without rewriting child `userInteractionEnabled` values. A TS port may set the `UIViewController.view` or native container view, but must not walk into RN/Fabric subviews to "fix" touches.
- Runtime API needed: none. The existing `refreshUIKitHostView(view)` is the correct generic primitive for detached host refresh; it should not be paired with recursive interaction mutation.
- Verification should include one-tap Pressable delivery after push, pop, modal dismiss, tab switching, and any cancelled gesture recovery.

Interactive pop cancellation parity:

- Upstream native behavior: a cancelled interactive pop emits gesture-cancel semantics and does not count as a dismissed route.
- Current NativeScript requirement: compare UIKit's shown stack to React Navigation's active route IDs in `didShow`. Only treat a closing transition as completed when the native stack is actually shorter.
- Runtime API needed: none. The fix belongs in the TS port's UIKit delegate/state reconciliation.
- Avoid JS-side timed "repair" calls that imperatively reset `viewControllers` before React state has processed the native dismissal event; they can re-add a popped screen mid-gesture and poison completed-dismiss bookkeeping.
- Verification should include: cancel edge gesture, complete the next edge gesture, then push/pop again with one tap.

JS-owned transition state replay:

- Upstream native behavior: React Navigation may update JS state while UIKit is still animating a state-driven push/pop, and the native stack must converge after the transition completes.
- Current NativeScript requirement: tag JS-requested transitions separately from native gestures, and record pending reconcile keys when React state changes during a JS-owned transition.
- Runtime API needed: none. This is a library state-machine responsibility.
- Verification should include repeated one-tap push/pop loops plus double-tap push/pop loops; a state update that arrives during `stackTransitioning` must not be dropped.

Native content wrapper parity:

- Upstream native behavior: `RNSScreenContentWrapper` is a real native view that is mounted under `RNSScreen`, participates in React layout updates, lets `RNSScreen` observe its frame for sheet/content sizing, and lets `RNSScreen` coerce a descendant `UIScrollView` frame for `formSheet` layouts.
- Current NativeScript requirement: port wrapper components as `defineUIKitContainer` hosts when upstream has a native wrapper. Do not replace them with a plain React Native `View`; that loses native wrapper lifetime/layout semantics and can leave modal content clipped even when the presented UIKit controller has a correct full-width frame. Store the native wrapper view in package state so screen-layout and wrapper-layout callbacks can mirror upstream `coerceChildScrollViewComponentSizeToSize` for direct scroll children, header-plus-scroll children, and the iOS 26 safe-area wrapper path.
- Runtime API needed: none. `defineUIKitContainer`, `attachNativeView`, and `refreshUIKitHostView(view)` are the generic primitives. The library port owns wrapper sizing/autoresizing just like the upstream native component does.
- Debugging rule: if UIKit controller/view frames are correct but content is clipped or partially untouchable, inspect the hosted RN wrapper chain. A stale zero-origin wrapper can expose the intended `contentSize.width` while keeping an old `frame.size.width`; repair only those wrapper levels, never arbitrary leaf layout.
- Verification should include modal presentation after an interactive back gesture, lower-half modal taps, scroll-view content width, direct/header/safe-area form-sheet scroll coercion unit tests, and one-tap controls after the next settled route is revealed.

Native-owned unmount snapshot parity:

- Upstream native behavior: before a native-owned pop/dismiss removes a React-backed screen, `RNSScreen.setViewToSnapshot` replaces its live view with `snapshotViewAfterScreenUpdates:` so UIKit can finish animating a stable visual even if React unmounts the route content.
- Current NativeScript requirement: expose the same selector on the TS `RNSScreen` controller subclass and call it from the controller dispose path when UIKit is still retaining the controller for a native pop/dismiss. JS-owned transition retention may still keep an inactive React element until UIKit completes, because React owns that element lifetime in the TS port.
- Runtime API needed: none. `snapshotViewAfterScreenUpdates:` and `UIView` reparenting are ordinary UIKit selectors available through NativeScript interop; a NativeScript snapshot helper would be package-specific duplication.
- Verification should include source/unit guards for `setViewToSnapshot`, simulator native-back/header-back/modal dismissal, and post-transition one-tap recovery.

Form sheet detent event parity:

- Upstream native behavior: `RNSScreen` sets itself as the sheet presentation controller delegate and emits `onSheetDetentChanged` from `sheetPresentationControllerDidChangeSelectedDetentIdentifier:` after converting UIKit's selected detent identifier to the JS detent index.
- Current NativeScript requirement: keep the delegate selector on the TS screen controller subclass, map `medium`/`large`/custom numeric detent identifiers the same way upstream does, and emit `{ index, isStable: true }` through the existing screen context.
- Runtime API needed: none. This is ordinary UIKit delegate dispatch through NativeScript `NativeClass` selector exposure.
- Verification should include source guards for the delegate selector, identifier mapping, event emission, and absence of form-sheet-specific runtime helpers.

Tabs navigation-state event parity:

- Upstream native behavior: `RNSTabBarController` owns a tab navigation state with `selectedScreenKey` and monotonic `provenance`, rejects stale or repeated JS requests, emits prevented selection when `preventNativeSelection` blocks UIKit, and emits a dedicated More-tab event without mutating the selected route.
- Current NativeScript requirement: keep the same state machine in the TS `UITabBarController` host. Host updates must preserve native-owned provenance instead of overwriting it from props, and delegate/fallback gesture paths must route through the same selected/prevented/rejected/More event helpers. Because the TS port receives host and tab-screen records through separate React/native host mounts, the first-paint commit window may be bounded and token-coalesced, but it must not become a persistent restore interval or a delayed navigation-state repair path.
- Parity detail: initialized state is explicit. A stale numeric `provenance`
  field from an older registry shape must not be treated as a live
  `RNSTabsNavigationState`; upstream creates provenance `0` when
  `_navigationState == nil`.
- Runtime API needed: none. NativeScript already exposes UIKit delegates, gesture actions, and host event delivery; the package port owns the React Navigation state reconciliation.
- Verification should include unit tests for user selection, repeated selection, stale/repeated JS request rejection, prevented native selection, More-tab selection, first-paint labels/icons, and simulator one-tap tab/navigation stress.

Search-controller header handoff parity:

- Upstream native behavior: `RNSSearchBar` owns a `UISearchController`, applies search-bar props and `UISearchBarDelegate` events, while `RNSScreenStackHeaderConfig` installs that controller into `UINavigationItem.searchController` and mirrors placement/scrolling/toolbar-integration flags.
- Current NativeScript requirement: port `RNSSearchBar` as a TS NativeScript UIKit component that allocates `UISearchController`, registers the controller through the package header-subview registry, and lets the NativeScript stack header attach it to `UINavigationItem`.
- Runtime API needed: none. This uses `defineUIKitContainer`, `ctx.delegate`, existing host refs with `runOnUI`, and package-owned context/registry state.
- Verification should include owner tests for controller creation, prop application, delegate events, command wiring, and container tests for `UINavigationItem` search-controller placement including iOS 26 stacked toolbar-integration behavior.

Custom back-indicator image parity:

- Upstream native behavior: `RNSScreenStackHeaderConfig` scans `RNSScreenStackHeaderSubviewTypeBackButton`, reads the child `RCTImageComponentView.image`, and installs it as both `UINavigationBarAppearance.backIndicatorImage` and `transitionMaskImage`.
- Current NativeScript requirement: carry `ScreenStackHeaderBackButtonImage`'s resolved image source through the NativeScript header-subview registry and resolve it with the same generic local/RN image-loader path used for header bar-button images.
- Runtime API needed: none. This is package-owned data flow plus existing generic image loading; a NativeScript snapshot/rendered-pixel API is unnecessary for source-backed back indicators.
- Verification should include a unit test that registered back-image records set and clear the standard/compact/scroll-edge appearance back indicator images.

Source back-button item parity:

- Upstream native behavior: `RNSScreenStackHeaderConfig.configureBackItem` sets the previous `UINavigationItem.backButtonTitle` and `backButtonDisplayMode`, then leaves UIKit's default back item alone unless `disableBackButtonMenu`, `backTitleFontFamily`, or `backTitleFontSize` requires a custom `RNSBackBarButtonItem`. When `backTitleVisible=false`, it uses minimal display mode but keeps the title for the back menu.
- Current NativeScript requirement: implement `RNSBackBarButtonItem` as a TS `NativeClass` subclass of `UIBarButtonItem`, override `setMenu:` to honor `menuHidden`, and assign it only for the same customization cases as upstream. If the previous item lost its title after controller recreation, recover it from the source screen header config before setting `backButtonTitle`.
- Runtime API needed: none. `NativeClass` subclassing, UIKit item properties, and selector overrides are generic NativeScript interop capabilities.
- Verification should include unit/source guards for default UIKit ownership, minimal-title menu preservation, custom menu/font item assignment, recreated-title fallback, and explicit comments for any package-local stale-item cleanup deviation.

## APIs Rejected

`afterUIKitTransition(viewController, callback)`:

- Rejected because it hard-coded a transition-coordinator use case into the runtime.
- Correct replacement: call UIKit directly from TS and wrap the completion callback with `runtimeInvoker`.

Per-call `dispatch_sync` wrappers:

- Rejected because granular native calls should not cross threads one call at a time.
- Correct replacement: run cohesive native work inside a UI worklet.

RN JS runtime on UI thread:

- Rejected as unsafe.
- Correct replacement: worklets are mandatory for NativeScript native UI.

Demo-only navigation hooks:

- Rejected as a parity substitute because they do not prove touch delivery, native gestures, or library event ordering.
- Correct replacement: fix generic host hit-testing/accessibility/touch passthrough, then test the public UI.

## Tests Required For Runtime Changes

Each runtime primitive needs at least:

- Type-level or source-level test in `packages/react-native/test`.
- Shared FFI behavior test when callback or serialization semantics change.
- Engine parity check for V8, JSC, QuickJS, and Hermes paths when selector metadata or callback policy changes.
- Demo validation on iOS simulator when UIKit host behavior changes.

Useful test names from this port:

- `runtime-callback-policy.test.js`
- `callback-thread-policy.test.js`
- `uikit-gesture-action-api.test.js`
- `uikit-host-ready-api.test.js`
- `uikit-host-refresh-api.test.js`

For hosted child passthrough, add tests that assert the native host does not
hide or swallow accessibility/touch traversal for visible React children after
they are moved into `childrenView`.

## Open Gap Audit Template

When a port hits a missing capability, record:

```md
## Gap: <generic capability>

- Upstream native behavior:
- Current TS workaround:
- Why existing NativeScript APIs are insufficient:
- Proposed generic primitive:
- Engines/FFI files touched:
- Tests added:
- Demo behavior verified:
- Package-specific API avoided:
```

Only leave a workaround in the fork if the gap is understood and tracked. Prefer fixing NativeScript generically before polishing library-specific code.
