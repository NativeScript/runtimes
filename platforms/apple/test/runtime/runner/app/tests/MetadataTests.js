describe("Metadata", function () {
    it("where method in category is implemented with property, the property access and modification should work and the method should be 'hidden'.", function () {
        var object = TNSPropertyMethodConflictClass.alloc().init();
        expect(object.conflict).toBe(false);
    });

    it("Swift objects should be marshalled correctly", function () {
        expect(global.TNSSwiftLikeFactory).toBeDefined();
        expect(global.TNSSwiftLikeFactory.name).toBe("TNSSwiftLikeFactory");
        const swiftLikeObj = TNSSwiftLikeFactory.create();
        // Verify the object is a valid native object
        expect(swiftLikeObj).toBeDefined();
        expect(swiftLikeObj.className).toBeDefined();
        // Verify the runtime class name
        var className = swiftLikeObj.className;
        expect([
            "_TtC17NativeScriptTests12TNSSwiftLike",
            "NativeScriptTests.TNSSwiftLike",
            "TNSSwiftLike"
        ].indexOf(className) !== -1).toBe(true);
    });
});
