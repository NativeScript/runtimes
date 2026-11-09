let isFinalized = false;

const registry = new FinalizationRegistry((heldValue) => {
  console.log(`Finalized: ${heldValue}`);
  isFinalized = true;
});

let weakRef;

(function localScope() {
  const obj = NSObject.new();
  registry.register(obj, "some value");
  weakRef = new WeakRef(obj);
})();

function tick() {
  // Nudge GC a few times and yield
  if (global.gc) gc();
  setTimeout(() => {
    const o = weakRef.deref();
    if (!o) {
      console.log("Object is gone");
      __drainMicrotaskQueue();

      // Give the FR a chance to print too
      // setTimeout(() => {
      //   if (typeof NSApplication === "function") {
      //     // NSApplication.sharedApplication.terminate(null);
      //   } else {
      //     process.exit(0);
      //   }
      // }, 0);

      (function tickTillFinalized() {
        gc();
        if (isFinalized) {
          console.log("Finalization complete");
          if (typeof NSApplication === "function") {
            NSApplication.sharedApplication.terminate(null);
          } else {
            process.exit(0);
          }
        } else {
          console.log("Waiting for finalization...");
          setTimeout(tickTillFinalized, 10);
        }
      })();

      // setTimeout(tick, 10);
    } else {
      // Create real pressure to encourage major GC
      const junk = [];
      for (let i = 0; i < 1e4; i++) junk.push(new Array(10_000).fill(i));
      junk.length = 0;
      setTimeout(tick, 10);
    }
  }, 0);
}

tick();

if (typeof NSApplicationMain === "function") {
  NSApplicationMain(0, null);
}
