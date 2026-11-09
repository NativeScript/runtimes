function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function assertEqual(actual, expected, message) {
  if (!Object.is(actual, expected)) {
    throw new Error(
      `${message}: expected ${String(expected)}, got ${String(actual)}`,
    );
  }
}

const vm = require("node:vm");
const vmAlias = require("vm");

assert(typeof vm.createContext === "function", "vm.createContext should exist");
assert(
  typeof vmAlias.runInContext === "function",
  "require('vm') should resolve the builtin module",
);

const sandbox = { count: 1 };
const context = vm.createContext(sandbox);

assert(context === sandbox, "createContext should return the sandbox");
assert(vm.isContext(sandbox) === true, "contextified sandbox should be tagged");
assert(vm.isContext({}) === false, "plain objects are not vm contexts");

const contextResult = vm.runInContext(
  "count += 2; sameGlobal = this === globalThis; result = count * 3; result",
  sandbox,
  { filename: "vm-context.js" },
);

assertEqual(contextResult, 9, "runInContext should return the last expression");
assertEqual(sandbox.count, 3, "runInContext should persist sandbox mutations");
assertEqual(
  sandbox.sameGlobal,
  true,
  "sandbox execution should bind this to globalThis",
);
assertEqual(sandbox.result, 9, "runInContext should sync new globals back");

const newContextSandbox = { total: 4 };
const newContextResult = vm.runInNewContext(
  "total += 5; status = 'ok'; total",
  newContextSandbox,
  { filename: "vm-new-context.js" },
);

assertEqual(
  newContextResult,
  9,
  "runInNewContext should evaluate against a fresh sandbox",
);
assertEqual(
  newContextSandbox.total,
  9,
  "runInNewContext should write mutations back to the provided sandbox",
);
assertEqual(
  newContextSandbox.status,
  "ok",
  "runInNewContext should preserve newly assigned globals",
);

globalThis.__vmGlobalValue = 10;
const thisContextResult = vm.runInThisContext(
  "globalThis.__vmGlobalValue += 7; globalThis.__vmGlobalValue",
  { filename: "vm-this-context.js" },
);
assertEqual(
  thisContextResult,
  17,
  "runInThisContext should execute against the current global",
);
assertEqual(
  globalThis.__vmGlobalValue,
  17,
  "runInThisContext should mutate the active global object",
);
delete globalThis.__vmGlobalValue;

const script = new vm.Script("value += 3; marker = 'script'; value", {
  filename: "vm-script.js",
});
const scriptSandbox = vm.createContext({ value: 5 });
const scriptResult = script.runInContext(scriptSandbox);

assertEqual(scriptResult, 8, "Script.runInContext should execute stored code");
assertEqual(scriptSandbox.value, 8, "Script.runInContext should update sandbox");
assertEqual(
  scriptSandbox.marker,
  "script",
  "Script.runInContext should sync new globals",
);

const scriptNewContextSandbox = { value: 2 };
const scriptNewContextResult = script.runInNewContext(scriptNewContextSandbox);

assertEqual(
  scriptNewContextResult,
  5,
  "Script.runInNewContext should run inside a fresh context",
);
assertEqual(
  scriptNewContextSandbox.value,
  5,
  "Script.runInNewContext should sync sandbox changes",
);

globalThis.__vmScriptValue = 1;
const scriptThisResult = new vm.Script(
  "globalThis.__vmScriptValue += 4; globalThis.__vmScriptValue",
).runInThisContext();
assertEqual(
  scriptThisResult,
  5,
  "Script.runInThisContext should execute against the active global",
);
assertEqual(
  globalThis.__vmScriptValue,
  5,
  "Script.runInThisContext should update the active global",
);
delete globalThis.__vmScriptValue;

assert(
  typeof vm.compileFunction === "function",
  "vm.compileFunction should exist",
);
assert(
  typeof vm.SourceTextModule === "function",
  "vm.SourceTextModule should exist",
);
assert(
  typeof vm.SyntheticModule === "function",
  "vm.SyntheticModule should exist",
);
assert(
  typeof vm.measureMemory === "function",
  "vm.measureMemory should exist",
);
assert(
  vm.constants && typeof vm.constants === "object",
  "vm.constants should exist",
);
assert(
  vm.constants.DONT_CONTEXTIFY,
  "vm.constants.DONT_CONTEXTIFY should exist",
);
assert(
  vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER,
  "vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER should exist",
);

const dontContextify = vm.createContext(vm.constants.DONT_CONTEXTIFY);
assert(
  vm.isContext(dontContextify) === true,
  "createContext(DONT_CONTEXTIFY) should still create a context",
);

const cachedData = script.createCachedData();
assert(
  cachedData && typeof cachedData.byteLength === "number",
  "Script.createCachedData should return a byte container",
);
assert(
  cachedData.byteLength > 0,
  "Script.createCachedData should not be empty",
);

