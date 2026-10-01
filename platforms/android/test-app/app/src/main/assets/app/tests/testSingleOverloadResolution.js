describe("Tests metadata-resolved single overload invocation", function () {
    var F = com.tns.tests.SingleOverloadResolutionFixture;

    it("calls a static method with null/undefined arguments, including null for primitives", function () {
        var ctx = new android.content.ContextWrapper(null);
        expect(F.htmlLike(ctx, "fonts", null, "<b>x</b>", false, -16776961))
            .toBe("true|fonts|null|<b>x</b>|false|-16776961");
        expect(F.htmlLike(ctx, "fonts", undefined, "<b>x</b>", true, undefined))
            .toBe("true|fonts|null|<b>x</b>|true|0");
        expect(F.htmlLike(null, null, null, null, false, 0))
            .toBe("false|null|null|null|false|0");
    });

    it("calls an instance method taking String[] with a plain JS array", function () {
        var f = new F();
        expect(f.requestLike(["a", "b"], 7)).toBe("a,b#7");
        expect(f.requestLike([], 1)).toBe("#1");
        expect(f.requestLike(null, 2)).toBe("null#2");
    });

    it("calls a static method taking int[] with a plain JS array", function () {
        expect(F.intArray([1, 2, 3])).toBe("sum=6");
    });

    it("passes JS values to Object, boxed and CharSequence parameters", function () {
        expect(F.objectParam(null)).toBe("null");
        expect(F.objectParam("s")).toBe("java.lang.String");
        expect(F.boxedInt(java.lang.Integer.valueOf(5))).toBe("boxed=5");
        expect(F.boxedInt(null)).toBe("boxed=null");
        expect(F.boxedBool(java.lang.Boolean.valueOf(true))).toBe("bool=true");
        expect(F.charSeq("abc")).toBe("cs=abc");
        expect(F.twoStrings(null, "b")).toBe("null+b");
    });

    it("passes a JS interface implementation", function () {
        var ran = false;
        var r = new java.lang.Runnable({ run: function () { ran = true; } });
        expect(F.runIt(r)).toBe("ran");
        expect(ran).toBe(true);
    });

    it("lets non-null arguments discriminate same-arity overloads when another is null", function () {
        expect(F.mixed(null, 5)).toBe("string-int");
        expect(F.mixed(null, "x")).toBe("string-string");
        expect(F.mixed("a", 5)).toBe("string-int");
    });

    it("throws instead of returning undefined when no overload matches", function () {
        var builder = new com.tns.tests.OverloadResolutionFixture.Builder();
        var error;
        var result;
        try {
            result = builder.url(123);
        } catch (e) {
            error = e;
        }
        expect(result).toBeUndefined();
        expect(error).toBeDefined();
    });
});
