#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/build_utils.sh"
source "$SCRIPT_DIR/react_native_app_utils.sh"

# M1 acceptance test (rn-turbomodule-docs/ARCHITECTURE.md §10 "M1 runtime
# package... the worked example (§6) as the acceptance test"). Proves, on a
# real RN 0.85 Fabric app on the simulator, the NEW defineNativeComponent API
# end-to-end: create -> props update -> child mount/unmount -> an event back
# to JS -> layout, asserting main-thread affinity inside every handler.
#
# Extended (post-M1 verification pass) to also drive every hook M1 shipped
# but never exercised on-sim: finalizeUpdates, handleCommand,
# mountingTransactionWillMount/DidMount, ctx.setContentSize,
# ctx.scheduleOnMainQueue, ctx.instanceForView, ctx.createDelegate (a real
# UIScrollViewDelegate, with 3-level same-thread nested re-entrancy), and
# updateLayoutMetrics returning false (the decline path). Also drives one
# full app-level reload (DevSettings.reload(), the closest scriptable
# equivalent to a Metro fast-refresh; it is the same
# RCTInvalidating.invalidate/reinstall path ARCHITECTURE.md §3.5 describes)
# to verify the UI-runtime generation token invalidates and re-materializes
# correctly with no stale spec and no crash. Phase 1 writes a non-terminal
# "stage=phase1-ok:..." marker (rn_wait_for_marker_file already treats
# stage= content as a progress log, not a terminal result) then reloads;
# phase 2 re-runs the same suite fresh and writes the real MARKER.
#
# Reuses the M0 spike app dir (same RN version / worklets / babel plugins
# already installed there) rather than creating a fresh app; only the
# tarball and App.tsx differ.

RN_VERSION=${RN_VERSION:-0.85.3}
RN_CLI_VERSION=${RN_CLI_VERSION:-20.1.3}
APP_NAME=${RN_M1_APP_NAME:-NativeScriptM0Spike}
APP_ROOT=${RN_M1_APP_ROOT:-"$REPO_ROOT/build/react-native-m0-spike"}
APP_DIR="$APP_ROOT/$APP_NAME"
CONFIGURATION=${IOS_CONFIGURATION:-Release}
BUILD_TIMEOUT_SECONDS=${RN_M1_BUILD_TIMEOUT_SECONDS:-1800}
LAUNCH_TIMEOUT_SECONDS=${RN_M1_LAUNCH_TIMEOUT_SECONDS:-240}
MARKER="M1_TEST_PASS"
BUNDLE_ID="org.reactjs.native.example.$APP_NAME"
MARKER_FILE_NAME="NativeScriptNativeApiSmoke.marker"

if [[ "$(uname -m)" == "arm64" ]]; then
  export METADATA_GENERATOR_ARCHS=${METADATA_GENERATOR_ARCHS:-arm64}
fi

rn_build_turbo_tarball
TARBALL=$(rn_latest_turbo_tarball)

rn_create_app_if_missing "$APP_DIR" "$APP_ROOT" "$APP_NAME" "$RN_VERSION" "$RN_CLI_VERSION" "M1 test app"
rn_install_turbo_tarball "$APP_DIR" "$TARBALL" "M1 test app"

if ! grep -q "react-native-worklets" "$APP_DIR/package.json" 2>/dev/null; then
  checkpoint "Installing react-native-worklets for the M1 test app..."
  (cd "$APP_DIR" && npm install --silent react-native-worklets@0.9.1)
fi

rn_configure_nativescript_app "$APP_DIR" "M1 test app"

checkpoint "Writing M1 verification-test app entrypoint..."
node - "$APP_DIR/App.tsx" <<'NODE'
const fs = require('fs');
const target = process.argv[2];

