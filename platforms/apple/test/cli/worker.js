console.log("Hello from Worker!");

globalThis.onmessage = (e) => {
  console.log("Worker received message:", e.data);
  const sab = e.data;
  const view = new Int32Array(sab);
  console.log("Worker view[0]:", view[0]);
  view[0] = 100;
  globalThis.postMessage("Worker done!");
  console.log("Worker done!");
};
