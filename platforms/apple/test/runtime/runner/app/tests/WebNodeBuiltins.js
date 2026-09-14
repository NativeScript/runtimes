function dynamicImport(specifier) {
    return (0, eval)("import(" + JSON.stringify(specifier) + ")");
}

describe("Web builtins", function () {
    function hasWebBuiltins() {
        return (
            typeof fetch === "function" &&
            typeof Headers === "function" &&
            typeof Request === "function" &&
            typeof Response === "function" &&
            typeof WebSocket === "function" &&
            typeof ReadableStream === "function"
        );
    }

    function ensureWebBuiltins() {
        if (!hasWebBuiltins()) {
            pending("Web builtins are not enabled in this runtime build.");
            return false;
        }

        return true;
    }

    it("should expose web globals", function () {
        if (!ensureWebBuiltins()) {
            return;
        }

        expect(typeof fetch).toBe("function");
        expect(typeof Headers).toBe("function");
        expect(typeof Request).toBe("function");
        expect(typeof Response).toBe("function");
        expect(typeof WebSocket).toBe("function");
        expect(typeof ReadableStream).toBe("function");
    });

    it("should expose internal web modules", function () {
        if (!ensureWebBuiltins()) {
            return;
        }

        const web = require("web");
        expect(web).toBeDefined();
        expect(web.fetch).toBe(fetch);
        expect(web.Headers).toBe(Headers);
        expect(web.Request).toBe(Request);
        expect(web.Response).toBe(Response);
        expect(web.WebSocket).toBe(WebSocket);

        const streamWeb = require("stream/web");
        expect(streamWeb).toBeDefined();
        expect(streamWeb.ReadableStream).toBe(ReadableStream);
        expect(typeof streamWeb.WritableStream).toBe("function");
        expect(typeof streamWeb.TransformStream).toBe("function");
    });

    it("Headers should be case-insensitive", function () {
        if (!ensureWebBuiltins()) {
            return;
        }

        const headers = new Headers();
        headers.append("Content-Type", "text/plain");
        headers.append("x-test", "a");
        headers.append("X-Test", "b");

        expect(headers.get("content-type")).toBe("text/plain");
        expect(headers.get("X-Test")).toBe("a, b");
        expect(headers.has("x-test")).toBe(true);
    });

    it("Request and Response should support body helpers", function (done) {
        if (!ensureWebBuiltins()) {
            done();
            return;
        }

        const request = new Request("https://example.com/post", {
            method: "POST",
            headers: { "content-type": "text/plain" },
            body: "hello"
        });

        expect(request.method).toBe("POST");
        expect(request.headers.get("content-type")).toBe("text/plain");

        request
            .text()
            .then(function (text) {
                expect(text).toBe("hello");

                const response = new Response('{"ok":true}', {
                    status: 201,
                    statusText: "Created",
                    headers: { "content-type": "application/json" }
                });

                expect(response.ok).toBe(true);
                expect(response.status).toBe(201);
                return response.json();
            })
            .then(function (json) {
                expect(json.ok).toBe(true);
                done();
            })
            .catch(function (error) {
                fail(error);
                done();
            });
    });

    it("fetch should reject invalid URLs", function (done) {
        if (!ensureWebBuiltins()) {
            done();
            return;
        }

        fetch("://invalid-url")
            .then(function () {
                fail("Expected fetch to reject");
                done();
            })
            .catch(function (error) {
                expect(error).toBeDefined();
                done();
            });
    });

    it("WebSocket should validate URL scheme", function () {
        if (!ensureWebBuiltins()) {
            return;
        }

        expect(function () {
            return new WebSocket("https://example.com");
        }).toThrow();
    });

    it("should resolve web modules via ESM dynamic import", function (done) {
        if (!ensureWebBuiltins()) {
            done();
            return;
        }

        dynamicImport("web")
            .then(function (web) {
                expect(web).toBeDefined();
                expect(typeof web.fetch).toBe("function");
                expect(typeof web.WebSocket).toBe("function");
                return dynamicImport("stream/web");
            })
            .then(function (streamWeb) {
                expect(streamWeb).toBeDefined();
                expect(typeof streamWeb.ReadableStream).toBe("function");
                done();
            })
            .catch(function (error) {
                pending("ESM dynamic import for web builtins is unavailable: " + error);
                done();
            });
    });
});

