(function () {
  "use strict";

  var marker = "NS_BENCH_RESULT:";
  var runtime = globalThis.__NS_BENCHMARK_RUNTIME || "unknown";
  var variant = globalThis.__NS_BENCHMARK_VARIANT || "default";
  var options = globalThis.__NS_BENCHMARK_OPTIONS__ || {};
  var baseIterations = Math.max(1, Number(options.iterations || 250000) | 0);
  var warmupIterations = Math.max(0, Number(options.warmupIterations || Math.min(10000, baseIterations / 10)) | 0);
  var sink = 0;

  function nowMs() {
    if (globalThis.performance && typeof globalThis.performance.now === "function") {
      return globalThis.performance.now();
    }
    return Date.now();
  }

  function consume(value) {
    var n = 0;
    switch (typeof value) {
      case "number":
        n = value | 0;
        break;
      case "boolean":
        n = value ? 1 : 0;
        break;
      case "string":
        n = value.length;
        break;
      case "object":
      case "function":
        if (value === null || value === undefined) {
          n = 0;
        } else if (typeof value.length === "number") {
          n = value.length | 0;
        } else if (typeof value.count === "number") {
          n = value.count | 0;
        } else {
          n = 1;
        }
        break;
      default:
        n = value ? 1 : 0;
        break;
    }

    sink = ((sink << 5) - sink + n) | 0;
  }

  function runLoop(iterations, fn) {
    for (var i = 0; i < iterations; i++) {
      consume(fn(i));
    }
  }

  function bench(name, factor, fn) {
    var iterations = Math.max(1, Math.floor(baseIterations * factor));
    var warmup = Math.min(warmupIterations, iterations);
    runLoop(warmup, fn);

    var started = nowMs();
    runLoop(iterations, fn);
    var elapsedMs = nowMs() - started;

    return {
      name: name,
      iterations: iterations,
      ms: elapsedMs,
      nsPerOp: elapsedMs * 1000000 / iterations
    };
  }

  function emit(payload) {
    console.log(marker + JSON.stringify(payload));
  }

  function addCase(cases, name, factor, fn) {
    try {
      consume(fn(0));
      cases.push({ name: name, factor: factor, fn: fn });
    } catch (error) {
      cases.push({
        name: name,
        skip: true,
        error: error && error.message ? error.message : String(error)
      });
    }
  }

  function buildCases() {
    var cases = [];
    var object = NSObject.alloc().init();
    var otherObject = NSObject.alloc().init();
    var string = NSString.stringWithString("NativeScript dispatch benchmark");
    var compareString = NSString.stringWithString("NativeScript dispatch baseline");
    var prefix = NSString.stringWithString("NativeScript");
    var key = NSString.stringWithString("benchmark-key");
    var array = NSMutableArray.alloc().init();
    array.addObject(object);
    array.addObject(otherObject);
    array.addObject(string);

    var immutableArray = NSArray.arrayWithArray([object, otherObject, string, object]);
    var dictionary = NSMutableDictionary.alloc().init();
    var date = NSDate.dateWithTimeIntervalSince1970(123456);

    addCase(cases, "js.loop.baseline", 1, function (i) {
      return i;
    });

    if (globalThis.performance && typeof globalThis.performance.now === "function") {
      var performanceNow = globalThis.performance.now;
      function NativePrototypeCall() {}
      NativePrototypeCall.prototype.nativeCall = performanceNow;
      var nativePrototypeCall = new NativePrototypeCall();

      addCase(cases, "js.nativeFunction.performance.now", 1, function () {
        return performanceNow();
      });

      addCase(
        cases,
        "js.prototype.nativeFunction.performance.now",
        1,
        function () {
          return nativePrototypeCall.nativeCall();
        }
      );
    }

    addCase(cases, "NSObject.respondsToSelector", 1, function () {
      return object.respondsToSelector("description");
    });

    addCase(cases, "NSObject.isKindOfClass", 1, function () {
      return object.isKindOfClass(NSObject);
    });

    addCase(cases, "NSObject.description.getter", 0.25, function () {
      return object.description;
    });

    addCase(cases, "NSObject.hash.getter", 1, function () {
      return object.hash;
    });

    addCase(cases, "NSString.length.getter", 1, function () {
      return string.length;
    });

    addCase(cases, "NSString.characterAtIndex", 1, function (i) {
      return string.characterAtIndex(i & 7);
    });

    addCase(cases, "NSString.compare", 1, function () {
      return string.compare(compareString);
    });

    addCase(cases, "NSString.hasPrefix", 1, function () {
      return string.hasPrefix(prefix);
    });

    addCase(cases, "NSArray.objectAtIndex", 1, function (i) {
      return immutableArray.objectAtIndex(i & 3);
    });

    addCase(cases, "NSMutableArray.count.getter", 1, function () {
      return array.count;
    });

    addCase(cases, "NSMutableArray.addRemoveObject", 0.5, function () {
      array.addObject(object);
      array.removeObjectAtIndex(array.count - 1);
      return array.count;
    });

    addCase(cases, "NSMutableDictionary.setRemoveObject", 0.5, function () {
      dictionary.setObjectForKey(object, key);
      dictionary.removeObjectForKey(key);
      return dictionary.count;
    });

    addCase(cases, "NSDate.timeIntervalSince1970", 1, function () {
      return date.timeIntervalSince1970;
    });

    if (typeof CGPointMake === "function") {
      addCase(cases, "CoreGraphics.CGPointMake", 0.5, function (i) {
        return CGPointMake(i & 255, (i + 1) & 255).x;
      });
    }

    return cases;
  }

  var results = [];
  var skipped = [];
  var cases = buildCases();

  var startedAt = nowMs();
  for (var i = 0; i < cases.length; i++) {
    var item = cases[i];
    if (item.skip) {
      var skippedCase = { name: item.name, error: item.error };
      skipped.push(skippedCase);
      continue;
    }
    var result = bench(item.name, item.factor, item.fn);
    results.push(result);
  }
  var totalMs = nowMs() - startedAt;

  for (var resultIndex = 0; resultIndex < results.length; resultIndex++) {
    var result = results[resultIndex];
    emit({
      kind: "case",
      name: result.name,
      iterations: result.iterations,
      ms: result.ms,
      nsPerOp: result.nsPerOp
    });
  }

  for (var skippedIndex = 0; skippedIndex < skipped.length; skippedIndex++) {
    var skippedCase = skipped[skippedIndex];
    emit({ kind: "skip", name: skippedCase.name, error: skippedCase.error });
  }

  var report = {
    kind: "done",
    version: 1,
    runtime: runtime,
    variant: variant,
    baseIterations: baseIterations,
    warmupIterations: warmupIterations,
    totalMs: totalMs,
    sink: sink,
    resultCount: results.length,
    skippedCount: skipped.length
  };

  emit(report);
}());
