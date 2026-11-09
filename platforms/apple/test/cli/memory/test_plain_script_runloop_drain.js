"use strict";

const totalMainQueueCallbacks = 1000;
const startedAt = Date.now();
let completedMainQueueCallbacks = 0;
let checksum = 0;
let finished = false;

function finish(pass, reason) {
  if (finished) {
    return;
  }
  finished = true;

  if (typeof gc === "function") {
    gc();
    gc();
  }

  console.log(`MEMTEST_RESULT:${JSON.stringify({
    name: "plain-script-runloop-drain",
    pass,
    reason: reason || null,
    details: {
      totalMainQueueCallbacks,
      completedMainQueueCallbacks,
      checksum,
      elapsedMs: Date.now() - startedAt,
    },
  })}`);
}

for (let i = 0; i < totalMainQueueCallbacks; i++) {
  dispatch_async(dispatch_get_global_queue(qos_class_t.UTILITY, 0), () => {
    const value = i & 0xff;
    NSOperationQueue.mainQueue.addOperationWithBlock(() => {
      completedMainQueueCallbacks += 1;
      checksum += value;
      if (completedMainQueueCallbacks === totalMainQueueCallbacks) {
        setTimeout(() => finish(true), 0);
      }
    });
  });
}

setTimeout(() => {
  if (completedMainQueueCallbacks !== totalMainQueueCallbacks) {
    finish(
      false,
      `pending callbacks ${completedMainQueueCallbacks}/${totalMainQueueCallbacks}`,
    );
  }
}, 9_000);
