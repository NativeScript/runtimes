// macOS compatibility wrapper.
// Keep the same test entrypoint behavior as iOS (`index.js`) and only provide
// small shims needed by nsr-style runners.

if (typeof global.TNSGetOutput !== "function" ||
    typeof global.TNSLog !== "function" ||
    typeof global.TNSClearOutput !== "function") {
    let tnsOutput = null;

    global.TNSGetOutput = function () {
        if (tnsOutput === null) {
            tnsOutput = "";
        }

        return tnsOutput;
    };

    global.TNSLog = function (message) {
        let text;
        try {
            text = `${message}`;
        } catch (_) {
            text = Object.prototype.toString.call(message);
        }

        tnsOutput = global.TNSGetOutput() + text;
    };

    global.TNSClearOutput = function () {
        tnsOutput = null;
    };
}

if (typeof global.TNSSaveResults !== "function") {
    // `index.js` delegates JUnit persistence to TNSSaveResults.
    // App-based runners provide it natively; nsr-compatible runs can no-op.
    global.TNSSaveResults = function () {};
}

require("./index");