describe("Node fs builtin", function () {
    function tryRequire(name) {
        try {
            return require(name);
        } catch (_) {
            return null;
        }
    }

    function ensureFsBuiltins(fs, nodeFs) {
        if (!fs || !nodeFs) {
            pending("Node fs builtins are not enabled in this runtime build.");
            return false;
        }

        return true;
    }

    function getTempRoot() {
        if (typeof NSTemporaryDirectory === "function") {
            return String(NSTemporaryDirectory());
        }

        return String(NSHomeDirectory());
    }

    function joinPath() {
        const parts = Array.prototype.slice.call(arguments).filter(function (p) {
            return p !== undefined && p !== null && String(p).length > 0;
        });

        const first = String(parts.shift() || "");
        return [first.replace(/\/+$/, "")]
            .concat(parts.map(function (part) {
                return String(part).replace(/^\/+/, "");
            }))
            .join("/");
    }

    function randomSuffix() {
        return Date.now().toString(16) + "-" + Math.floor(Math.random() * 100000).toString(16);
    }

    let fs;
    let nodeFs;
    let testDir;

    beforeEach(function () {
        fs = tryRequire("fs");
        nodeFs = tryRequire("node:fs");

        if (!fs || !nodeFs) {
            testDir = null;
            return;
        }

        testDir = joinPath(getTempRoot(), "ns-runtime-web-node-tests-" + randomSuffix());
        fs.mkdirSync(testDir, { recursive: true });
    });

    afterEach(function () {
        if (!fs || !testDir) {
            return;
        }

        try {
            fs.rmSync(testDir, { recursive: true, force: true });
        } catch (_) {
        }
    });

    it("should expose fs and node:fs", function () {
        if (!ensureFsBuiltins(fs, nodeFs)) {
            return;
        }

        expect(fs).toBeDefined();
        expect(nodeFs).toBeDefined();
        expect(typeof fs.readFileSync).toBe("function");
        expect(typeof nodeFs.readFileSync).toBe("function");
        expect(typeof fs.promises.readFile).toBe("function");
    });

    it("should write and read utf8 text", function () {
        if (!ensureFsBuiltins(fs, nodeFs)) {
            return;
        }

        const filePath = joinPath(testDir, "hello.txt");
        const text = "hello web+node";

        fs.writeFileSync(filePath, text, "utf8");
        expect(fs.existsSync(filePath)).toBe(true);
        expect(fs.readFileSync(filePath, "utf8")).toBe(text);
    });

    it("should read binary as ArrayBuffer", function () {
        if (!ensureFsBuiltins(fs, nodeFs)) {
            return;
        }

        const filePath = joinPath(testDir, "bin.dat");
        fs.writeFileSync(filePath, "abc", "utf8");

        const data = fs.readFileSync(filePath);
        expect(data instanceof ArrayBuffer).toBe(true);
        expect(new Uint8Array(data).length).toBe(3);
    });

    it("should support stat/lstat and readdir with dirents", function () {
        if (!ensureFsBuiltins(fs, nodeFs)) {
            return;
        }

        const nestedDir = joinPath(testDir, "nested");
        const filePath = joinPath(nestedDir, "item.txt");

        fs.mkdirSync(nestedDir, { recursive: true });
        fs.writeFileSync(filePath, "x", "utf8");

        const stat = fs.statSync(filePath);
        expect(stat.isFile()).toBe(true);
        expect(stat.isDirectory()).toBe(false);
        expect(stat.size).toBe(1);

        const lstat = fs.lstatSync(filePath);
        expect(lstat.isFile()).toBe(true);

        const entries = fs.readdirSync(nestedDir, { withFileTypes: true });
        expect(entries.length).toBe(1);
        expect(entries[0].name).toBe("item.txt");
        expect(entries[0].isFile()).toBe(true);
    });

    it("should support callback and promise forms", function (done) {
        if (!ensureFsBuiltins(fs, nodeFs)) {
            done();
            return;
        }

        const filePath = joinPath(testDir, "async.txt");
        fs.writeFileSync(filePath, "async-content", "utf8");

        fs.readFile(filePath, "utf8", function (error, content) {
            expect(error).toBeNull();
            expect(content).toBe("async-content");

            fs.promises
                .readFile(filePath, "utf8")
                .then(function (promiseContent) {
                    expect(promiseContent).toBe("async-content");
                    done();
                })
                .catch(function (promiseError) {
                    fail(promiseError);
                    done();
                });
        });
    });

    it("should resolve fs via ESM dynamic import", function (done) {
        if (!ensureFsBuiltins(fs, nodeFs)) {
            done();
            return;
        }

        dynamicImport("node:fs")
            .then(function (esmFs) {
                expect(esmFs).toBeDefined();
                expect(typeof esmFs.readFileSync).toBe("function");
                return dynamicImport("node:fs/promises");
            })
            .then(function (esmFsPromises) {
                expect(esmFsPromises).toBeDefined();
                expect(typeof esmFsPromises.readFile).toBe("function");
                done();
            })
            .catch(function (error) {
                pending("ESM dynamic import for fs builtins is unavailable: " + error);
                done();
            });
    });
});