const sourceMapScript = new vm.Script(
  "value = 1;\n//# sourceMappingURL=vm-script.map",
);
assertEqual(
  sourceMapScript.sourceMapURL,
  "vm-script.map",
  "Script.sourceMapURL should expose the magic comment value",
);
assertEqual(
  sourceMapScript.cachedDataRejected,
  false,
  "Script.cachedDataRejected should default to false",
);

const compileSandbox = vm.createContext({ base: 6 });
const compiled = vm.compileFunction("return base + increment + offset;", ["increment"], {
  filename: "vm-compile.js",
  parsingContext: compileSandbox,
  contextExtensions: [{ offset: 5 }],
});

assertEqual(compiled.length, 1, "compileFunction should preserve arity");
assertEqual(
  compiled(4),
  15,
  "compileFunction should run against the parsing context",
);
const compiledThis = vm.compileFunction("return this.multiplier * value;", ["value"]);
assertEqual(
  compiledThis.call({ multiplier: 7 }, 6),
  42,
  "compileFunction should preserve explicit call() receivers",
);
assertEqual(
  compiledThis.apply({ multiplier: 8 }, [5]),
  40,
  "compileFunction should preserve explicit apply() receivers",
);
const compiledCache = compiled.createCachedData();
assert(
  compiledCache && typeof compiledCache.byteLength === "number",
  "compileFunction.createCachedData should return a byte container",
);

let moduleCtorThrew = false;
try {
  // eslint-disable-next-line no-new
  new vm.Module();
} catch (error) {
  moduleCtorThrew = true;
}
assert(moduleCtorThrew, "vm.Module should be abstract");

(async () => {
  const importedVm = await import("node:vm");
  assert(
    typeof importedVm.createContext === "function",
    "dynamic import('node:vm') should expose named vm exports",
  );
  assert(
    importedVm.default && typeof importedVm.default.runInContext === "function",
    "dynamic import('node:vm') should expose the default vm export",
  );
  const synthetic = new vm.SyntheticModule(
    ["value", "label"],
    function () {
      this.setExport("value", 40);
      this.setExport("label", "synthetic");
    },
    { identifier: "synthetic:dep" },
  );

  await synthetic.evaluate();
  assertEqual(
    synthetic.namespace.value,
    40,
    "SyntheticModule.evaluate should populate namespace exports",
  );

  const linkedModule = new vm.SourceTextModule(
    [
      "import { value } from 'dep';",
      "const result = value + 2;",
      "export { result };",
      "export default value + 1;",
    ].join("\n"),
    { identifier: "source:text" },
  );

  assertEqual(
    linkedModule.status,
    "unlinked",
    "SourceTextModule should start unlinked",
  );

  await linkedModule.link(async (specifier) => {
    assertEqual(specifier, "dep", "SourceTextModule.link should receive import specifiers");
    return synthetic;
  });

  assertEqual(
    linkedModule.status,
    "linked",
    "SourceTextModule.link should transition to linked",
  );
  assertEqual(
    linkedModule.identifier,
    "source:text",
    "SourceTextModule should preserve identifier",
  );
  assertEqual(
    linkedModule.dependencySpecifiers[0],
    "dep",
    "dependencySpecifiers should expose static imports",
  );
  assertEqual(
    linkedModule.moduleRequests[0].specifier,
    "dep",
    "moduleRequests should expose static imports",
  );
  assertEqual(
    linkedModule.hasTopLevelAwait(),
    false,
    "hasTopLevelAwait should reflect simple synchronous modules",
  );
  assertEqual(
    linkedModule.hasAsyncGraph(),
    false,
    "hasAsyncGraph should be false for synchronous graphs",
  );

  const moduleCache = linkedModule.createCachedData();
  assert(
    moduleCache && typeof moduleCache.byteLength === "number",
    "SourceTextModule.createCachedData should return a byte container",
  );

  await linkedModule.evaluate();
  assertEqual(
    linkedModule.status,
    "evaluated",
    "SourceTextModule.evaluate should transition to evaluated",
  );
  assertEqual(
    linkedModule.namespace.result,
    42,
    "SourceTextModule.evaluate should populate named exports",
  );
  assertEqual(
    linkedModule.namespace.default,
    41,
    "SourceTextModule.evaluate should populate the default export",
  );

  const linkedByRequests = new vm.SourceTextModule(
    [
      "import { value } from 'dep';",
      "export const total = value + 3;",
    ].join("\n"),
    { identifier: "source:requests" },
  );
  linkedByRequests.linkRequests([synthetic]);
  linkedByRequests.instantiate();
  await linkedByRequests.evaluate();
  assertEqual(
    linkedByRequests.namespace.total,
    43,
    "linkRequests() + instantiate() should support SourceTextModule evaluation",
  );

  const memory = await vm.measureMemory({ mode: "summary" });
  assert(memory && typeof memory === "object", "measureMemory should resolve an object");
  assert(
    memory.total && typeof memory.total.jsMemoryEstimate === "number",
    "measureMemory should include a numeric total.jsMemoryEstimate",
  );

  console.log(`vm PASS (${process.versions.engine})`);
})().catch((error) => {
  console.log(error && error.stack ? error.stack : String(error));
  throw error;
});
