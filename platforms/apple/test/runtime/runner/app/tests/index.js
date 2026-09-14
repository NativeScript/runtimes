// Inform the test results runner that the runtime is up.
console.log('Application Start!');

require("./Infrastructure/timers");
require("./Infrastructure/simulator");
global.utf8 = require("./Infrastructure/utf8")

global.UNUSED = function (param) {
};

if (typeof global.TNSSaveResults !== "function") {
    global.TNSSaveResults = function () {};
}

if (typeof global.__runtimeVersion === "undefined") {
    global.__runtimeVersion = "napi-ios";
}

// var args = NSProcessInfo.processInfo.arguments();
// console.log("TKUnit: Process arguments: " + args);
function getProcessArgs() {
    var processInfo = NSProcessInfo.processInfo;
    var args = processInfo.arguments;

    if (typeof args === "function") {
        try {
            args = args.call(processInfo);
        } catch (_) {
            args = null;
        }
    }

    if (!args) {
        return [];
    }

    if (Array.isArray(args)) {
        return args.map(function (value) { return String(value); });
    }

    var count = typeof args.count === "function" ? args.count() : args.count;
    if (typeof count === "number" && count >= 0) {
        var list = [];
        for (var i = 0; i < count; i++) {
            var value = args.objectAtIndex ? args.objectAtIndex(i) : args[i];
            list.push(String(value));
        }
        return list;
    }

    if (typeof args.length === "number" && args.length >= 0) {
        var result = [];
        for (var j = 0; j < args.length; j++) {
            result.push(String(args[j]));
        }
        return result;
    }

    return [];
}

function hasProcessArg(expected) {
    var args = getProcessArgs();
    for (var i = 0; i < args.length; i++) {
        if (args[i] === expected) {
            return true;
        }
    }

    return false;
}

var logjunit = hasProcessArg("-logjunit");
var verboseSpecLogs = hasProcessArg("-verbose-specs");
var skipApplicationMain = hasProcessArg("-skip-application-main");

function getProcessArgValue(flag) {
    var args = getProcessArgs();
    for (var i = 0; i < args.length; i++) {
        var value = args[i];

        if (value === flag && i + 1 < args.length) {
            return args[i + 1];
        }

        if (value.indexOf(flag + "=") === 0) {
            return value.substring(flag.length + 1);
        }
    }

    return null;
}

var requestedTests = (function () {
    var value = getProcessArgValue("-tests") || getProcessArgValue("--tests");
    if (!value) {
        return null;
    }

    return String(value)
        .split(",")
        .map(function (item) { return item.trim(); })
        .filter(function (item) { return item.length > 0; });
})();

var requestedSpecs = (function () {
    var value = getProcessArgValue("-specs") || getProcessArgValue("--specs");
    if (!value) {
        return null;
    }

    return String(value)
        .split(",")
        .map(function (item) { return item.trim(); })
        .filter(function (item) { return item.length > 0; });
})();

function shouldRun(modulePath) {
    if (!requestedTests || requestedTests.length === 0) {
        return true;
    }

    return requestedTests.some(function (token) {
        return modulePath === token || modulePath.indexOf(token) !== -1;
    });
}

function loadTest(modulePath) {
    if (!shouldRun(modulePath)) {
        return;
    }

    require(modulePath);
}

// Provides an output channel for jasmine JUnit test result xml.
global.__JUnitSaveResults = function (text) {
    TNSSaveResults(text);

    if (logjunit) {
        text.split('\n').forEach(function (line) {
            console.log("TKUnit: " + line);
        });
    }

    var reportUrl = NSProcessInfo.processInfo.environment.objectForKey("REPORT_BASEURL");
    if (reportUrl) {
        var urlRequest = NSMutableURLRequest.requestWithURL(NSURL.URLWithString(reportUrl));
        urlRequest.HTTPMethod = "POST";
        urlRequest.setValueForHTTPHeaderField("Content-Type", "application/xml");
        urlRequest.HTTPBody = NSString.stringWithString(text).dataUsingEncoding(4);
        var sessionConfig = NSURLSessionConfiguration.defaultSessionConfiguration;
        var queue = NSOperationQueue.mainQueue;
        var session = NSURLSession.sessionWithConfigurationDelegateDelegateQueue(sessionConfig, null, queue);
        var dataTask = session.dataTaskWithRequestCompletionHandler(urlRequest, (data, response, error) => { });
        dataTask.resume();
    }
};