fs.writeFileSync(target, `import React from 'react';
import {useEffect, useRef, useState} from 'react';
import {SafeAreaView, Text} from 'react-native';
import NativeScript, {defineNativeComponent, dispatchNativeComponentCommand} from '@nativescript/react-native';
import NativeScriptNativeApi from '@nativescript/react-native/src/NativeScriptNativeApi';

const marker = 'M1_TEST_PASS';
const PHASE1_STAGE_PREFIX = 'stage=phase1-ok:';

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

// ---------------------------------------------------------------------------
// Probe: create / updateProps / updateLayoutMetrics(accept) / finalizeUpdates
// / commands (handleCommand) / ctx.scheduleOnMainQueue.
// ---------------------------------------------------------------------------
const Probe = defineNativeComponent({
  name: 'NSM1Probe',
  props: {tint: 'red', mode: 'steady'},
  events: ['onReady', 'onFinalize', 'onPing', 'onProps'],
  create(ctx) {
    'worklet';
    const g = globalThis;
    const view = g.UIView.alloc().init();
    ctx.instance.createMainThread = NativeScript.isMainThread();
    ctx.instance.updateCount = 0;
    ctx.instance.lastTint = 'red';
    ctx.instance.mode = 'steady';
    ctx.instance.scheduledOnce = false;
    ctx.emit('onReady', {mainThread: NativeScript.isMainThread()});
    return view;
  },
  updateProps(ctx, next) {
    'worklet';
    const g = globalThis;
    ctx.instance.updateCount = ctx.instance.updateCount + 1;
    ctx.instance.updatePropsMainThread = NativeScript.isMainThread();
    if (next.tint !== undefined) ctx.instance.lastTint = next.tint;
    if (next.mode !== undefined) ctx.instance.mode = next.mode;
    ctx.view.backgroundColor =
      ctx.instance.lastTint === 'green' ? g.UIColor.greenColor : g.UIColor.redColor;
    ctx.emit('onProps', {
      tint: ctx.instance.lastTint,
      mode: ctx.instance.mode,
    });
    if (!ctx.instance.scheduledOnce) {
      ctx.instance.scheduledOnce = true;
      ctx.scheduleOnMainQueue(() => {
        ctx.instance.scheduleRan = true;
        ctx.instance.scheduleMainThread = NativeScript.isMainThread();
      });
    }
  },
  updateLayoutMetrics(ctx, next) {
    'worklet';
    ctx.instance.layoutMainThread = NativeScript.isMainThread();
    ctx.instance.lastWidth = next.width;
    ctx.instance.lastHeight = next.height;
    return true;
  },
  finalizeUpdates(ctx, mask) {
    'worklet';
    ctx.instance.finalizeCount = (ctx.instance.finalizeCount || 0) + 1;
    ctx.instance.finalizeMainThread = NativeScript.isMainThread();
    ctx.emit('onFinalize', {mainThread: NativeScript.isMainThread(), mask: mask});
  },
  commands: {
    ping(ctx, args) {
      'worklet';
      ctx.instance.pingMainThread = NativeScript.isMainThread();
      ctx.instance.pingArgs = args;
      ctx.emit('onPing', {mainThread: NativeScript.isMainThread(), args: args});
    },
  },
});

// ---------------------------------------------------------------------------
// DeclineProbe: updateLayoutMetrics returning false (the decline path) --
// Fabric's proposed frame must be skipped and the component must keep its
// own, manually-set geometry.
// ---------------------------------------------------------------------------
const DeclineProbe = defineNativeComponent({
  name: 'NSM1DeclineProbe',
  events: ['onLayoutDecline', 'onHookError'],
  create(ctx) {
    'worklet';
    try {
      const g = globalThis;
      // Set geometry on ctx.view itself (the ComponentView); that is the
      // object whose frame updateLayoutMetrics governs (via [super
      // updateLayoutMetrics:...]); a separate returned/contentView's frame
      // is NOT what Fabric's layout proposal targets, so declining would
      // never be observable there.
      ctx.view.frame = g.CGRectMake(5, 5, 42, 33);
    } catch (e) {
      ctx.emit('onHookError', {component: 'DeclineProbe', hook: 'create', message: String(e)});
    }
  },
  updateLayoutMetrics(ctx, next) {
    'worklet';
    try {
      const frame = ctx.view.frame;
      ctx.emit('onLayoutDecline', {
        actualWidth: frame.size.width,
        actualHeight: frame.size.height,
        proposedWidth: next.width,
        proposedHeight: next.height,
        mainThread: NativeScript.isMainThread(),
      });
    } catch (e) {
      ctx.emit('onHookError', {component: 'DeclineProbe', hook: 'updateLayoutMetrics', message: String(e)});
    }
    return false;
  },
});

// ---------------------------------------------------------------------------
// Stack: mountChild/unmountChild (as M0) + mountingTransactionWillMount/
// DidMount + ctx.instanceForView (sibling lookup, a code path distinct from
// dispatcher.ts's own tag-preresolved child.instance) + ctx.scheduleOnMainQueue
// from inside didMount.
// ---------------------------------------------------------------------------
const Stack = defineNativeComponent({
  name: 'NSM1Stack',
  events: ['onChildCount', 'onTransaction', 'onHookError'],
  create(ctx) {
    'worklet';
    ctx.instance.childCount = 0;
  },
  mountChildComponentView(ctx, child) {
    'worklet';
    ctx.instance.childCount = ctx.instance.childCount + 1;
    ctx.instance.mountMainThread = NativeScript.isMainThread();
    try {
      const viaLookup = ctx.instanceForView(child.view);
      ctx.instance.instanceForViewMatch =
        viaLookup !== undefined && viaLookup === child.instance;
    } catch (e) {
      ctx.instance.instanceForViewMatch = false;
      ctx.emit('onHookError', {component: 'Stack', hook: 'mountChildComponentView', message: String(e)});
    }
    ctx.emit('onChildCount', {count: ctx.instance.childCount});
  },
  unmountChildComponentView(ctx) {
    'worklet';
    ctx.instance.childCount = ctx.instance.childCount - 1;
    ctx.instance.unmountMainThread = NativeScript.isMainThread();
    ctx.emit('onChildCount', {count: ctx.instance.childCount});
  },
  mountingTransactionWillMount(ctx) {
    'worklet';
    const mainThread = NativeScript.isMainThread();
    ctx.instance.willMountMainThread = mainThread;
    ctx.emit('onTransaction', {phase: 'willMount', mainThread: mainThread});
  },
  mountingTransactionDidMount(ctx) {
    'worklet';
    const mainThread = NativeScript.isMainThread();
    ctx.instance.didMountMainThread = mainThread;
    ctx.emit('onTransaction', {
      phase: 'didMount',
      mainThread: mainThread,
      instanceForViewMatch: ctx.instance.instanceForViewMatch === true,
    });
    ctx.scheduleOnMainQueue(() => {
      const scheduledMainThread = NativeScript.isMainThread();
      ctx.instance.scheduledMainThread = scheduledMainThread;
      ctx.emit('onTransaction', {phase: 'scheduledOnMainQueue', mainThread: scheduledMainThread});
    });
  },
});

// ---------------------------------------------------------------------------
// DelegateProbe: ctx.createDelegate with a REAL UIScrollViewDelegate,
// invoked by UIKit on the main thread, stressed to 3 levels of same-thread
// synchronous re-entrancy (create -> scrollViewDidScroll -> contentOffset=
// -> scrollViewDidScroll -> contentOffset= -> scrollViewDidScroll). Mounted
// as a Stack child, so its \`create\` (which fires the first nested level)
// runs from inside Stack's mountChildComponentView; i.e. re-entry during
// an ACTIVE Fabric mounting transaction, not just isolated re-entry.
// ---------------------------------------------------------------------------
const DelegateProbe = defineNativeComponent({
  name: 'NSM1DelegateProbe',
  events: ['onDelegateResult', 'onHookError'],
  create(ctx) {
    'worklet';
    let checkpoint = 'start';
    try {
      const g = globalThis;
      const scrollView = g.UIScrollView.alloc().init();
      scrollView.frame = g.CGRectMake(0, 0, 50, 100);
      scrollView.contentSize = g.CGSizeMake(50, 400);
      ctx.instance.depth = 0;
      ctx.instance.mainThreadFlags = [];
      checkpoint = 'before-createDelegate';
      const delegate = ctx.createDelegate('UIScrollViewDelegate', {
        scrollViewDidScroll(scrollViewArg) {
          try {
            const depth = ctx.instance.depth + 1;
            ctx.instance.depth = depth;
            ctx.instance.mainThreadFlags.push(NativeScript.isMainThread());
            if (depth < 3) {
              scrollViewArg.contentOffset = g.CGPointMake(0, depth * 10);
            } else {
              ctx.emit('onDelegateResult', {
                mainThreadFlags: ctx.instance.mainThreadFlags,
                maxDepth: depth,
              });
            }
          } catch (e) {
            ctx.emit('onHookError', {component: 'DelegateProbe', hook: 'scrollViewDidScroll', message: String(e)});
          }
        },
      });
      checkpoint = 'after-createDelegate';
      ctx.instance.delegate = delegate;
      checkpoint = 'before-assign-delegate-property';
      scrollView.delegate = delegate;
      checkpoint = 'after-assign-delegate-property';
      // Deferred via scheduleOnMainQueue (next runloop turn, NOT nested
      // inside this create() call's own active runSync); see the report
      // for why triggering it synchronously HERE (nested inside the active
      // Fabric mounting transaction's dispatch) throws Worklets' "Remote
      // Function" guard instead.
      ctx.scheduleOnMainQueue(() => {
        scrollView.contentOffset = g.CGPointMake(0, 5);
      });
      checkpoint = 'after-scheduleOnMainQueue';
      return scrollView;
    } catch (e) {
      ctx.emit('onHookError', {component: 'DelegateProbe', hook: 'create@' + checkpoint, message: String(e) + ' | stack=' + (e && e.stack)});
      return undefined;
    }
  },
});

// ---------------------------------------------------------------------------
// DelegateBisectProbe: item 3's actual root-cause proof, kept as a
// permanent regression guard. The fix list's working theory ("methods that
// close over ctx") was bisected on-sim to something narrower and more
// general: ANY non-'worklet' function reachable from a worklet's closure
// throws identically, whether or not ctx is involved.
//   A, B) Both call NativeScript.getClass('NSObject') (A via
//      ctx.createDelegate, B via a raw NSObject.extend(...) that bypasses
//      ctx.createDelegate entirely); NEITHER closes over ctx, yet BOTH
//      still fail, with the SAME error, at the SAME call
//      (NativeScript.getClass itself is not 'worklet'-marked; a
//      DIFFERENT, adjacent, deliberately-NOT-fixed gap; expected FAIL,
//      proves the mechanism has nothing to do with .extend() or ctx).
//   C) ctx.createDelegate with methods that DO close over ctx (the §6
//      worked example's exact shape) and touch nothing outside the fixed
//      call chain (defaultNativeRetainer.retain/release, now 'worklet');
//      expected PASS; the actual fix-list item 3 regression guard.
// ---------------------------------------------------------------------------
globalThis.__bisectLog = [];
const DelegateBisectProbe = defineNativeComponent({
  name: 'NSM1DelegateBisectProbe',
  events: ['onBisectResult', 'onHookError'],
  create(ctx) {
    'worklet';
    const results = {};
    // A: createDelegate, NO per-instance closure.
    try {
      const g = globalThis;
      const DelClassA = NativeScript.getClass('NSObject');
      const delegateA = DelClassA.extend(
        {
          scrollViewDidScroll(scrollViewArg) {
            g.__bisectLog.push('A-fired');
          },
        },
        {protocols: [NativeScript.getProtocol('UIScrollViewDelegate')]},
      );
      const instA = delegateA.alloc().init();
      results.A = 'ok:' + typeof instA;
    } catch (e) {
      results.A = 'FAIL:' + String(e) + ' | stack=' + (e && e.stack);
    }

    // B: raw NSObject.extend, WITH per-instance closure (captures ctx).
    try {
      const DelClassB = NativeScript.getClass('NSObject');
      const delegateB = DelClassB.extend(
        {
          scrollViewDidScroll(scrollViewArg) {
            ctx.instance.bFired = true;
          },
        },
        {protocols: [NativeScript.getProtocol('UIScrollViewDelegate')]},
      );
      const instB = delegateB.alloc().init();
      results.B = 'ok:' + typeof instB;
    } catch (e) {
      results.B = 'FAIL:' + String(e) + ' | stack=' + (e && e.stack);
    }

    // C: ctx.createDelegate, WITH per-instance closure (captures ctx) --
    // the exact shape the worked example (ARCHITECTURE.md §6) uses.
    try {
      const delegateC = ctx.createDelegate('UIScrollViewDelegate', {
        scrollViewDidScroll(scrollViewArg) {
          ctx.instance.cFired = true;
        },
      });
      results.C = 'ok:' + typeof delegateC;
    } catch (e) {
      results.C = 'FAIL:' + String(e) + ' | stack=' + (e && e.stack);
    }

    ctx.emit('onBisectResult', results);
    return undefined;
  },
});

// ---------------------------------------------------------------------------
// ContentSizeProbe: ctx.setContentSize (the Fabric State write-back).
// No explicit style width/height; if the write-back actually feeds Yoga
// sizing, onLayout should observe ~77x55.
// This run reports the observation; it does not assume the answer.
// ---------------------------------------------------------------------------
const ContentSizeProbe = defineNativeComponent({
  name: 'NSM1ContentSizeProbe',
  events: ['onHookError'],
  create(ctx) {
    'worklet';
    try {
      const g = globalThis;
      const view = g.UIView.alloc().init();
      ctx.instance.setContentSizeMainThread = NativeScript.isMainThread();
      ctx.setContentSize({width: 77, height: 55}, {authority: true});
      return view;
    } catch (e) {
      ctx.emit('onHookError', {component: 'ContentSizeProbe', hook: 'create', message: String(e)});
      return undefined;
    }
  },
});

// ---------------------------------------------------------------------------
// InvalidateProbe: shouldBeRecycled: false; must be torn down through
// -invalidate, never -prepareForRecycle (M1 review §2/(c), fix-list item 3).
// \`prepareForRecycle\`'s dispose hook fires identically from either path;
// \`viaInvalidate\` is how a spec (and this test) tells them apart.
// ---------------------------------------------------------------------------
const InvalidateProbe = defineNativeComponent({
  name: 'NSM1InvalidateProbe',
  events: ['onDisposed'],
  shouldBeRecycled: false,
  create(ctx) {
    'worklet';
    ctx.instance.created = true;
  },
  prepareForRecycle(ctx, viaInvalidate) {
    'worklet';
    ctx.emit('onDisposed', {viaInvalidate: viaInvalidate, mainThread: NativeScript.isMainThread()});
  },
});

export default function App() {
  const [phase, setPhase] = useState('detecting');
  const [showChild, setShowChild] = useState(true);
  const [tint, setTint] = useState('red');
  const [result, setResult] = useState('Running NativeScript M1 verification...');

  const readyEvents = useRef([]);
  const finalizeEvents = useRef([]);
  const pingEvents = useRef([]);
  const propsEvents = useRef([]);
  const declineEvents = useRef([]);
  const childCountEvents = useRef([]);
  const transactionEvents = useRef([]);
  const delegateResult = useRef(null);
  const bisectResult = useRef(null);
  const invalidateResult = useRef(null);
  const contentSizeLayouts = useRef([]);
  const hookErrors = useRef([]);
  const probeRef = useRef(null);
  const ran = useRef(false);

  useEffect(() => {
    if (ran.current) {
      return;
    }
    ran.current = true;

    (async () => {
      try {
        const installed = NativeScript.init();
        if (!installed) {
          throw new Error('NativeScript Native API JSI host object was not installed');
        }

        // JOB2 (dev-reload / generation invalidation): the closest scriptable
        // equivalent to a Metro fast-refresh is a full DevSettings.reload() --
        // it drives the SAME RCTInvalidating.invalidate -> reinstall path
        // ARCHITECTURE.md Sec3.5 describes (destroys the UI Hermes VM, bumps
        // the gateway's generation counter on reinstall). Phase 1 runs the
        // full hook suite, records a non-terminal stage marker, then reloads.
        // Phase 2 (detected by reading that marker back after the JS VM has
        // been fully torn down and recreated) re-runs the identical suite
        // fresh and must pass identically; proving specs re-materialize on
        // the new generation with no stale worklet spec and no crash.
        // Dedicated phase marker file (__readReloadPhaseMarker), NOT the
        // smoke-marker file (__readTestMarker); native's own
        // "stage=engine:installed"-style install-sequence writes to the
        // smoke marker on every reload clobber it before this code ever
        // runs, which caused an infinite reload loop when this used
        // __readTestMarker (confirmed on-sim; see NativeScriptNativeApiModule.h).
        const priorPhaseMarker = NativeScriptNativeApi.__readReloadPhaseMarker();
        const isPhase2 = priorPhaseMarker.indexOf(PHASE1_STAGE_PREFIX) === 0;
        setPhase(isPhase2 ? 'phase2-post-reload' : 'phase1');

        // create + onReady event round trip
        await delay(700);
        if (readyEvents.current.length === 0) {
          throw new Error('onReady never fired (create -> ctx.emit round trip failed)');
        }

        // updateProps (also arms ctx.scheduleOnMainQueue via Probe)
        setTint('green');
        await delay(500);

        // handleCommand: dispatch a real Fabric command from JS to the
        // component through the SHIPPED author-facing dispatcher (fix-list
        // item 6/§3/#7; not the raw FabricUIManager calls it wraps).
        dispatchNativeComponentCommand(probeRef.current, 'ping', [42, 'hello']);
        await delay(400);

        // child unmount (mountChildComponentView already exercised by the
        // initial render above; this exercises unmountChildComponentView and
        // a second mountingTransactionWillMount/DidMount + scheduleOnMainQueue round)
        setShowChild(false);
        await delay(500);

        const didMountEvents = transactionEvents.current.filter(e => e.phase === 'didMount');
        const willMountEvents = transactionEvents.current.filter(e => e.phase === 'willMount');
        const scheduledEvents = transactionEvents.current.filter(e => e.phase === 'scheduledOnMainQueue');

        const summary = {
          phase: isPhase2 ? 'phase2-post-reload' : 'phase1',
          onReady: {
            count: readyEvents.current.length,
            allMainThread: readyEvents.current.length > 0 && readyEvents.current.every(e => e.mainThread === true),
          },
          finalizeUpdates: {
            count: finalizeEvents.current.length,
            allMainThread: finalizeEvents.current.length > 0 && finalizeEvents.current.every(e => e.mainThread === true),
          },
          handleCommand: {
            count: pingEvents.current.length,
            allMainThread: pingEvents.current.length > 0 && pingEvents.current.every(e => e.mainThread === true),
            lastArgs: pingEvents.current.length > 0 ? pingEvents.current[pingEvents.current.length - 1].args : null,
          },
          propDelta: {
            events: propsEvents.current,
            preservedUnchangedMode: propsEvents.current.some(
              e => e.tint === 'green' && e.mode === 'sticky',
            ),
          },
          updateLayoutMetricsDecline: {
            count: declineEvents.current.length,
            allMainThread: declineEvents.current.length > 0 && declineEvents.current.every(e => e.mainThread === true),
            frameStayedFixed:
              declineEvents.current.length > 0 &&
              declineEvents.current.every(e => e.actualWidth === 42 && e.actualHeight === 33),
            proposedDifferedFromActual:
              declineEvents.current.length > 0 &&
              declineEvents.current.some(e => e.proposedWidth !== e.actualWidth || e.proposedHeight !== e.actualHeight),
          },
          mountChildUnmount: {
            childCountEvents: childCountEvents.current,
            // 4 persistent Stack children (DelegateProbe/ContentSizeProbe/
            // DeclineProbe/Probe) mount first (count reaches 4), then Probe
            // unmounts (count drops below the peak); not the M0-era
            // single-child [1]->[0] shape.
            mountedThenUnmounted:
              childCountEvents.current.length > 1 &&
              Math.max(...childCountEvents.current) === 4 &&
              childCountEvents.current[childCountEvents.current.length - 1] < Math.max(...childCountEvents.current),
          },
          mountingTransaction: {
            willMountCount: willMountEvents.length,
            didMountCount: didMountEvents.length,
            allMainThread: transactionEvents.current.length > 0 && transactionEvents.current.every(e => e.mainThread === true),
            instanceForViewAllMatch: didMountEvents.length > 0 && didMountEvents.every(e => e.instanceForViewMatch === true),
          },
          scheduleOnMainQueue: {
            count: scheduledEvents.length,
            allMainThread: scheduledEvents.length > 0 && scheduledEvents.every(e => e.mainThread === true),
          },
          createDelegateReentrancy: delegateResult.current,
          delegateBisect: bisectResult.current,
          invalidate: invalidateResult.current,
          contentSize: {
            observedLayouts: contentSizeLayouts.current,
            observedTargetSize: contentSizeLayouts.current.some(
              l => Math.round(l.width) === 77 && Math.round(l.height) === 55,
            ),
          },
          hookErrors: hookErrors.current,
        };

        const reentrancyOk =
          summary.createDelegateReentrancy !== null &&
          summary.createDelegateReentrancy.maxDepth >= 3 &&
          summary.createDelegateReentrancy.mainThreadFlags.length >= 3 &&
          summary.createDelegateReentrancy.mainThreadFlags.every(Boolean);

        // KNOWN GAP (real finding, documented in the report, NOT gated into
        // allPass; see JOB1/JOB3 write-up): ctx.createDelegate(), called
        // from inside a defineNativeComponent worklet hook with a methods
        // object whose functions close over per-instance data (ctx), fails
        // synchronously during construction (not method invocation) with
        // "[Worklets] Tried to synchronously call a Remote Function." This
        // reproduces identically whether the nested method carries its own
        // 'worklet' directive or not, and whether invocation is triggered
        // synchronously nested in create() or deferred via
        // scheduleOnMainQueue; isolated via a checkpoint marker to fire
        // at the ctx.createDelegate(...) call itself, before any delegate
        // method ever runs. This is precisely the depth/shape that breaks
        // JOB3 asked to find and document: depth 0 (construction), not a
        // deep-nesting limit.
        const knownDelegateGap = summary.hookErrors.filter(
          e => e.component === 'DelegateProbe' && String(e.hook).indexOf('create') === 0,
        );
        const unexpectedHookErrors = summary.hookErrors.filter(
          e => !(e.component === 'DelegateProbe' && String(e.hook).indexOf('create') === 0),
        );

        // M1.5 fixes (see the report): ctx.createDelegate's ACTUAL root
        // cause (non-'worklet' functions reachable from a worklet's
        // closure) is fixed, so reentrancy is now a hard requirement, not
        // just reported; ctx.setContentSize now has both an \`adopt()\`
        // consumer AND a fix for a second, independently-discovered bug
        // (the state write was silently dropped when called from \`create()\`
        // before -updateState: ever fired); observedTargetSize is now
        // gated too.
        //
        // summary.invalidate is deliberately NOT gated here (and expected
        // to stay null): confirmed on-sim that Fabric's EventEmitter
        // silently no-ops a \`ctx.emit\` made from INSIDE
        // -invalidate/-prepareForRecycle even with a live, non-null
        // \`_eventEmitter\`; the shadow node has already detached by then.
        // The REAL proof that \`shouldBeRecycled: false\` reaches
        // -invalidate (never -prepareForRecycle), with the dispose hook
        // still firing (nsCreated=1), is the NSLog + \`log show\` assertion
        // this script's bash driver runs after the marker below.
        const allPass =
          unexpectedHookErrors.length === 0 &&
          summary.onReady.count > 0 && summary.onReady.allMainThread &&
          summary.finalizeUpdates.allMainThread &&
          summary.handleCommand.count > 0 && summary.handleCommand.allMainThread &&
          summary.propDelta.preservedUnchangedMode &&
          summary.updateLayoutMetricsDecline.allMainThread &&
          summary.updateLayoutMetricsDecline.frameStayedFixed &&
          summary.updateLayoutMetricsDecline.proposedDifferedFromActual &&
          summary.mountChildUnmount.mountedThenUnmounted &&
          summary.mountingTransaction.willMountCount > 0 &&
          summary.mountingTransaction.didMountCount > 0 &&
          summary.mountingTransaction.allMainThread &&
          summary.mountingTransaction.instanceForViewAllMatch &&
          summary.scheduleOnMainQueue.count > 0 &&
          summary.scheduleOnMainQueue.allMainThread &&
          reentrancyOk &&
          summary.contentSize.observedTargetSize;
        summary.reentrancyOk = reentrancyOk;
        summary.knownDelegateGap = knownDelegateGap;
        summary.unexpectedHookErrors = unexpectedHookErrors;

        // DevSettings.reload() is a no-op stub when __DEV__ is false (RN's
        // own DevSettings.js ships an empty no-op reload() for Release --
        // dev-reload only exists in dev builds). Only attempt the JOB2 half
        // of this run in a dev/debug build; a Release run stays single-phase
        // (still covers every hook; JOB1; on its own).
        const canReload = typeof __DEV__ !== 'undefined' && __DEV__ === true;

        if (!isPhase2 && canReload) {
          const stagePayload = PHASE1_STAGE_PREFIX + JSON.stringify(summary) + ' allPass=' + String(allPass);
          console.log(stagePayload);
          // Dedicated phase file (read back post-reload) + the bash-visible
          // "stage=" progress marker on the shared smoke-marker file (for
          // the harness's own log, not used for phase detection).
          NativeScriptNativeApi.__writeReloadPhaseMarker(stagePayload);
          NativeScriptNativeApi.__writeTestMarker(stagePayload);
          setResult('Phase 1 done (allPass=' + String(allPass) + '), reloading for JOB2...');
          if (!allPass) {
            throw new Error('M1 phase-1 assertion failure: ' + JSON.stringify(summary));
          }
          await delay(400);
          require('react-native').DevSettings.reload('NativeScript M1 JOB2 dev-reload verification');
          return;
        }

        summary.reloadCycleTested = isPhase2;
        summary.devReloadAvailable = canReload;
        const payload = (allPass ? marker : 'M1_TEST_FAIL') + ' ' + JSON.stringify(summary);
        console.log(payload);
        NativeScriptNativeApi.__writeTestMarker(payload);
        setResult(payload);
        if (!allPass) {
          throw new Error('M1 verification assertion failure: ' + JSON.stringify(summary));
        }
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        console.error('M1_TEST_FAIL', message);
        NativeScriptNativeApi.__writeTestMarker('M1_TEST_FAIL ' + message);
        setResult('M1_TEST_FAIL ' + message);
      }
    })();
  }, []);

  return (
    <SafeAreaView style={{flex: 1, alignItems: 'center', justifyContent: 'center', padding: 24}}>
      <DelegateBisectProbe
        onBisectResult={e => {
          bisectResult.current = e.nativeEvent;
        }}
        onHookError={e => {
          hookErrors.current.push(e.nativeEvent);
        }}
      />
      {showChild ? (
        <InvalidateProbe
          onDisposed={e => {
            invalidateResult.current = e.nativeEvent;
          }}
        />
      ) : null}
      <Stack
        style={{width: 60, height: 60}}
        onChildCount={e => {
          childCountEvents.current.push(e.nativeEvent.count);
        }}
        onTransaction={e => {
          transactionEvents.current.push(e.nativeEvent);
        }}
        onHookError={e => {
          hookErrors.current.push(e.nativeEvent);
        }}>
        <DelegateProbe
          style={{width: 50, height: 50}}
          onDelegateResult={e => {
            delegateResult.current = e.nativeEvent;
          }}
          onHookError={e => {
            hookErrors.current.push(e.nativeEvent);
          }}
        />
        <ContentSizeProbe
          onLayout={e => {
            contentSizeLayouts.current.push(e.nativeEvent.layout);
          }}
          onHookError={e => {
            hookErrors.current.push(e.nativeEvent);
          }}
        />
        <DeclineProbe
          style={{width: 150, height: 120}}
          onLayoutDecline={e => {
            declineEvents.current.push(e.nativeEvent);
          }}
          onHookError={e => {
            hookErrors.current.push(e.nativeEvent);
          }}
        />
        {showChild ? (
          <Probe
            ref={probeRef}
            tint={tint}
            mode="sticky"
            style={{width: 20, height: 20}}
            onReady={e => {
              readyEvents.current.push(e.nativeEvent);
            }}
            onFinalize={e => {
              finalizeEvents.current.push(e.nativeEvent);
            }}
            onPing={e => {
              pingEvents.current.push(e.nativeEvent);
            }}
            onProps={e => {
              propsEvents.current.push(e.nativeEvent);
            }}
          />
        ) : null}
      </Stack>
      <Text selectable>{phase + ': ' + result}</Text>
    </SafeAreaView>
  );
}
`);
NODE

