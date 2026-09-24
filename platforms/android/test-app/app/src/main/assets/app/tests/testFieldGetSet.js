describe("Tests final fields set", function () {
		
	it("When trying to set a final field throw exception", function () {
		
		var exceptionCaught = false;
		try
		{
			com.tns.tests.Button1.STATIC_IMAGE_ID = "NEW STATIC IMAGE ID VALUE";	
		}
		catch (e)
		{
			exceptionCaught = true;
		}
		
		expect(exceptionCaught).toBe(true);
	});
	
	it("When setting a field with null it should return null object", function () {
		
		var dc = new com.tns.tests.DummyClass();
		
		dc.nameField = null;
		var s = dc.nameField;
		
		expect(s).toBe(null);
	});

    it("When setting a field with undefined it should return null object", function () {

        var dc = new com.tns.tests.DummyClass();

        dc.nameField = undefined;
        var s = dc.nameField;
        var isNull = dc.isNameFieldNull();

        expect(s).toBe(null);
        expect(isNull).toBe(true);
    });

    // The byte and short write paths tested a number argument with an inverted
    // condition, so writing a number stored 0 and only a non-number was read as
    // an int32. Nothing covered them, so it survived in both runtimes.
    it("When setting a byte field with a number it should store that number", function () {

        var dc = new com.tns.tests.DummyClass();

        dc.byteField = 42;

        expect(dc.byteField).toBe(42);
    });

    it("When setting a short field with a number it should store that number", function () {

        var dc = new com.tns.tests.DummyClass();

        dc.shortField = 1234;

        expect(dc.shortField).toBe(1234);
    });

    it("When setting a static byte field with a number it should store that number", function () {

        com.tns.tests.DummyClass.staticByteField = -7;

        expect(com.tns.tests.DummyClass.staticByteField).toBe(-7);
    });

    it("When setting a static short field with a number it should store that number", function () {

        com.tns.tests.DummyClass.staticShortField = -4321;

        expect(com.tns.tests.DummyClass.staticShortField).toBe(-4321);
    });
});