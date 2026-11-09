describe("Native API engine bridge", function () {
    function apiOrPending() {
        var api = global.__nativeScriptNativeApi;
        if (!api) {
            pending("Native API engine bridge is only installed for engine FFI backends.");
        }
        return api;
    }

    function expectBridgeIdentity(api) {
        if (api.runtime === "jsi") {
            expect(api.backend).toBe("hermes");
            return;
        }

        expect(api.runtime).toBe(api.backend);
        expect(["v8", "jsc", "quickjs"]).toContain(api.backend);
    }

    afterEach(function () {
        TNSClearOutput();
    });

    it("exposes the native API host object", function () {
        var api = apiOrPending();

        expectBridgeIdentity(api);
        expect(api.metadata.classes).toBeGreaterThan(0);
        expect(api.metadata.functions).toBeGreaterThan(0);
        expect(api.getClass("NSObject").available).toBe(true);
    });

    it("calls metadata-backed C functions through the engine bridge", function () {
        var api = apiOrPending();
        var fn = api.getFunction("functionWithInt");

        expect(typeof fn).toBe("function");
        expect(fn(42)).toBe(42);
        expect(TNSGetOutput()).toBe("42");
    });

    it("sends Objective-C selectors through the engine bridge", function () {
        var api = apiOrPending();
        var primitives = api.getClass("TNSPrimitives").alloc().invoke("init");

        expect(primitives.methodWithInt(24)).toBe(24);
        expect(TNSGetOutput()).toBe("24");
    });

    it("decodes Objective-C runtime struct signatures through the engine bridge", function () {
        apiOrPending();
        if (typeof UIView === "undefined" || typeof CGRectMake !== "function") {
            pending("UIKit CGRect runtime selector path is only available on iOS.");
            return;
        }

        var view = UIView.alloc().initWithFrame(CGRectMake(10, 20, 30, 40));
        var bounds = view.invoke("bounds");
        expect(bounds.origin.x).toBe(0);
        expect(bounds.origin.y).toBe(0);
        expect(bounds.size.width).toBe(30);
        expect(bounds.size.height).toBe(40);

        view.invoke("setBounds:", CGRectMake(1, 2, 3, 4));
        bounds = view.invoke("bounds");
        expect(bounds.origin.x).toBe(1);
        expect(bounds.origin.y).toBe(2);
        expect(bounds.size.width).toBe(3);
        expect(bounds.size.height).toBe(4);
    });

    it("decodes metadata-less Objective-C runtime struct signatures through the engine bridge", function () {
        apiOrPending();
        var provider = TNSRuntimeOnlyStructProviderMake();
        var pair = provider.invoke("runtimeOnlyPair");
        expect(pair.field0).toBe(12.5);
        expect(pair.field1).toBe(25.5);

        provider.invoke("setRuntimeOnlyPair:", { field0: 3.25, field1: 4.5 });
        pair = provider.invoke("runtimeOnlyPair");
        expect(pair.field0).toBe(3.25);
        expect(pair.field1).toBe(4.5);
    });
});