var fileManager = NSFileManager.defaultManager;
var bundlePath = NSString.stringWithString(NSBundle.mainBundle.bundlePath).stringByResolvingSymlinksInPath;
var resourcePath = NSString.stringWithString(NSBundle.mainBundle.resourcePath).stringByResolvingSymlinksInPath;

if (fileManager.fileExistsAtPath(bundlePath + "/app")) {
    global.__approot = bundlePath;
} else if (fileManager.fileExistsAtPath(resourcePath + "/app")) {
    global.__approot = resourcePath;
} else {
    global.__approot = bundlePath;
}

if (typeof global.__nativeRequire === "function") {
    var appRoot = global.__approot + "/app";
    global.require = function (modulePath) {
        return global.__nativeRequire(modulePath, appRoot);
    };
}

require("./Infrastructure/Jasmine/jasmine-2.0.1/boot");

if (requestedSpecs && requestedSpecs.length > 0) {
    var originalIt = global.it;
    global.it = function (description, specDefinitions, timeout) {
        var shouldRunSpec = requestedSpecs.some(function (token) {
            return description === token || description.indexOf(token) !== -1;
        });

        if (shouldRunSpec) {
            return originalIt(description, specDefinitions, timeout);
        }

        return xit(description, specDefinitions, timeout);
    };
}

if (verboseSpecLogs) {
    jasmine.getEnv().addReporter({
        specStarted: function (result) {
            console.log("SPEC START: " + result.fullName);
        },
        specDone: function (result) {
            console.log("SPEC DONE: " + result.fullName + " => " + result.status);
        }
    });
}

loadTest("./Marshalling/Primitives/Function");
loadTest("./Marshalling/Primitives/Static");
loadTest("./Marshalling/Primitives/Instance");
loadTest("./Marshalling/Primitives/Derived");
//
loadTest("./Marshalling/ObjCTypesTests");
loadTest("./Marshalling/ConstantsTests");
loadTest("./Marshalling/RecordTests");
loadTest("./Marshalling/VectorTests");
// todo: figure out why this test is failing with a EXC_BAD_ACCESS on TNSRecords.m matrix initialization
// require("./Marshalling/MatrixTests");
loadTest("./Marshalling/NSStringTests");
//import "./Marshalling/TypesTests";
loadTest("./Marshalling/PointerTests");
loadTest("./Marshalling/ReferenceTests");
loadTest("./Marshalling/FunctionPointerTests");
loadTest("./Marshalling/EnumTests");
loadTest("./Marshalling/ProtocolTests");
loadTest("./Marshalling/ProtocolOnlyMethodMetadataTests");
//
// import "./Inheritance/ConstructorResolutionTests";
loadTest("./Inheritance/InheritanceTests");
loadTest("./Inheritance/ProtocolImplementationTests");
loadTest("./Inheritance/TypeScriptTests");
//
loadTest("./MethodCallsTests");
//import "./FunctionsTests";
loadTest("./VersionDiffTests");
loadTest("./ObjCConstructors");
//
loadTest("./MetadataTests");
//
loadTest("./ApiTests");
loadTest("./DeclarationConflicts");
//
loadTest("./Promises");
loadTest("./Modules");
//
loadTest("./RuntimeImplementedAPIs");
loadTest("./NativeApiJsiTests");
loadTest("./WebNodeBuiltins");
loadTest("./VMTests");

loadTest("./Timers");

loadTest("./URL");
loadTest("./URLSearchParams");
loadTest("./URLPattern");

// Exception handling tests
loadTest("./ExceptionHandlingTests");

// Tests common for all runtimes.
if (shouldRun("./shared/index")) {
    require("./shared/index").runAllTests();
}

// (Optional) Custom testing for various optional sdk's and frameworks
// These can be turned on manually to verify if needed anytime
//require("./sdks/MusicKit");

execute();

if (typeof UIApplicationMain === "function" && !skipApplicationMain) {
    UIApplicationMain(0, null, null, null);
} else if (typeof NSApplicationMain === "function" && !logjunit) {
    NSApplicationMain(0, null);
}
