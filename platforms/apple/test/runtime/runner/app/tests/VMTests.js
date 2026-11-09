function dynamicImport(specifier) {
    return (0, eval)("import(" + JSON.stringify(specifier) + ")");
}

describe("node:vm builtin", function () {
    it("compileFunction should preserve an explicit receiver", function () {
        const vm = require("node:vm");
        const compiled = vm.compileFunction("return this.value;", []);
        const receiver = { value: 42 };

        expect(compiled.call(receiver)).toBe(42);
        expect(compiled.apply(receiver)).toBe(42);
    });

    it("should resolve node:vm via ESM dynamic import", function (done) {
        dynamicImport("node:vm")
            .then(function (vm) {
                expect(vm).toBeDefined();
                expect(typeof vm.compileFunction).toBe("function");
                expect(typeof vm.createContext).toBe("function");
                done();
            })
            .catch(function (error) {
                fail(error);
                done();
            });
    });
});
