describe("Tests overload resolution with same-arity reference overloads", function () {
    it("selects the String overload when metadata also contains unrelated reference overloads", function () {
        var builder = new com.tns.tests.OverloadResolutionFixture.Builder();
        var result;
        var error;

        try {
            result = builder.url("https://example.com");
        } catch (e) {
            error = e;
        }

        expect(error).toBeUndefined();
        expect(result).toBe("string");
    });

    it("selects the most specific Java-object overload from factual runtime types", function () {
        var httpUrl = new com.tns.tests.OverloadResolutionFixture.HttpUrl();
        var javaUrl = new java.net.URL("https://example.com");

        expect(com.tns.tests.OverloadResolutionFixture.pick("value")).toBe("string");
        expect(com.tns.tests.OverloadResolutionFixture.pick(httpUrl)).toBe("http-url");

        var builder = new com.tns.tests.OverloadResolutionFixture.Builder();
        expect(builder.url(httpUrl)).toBe("http-url");
        expect(builder.url(javaUrl)).toBe("java-url");
    });

    it("resolves a real external-library overload when metadata exposes an incomplete candidate set", function () {
        var builder = new okhttp3.Request.Builder();
        var request;
        var error;

        try {
            request = builder.url("https://example.com").build();
        } catch (e) {
            error = e;
        }

        expect(error).toBeUndefined();
        expect(request.url().toString()).toBe("https://example.com/");
    });

    it("resolves String, CharSequence, and Object overloads by runtime facts", function () {
        expect(com.tns.tests.OverloadResolutionFixture.text("value")).toBe("string");
        expect(com.tns.tests.OverloadResolutionFixture.text(new java.lang.String("value"))).toBe("string");
    });

    it("prefers an implemented interface over Object for a Java proxy", function () {
        var marker = new com.tns.tests.OverloadResolutionFixture.MarkerImpl();
        expect(com.tns.tests.OverloadResolutionFixture.marker(marker)).toBe("marker");
    });

    it("resolves numeric overloads without choosing an incompatible signature", function () {
        expect(com.tns.tests.OverloadResolutionFixture.numeric(java.lang.Integer.valueOf(7))).toBe("int");
        expect(com.tns.tests.OverloadResolutionFixture.numeric(long(7))).toBe("long");
        expect(com.tns.tests.OverloadResolutionFixture.numeric(double(7))).toBe("double");
    });

    it("resolves array overloads from the Java array runtime type", function () {
        var strings = Array.create("java.lang.String", 1);
        var objects = Array.create(java.lang.Object, 1);

        expect(com.tns.tests.OverloadResolutionFixture.array(strings)).toBe("string-array");
        expect(com.tns.tests.OverloadResolutionFixture.array(objects)).toBe("object-array");
    });
});
