// Covers the fix for FFI method metadata resolution when a selector is
// declared solely on an Objective-C protocol and the conforming class's
// public header (all the metadata generator ever parses) never declares
// that conformance -- the same shape of gap as
// UIViewControllerTransitionCoordinator, where the concrete class is
// private. Before the fix, calling such a method with a bare JS closure
// block argument threw "Native callback metadata is unavailable."; the
// documented workaround was to hand-supply the ObjC encoding via
// interop.Block(fn, "v@?@"). These tests assert a bare closure now works
// directly, and that the already-working class-hierarchy path (conformance
// declared on the interface) is unaffected.
describe(module.id, function () {
    it("resolves a block-parameter method declared only on a conformed protocol, hidden from metadata on the concrete class", function () {
        var obj = TNSProtocolOnlyMembersFactory.createImplementorWithHiddenConformance();

        var seen;
        obj.invokeBlockCallback(function (value) {
            seen = value;
        });
        expect(seen).toBe(42);
    });

    it("resolves a block-parameter method with a return value, declared only on a conformed protocol", function () {
        var obj = TNSProtocolOnlyMembersFactory.createImplementorWithHiddenConformance();

        var sum = obj.invokeBlockCallbackReturningSum(function (a, b) {
            return a + b;
        });
        expect(sum).toBe(7);
    });

    it("still resolves the same protocol methods via the ordinary class-hierarchy path when conformance IS declared on the interface", function () {
        var obj = TNSProtocolOnlyMembersFactory.createImplementorWithDeclaredConformance();

        var seen;
        obj.invokeBlockCallback(function (value) {
            seen = value;
        });
        expect(seen).toBe(42);

        var sum = obj.invokeBlockCallbackReturningSum(function (a, b) {
            return a + b;
        });
        expect(sum).toBe(7);
    });
});