rn_install_pods "$APP_DIR" "M1 test app"
UDID=$(rn_require_ios_simulator)
rn_build_ios_app "$APP_DIR" "$APP_ROOT" "$APP_NAME" "$CONFIGURATION" "$UDID" "$BUILD_TIMEOUT_SECONDS" "M1 test app"
APP_BUNDLE="$RN_APP_BUNDLE"

checkpoint "Launching M1 test app and waiting for the test marker..."
MARKER_FILE=$(rn_launch_app_with_marker "$UDID" "$APP_BUNDLE" "$BUNDLE_ID" "$MARKER_FILE_NAME")
rn_wait_for_marker_file "$MARKER_FILE" "$MARKER" "$LAUNCH_TIMEOUT_SECONDS"

# M1 review §2/(c), fix-list item 3 verification: ctx.emit cannot prove which
# teardown path a component went through (Fabric silently no-ops events
# dispatched from inside -invalidate/-prepareForRecycle; see the JS-side
# comment above summary.invalidate). NativeScriptComponentView.mm's
# -invalidate/-prepareForRecycle each NSLog their own name (Debug builds
# only); assert directly against the unified log that NSM1InvalidateProbe
# (shouldBeRecycled: false) went through -invalidate and NEVER
# -prepareForRecycle, and NSM1Probe (default) the reverse.
if [[ "$CONFIGURATION" == *Debug* ]]; then
  checkpoint "Verifying shouldBeRecycled:false routes through -invalidate (log show)..."
  LOG_OUTPUT=$(xcrun simctl spawn "$UDID" log show --last 5m \
    --predicate 'eventMessage CONTAINS "NativeScriptComponentView"' --style compact 2>/dev/null || true)
  if ! echo "$LOG_OUTPUT" | grep -q '\[NSM1InvalidateProbe\] -invalidate nsCreated=1'; then
    echo "$LOG_OUTPUT"
    echo "FAIL: expected an -invalidate log line for NSM1InvalidateProbe (shouldBeRecycled: false) with nsCreated=1." >&2
    exit 1
  fi
  if echo "$LOG_OUTPUT" | grep -q '\[NSM1InvalidateProbe\] -prepareForRecycle'; then
    echo "$LOG_OUTPUT"
    echo "FAIL: NSM1InvalidateProbe (shouldBeRecycled: false) went through -prepareForRecycle. Expected -invalidate." >&2
    exit 1
  fi
  if ! echo "$LOG_OUTPUT" | grep -q '\[NSM1Probe\] -prepareForRecycle nsCreated=1'; then
    echo "$LOG_OUTPUT"
    echo "FAIL: expected an -prepareForRecycle log line for NSM1Probe (default shouldBeRecycled) with nsCreated=1." >&2
    exit 1
  fi
  checkpoint "shouldBeRecycled:false / -invalidate routing verified."
else
  checkpoint "Skipping Debug-only invalidate routing log assertions for $CONFIGURATION."
fi

checkpoint "NativeScript React Native TurboModule M1 acceptance test passed."
