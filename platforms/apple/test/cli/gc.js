const registry = new FinalizationRegistry((heldValue) => {
  console.log(`An object with held value '${heldValue}' was garbage collected.`);
});

let weakRef;

function localScope() {
  const obj = { hello: "world" };
  registry.register(obj, "some value");
  weakRef = new WeakRef(obj);
}

console.log("Creating an object in a local scope...");

localScope();

console.log("Waiting for garbage collection...");

// Force garbage collection (only works in some environments)
if (globalThis.gc) {
  gc();
  console.log("Garbage collection triggered.");
} else {
  console.warn("Garbage collection is not exposed. Run with --expose-gc to test.");
}

for (let i = 0; i < 1e6; i++) {
  const _temp = new Uint32Array(1000).fill(i);
  // gc();
}

// Give some time for the garbage collector to run
setTimeout(() => {
  console.log("Finished waiting for garbage collection.");
  const obj = weakRef.deref();
  if (obj) {
    console.log("The object is still alive.");
  } else {
    console.log("The object has been garbage collected.");
  }
  if (typeof NSApplication === "function") {
    NSApplication.sharedApplication.terminate(null);
  }
}, 1000);

if (typeof NSApplicationMain === "function") {
  NSApplicationMain(0, null);
}
