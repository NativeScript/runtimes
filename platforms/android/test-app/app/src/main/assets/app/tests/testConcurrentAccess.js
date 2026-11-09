// WARNING: IF THIS TEST FAILS IT COMPLETELY BREAKS ALL OTHER TESTS!

describe("Tests concurrent access to JNI", function () {
  // Customizable test parameters
  const BACKGROUND_THREADS = 5;
  const SYNC_CALLS = 2;
  const ITERATIONS_PER_CALL = 100;
  // Upper bound on how long the background threads may take before the spec
  // gives up, plus how often it checks. This is a ceiling, not a sleep: the
  // spec finishes as soon as every invocation has landed.
  const MAX_WAIT_MS = 30000;
  const POLL_INTERVAL_MS = 100;

  // The poll above can legitimately outlast jasmine's 5s default, and this is
  // an async spec so that budget really does apply. Same pattern as
  // shared/Workers/index.js.
  let originalTimeout;

  beforeEach(function () {
    originalTimeout = jasmine.DEFAULT_TIMEOUT_INTERVAL;
    jasmine.DEFAULT_TIMEOUT_INTERVAL = MAX_WAIT_MS + 30000;
  });

  afterEach(function () {
    jasmine.DEFAULT_TIMEOUT_INTERVAL = originalTimeout;
  });

  it("test_high_contention_concurrent_access_with_multiple_objects", (done) => {
    console.log('STARTING PROBLEMATIC TEST. THIS MIGHT CRASH OR CAUSE ISSUES IN OTHER TESTS IF IT FAILS. If this is close to the end of the log, check test_high_contention_concurrent_access_with_multiple_objects');
    let callbackInvocations = 0;

    const callback = new com.tns.tests.ConcurrentAccessTest.Callback({
      invoke: (
        list1,
        list2,
        list3,
        list4,
        list5,
        list6,
        list7,
        list8,
        list9,
        list10,
      ) => {
        callbackInvocations++;
        // Assert that accessing size() on any of the lists doesn't throw
        expect(() => list1.size()).not.toThrow();
        expect(() => list2.size()).not.toThrow();
        expect(() => list3.size()).not.toThrow();
        expect(() => list4.size()).not.toThrow();
        expect(() => list5.size()).not.toThrow();
        expect(() => list6.size()).not.toThrow();
        expect(() => list7.size()).not.toThrow();
        expect(() => list8.size()).not.toThrow();
        expect(() => list9.size()).not.toThrow();
        expect(() => list10.size()).not.toThrow();

        // Verify that the lists actually have content
        expect(list1.size()).toBe(5);
        expect(list2.size()).toBe(5);
        expect(list3.size()).toBe(5);
        expect(list4.size()).toBe(5);
        expect(list5.size()).toBe(5);
        expect(list6.size()).toBe(5);
        expect(list7.size()).toBe(5);
        expect(list8.size()).toBe(5);
        expect(list9.size()).toBe(5);
        expect(list10.size()).toBe(5);
      },
    });

    // Start multiple background threads
    for (let i = 0; i < BACKGROUND_THREADS; i++) {
      com.tns.tests.ConcurrentAccessTest.callFromBackgroundThread(
        callback,
        ITERATIONS_PER_CALL,
      );
    }

    // Call synchronously multiple times
    for (let i = 0; i < SYNC_CALLS; i++) {
      com.tns.tests.ConcurrentAccessTest.callSynchronously(
        callback,
        ITERATIONS_PER_CALL,
      );
    }

    // Wait for all threads to complete.
    //
    // This used to be a single fixed sleep, which asserts "the background
    // threads finished within TIMEOUT_MS" -- a statement about marshalling
    // speed, not about concurrency correctness. On the slower engines a few
    // invocations were simply still in flight when the clock ran out (684 of
    // 700 on QuickJS-NG). Poll instead, so the spec fails only if the
    // invocations never arrive.
    const expectedInvocations =
      (BACKGROUND_THREADS + SYNC_CALLS) * ITERATIONS_PER_CALL;
    let waited = 0;

    (function waitForCompletion() {
      if (callbackInvocations >= expectedInvocations || waited >= MAX_WAIT_MS) {
        expect(callbackInvocations).toBe(expectedInvocations);
        done();
        return;
      }
      waited += POLL_INTERVAL_MS;
      setTimeout(waitForCompletion, POLL_INTERVAL_MS);
    })();
  });
});
