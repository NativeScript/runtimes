// https://github.com/NativeScript/NativeScript/blob/master/tns-core-modules/timer/timer.ios.ts

if (typeof global.__ns__setTimeout === "function" &&
    typeof global.__ns__clearTimeout === "function") {
    global.setTimeout = global.__ns__setTimeout;
    global.clearTimeout = global.__ns__clearTimeout;
    if (typeof global.__ns__setInterval === "function") {
        global.setInterval = global.__ns__setInterval;
    }
    if (typeof global.__ns__clearInterval === "function") {
        global.clearInterval = global.__ns__clearInterval;
    }
} else if (global.__nativeScriptNativeApi &&
    typeof NSTimer !== "undefined" &&
    typeof NSTimer.scheduledTimerWithTimeIntervalRepeatsBlock === "function") {
    var directTimeoutCallbacks = new Map();
    var directTimerId = 0;

    function createDirectTimerAndGetId(callback, milliseconds, shouldRepeat) {
        directTimerId++;
        var id = directTimerId;
        var pair = { k: null, disposed: false };
        var timer = NSTimer.scheduledTimerWithTimeIntervalRepeatsBlock(
            milliseconds / 1000,
            shouldRepeat,
            function () {
            if (pair.disposed) {
                return;
            }
            callback();
            if (typeof global.__drainMicrotaskQueue === "function") {
                global.__drainMicrotaskQueue();
            }
            if (!shouldRepeat) {
                    pair.disposed = true;
                    directTimeoutCallbacks.delete(id);
                }
            });
        NSRunLoop.currentRunLoop.addTimerForMode(timer, NSRunLoopCommonModes);
        pair.k = timer;
        directTimeoutCallbacks.set(id, pair);
        return id;
    }

    global.setTimeout = function (callback, milliseconds) {
        if (milliseconds === void 0) { milliseconds = 0; }
        var args = [];
        for (var i = 2; i < arguments.length; i++) {
            args[i - 2] = arguments[i];
        }
        return createDirectTimerAndGetId(function () {
            return callback.apply(void 0, args);
        }, milliseconds, false);
    };

    global.clearTimeout = function (id) {
        var pair = directTimeoutCallbacks.get(id);
        if (pair && !pair.disposed) {
            pair.disposed = true;
            pair.k.invalidate();
            directTimeoutCallbacks.delete(id);
        }
    };

    global.setInterval = function (callback, milliseconds) {
        if (milliseconds === void 0) { milliseconds = 0; }
        var args = [];
        for (var i = 2; i < arguments.length; i++) {
            args[i - 2] = arguments[i];
        }
        return createDirectTimerAndGetId(function () {
            return callback.apply(void 0, args);
        }, milliseconds, true);
    };

    global.clearInterval = global.clearTimeout;
} else {
var timeoutCallbacks = new Map();
var timerId = 0;

var TimerTargetImpl = (function (_super) {
    __extends(TimerTargetImpl, _super);
    function TimerTargetImpl() {
        return _super !== null && _super.apply(this, arguments) || this;
    }
    TimerTargetImpl.initWithCallback = function (callback, id, shouldRepeat) {
        var handler = TimerTargetImpl.new();
        handler.callback = callback;
        handler.id = id;
        handler.shouldRepeat = shouldRepeat;
        return handler;
    };
    TimerTargetImpl.prototype.tick = function (timer) {
        if (!this.disposed) {
            this.callback();
        }
        if (!this.shouldRepeat) {
            this.unregister();
        }
    };
    TimerTargetImpl.prototype.unregister = function () {
        if (!this.disposed) {
            this.disposed = true;
            var timer = timeoutCallbacks.get(this.id).k;
            timer.invalidate();
            timeoutCallbacks.delete(this.id);
        }
    };
    TimerTargetImpl.ObjCExposedMethods = {
        "tick": { returns: interop.types.void, params: [NSTimer] }
    };
    return TimerTargetImpl;
}(NSObject));
function createTimerAndGetId(callback, milliseconds, shouldRepeat) {
    timerId++;
    var id = timerId;
    var timerTarget = TimerTargetImpl.initWithCallback(callback, id, shouldRepeat);
    var timer = NSTimer.scheduledTimerWithTimeIntervalTargetSelectorUserInfoRepeats(milliseconds / 1000, timerTarget, "tick", null, shouldRepeat);
    NSRunLoop.currentRunLoop.addTimerForMode(timer, NSRunLoopCommonModes);
    var pair = { k: timer, v: timerTarget };
    timeoutCallbacks.set(id, pair);
    return id;
}

function setTimeout(callback, milliseconds) {
    if (milliseconds === void 0) { milliseconds = 0; }
    var args = [];
    for (var _i = 2; _i < arguments.length; _i++) {
        args[_i - 2] = arguments[_i];
    }
    var invoke = function () { return callback.apply(void 0, args); };
    return createTimerAndGetId(invoke, milliseconds, false);
}

function clearTimeout(id) {
    var pair = timeoutCallbacks.get(id);
    if (pair) {
        pair.v.unregister();
    }
}

global.setTimeout = setTimeout;
global.clearTimeout = clearTimeout;
}
