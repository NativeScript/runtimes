"use strict";

const { runAsyncMemoryTest } = require("./_harness");

function makeFrame(x, y, width, height) {
  return {
    origin: { x, y },
    size: { width, height },
  };
}

runAsyncMemoryTest("appkit-navigation-extreme", async (t) => {
  const cycles = 96;
  const warmupCycles = 32;
  const maxDepth = 12;
  const subviewsPerScreen = 24;

  const nativeWeakControllers = NSHashTable.weakObjectsHashTable();
  const nativeWeakViews = NSHashTable.weakObjectsHashTable();
  const jsWeakControllers = [];
  const jsWeakViews = [];

  let createdControllers = 0;
  let createdViews = 0;
  let pushes = 0;
  let pops = 0;
  let routeId = 0;

  function countAlive(weakRefs) {
    let alive = 0;
    for (const ref of weakRefs) {
      if (ref.deref()) {
        alive += 1;
      }
    }
    return alive;
  }

  function liveNativeCount(table) {
    let count = 0;
    for (const ignored of table) {
      void ignored;
      count += 1;
    }
    return count;
  }

  function createRouteController(depth, cycle) {
    const controller = NSViewController.new();
    const root = NSView.alloc().initWithFrame(makeFrame(0, 0, 960, 640));
    root.wantsLayer = true;
    root.layer.backgroundColor = NSColor.colorWithSRGBRedGreenBlueAlpha(
      (routeId % 2) === 0 ? 0.08 : 0.9,
      (depth % 2) === 0 ? 0.2 : 0.82,
      (cycle % 2) === 0 ? 0.15 : 0.78,
      1,
    ).CGColor;

    const header = NSTextField.alloc().initWithFrame(makeFrame(60, 540, 840, 72));
    header.stringValue = `EXTREME route ${routeId} depth ${depth} cycle ${cycle}`;
    header.bezeled = false;
    header.drawsBackground = false;
    header.editable = false;
    header.selectable = false;
    header.alignment = NSTextAlignment.Center;
    header.font = NSFont.boldSystemFontOfSize(34);
    header.textColor = NSColor.whiteColor;
    root.addSubview(header);

    for (let i = 0; i < subviewsPerScreen; i++) {
      const container = NSView.alloc().initWithFrame(
        makeFrame(8 + i * 7, 10 + i * 5, 260 + (i % 6) * 14, 34 + (i % 5) * 8),
      );
      container.wantsLayer = true;
      container.layer.backgroundColor = NSColor.colorWithSRGBRedGreenBlueAlpha(
        ((i % 17) + 1) / 18,
        ((depth + i) % 19) / 18,
        ((cycle + i) % 23) / 22,
        1,
      ).CGColor;

      const label = NSTextField.alloc().initWithFrame(makeFrame(2, 2, 220, 22));
      label.stringValue = `r:${routeId} d:${depth} i:${i}`;
      label.bezeled = false;
      label.drawsBackground = false;
      label.editable = false;
      label.selectable = false;
      container.addSubview(label);

      root.addSubview(container);

      nativeWeakViews.addObject(container);
      nativeWeakViews.addObject(label);
      jsWeakViews.push(new WeakRef(container));
      jsWeakViews.push(new WeakRef(label));
      createdViews += 2;
    }

    controller.view = root;
    controller.title = `extreme-${routeId++}`;

    nativeWeakControllers.addObject(controller);
    nativeWeakViews.addObject(root);
    jsWeakControllers.push(new WeakRef(controller));
    jsWeakViews.push(new WeakRef(root));
    createdControllers += 1;
    createdViews += 1;

    return controller;
  }

  let window = NSWindow.alloc().initWithContentRectStyleMaskBackingDefer(
    makeFrame(-100000, -100000, 960, 640),
    NSWindowStyleMask.Borderless,
    NSBackingStoreType.Buffered,
    false,
  );
  window.contentViewController = createRouteController(0, 0);

  const navStack = [window.contentViewController];

  const pushController = (controller) => {
    navStack.push(controller);
    window.contentViewController = controller;
    pushes += 1;
  };

  const popController = () => {
    if (navStack.length <= 1) {
      return;
    }
    navStack.pop();
    window.contentViewController = navStack[navStack.length - 1];
    pops += 1;
  };

  for (let cycle = 0; cycle < cycles; cycle++) {
    t.autoreleasepool(() => {
      for (let depth = 1; depth <= maxDepth; depth++) {
        pushController(createRouteController(depth, cycle));
      }

      for (let depth = maxDepth; depth >= 1; depth--) {
        popController();
      }

      for (let i = 0; i < maxDepth * 2; i++) {
        pushController(createRouteController((i % maxDepth) + 1, cycle));
        popController();
      }
    });
    CATransaction.flush();

    if ((cycle + 1) % 8 === 0) {
      await t.forceGC(3, 36 * 1024 * 1024, 6);
    } else {
      await t.sleep(20);
    }

    if (cycle + 1 === warmupCycles) {
      await t.forceGC(2, 24 * 1024 * 1024, 5);
      t.markRssBaseline();
    }
  }

  while (navStack.length > 1) {
    popController();
  }

  const preCloseNativeControllers = nativeWeakControllers.count;
  const preCloseNativeViews = nativeWeakViews.count;

  navStack.length = 0;
  t.autoreleasepool(() => {
    window.contentViewController = null;
    window.orderOut(null);
    window.close();
  });
  CATransaction.flush();
  window = null;

  let released = false;
  let postCloseNativeControllers = Number.MAX_SAFE_INTEGER;
  let postCloseNativeViews = Number.MAX_SAFE_INTEGER;
  let postCloseJsControllers = Number.MAX_SAFE_INTEGER;
  let postCloseJsViews = Number.MAX_SAFE_INTEGER;

  const releaseDeadline = t.now() + 40_000;
  while (t.now() < releaseDeadline) {
    await t.forceGC(2, 32 * 1024 * 1024, 6);
    await t.drainRunLoopUntilIdle(() => true, {
      timeoutMs: 200,
      tickMs: 8,
      settleTicks: 3,
    });
    CATransaction.flush();

    postCloseNativeControllers = liveNativeCount(nativeWeakControllers);
    postCloseNativeViews = liveNativeCount(nativeWeakViews);
    postCloseJsControllers = countAlive(jsWeakControllers);
    postCloseJsViews = countAlive(jsWeakViews);

    const controllerLiveRatio = createdControllers > 0
      ? postCloseNativeControllers / createdControllers
      : 0;
    const viewLiveRatio = createdViews > 0
      ? postCloseNativeViews / createdViews
      : 0;

    if (controllerLiveRatio <= 0.15 &&
        viewLiveRatio <= 0.15 &&
        postCloseNativeControllers <= preCloseNativeControllers + 12 &&
        postCloseNativeViews <= preCloseNativeViews + 120 &&
        postCloseJsControllers <= 12 &&
        postCloseJsViews <= 24) {
      released = true;
      break;
    }
  }

  t.assert(pushes === pops, `navigation imbalance pushes=${pushes} pops=${pops}`);
  t.assert(
    released,
    `cleanup incomplete createdControllers=${createdControllers} createdViews=${createdViews} preNativeControllers=${preCloseNativeControllers} preNativeViews=${preCloseNativeViews} postNativeControllers=${postCloseNativeControllers} postNativeViews=${postCloseNativeViews} postJsControllers=${postCloseJsControllers} postJsViews=${postCloseJsViews}`,
  );

  return {
    cycles,
    warmupCycles,
    maxDepth,
    subviewsPerScreen,
    createdControllers,
    createdViews,
    pushes,
    pops,
    preCloseNativeControllers,
    preCloseNativeViews,
    postCloseNativeControllers,
    postCloseNativeViews,
    postCloseJsControllers,
    postCloseJsViews,
  };
}, {
  timeoutMs: 90_000,
  activationPolicy: NSApplicationActivationPolicy.Prohibited,
});