describe("Node process builtin", function () {
    function tryRequire(name) {
        try {
            return require(name);
        } catch (_) {
            return null;
        }
    }

    function ensureProcessBuiltins(processModule, nodeProcessModule) {
        if (!processModule || !nodeProcessModule || typeof process !== "object") {
            pending("Node process builtin is not enabled in this runtime build.");
            return false;
        }

        return true;
    }

    function getTempRoot() {
        if (typeof NSTemporaryDirectory === "function") {
            return String(NSTemporaryDirectory());
        }

        return String(NSHomeDirectory());
    }

    function randomSuffix() {
        return Date.now().toString(16) + "-" + Math.floor(Math.random() * 100000).toString(16);
    }

    it("should expose global process and internal process modules", function () {
        const processModule = tryRequire("process");
        const nodeProcessModule = tryRequire("node:process");

        if (!ensureProcessBuiltins(processModule, nodeProcessModule)) {
            return;
        }

        expect(processModule).toBe(process);
        expect(nodeProcessModule).toBe(process);
        expect(typeof process.cwd).toBe("function");
        expect(typeof process.env).toBe("object");
    });

    it("should expose common process metadata", function () {
        const processModule = tryRequire("process");
        const nodeProcessModule = tryRequire("node:process");

        if (!ensureProcessBuiltins(processModule, nodeProcessModule)) {
            return;
        }

        expect(typeof process.platform).toBe("string");
        expect(process.platform.length).toBeGreaterThan(0);
        expect(typeof process.arch).toBe("string");
        expect(process.arch.length).toBeGreaterThan(0);
        expect(typeof process.pid).toBe("number");
        expect(typeof process.ppid).toBe("number");
        expect(typeof process.version).toBe("string");
        expect(typeof process.versions).toBe("object");
        expect(typeof process.versions.node).toBe("string");
        expect(Array.isArray(process.argv)).toBe(true);
        expect(process.argv.length).toBeGreaterThan(0);
        expect(process.argv0).toBe(process.argv[0]);
        expect(typeof process.execPath).toBe("string");
        expect(process.execPath.length).toBeGreaterThan(0);

        const marker = "__nsProcessMarker_" + randomSuffix();
        process.env[marker] = "1";
        expect(process.env[marker]).toBe("1");
        delete process.env[marker];
    });

    it("should support cwd and chdir", function () {
        const processModule = tryRequire("process");
        const nodeProcessModule = tryRequire("node:process");
        const fs = tryRequire("fs");

        if (!ensureProcessBuiltins(processModule, nodeProcessModule)) {
            return;
        }

        if (!fs) {
            pending("fs builtin is required for process.chdir test setup.");
            return;
        }

        const originalCwd = process.cwd();
        expect(typeof originalCwd).toBe("string");
        expect(originalCwd.length).toBeGreaterThan(0);

        const directoryName = "ns-process-tests-" + randomSuffix();
        const testDir = getTempRoot().replace(/\/+$/, "") + "/" + directoryName;
        fs.mkdirSync(testDir, { recursive: true });

        try {
            process.chdir(testDir);
            expect(process.cwd().indexOf(directoryName)).not.toBe(-1);
        } finally {
            process.chdir(originalCwd);
            fs.rmSync(testDir, { recursive: true, force: true });
        }
    });

    it("should expose timing helpers", function () {
        const processModule = tryRequire("process");
        const nodeProcessModule = tryRequire("node:process");

        if (!ensureProcessBuiltins(processModule, nodeProcessModule)) {
            return;
        }

        expect(typeof process.uptime).toBe("function");
        expect(process.uptime()).not.toBeLessThan(0);

        expect(typeof process.hrtime).toBe("function");
        const start = process.hrtime();
        expect(Array.isArray(start)).toBe(true);
        expect(start.length).toBe(2);

        const diff = process.hrtime(start);
        expect(Array.isArray(diff)).toBe(true);
        expect(diff.length).toBe(2);
        expect(diff[0]).not.toBeLessThan(0);
        expect(diff[1]).not.toBeLessThan(0);
        expect(diff[1]).toBeLessThan(1000000000);

        if (typeof BigInt === "function" && process.hrtime.bigint) {
            expect(typeof process.hrtime.bigint()).toBe("bigint");
        }
    });

    it("should expose memoryUsage info", function () {
        const processModule = tryRequire("process");
        const nodeProcessModule = tryRequire("node:process");

        if (!ensureProcessBuiltins(processModule, nodeProcessModule)) {
            return;
        }

        expect(typeof process.memoryUsage).toBe("function");

        const usage = process.memoryUsage();
        expect(usage).toBeDefined();
        expect(typeof usage.rss).toBe("number");
        expect(typeof usage.heapTotal).toBe("number");
        expect(typeof usage.heapUsed).toBe("number");
        expect(typeof usage.external).toBe("number");
        expect(typeof usage.arrayBuffers).toBe("number");
        expect(usage.rss).not.toBeLessThan(0);
        expect(usage.heapTotal).not.toBeLessThan(0);
        expect(usage.heapUsed).not.toBeLessThan(0);
        expect(usage.external).not.toBeLessThan(0);
        expect(usage.arrayBuffers).not.toBeLessThan(0);

        expect(typeof process.memoryUsage.rss).toBe("function");
        const rssValue = process.memoryUsage.rss();
        expect(typeof rssValue).toBe("number");
        expect(rssValue).not.toBeLessThan(0);
    });

    it("should expose simple stdio write streams", function () {
        const processModule = tryRequire("process");
        const nodeProcessModule = tryRequire("node:process");

        if (!ensureProcessBuiltins(processModule, nodeProcessModule)) {
            return;
        }

        expect(process.stdin).toBeDefined();
        expect(process.stdout).toBeDefined();
        expect(process.stderr).toBeDefined();
        expect(typeof process.stdout.write).toBe("function");
        expect(typeof process.stderr.write).toBe("function");
        expect(process.stdin.fd).toBe(0);
        expect(process.stdout.fd).toBe(1);
        expect(process.stderr.fd).toBe(2);

        let stdoutCallbackCalled = false;
        const stdoutResult = process.stdout.write("[process.stdout.write smoke]\\n", function () {
            stdoutCallbackCalled = true;
        });
        expect(stdoutResult).toBe(true);
        expect(stdoutCallbackCalled).toBe(true);

        let stderrCallbackCalled = false;
        const stderrResult = process.stderr.write(
            "[process.stderr.write smoke]\\n",
            "utf8",
            function () {
                stderrCallbackCalled = true;
            }
        );
        expect(stderrResult).toBe(true);
        expect(stderrCallbackCalled).toBe(true);
    });

    it("should support process.on-style listener APIs", function () {
        const processModule = tryRequire("process");
        const nodeProcessModule = tryRequire("node:process");

        if (!ensureProcessBuiltins(processModule, nodeProcessModule)) {
            return;
        }

        expect(typeof process.on).toBe("function");
        expect(typeof process.off).toBe("function");
        expect(typeof process.once).toBe("function");
        expect(typeof process.emit).toBe("function");
        expect(typeof process.exit).toBe("function");
        expect(typeof process.listenerCount).toBe("function");

        const eventName = "__ns-process-on-smoke";
        let value = 0;
        function handler(delta) {
            value += delta;
        }

        const returned = process.on(eventName, handler);
        expect(returned).toBe(process);
        expect(process.listenerCount(eventName)).toBe(1);

        const emitted = process.emit(eventName, 2);
        expect(emitted).toBe(true);
        expect(value).toBe(2);

        process.off(eventName, handler);
        expect(process.listenerCount(eventName)).toBe(0);
        expect(process.emit(eventName, 1)).toBe(false);

        let sawSigint = false;
        function sigintListener() {
            sawSigint = true;
        }

        process.on("SIGINT", sigintListener);
        expect(process.listenerCount("SIGINT")).toBe(1);
        expect(process.emit("SIGINT")).toBe(true);
        expect(sawSigint).toBe(true);
        process.off("SIGINT", sigintListener);
        expect(process.listenerCount("SIGINT")).toBe(0);
    });
});
