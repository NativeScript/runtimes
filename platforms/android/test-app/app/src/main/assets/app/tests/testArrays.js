describe("Tests array operations", function () {
	
	var myCustomEquality = function(first, second) {
		return first == second;
	};
	
	beforeEach(function() {
		jasmine.addCustomEqualityTester(myCustomEquality);
	});

	xit("TestWorkingWithJavaArrayDoesNotMakeMemoryLeak", function () {
		__log("TEST: TestWorkingWithJavaArrayDoesNotMakeMemoryLeak");
		
		var size = 10 * 1024 * 1024;
		
		for (var i = 0; i < 100; i++) {
		
			var arr = java.lang.reflect.Array.newInstance(java.lang.Byte.class.getField("TYPE").get(null), size);
			
			var length = arr.length;
			
			expect(length).toEqual(size);
			
			arr[0] = 123;
			
			var el = arr[0];
			
			expect(el).toEqual(123);

			global.__releaseNativeCounterpart(arr);
		}
	});

	it("TestArraySize", function () {
		
		var size = 12345;
		
		var arr = java.lang.reflect.Array.newInstance(java.lang.Object.class, size);
		
		expect(arr.length).toBe(size);
	});
	
	it("TestArrayOperations", function () {

		// keep 'size' value large (>512) in order to test for JNI reference leaks as well
		var size = 10000;

		var arr = java.lang.reflect.Array.newInstance(java.lang.Integer.class, size);

		expect(arr.length).toBe(size);

		for (var i = 0; i < size; i++) {
			arr[i] = java.lang.Integer.valueOf(i);
		}

		for (var i = 0; i < size; i++) {
			var n = arr[i].intValue();
			expect(n).toBe(i);
		}
	});

	it("TestArrays", function () {
		
		__log("TEST: TestArrays");
		
		var MyButton = com.tns.tests.Button1.extend("MyButton639", {
			toString : function() {
			  	return "button1";	
			}
		});
		var tester = new MyButton(); 
		var instances = tester.getDummyInstances();
		
		var instanceFound = false;

		for (var i = 0; i < instances.length; i++)
		{
			if (instances[i].getName() == "second");
			{
				instanceFound = true;
			}
		}
		
		expect(instanceFound).toEqual(true);
		
		instances[0] = instances[1];
		
		var instances0name = instances[0].getName();
		var instances1name = instances[1].getName();
		
		expect(instances0name).toEqual(instances1name);
	});
	
	it("TestArrayLengthPropertyIsNumber", function () {
		
		__log("TEST: TestArrayLengthPropertyIsNumber");
		
		var expectedLength = 10;

		function getLength(x)
		{
			var arr = x.getIntArray1(expectedLength);
			
			return arr ? arr.length : 123456;
		}
		
		var MyButton = com.tns.tests.Button1.extend("MyButton680", {
			toString : function() {
			  	return "button1";	
			}
		});
		
		var count = getLength(new MyButton());
		
		expect(count).toBe(expectedLength);
		
	});

	describe("assigning values of another type to a primitive array element", function () {

		function createArray(wrapperName) {
			return java.lang.reflect.Array.newInstance(java.lang[wrapperName].class.getField("TYPE").get(null), 1);
		}

		it("converts the value to a number for float and double arrays", function () {
			["Float", "Double"].forEach(function (wrapperName) {
				var arr = createArray(wrapperName);

				arr[0] = undefined;
				expect(isNaN(arr[0])).toBe(true);

				arr[0] = null;
				expect(arr[0]).toBe(0);

				arr[0] = "3.5";
				expect(arr[0]).toBe(3.5);

				arr[0] = true;
				expect(arr[0]).toBe(1);

				arr[0] = { valueOf: function () { return 7; } };
				expect(arr[0]).toBe(7);
			});
		});

		it("converts the value to an integer for byte, short, int and long arrays", function () {
			["Byte", "Short", "Integer", "Long"].forEach(function (wrapperName) {
				var arr = createArray(wrapperName);

				arr[0] = undefined;
				expect(arr[0]).toBe(0);

				arr[0] = null;
				expect(arr[0]).toBe(0);

				arr[0] = "42";
				expect(arr[0]).toBe(42);

				arr[0] = true;
				expect(arr[0]).toBe(1);
			});
		});

		it("uses valueOf of an object assigned to byte, short and int arrays", function () {
			["Byte", "Short", "Integer"].forEach(function (wrapperName) {
				var arr = createArray(wrapperName);

				arr[0] = { valueOf: function () { return 7; } };
				expect(arr[0]).toBe(7);
			});
		});

		it("converts the value to a boolean for boolean arrays", function () {
			var arr = createArray("Boolean");

			arr[0] = "x";
			expect(arr[0]).toBe(true);

			arr[0] = 0;
			expect(arr[0]).toBe(false);

			arr[0] = undefined;
			expect(arr[0]).toBe(false);

			arr[0] = "";
			expect(arr[0]).toBe(false);
		});

		it("converts the index of getValueAtIndex and setValueAtIndex to a number", function () {
			var arr = java.lang.reflect.Array.newInstance(java.lang.Integer.class.getField("TYPE").get(null), 2);

			arr.setValueAtIndex("1", 5);
			expect(arr.getValueAtIndex(1)).toBe(5);
			expect(arr.getValueAtIndex("1")).toBe(5);
		});
	});

});
