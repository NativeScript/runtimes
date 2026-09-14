"use strict";

const { runAsyncMemoryTest } = require("./_harness");

function makeFrame(x, y, width, height) {
  return {
    origin: { x, y },
    size: { width, height },
  };
}

runAsyncMemoryTest("appkit-navigation-visual-demo", async (t) => {
  const cycles = 8;
  const depth = 5;
  let route = 0;

  function makeController(d, c) {
    const controller = NSViewController.new();
    const root = NSView.alloc().initWithFrame(makeFrame(0, 0, 920, 620));
    root.wantsLayer = true;
    root.layer.backgroundColor = NSColor.colorWithSRGBRedGreenBlueAlpha(
      (route % 2) === 0 ? 0.08 : 0.9,
      (d % 2) === 0 ? 0.2 : 0.82,
      (c % 2) === 0 ? 0.15 : 0.78,
      1,
    ).CGColor;

    const title = NSTextField.alloc().initWithFrame(makeFrame(40, 460, 840, 120));
    title.stringValue = `Route ${route}  |  Depth ${d}  |  Cycle ${c}`;
    title.bezeled = false;
    title.drawsBackground = false;
    title.editable = false;
    title.selectable = false;
    title.alignment = NSTextAlignment.Center;
    title.font = NSFont.boldSystemFontOfSize(48);
    title.textColor = NSColor.whiteColor;
    root.addSubview(title);

    controller.view = root;
    controller.title = `route-${route}`;
    route += 1;
    return controller;
  }

  const window = NSWindow.windowWithContentViewController(makeController(0, 0));
  window.setFrameDisplay(makeFrame(0, 0, 920, 620), false);
  window.center();
  window.makeKeyAndOrderFront(null);
  window.orderFrontRegardless();
  NSApplication.sharedApplication.activateIgnoringOtherApps(true);

  const stack = [window.contentViewController];

  for (let c = 1; c <= cycles; c++) {
    window.title = `Visual Demo ${c}/${cycles}`;
    for (let d = 1; d <= depth; d++) {
      const next = makeController(d, c);
      stack.push(next);
      window.contentViewController = next;
      await t.sleep(180);
    }
    for (let d = depth; d >= 1; d--) {
      stack.pop();
      window.contentViewController = stack[stack.length - 1];
      await t.sleep(180);
    }
    console.log(`visual demo cycle ${c}/${cycles}`);
    await t.sleep(260);
  }

  window.close();
  console.log("visual demo done");
  return {
    cycles,
    depth,
    routes: route,
  };
}, {
  timeoutMs: 90_000,
  activationPolicy: NSApplicationActivationPolicy.Regular,
});
