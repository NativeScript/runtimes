"use strict";

const { runAsyncMemoryTest } = require("./_harness");

function makeFrame(x, y, width, height) {
  return {
    origin: { x, y },
    size: { width, height },
  };
}

runAsyncMemoryTest("appkit-navigation-throughput", async (t) => {
  const cycles = 90;
  const maxDepth = 8;
  const subviewsPerScreen = 16;

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
    const root = NSView.alloc().initWithFrame(makeFrame(0, 0, 720, 480));
    root.wantsLayer = true;
    root.layer.backgroundColor = NSColor.colorWithSRGBRedGreenBlueAlpha(
      (routeId % 2) === 0 ? 0.12 : 0.82,
      (depth % 2) === 0 ? 0.22 : 0.72,
      (cycle % 2) === 0 ? 0.18 : 0.76,
      1,
    ).CGColor;

    const header = NSTextField.alloc().initWithFrame(makeFrame(40, 390, 640, 64));
    header.stringValue = `route ${routeId} depth ${depth} cycle ${cycle}`;
    header.bezeled = false;
    header.drawsBackground = false;
    header.editable = false;
    header.selectable = false;
    header.alignment = NSTextAlignment.Center;
    header.font = NSFont.boldSystemFontOfSize(32);
    header.textColor = NSColor.whiteColor;
    root.addSubview(header);

    for (let i = 0; i < subviewsPerScreen; i++) {
      const subview = NSView.alloc().initWithFrame(
        makeFrame(10 + i * 8, 12 + i * 6, 180 + (i % 5) * 10, 24 + (i % 4) * 6),
      );
      subview.wantsLayer = true;
      subview.layer.backgroundColor = NSColor.colorWithSRGBRedGreenBlueAlpha(
        ((i % 13) + 1) / 14,
        ((depth + i) % 17) / 16,
        ((cycle + i) % 11) / 10,
        1,
      ).CGColor;
      root.addSubview(subview);
      nativeWeakViews.addObject(subview);
      jsWeakViews.push(new WeakRef(subview));
      createdViews += 1;
    }

    controller.view = root;
    controller.title = `route-${routeId++}`;

    nativeWeakControllers.addObject(controller);
    nativeWeakViews.addObject(root);
    jsWeakControllers.push(new WeakRef(controller));
    jsWeakViews.push(new WeakRef(root));
    createdControllers += 1;
    createdViews += 1;

    return controller;
  }

  let window = NSWindow.alloc().initWithContentRectStyleMaskBackingDefer(
    makeFrame(-100000, -100000, 720, 480),
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

      for (let depth = 1; depth <= maxDepth; depth++) {
        pushController(createRouteController(depth, cycle));
        popController();
      }
    });

    if ((cycle + 1) % 6 === 0) {
      await t.forceGC(2, 20 * 1024 * 1024, 4);
    } else {
      await t.sleep(20);
    }
  }

  while (navStack.length > 1) {
    popController();
  }

  const preCloseNativeControllers = nativeWeakControllers.count;
  const preCloseNativeViews = nativeWeakViews.count;

  navStack.length = 0;
  window.contentViewController = null;
  window.orderOut(null);
  window.close();
  window = null;

  let released = false;
  let postCloseNativeControllers = Number.MAX_SAFE_INTEGER;
  let postCloseNativeViews = Number.MAX_SAFE_INTEGER;
  let postCloseJsControllers = Number.MAX_SAFE_INTEGER;
  let postCloseJsViews = Number.MAX_SAFE_INTEGER;

  const releaseDeadline = t.now() + 16_000;
  while (t.now() < releaseDeadline) {
    await t.forceGC(1, 12 * 1024 * 1024, 4);
    await t.sleep(10);

    postCloseNativeControllers = liveNativeCount(nativeWeakControllers);
    postCloseNativeViews = liveNativeCount(nativeWeakViews);
    postCloseJsControllers = countAlive(jsWeakControllers);
    postCloseJsViews = countAlive(jsWeakViews);

    const controllerShrink = preCloseNativeControllers > 0
      ? postCloseNativeControllers / preCloseNativeControllers
      : 0;
    const viewShrink = preCloseNativeViews > 0
      ? postCloseNativeViews / preCloseNativeViews
      : 0;
    const controllerLiveRatio = createdControllers > 0
      ? postCloseNativeControllers / createdControllers
      : 0;
    const viewLiveRatio = createdViews > 0
      ? postCloseNativeViews / createdViews
      : 0;

    if (controllerLiveRatio <= 0.15 &&
        viewLiveRatio <= 0.15 &&
        postCloseNativeControllers <= preCloseNativeControllers + 8 &&
        postCloseNativeViews <= preCloseNativeViews + 64 &&
        controllerShrink <= 1.0 &&
        viewShrink <= 1.0 &&
        postCloseJsControllers <= 8 &&
        postCloseJsViews <= 16) {
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
  timeoutMs: 40_000,
  activationPolicy: NSApplicationActivationPolicy.Prohibited,
});
