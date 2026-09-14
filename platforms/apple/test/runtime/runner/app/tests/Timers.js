describe("native timer", () => {
  const isHermes =
    global.process &&
    global.process.versions &&
    global.process.versions.engine === "hermes";
  const isJSC =
    global.process &&
    global.process.versions &&
    global.process.versions.engine === "jsc";
  const isIOS =
    typeof UIDevice !== "undefined" &&
    UIDevice.currentDevice &&
    UIDevice.currentDevice.systemVersion;
  const getActiveTimerCount =
    typeof global.__nsActiveTimerCount === "function"
      ? global.__nsActiveTimerCount.bind(global)
      : null;
  /** @type {global.setTimeout} */
  let setTimeout = global.__ns__setTimeout;
  /** @type {global.setInterval} */
  let setInterval = global.__ns__setInterval;
  /** @type global.setTimeout */
  /** @type {global.clearTimeout} */
  let clearTimeout = global.__ns__clearTimeout;
  /** @type {global.clearInterval} */
  let clearInterval = global.__ns__clearInterval;
  /** @type {global.queueMicrotask} */
  let queueMicrotask = global.__ns__queueMicrotask || global.queueMicrotask;

  function expectWeakRefCleared(ref, done, timeoutMs) {
    const deadline = Date.now() + (timeoutMs || 1000);

    function poll() {
      gc();
      if (!ref.get()) {
        done();
        return;
      }

      if (Date.now() >= deadline) {
        expect(!!ref.get()).toBe(false);
        done();
        return;
      }

      setTimeout(poll, 20);
    }

    setTimeout(poll, 0);
  }

  it("exists", () => {
    expect(setTimeout).toBeDefined();
    expect(setInterval).toBeDefined();
    expect(clearTimeout).toBeDefined();
    expect(clearInterval).toBeDefined();
    expect(queueMicrotask).toBeDefined();
  });

  it("triggers timeout", (done) => {
    const now = Date.now();
    setTimeout(() => {
      expect(Date.now() - now).not.toBeLessThan(100);
      done();
    }, 100);
  });

  it("triggers timeout", (done) => {
    const now = Date.now();
    setTimeout(() => {
      expect(Date.now() - now).not.toBeLessThan(100);
      done();
    }, 100);
  });

  it("triggers interval", (done) => {
    const startedAt = Date.now();
    let calls = 0;
    const itv = setInterval(() => {
      calls++;
      if (calls < 10) {
        return;
      }

      clearInterval(itv);
      clearTimeout(deadline);
      expect(Date.now() - startedAt).not.toBeLessThan(900);
      done();
    }, 100);
    const deadline = setTimeout(() => {
      clearInterval(itv);
      expect(calls).toBeGreaterThanOrEqual(10);
      done();
    }, 2500);
  });

  it("cancels timeout", (done) => {
    let triggered = false;
    const now = Date.now();
    const timeout = setTimeout(() => {
      triggered = true;
    }, 100);
    clearTimeout(timeout);
    setTimeout(() => {
      expect(triggered).toBe(false);
      done();
    }, 200);
  });

  it("cancels interval", (done) => {
    let triggered = false;
    const now = Date.now();
    const timeout = setInterval(() => {
      triggered = true;
    }, 100);
    clearInterval(timeout);
    setTimeout(() => {
      expect(triggered).toBe(false);
      done();
    }, 200);
  });

  it("cancels interval inside function", (done) => {
    let calls = 0;
    const itv = setInterval(() => {
      calls++;
      clearInterval(itv);
    }, 10);
    setTimeout(() => {
      expect(calls).toBe(1);
      done();
    }, 100);
  });

  it("preserves order", (done) => {
    let calls = 0;
    setTimeout(() => {
      expect(calls).toBe(0);
      calls++;
    });
    setTimeout(() => {
      expect(calls).toBe(1);
      calls++;
      done();
    });
  });

  it("runs microtask before timeout", (done) => {
    const order = [];

    queueMicrotask(() => {
      order.push("microtask");
    });

    setTimeout(() => {
      order.push("timeout");
      expect(order[0]).toBe("microtask");
      expect(order[1]).toBe("timeout");
      done();
    }, 0);
  });
  it("frees up resources after complete", (done) => {
    let timeout = 0;
    let interval = 0;
    let weakRef;
    const baselineActiveTimerCount = getActiveTimerCount
      ? getActiveTimerCount()
      : null;
    const baselineHermesTimerCallbackCount =
      isHermes &&
      typeof global.__nsHermesTimerCallbackCount === "function"
        ? global.__nsHermesTimerCallbackCount()
        : null;
    {
      let obj = {
        value: 0,
      };
      weakRef = new WeakRef(obj);
      timeout = setTimeout(() => {
        obj.value++;
      }, 100);
      interval = setInterval(() => {
        obj.value++;
      }, 50);
    }
    setTimeout(() => {
      // use !! here because if you pass weakRef.get() it creates a strong reference (side effect of expect)
      expect(!!weakRef.get()).toBe(true);
      clearInterval(interval);
      clearTimeout(timeout);
      // use another timeout as native weakrefs can't be gced until we leave the isolate after being used once
      setTimeout(() => {
        if (
          isHermes &&
          typeof global.__nsHermesTimerCallbackCount === "function" &&
          typeof global.__nsHermesHasTimerCallback === "function"
        ) {
          gc();
          expect(global.__nsHermesTimerCallbackCount()).toBe(baselineHermesTimerCallbackCount);
          expect(global.__nsHermesHasTimerCallback(timeout.__timerId)).toBe(false);
          expect(global.__nsHermesHasTimerCallback(interval.__timerId)).toBe(false);
          done();
        } else if (
          isIOS &&
          isJSC &&
          getActiveTimerCount
        ) {
          gc();
          expect(getActiveTimerCount()).toBe(baselineActiveTimerCount);
          done();
        } else if (isIOS && isJSC) {
          pending("Timer cleanup introspection is unavailable in this JSC runtime configuration.");
          done();
        } else {
          expectWeakRefCleared(weakRef, done, isJSC ? 10000 : 1500);
        }
      });
    }, 200);
  });

  it("dispatches when invoked in another queue", (done) => {
    if (global.isSimulator) {
      pending("Background-queue timer dispatch is unreliable on Simulator.");
      done();
      return;
    }

    const defaultQosClass =
      typeof qos_class_t !== "undefined" &&
      qos_class_t &&
      qos_class_t.QOS_CLASS_DEFAULT != null
        ? qos_class_t.QOS_CLASS_DEFAULT
        : (typeof QOS_CLASS_DEFAULT !== "undefined" ? QOS_CLASS_DEFAULT : 0);
    const background_queue = dispatch_get_global_queue(
      Number(defaultQosClass),
      0
    );
    const current_queue = dispatch_get_current_queue();
    const deadline = setTimeout(() => {
      fail("Timer callback did not execute after dispatching from a background queue.");
      done();
    }, 5000);
    dispatch_async(background_queue, () => {
      setTimeout(() => {
        clearTimeout(deadline);
        expect(dispatch_get_current_queue()).toBe(current_queue);
        done();
      })
    });
  });
});
