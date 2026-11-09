const url = new URL("https://example.com/path");
console.log("url ok", typeof url, url.href);

const workerValue = new Worker("platforms/apple/test/cli/worker.js");
console.log("worker value", typeof workerValue, workerValue === undefined);
if (workerValue && typeof workerValue.terminate === "function") {
  workerValue.terminate();
}
