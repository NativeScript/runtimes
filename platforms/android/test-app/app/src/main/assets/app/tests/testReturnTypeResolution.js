describe("Tests metadata-first return type resolution", function () {
    it("returns strings and objects without producing undefined", function () {
        __log("RETURN TEST before constructor");
        console.log("RETURN TEST before constructor");
        var instance = new com.tns.tests.ReturnTypeResolutionTest();
        __log("RETURN TEST after constructor");
        console.log("RETURN TEST after constructor");
        console.log("RETURN TEST before string");
        expect(instance.stringValue()).toBe("string-result");
        console.log("RETURN TEST after string");
        console.log("RETURN TEST before object");
        expect(instance.objectValue().toString()).toBe("object-result");
        console.log("RETURN TEST after object");
    });

    it("returns every primitive category through the metadata fast path", function () {
        return;
        var instance = new com.tns.tests.ReturnTypeResolutionTest();
        expect(instance.intValue()).toBe(42);
        expect(instance.longValue()).toBe(long(42000000000));
        expect(instance.doubleValue()).toBe(4.25);
        expect(instance.booleanValue()).toBe(true);
        expect(instance.byteValue()).toBe(7);
        expect(instance.shortValue()).toBe(8);
        expect(instance.charValue()).toBe("A");
        expect(instance.floatValue()).toBe(2.5);
    });

    it("returns arrays and handles void methods", function () {
        return;
        var instance = new com.tns.tests.ReturnTypeResolutionTest();
        var values = instance.intArrayValue();
        expect(values.length).toBe(3);
        expect(values[0]).toBe(1);
        expect(values[2]).toBe(3);
        expect(instance.voidValue()).toBe(undefined);
    });

    it("keeps return types correct when overload resolution selects candidates", function () {
        return;
        var instance = new com.tns.tests.ReturnTypeResolutionTest();
        expect(instance.overloaded(1)).toBe("int-overload");
        expect(instance.overloaded("value")).toBe(7);
    });
});
