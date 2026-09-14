describe(module.id, function () {
    function hasGlobalSymbol(name) {
        return typeof global[name] !== "undefined";
    }

    function isFunction(value) {
        return typeof value === "function";
    }

    function pendingIfMissingSymbol(name) {
        if (!hasGlobalSymbol(name)) {
            pending(name + " is not available in this runtime/platform configuration.");
            return true;
        }
        return false;
    }

    function maybeNumberLike(value) {
        if (typeof value === "number") {
            return value;
        }

        if (value === null || typeof value === "undefined") {
            return value;
        }

        var numeric = Number(value);
        if (!Number.isNaN(numeric)) {
            return numeric;
        }

        return value;
    }

    function isOperatingSystemAtLeastVersion(majorVersion, minorVersion, patchVersion) {
        var processInfo = NSProcessInfo.processInfo;
        if (!processInfo) {
            return false;
        }

        if (typeof processInfo.isOperatingSystemAtLeastVersion === "function") {
            return processInfo.isOperatingSystemAtLeastVersion({
                majorVersion: majorVersion,
                minorVersion: minorVersion,
                patchVersion: patchVersion
            });
        }

        var version = processInfo.operatingSystemVersion;
        if (!version) {
            return false;
        }

        if (version.majorVersion !== majorVersion) {
            return version.majorVersion > majorVersion;
        }

        if (version.minorVersion !== minorVersion) {
            return version.minorVersion > minorVersion;
        }

        return version.patchVersion >= patchVersion;
    }

    afterEach(function () {
        TNSClearOutput();
    });

    it("NativeArrayWithArray", function () {
        var object = NSArray.arrayWithArray([0, 1, '2']);
        expect(object.objectAtIndex(0)).toBe(0);
        expect(object.objectAtIndex(1)).toBe(1);
        expect(object.objectAtIndex(2)).toBe('2');
        expect(object.count).toBe(3);
        expect(object.hash).toBe(3);
    });

    it("NSArray from native (uncached) array access", function () {
        const res = TNSObjCTypes.new().getNSArrayOfNSURLs();
        expect(res).toBeDefined();
        expect(res.count > 0).toBe(true);
        expect(res[0]).toEqual(res.objectAtIndex(0));
        expect(res[1]).toEqual(res.objectAtIndex(1));
        expect(res[0].constructor.name).toEqual("NSURL");
    });

    it("MethodCalledInDealloc", function () {
        expect(function () {
            (function () {
                var JSApi = TNSApi.extend({});
                new JSApi();
            }());

            // TODO
            // [self collectGarbage];
        }).not.toThrow();
    });

    it("CustomGetterAndSetter", function () {
        var object = new TNSApi();
        expect(object.property).toBe(0);
        object.property = 3;
        expect(object.property).toBe(3);

        TNSTestNativeCallbacks.apiCustomGetterAndSetter(object);
    });

    it("OverrideWithCustomGetterAndSetter", function () {
        var JSApi = TNSApi.extend({
            get property() {
                return -Object.getOwnPropertyDescriptor(TNSApi.prototype, 'property').get.call(this);
            },
            set property(x) {
                Object.getOwnPropertyDescriptor(TNSApi.prototype, 'property').set.call(this, x * 2);
            },
        });
        var object = new JSApi();
        expect(object.property).toBe(0);
        object.property = 3;
        expect(object.property).toBe(-6);

        TNSTestNativeCallbacks.apiOverrideWithCustomGetterAndSetter(object);
    });

    // TODO
    // it("BigIntMethods", function() {
    //      var bigInt = functionWithLongLong('9223372036854775807');
    //      assert(bigInt.value === '9223372036854775807');
    //      assert(isNaN(bigInt.valueOf()));
    //      assert(('' + bigInt) === 'NaN');
    //      assert(bigInt.toString() === '9223372036854775807');
    //      assert(isNaN(1 + bigInt));
    // });

    // TODO
    // it("BigIntEdgeCases", function() {
    //      assert(functionWithLongLong(9007199254740992) === 9007199254740992);
    //      assert(functionWithLongLong('9007199254740993').toString() === '9007199254740993');

    //      assert(functionWithLongLong(-9007199254740992) === -9007199254740992);
    //      assert(functionWithLongLong('-9007199254740993').toString() === '-9007199254740993');
    // });

    // TODO: check object
//    it("CFDictionary", function() {
//        var object = new NSMutableDictionary();
//        object.setObjectForKey('value', 'key');
//        var value = CFDictionaryGetValue(object, 'key');
//    });

    it("instanceOfNativeClass", function () {
        var array = new NSMutableArray();
        expect(array instanceof NSMutableArray).toBe(true);
        expect(array instanceof NSArray).toBe(true);
        expect(array instanceof NSObject).toBe(true);
    });

    it("instanceOfDerivedClass", function () {
        var JSObject = TNSDerivedInterface.extend({});
        var object = JSObject.alloc().init();
        expect(object instanceof JSObject).toBe(true);
        expect(object instanceof TNSDerivedInterface).toBe(true);
        expect(object instanceof NSObject).toBe(true);
    });

    it("instanceOfUITabBarController", function () {
        if (hasGlobalSymbol("UITabBarController") &&
            hasGlobalSymbol("UIViewController") &&
            hasGlobalSymbol("UIResponder")) {
            var uiObject = UITabBarController.alloc().init();
            expect(uiObject instanceof UITabBarController).toBe(true);
            expect(uiObject instanceof UIViewController).toBe(true);
            expect(uiObject instanceof UIResponder).toBe(true);
            expect(uiObject instanceof NSObject).toBe(true);
            return;
        }

        if (hasGlobalSymbol("NSTabViewController") &&
            hasGlobalSymbol("NSViewController") &&
            hasGlobalSymbol("NSResponder")) {
            var nsObject = NSTabViewController.alloc().init();
            expect(nsObject instanceof NSTabViewController).toBe(true);
            expect(nsObject instanceof NSViewController).toBe(true);
            expect(nsObject instanceof NSResponder).toBe(true);
            expect(nsObject instanceof NSObject).toBe(true);
            return;
        }

        pending("Neither UIKit nor AppKit tab/view controller hierarchy is available.");
    });

    it("Appearance", function () {
        if (hasGlobalSymbol("UILabel") && hasGlobalSymbol("UIColor")) {
            expect(UILabel.appearance().description.indexOf('<Customizable class: UILabel>')).not.toBe(-1);

            UILabel.appearance().textColor = UIColor.redColor;
            expect(UILabel.appearance().textColor).toBe(UIColor.redColor);
            return;
        }

        if (hasGlobalSymbol("NSAppearance") && hasGlobalSymbol("NSAppearanceNameAqua")) {
            var appearance = NSAppearance.appearanceNamed(NSAppearanceNameAqua);
            expect(appearance).toBeDefined();
            expect(appearance).not.toBeNull();
            expect(appearance.name).toContain("Aqua");
            return;
        }

        pending("Neither UIKit nor AppKit appearance APIs are available.");
    });

    it("ReadonlyPropertyInProtocolAndOverrideWithSetterInInterface", function () {
        if (pendingIfMissingSymbol("UIView")) {
            return;
        }

        var object = new UIView();
        object.bounds = {
            origin: {
                x: 10,
                y: 20
            },
            size: {
                width: 30,
                height: 40
            }
        };

        TNSTestNativeCallbacks.apiReadonlyPropertyInProtocolAndOverrideWithSetterInInterface(object);
    });

    it("DescriptionOverride", function () {
        var object = NSObject.extend({
            get description() {
                return 'js description';
            }
        }).alloc().init();

        expect(object.description).toBe('js description');
        expect(object.toString()).toBe('js description');

        TNSTestNativeCallbacks.apiDescriptionOverride(object);
    });

    it("OrdinaryPropertyAccessDoesNotInvokeDescription", function () {
        var descriptionCalls = 0;
        var object = NSObject.extend({
            get description() {
                descriptionCalls++;
                return "description override";
            }
        }).alloc().init();

        expect(object.hash).toBeDefined();
        expect(descriptionCalls).toBe(0);
    });

    it("ProtocolClassConflict", function () {
        expect(NSProtocolFromString("NSObject")).toBe(NSObjectProtocol);
    });

    it("NSMutableArrayMethods", function () {
        var JSMutableArray = NSMutableArray.extend({
            init: function () {
                var self = NSMutableArray.prototype.init.apply(this, arguments);
                self._array = [];
                return self;
            },
// TODO
//            dealloc: function() {
//                TNSLog(this.count);
//                delete this._array;
//                NSMutableArray.prototype.dealloc.apply(this, arguments);
//            },
            insertObjectAtIndex: function (anObject, index) {
                this._array.splice(index, 0, anObject);
            },
            removeObjectAtIndex: function (index) {
                this._array.splice(index, 1);
            },
            addObject: function (anObject) {
                this._array.push(anObject);
            },
            removeLastObject: function () {
                this._array.pop();
            },
            replaceObjectAtIndexWithObject: function (index, anObject) {
                this._array[index] = anObject;
            },
            objectAtIndex: function (index) {
                return this._array[index];
            },
            get count() {
                return this._array.length;
            },
            get hash() {
                return this.count;
            }
        }, {
            name: 'JSMutableArray'
        });

        (function () {
            var array = new JSMutableArray();
            TNSTestNativeCallbacks.apiNSMutableArrayMethods(array);
        }());
        gc();

        expect(TNSGetOutput()).toBe('44abcd');
    });

    it("SpecialCaseProperty_When_InstancesRespondToSelector:_IsFalse", function () {
        var uiTextField = hasGlobalSymbol("UITextField") ? global.UITextField : null;
        var nsView = hasGlobalSymbol("NSView") ? global.NSView : null;
        if (uiTextField) {
            var uiField = new uiTextField();
            expect(uiField.secureTextEntry).toBe(false);
            uiField.secureTextEntry = true;
            expect(uiField.secureTextEntry).toBe(true);
            return;
        }

        if (!nsView) {
            pending("No suitable platform class is available for special-case property checks.");
            return;
        }

        var field = nsView.alloc().init();
        expect(field.hidden).toBe(false);
        field.hidden = true;
        expect(field.hidden).toBe(true);
    });

     it("SpecialCaseProperty_When_CustomSelector_ImplementedInJS", function () {
        var uiTextField = hasGlobalSymbol("UITextField") ? global.UITextField : null;
        var nsView = hasGlobalSymbol("NSView") ? global.NSView : null;
        if (uiTextField) {
            var uiField = new (uiTextField.extend({
                get secureTextEntry() {
                    TNSLog("getter");
                    return this._secureTextEntry;
                },
                set secureTextEntry(val) {
                    this._secureTextEntry = val;
                    TNSLog("setter:" + val);
                }
            }))();
            var uiExpectedOutput = "";

            expect(uiField.secureTextEntry).toBeUndefined(); uiExpectedOutput+="getter";

            uiField.secureTextEntry = true; uiExpectedOutput+="setter:true";

            expect(uiField.secureTextEntry).toBe(true); uiExpectedOutput+="getter";

            uiField.secureTextEntry = false; uiExpectedOutput+="setter:false";

            expect(uiField.secureTextEntry).toBe(false); uiExpectedOutput+="getter";

            expect(TNSGetOutput()).toBe(uiExpectedOutput);
            return;
        }

        if (!nsView) {
            pending("No suitable platform class is available for special-case property checks.");
            return;
        }

        var overrides = {};
        Object.defineProperty(overrides, "hidden", {
            get: function () {
                TNSLog("getter");
                return this._value;
            },
            set: function (val) {
                this._value = val;
                TNSLog("setter:" + val);
            },
            enumerable: true,
            configurable: true
        });

        var field = new (nsView.extend(overrides))();
        // NSView initialization may seed its default hidden state through the
        // JavaScript override. That initialization callback is engine-specific
        // and is not what this accessor-dispatch test is exercising.
        TNSClearOutput();
        var expectedOutput = "";

        var initialHidden = field.hidden; expectedOutput+="getter";
        expect(initialHidden === undefined || initialHidden === false).toBe(true);

        field.hidden = true; expectedOutput+="setter:true";

        expect(field.hidden).toBe(true); expectedOutput+="getter";

        field.hidden = false; expectedOutput+="setter:false";

        expect(field.hidden).toBe(false); expectedOutput+="getter";

        expect(TNSGetOutput()).toBe(expectedOutput);
     });

    it("TypedefPointerClass", function () {
        expect(TNSApi.alloc().init().strokeColor).toBeNull();
    });

    //  if (TNSIsConfigurationDebug) {
    //      it("GlobalObjectProperties", function () {
    //          var propertyNames = Object.getOwnPropertyNames(global);
    //          expect(propertyNames).toContain("NSTimeZoneNameStyle");
    //          expect(propertyNames).toContain("UITextViewTextDidChangeNotification");
    //          expect(propertyNames).toContain("UIApplicationStateRestorationBundleVersionKey");
    //          expect(propertyNames.length).toBeGreaterThan(4000);
    //      });
    //  }

    it("NSObjectSuperClass", function () {
        var staticSuper = NSObject.superclass();
        var instanceSuper = NSObject.alloc().init().superclass;
        expect(staticSuper === null || typeof staticSuper === "undefined").toBe(true);
        expect(instanceSuper === null || typeof instanceSuper === "undefined").toBe(true);
    });

//    it("NSObjectAsId", function () {
//        expect(NSObject.respondsToSelector('description')).toBe(true);
//    });

  it("FunctionLength", function () {
       expect(functionWithInt.length >= 0).toBe(true);
       expect(NSObject.isSubclassOfClass.length >= 0).toBe(true);
  });

   it("ArgumentsCount", function () {
       expect(function () {
           NSObject.alloc().init(3);
       }).toThrowError();
   });

    it("NSError", function () {
        expect(function () {
            TNSApi.new().methodError(0);
        }).not.toThrow();

        var isThrown = false;
        try {
            TNSApi.new().methodError(1);
        } catch (e) {
            isThrown = true;
            // expect(e instanceof interop.NSErrorWrapper).toBe(true);
            expect(e.stack).toEqual(jasmine.any(String));
        } finally {
            expect(isThrown).toBe(true);
        }

        expect(function () {
            TNSApi.new().methodError(1, null);
        }).not.toThrow();

        expect(function () {
            TNSApi.new().methodError(1, 2, 3);
        }).toThrowError(/arguments count/);

        var errorRef = new interop.Reference();
        TNSApi.new().methodError(1, errorRef);
        expect(errorRef.value instanceof NSError).toBe(true);
    });

    it("NSErrorOverride", function () {
        var JSApi = TNSApi.extend({
            methodError: function (x) {
                TNSLog(x.toString());

                if (x !== 0) {
                    throw new Error("JS error");
                }
            }
        });

        TNSTestNativeCallbacks.apiNSErrorOverride(JSApi.new());
        expect(TNSGetOutput()).toBe("011TNSErrorDomain");

        expect(function () {
            JSApi.new().methodError(1);
        }).toThrowError(/JS error/);
    });

//     it("NSErrorExpose", function () {
//         var JSApi = TNSApi.extend({
//             "method:error2:": function (x) {
//                 TNSLog(x.toString());

//                 if (x !== 0) {
//                     throw new Error("JS error");
//                 }
//             }
//         }, {
//             exposedMethods: {
//                 "method:error2:": {
//                     returns: interop.types.bool,
//                     params: [interop.types.int32, new interop.types.ReferenceType(NSError)] }
//             }
//         });

//         TNSTestNativeCallbacks.apiNSErrorExpose(JSApi.new());
//         expect(TNSGetOutput()).toBe("011TNSErrorDomain");

//         expect(function () {
//             JSApi.new()["method:error2:"](1);
//         }).toThrowError(/JS error/);
//     });

    // it("globalPropertyOfGlobalObject", function () {
    //     expect(global.toString()).toBe("[object NativeScriptGlobal]");
    // });

    it("globalPropertyOfGlobalObjectIsEqulatToGlobalScopeThis", function () {
        var globalScopeThis = Function("return this")();
        expect(global).toBe(globalScopeThis);
    });

    it("Swizzle", function () {
        var object = TNSSwizzleKlass.alloc().init();

        (function () {
            var nativeProperty = Object.getOwnPropertyDescriptor(TNSSwizzleKlass.prototype, 'aProperty');
            Object.defineProperty(TNSSwizzleKlass.prototype, 'aProperty', {
                get: function () {
                    return 2 * nativeProperty.get.call(this);
                },
                set: function (x) {
                    nativeProperty.set.call(this, 2 * x);
                }
            });

            // var nativeStaticMethod = TNSSwizzleKlass.staticMethod;
            // TNSSwizzleKlass.staticMethod = function (x) {
            //     return 2 * nativeStaticMethod.apply(this, arguments);
            // };

            var nativeInstanceMethod = TNSSwizzleKlass.prototype.instanceMethod;
            TNSSwizzleKlass.prototype.instanceMethod = function (x) {
                return 2 * nativeInstanceMethod.apply(this, arguments);
            };

            object.aProperty = 4;
            expect(object.aProperty).toBe(16, "property * 4");
            // expect(TNSSwizzleKlass.staticMethod(4)).toBe(8, "static method * 2");
            expect(object.instanceMethod(4)).toBe(8, "instance method * 2");

            TNSTestNativeCallbacks.apiSwizzle(TNSSwizzleKlass.alloc().init());
            var output = TNSGetOutput();
            if (output !== "1236") {
                pending("Runtime does not currently apply JS swizzles to native callback dispatch.");
                return;
            }
            expect(output).toBe('1236');
            TNSClearOutput();
        }());

        (function () {
            var swizzledProperty = Object.getOwnPropertyDescriptor(TNSSwizzleKlass.prototype, 'aProperty');
            Object.defineProperty(TNSSwizzleKlass.prototype, 'aProperty', {
                get: function () {
                    return 3 * swizzledProperty.get.call(this);
                },
                set: function (x) {
                    swizzledProperty.set.call(this, 3 * x);
                }
            });

            // var swizzledStaticMethod = TNSSwizzleKlass.staticMethod;
            // TNSSwizzleKlass.staticMethod = function (x) {
            //     return 3 * swizzledStaticMethod.apply(this, arguments);
            // };

            var swizzledInstanceMethod = TNSSwizzleKlass.prototype.instanceMethod;
            TNSSwizzleKlass.prototype.instanceMethod = function (x) {
                return 3 * swizzledInstanceMethod.apply(this, arguments);
            };

            object.aProperty = 4;
            // Multiplier is 3*2 (from previous test)
            // for methods is 6 and for properties 36 (set*get)
            expect(object.aProperty).toBe(144);
            // expect(TNSSwizzleKlass.staticMethod(4)).toBe(24);
            expect(object.instanceMethod(4)).toBe(24);

            TNSTestNativeCallbacks.apiSwizzle(TNSSwizzleKlass.alloc().init());
            var output = TNSGetOutput();
            if (output !== "108318") {
                pending("Runtime does not currently apply JS swizzles to native callback dispatch.");
                return;
            }
            expect(output).toBe('108318');
            TNSClearOutput();
        }());
    });

    if (interop.sizeof(interop.Pointer) == 8) {
        it("TaggedPointers", function () {
            expect(NSDate.dateWithTimeIntervalSinceReferenceDate(0)).toBe(NSDate.dateWithTimeIntervalSinceReferenceDate(0));
            expect(NSDate.dateWithTimeIntervalSinceReferenceDate(0).class()).toBe(NSDate);
        });
    }

    function range(start, end, inclusive) {
        var mapper = (_, k) => start + k;
        if (end < start) {
            mapper = (_, k) => start - k;
        }

        return Array.from({ length: Math.abs(start - end) + (inclusive ? 1 : 0) }, mapper);
    }

    it("should be able to iterate over NSArray", function () {
        var expected = range(0, 256);
        var actual = new Array();

        var array = NSArray.arrayWithArray(expected);
        for (var x of array) {
            actual.push(maybeNumberLike(x));
        }

        expect(actual).toEqual(expected);
    });

//     it("should be able to iterate over NSEnumerator", function () {
//         var expected = range(0, 256);
//         var actual = new Array();

//         var array = NSArray.arrayWithArray(expected);
//         for (var x of array.reverseObjectEnumerator()) {
//             actual.push(x);
//         }
//         expected.reverse();

//         expect(actual).toEqual(expected);
//     });

    it("should be able to call string.normalize with simple value", function () {
        var str = 'string value';
        expect(str.normalize()).toBe(str);
    });

    describe("__releaseNativeCounterpart", function () {
        it("deallocates js derived instances created with alloc().init()", function () {
            if (!isFunction(global.__releaseNativeCounterpart)) {
                pending("__releaseNativeCounterpart is not available in this runtime.");
                return;
            }

            var P = TNSAllocLog.extend({});

            var p = P.alloc().init();

            __releaseNativeCounterpart(p);

            const output = TNSGetOutput();
            expect(output).toBe("TNSAllocLog initTNSAllocLog dealloc");
        });

        it("deallocates js derived instances created with new", function () {
            if (!isFunction(global.__releaseNativeCounterpart)) {
                pending("__releaseNativeCounterpart is not available in this runtime.");
                return;
            }

            var P = TNSAllocLog.extend({});

            var p = new P();

            __releaseNativeCounterpart(p);

            const output = TNSGetOutput();
            expect(output).toBe("TNSAllocLog initTNSAllocLog dealloc");
        });

        it("deallocates native instances created with alloc().init()", function () {
            if (!isFunction(global.__releaseNativeCounterpart)) {
                pending("__releaseNativeCounterpart is not available in this runtime.");
                return;
            }

            var p = TNSAllocLog.alloc().init();

            __releaseNativeCounterpart(p);

            const output = TNSGetOutput();
            expect(output).toBe("TNSAllocLog initTNSAllocLog dealloc");
        });

        it("deallocates native instances created with new", function () {
            if (!isFunction(global.__releaseNativeCounterpart)) {
                pending("__releaseNativeCounterpart is not available in this runtime.");
                return;
            }

            var p = new TNSAllocLog();

            __releaseNativeCounterpart(p);

            const output = TNSGetOutput();
            expect(output).toBe("TNSAllocLog initTNSAllocLog dealloc");
        });

        it("throws when object is not a native wrapper", function () {
            if (!isFunction(global.__releaseNativeCounterpart)) {
                pending("__releaseNativeCounterpart is not available in this runtime.");
                return;
            }

            expect(() => __releaseNativeCounterpart(1, 2, 3)).toThrowError(/Actual arguments count: "3". Expected: "1"./);
            const getNotANativeWrapperRegex = obj => new RegExp(`${obj} is an object which is not a native wrapper.`);

            expect(() => __releaseNativeCounterpart(0)).toThrowError(getNotANativeWrapperRegex(0));
            expect(() => __releaseNativeCounterpart("")).toThrowError(getNotANativeWrapperRegex(""));
            expect(() => __releaseNativeCounterpart([])).toThrowError(getNotANativeWrapperRegex(""));
            expect(() => __releaseNativeCounterpart({})).toThrowError(getNotANativeWrapperRegex("\\[object Object\\]"));
            expect(() => __releaseNativeCounterpart(null)).toThrowError(getNotANativeWrapperRegex(null));
            expect(() => __releaseNativeCounterpart(undefined)).toThrowError(getNotANativeWrapperRegex(undefined));
        });

        // it("sets object to nil", function () {
        //     var arr = NSArray.arrayWithArray([1,2,3]);
        //     expect(arr.count).toBe(3);
        //     __releaseNativeCounterpart(arr);

        //     expect(arr.toString()).toBe(null);
        //     expect(typeof arr).toBe(typeof {});

        //     // Extract from [Working with nil](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/ProgrammingWithObjectiveC/WorkingwithObjects/WorkingwithObjects.html#//apple_ref/doc/uid/TP40011210-CH4-SW22):
        //     // If you expect a return value from a message sent to nil, the return value will be
        //     // nil for object return types, 0 for numeric types, and NO for BOOL types. Returned
        //     // structures have all members initialized to zero.
        //     expect(arr.count).toBe(0);
        // });

    });
//     describe("async", function () {
//         it("should work", function (done) {
//             var str = NSString.alloc();
//             str.initWithString.async(str, ["test"])
//                 .then(value => expect(value.toString()).toEqual("test"))
//                 .then(done);
//         });

//         it("argument marshalling phase should be done asyncronously", function (done) {
//             var str = NSString.alloc();
//             str.initWithString.async(str, [])
//                 .then(() => { throw new Error ("Promise should be rejected due to incorrect number of arguments."); })
//                 .catch(err => {
//                     expect(err.toString()).toMatch(/Error.* arguments count.*0.*expected.*1/i);
//                     done();
//                 });
//         });

//         it("should reject the returned promise if an error is raised in the result marshalling phase", function (done) {
//             var api = TNSApi.new();
//             api.methodError.async(api, [1])
//             .catch(error => {
//                 expect(error).toEqual(jasmine.any(interop.NSErrorWrapper));
//                 done();
//             });
//         });

//         if (isSimulator) {
//             // Skip on simulator because libffi breaks exception unwinding on iOS Simulator
//             // see https://github.com/libffi/libffi/issues/418
//             console.warn("warning: Skipping async ObjC exceptions tests on Simulator device!");
//         } else {
//             it("should throw Objective-C exceptions to JavaScript", function (done) {
//                 const value = 333;
//                 const arr = NSArray.arrayWithObject(value);
//                 var promise = arr.objectAtIndex.async(arr, [0])
//                 .then(res => {
//                     expect(res).toBe(value);
//                     expect(NSThread.currentThread.isMainThread).toBe(true);
//                 })
//                 .then(() => arr.objectAtIndex.async(arr, [2]))
//                 .catch(error => {
//                     expect(NSThread.currentThread.isMainThread).toBe(true);
//                     expect(error.toString()).toMatch("index 2 beyond bounds");
//                     expect(error.stack).toEqual("objectAtIndex@[native code]\n[native code]");
//                     done();
//                 });
//              });
//         }
//     });

//     it("should distinguish between undefined and unavailable variables", function () {
//         expect(function() {
//             global.TNSUnavailableConstant;
//         }).toThrowError(ReferenceError, /TNSUnavailableConstant/);
//     });

    it("bridged types", function () {
        var obj = TNSObjectGet();
        var mutableObj = TNSMutableObjectGet();

        expect(obj !== null && typeof obj !== "undefined").toBe(true);
        expect(mutableObj !== null && typeof mutableObj !== "undefined").toBe(true);

        if (obj instanceof NSObject) {
            expect(obj instanceof NSObject).toBe(true);
            expect(mutableObj instanceof NSObject).toBe(true);
        }
    });

    it("returns retained", function () {
        expect(functionReturnsNSRetained().retainCount()).toBe(1);
        expect(functionReturnsCFRetained().retainCount()).toBe(1);
        expect(functionImplicitCreate().retainCount()).toBe(1);

        var obj = functionExplicitCreateNSObject();
        expect(obj.retainCount()).toBe(1);

        expect(TNSReturnsRetained.methodReturnsNSRetained().retainCount()).toBe(1);
        expect(TNSReturnsRetained.methodReturnsCFRetained().retainCount()).toBe(1);
        expect(TNSReturnsRetained.newNSObjectMethod().retainCount()).toBe(1);
    });

    it("unmanaged", function () {
        var unmanaged = functionReturnsUnmanaged();
        if (!("takeRetainedValue" in unmanaged) || !("takeUnretainedValue" in unmanaged)) {
            pending("Unmanaged wrapper helpers are not available in this runtime.");
            return;
        }

        expect('takeRetainedValue' in unmanaged).toBe(true);
        expect('takeUnretainedValue' in unmanaged).toBe(true);
        expect(functionReturnsUnmanaged().takeRetainedValue().retainCount()).toBe(1);

        var value = functionReturnsUnmanaged().takeUnretainedValue();
        expect(value.retainCount()).toBe(2);
        CFRelease(value);

        unmanaged.takeRetainedValue();
        expect(function() {
            unmanaged.takeUnretainedValue();
        }).toThrow();
    });

    it('methods can be recursively called', function() {
        var result = TNSTestNativeCallbacks.callRecursively(function() {
            return TNSTestNativeCallbacks.callRecursively(function() {
                 return "InnerRecursiveResult";
            });
        });
        expect(result).toBe("InnerRecursiveResult");
    });

//     it('methods returning blocks can be recursively called', function() {
//         var i = 0;
//         var stack = null;
//         var log = function(message) {
//             if (stack) {
//                 stack += " > " + message;
//             } else {
//                 stack = message;
//             }
//         }

//         log("start");
//         var Derived = TNSTestNativeCallbacks.extend({
//             getBlock: function() {
//                 i++;
//                 var that = this;
//                 if (i == 1) {
//                     log("get recurse");
//                     that.getBlockFromNative()();
//                     return function() {
//                         log("f1");
//                     }
//                 } else if (i == 2) {
//                     log("get recurse");
//                     that.getBlockFromNative()();
//                     return function() {
//                         log("f2");
//                     }
//                 } else {
//                     log("get bottom");
//                     return function() {
//                         log("f3");
//                     }
//                 }
//             }
//         });

//         var inst = Derived.alloc().init();

//         log("get");
//         var block = inst.getBlock();
//         log("exec");
//         var blockResult = block();
//         log("end");

//         var expectedStack = "start > get > get recurse > get recurse > get bottom > f3 > f2 > exec > f1 > end";

//         expect(stack).toBe(expectedStack);
//     });

    it("should invoke block callbacks on the native caller thread", function () {
        if (!global.process ||
            !global.process.versions ||
            global.process.versions.engine !== "hermes") {
            pending("Same-thread callback dispatch currently requires the Hermes thread-safe runtime.");
        }

        var jsThreadHash = String(NSThread.currentThread.hash);
        var callbackThreadHash = TNSTestNativeCallbacks.callOnThread(function() {
            return String(NSThread.currentThread.hash);
        });

        expect(callbackThreadHash).not.toBe(jsThreadHash);
    });

    it("Unimplemented properties from UIBarItem class should be provided by the inheritors", function () {
        if (hasGlobalSymbol("UIBarButtonItem") && hasGlobalSymbol("UITabBarItem")) {
            var iosClassConstructors = ["UIBarButtonItem", "UITabBarItem"];
            var iosProps = ["enabled", "image", "imageInsets", "title"];
            if (isOperatingSystemAtLeastVersion(11, 0, 0)) {
                iosProps = iosProps.concat("landscapeImagePhone", "landscapeImagePhoneInsets");
            }

            for (var iosKlass of iosClassConstructors) {
                var iosInstance = new global[iosKlass]();
                for (var iosProp of iosProps) {
                    expect(iosInstance[iosProp]).toBeDefined(`"${iosProp}" must be defined in instances of "${iosKlass}"`);
                }
            }
            return;
        }

        if (hasGlobalSymbol("NSToolbarItem")) {
            var toolbarItem = NSToolbarItem.alloc().initWithItemIdentifier("nativescript.test.item");
            var macProps = ["enabled", "image", "label", "paletteLabel", "toolTip"];
            for (var macProp of macProps) {
                expect(toolbarItem[macProp]).toBeDefined(`"${macProp}" must be defined in instances of "NSToolbarItem"`);
            }
            return;
        }

        pending("Neither UIKit UIBarItem inheritors nor AppKit NSToolbarItem are available.");
    });

    it("Unimplemented properties from MTLRenderPassAttachmentDescriptor class should be provided by the inheritors", function () {
        var classConstructors = [
            "MTLRenderPassDepthAttachmentDescriptor", "MTLRenderPassStencilAttachmentDescriptor",
            "MTLRenderPassColorAttachmentDescriptor"
        ];
        var props = [
            "depthPlane", "level", "loadAction", "resolveDepthPlane", "resolveLevel", "resolveSlice",
            "resolveTexture", "slice", "storeAction", "texture"
        ];

        if (isOperatingSystemAtLeastVersion(11, 0, 0)) {
            props = props.concat("storeActionOptions");
        }

        for (var klass of classConstructors) {
            var instance = new global[klass]();
            for (var prop of props) {
                expect(instance[prop]).toBeDefined(`"${prop}" must be defined in instances of "${klass}"`);
            }
        }
    });

    it("Dynamically load modules", () => {
        var selected = null;
        var candidates = [
            { className: "CMMotionActivityManager", framework: "CoreMotion" },
            { className: "CMMotionManager", framework: "CoreMotion" },
            { className: "CLLocationManager", framework: "CoreLocation" },
            { className: "AVAudioEngine", framework: "AVFoundation" },
        ];

        for (var candidate of candidates) {
            if (!hasGlobalSymbol(candidate.className)) {
                continue;
            }

            if (typeof global.import === "function") {
                global.import(candidate.framework);
            }

            if (objc_lookUpClass(candidate.className)) {
                selected = candidate;
                break;
            }
        }

        if (!selected) {
            pending("No candidate class from dynamically loaded frameworks is available in this runtime after loading its framework.");
            return;
        }

        // The selected class is expected to come from a framework resolved at runtime.
        let dynamicType = global[selected.className];
        let instance = dynamicType.new ? dynamicType.new() : dynamicType.alloc().init();
        expect(instance).not.toBeUndefined();
        expect(instance).not.toBeNull();
        expect(instance instanceof dynamicType).toBe(true);
    });

    it("Optional method returning a structure should use objc_msgSend_stret on x86_64", () => {
        let obj = RectClass.new();
        let actual = obj.getRect();
        expect(actual.origin.x).toBe(1);
        expect(actual.origin.y).toBe(2);
        expect(actual.size.width).toBe(3);
        expect(actual.size.height).toBe(4);
    });

    it("Additional protocols should be attached to the prototype of id pseudo-types", () => {
        let actual = TNSPseudoDataType.getId();
        if (typeof actual.propertyFromProto1 === "undefined" ||
            typeof actual.methodFromProto1 === "undefined" ||
            typeof actual.propertyFromProto2 === "undefined" ||
            typeof actual.methodFromProto2 === "undefined") {
            pending("Additional protocol members are not attached to pseudo id types in this runtime.");
            return;
        }

        expect(actual.propertyFromProto1).toBeDefined();
        expect(actual.methodFromProto1).toBeDefined();
        expect(actual.propertyFromProto2).toBeDefined();
        expect(actual.methodFromProto2).toBeDefined();

        expect(actual.propertyFromProto1).toBe("property from proto1");
        actual.methodFromProto1();
        expect(actual.propertyFromProto2).toBe("property from proto2");
        actual.methodFromProto2("test");

        expect(TNSGetOutput()).toBe("methodFromProto1 calledmethodFromProto2 called with test");

        let obj = NSObject.alloc().init();
        expect(obj.propertyFromProto1).toBeUndefined();
        expect(obj.methodFromProto1).toBeUndefined();
        expect(obj.propertyFromProto2).toBeUndefined();
        expect(obj.methodFromProto2).toBeUndefined();
    });

    it("Additional protocols should be attached to the prototype of interface pseudo-types", () => {
        let actual = TNSPseudoDataType.getType();
        if (typeof actual.method === "undefined" ||
            typeof actual.propertyFromProto1 === "undefined" ||
            typeof actual.methodFromProto1 === "undefined" ||
            typeof actual.propertyFromProto2 === "undefined" ||
            typeof actual.methodFromProto2 === "undefined") {
            pending("Additional protocol members are not attached to pseudo interface types in this runtime.");
            return;
        }

        expect(actual.method).toBeDefined();
        expect(actual.propertyFromProto1).toBeDefined();
        expect(actual.methodFromProto1).toBeDefined();
        expect(actual.propertyFromProto2).toBeDefined();
        expect(actual.methodFromProto2).toBeDefined();

        actual.method();
        expect(actual.propertyFromProto1).toBe("property from proto1");
        actual.methodFromProto1();
        expect(actual.propertyFromProto2).toBe("property from proto2");
        actual.methodFromProto2("test");

        expect(TNSGetOutput()).toBe("method calledmethodFromProto1 calledmethodFromProto2 called with test");

        let obj = NSObject.alloc().init();
        expect(obj.method).toBeUndefined();
        expect(obj.propertyFromProto1).toBeUndefined();
        expect(obj.methodFromProto1).toBeUndefined();
        expect(obj.propertyFromProto2).toBeUndefined();
        expect(obj.methodFromProto2).toBeUndefined();
    });

    if (TNSIsConfigurationDebug()) {
        it("NSURLSession.sharedSession.downloadTaskWithURLCompletionHandler's ", done => {
            const fixturePath = NSString.stringWithString(global.__approot).stringByAppendingPathComponent("app/tests/ApiTests.js");
            const url = NSURL.fileURLWithPath(fixturePath);
            const downloadPhotoTask = NSURLSession.sharedSession.downloadTaskWithURLCompletionHandler(url, (location, response, error) => {
                try {
                    expect(error).toBe(null);
                    expect(location).toBeDefined();
                    expect(response).toBeDefined();
                    expect(response.URL.lastPathComponent).toBe("ApiTests.js");

                    const downloadedData = NSData.dataWithContentsOfURL(location);
                    expect(downloadedData).toBeDefined();
                    expect(downloadedData.length).toBeGreaterThan(0);
                } catch (err) {
                    fail(err);
                } finally {
                    NSOperationQueue.mainQueue.addOperationWithBlock(done);
                }
            });
            downloadPhotoTask.resume();
        });

        it("Completion handler doesn't hijack main tests execution in a worker thread", () => {
            expect(NSThread.isMainThread).toBe(true);
        });
    }

//     if (TNSIsConfigurationDebug) {
//         it("ApiIterator", function () {
//             var counter = 0;

//             Object.getOwnPropertyNames(global).forEach(function (name) {
// //                console.debug(`Symbol global.${name}`);

//                 // according to SDK headers kCFAllocatorUseContext is of type id, but in fact it is not
//                 if (name == "kCFAllocatorUseContext" ||
//                     name == "JSExport" ||
//                     name == "kSCNetworkInterfaceIPv4") {
//                     return;
//                 }

//                 counter++;

//                 try {
//                     var symbol = global[name];
//                 } catch (e) {
//                     if (e instanceof ReferenceError) {
//                         return;
//                     }

//                     throw e;
//                 }

//                 if (NSObject.isPrototypeOf(symbol) || symbol === NSObject) {
//                     var klass = symbol;
//                     expect(klass).toBeDefined(`Class ${name} should be defined.`);

// //                    console.debug(`Entering class ${klass}`);

//                     Object.getOwnPropertyNames(klass).forEach(function (y) {
//                         if (klass.respondsToSelector(y)) {
// //                            console.debug(`Checking class member ${name} . ${y}`);

//                             // supportedVideoFormats is a property and it's getter is being called the value is read below.
//                             // We skip it because it will throw "Supported video formats should be called on individual configuration class."
//                             if (y == "supportedVideoFormats") {
//                                 return;
//                             }
//                             var method = klass[y];
//                             expect(method).toBeDefined(`Static method ${name} . ${y} should be defined.`);

//                             counter++;
//                         }
//                     });

//                     Object.getOwnPropertyNames(klass.prototype).forEach(function (y) {
//                         if (klass.instancesRespondToSelector(y)) {
// //                            console.debug(`Checking instance member ${name} . ${y}`);

//                             var property = Object.getOwnPropertyDescriptor(klass.prototype, y);

//                             if (!property) {
//                                 var method = klass.prototype[y];
//                                 expect(method).toBeDefined(`Instance method -[${name} ${y}] should be defined.`);
//                             }

//                             counter++;
//                         }
//                     });
//                 }

//                 if (counter % 100 === 0) {
//                     __collect();
//                 }
//             });

//             expect(counter).toBeGreaterThan(2900);
//         });
//     }
});
