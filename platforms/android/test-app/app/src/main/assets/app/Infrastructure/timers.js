var timeoutHandler;
var timeoutCallbacks = {};
// Monotonic. This used to return `new Date().getUTCMilliseconds()` -- the
// millisecond *within the current second*, so only 1000 ids existed and
// collisions were routine. On a collision the newer runnable was dropped (the
// store below was guarded by `if (!timeoutCallbacks[id])`), so clearTimeout(id)
// cancelled the *older* timer's runnable and left the newer one queued to fire
// later. jasmine's own per-spec timeouts are set and cleared constantly, so
// stale 5s timers escaped and fired during whichever spec was running then.
var nextTimerId = 0;
function createHadlerAndGetId() {
    if (!timeoutHandler) {
        timeoutHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    }
    nextTimerId++;
    return nextTimerId;
}
function setTimeout(callback, milliseconds) {
    if (milliseconds === void 0) { milliseconds = 0; }
    var id = createHadlerAndGetId();
    var runnable = new java.lang.Runnable({
        run: function () {
            callback();
            delete timeoutCallbacks[id];
        }
    });
    timeoutCallbacks[id] = runnable;
    timeoutHandler.postDelayed(runnable, long(milliseconds));
    return id;
}
global.setTimeout = setTimeout;
function clearTimeout(id) {
    var runnable = timeoutCallbacks[id];
    if (runnable) {
        timeoutHandler.removeCallbacks(runnable);
        delete timeoutCallbacks[id];
    }
}
global.clearTimeout = clearTimeout;
function setInterval(callback, milliseconds) {
    if (milliseconds === void 0) { milliseconds = 0; }
    var id = createHadlerAndGetId();
    var handler = timeoutHandler;
    var runnable = new java.lang.Runnable({
        run: function () {
            callback();
            // Only re-arm while the interval is still registered, so a
            // clearInterval that lands during the callback actually stops it.
            if (timeoutCallbacks[id]) {
                handler.postDelayed(runnable, long(milliseconds));
            }
        }
    });
    timeoutCallbacks[id] = runnable;
    timeoutHandler.postDelayed(runnable, long(milliseconds));
    return id;
}
global.setInterval = setInterval;
global.clearInterval = clearTimeout;
