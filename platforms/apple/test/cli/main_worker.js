const worker = new Worker("platforms/apple/test/cli/worker.js");

worker.onerror = (e) => {
  console.error("Worker error:", e);
};

const sab = typeof SharedArrayBuffer !== "undefined" ? new SharedArrayBuffer(4) : new ArrayBuffer(4);
const view = new Int32Array(sab);
view[0] = 42;

console.log("Main thread: SharedArrayBuffer value before worker:", view[0]);

worker.onmessage = (e) => {
  console.log("Worker message:", e.data);

  console.log("SharedArrayBuffer value now:", view[0]);
};

worker.postMessage(sab);

NSApplicationMain(0, null);
