console.log("typeof SAB", typeof SharedArrayBuffer);
console.log("typeof Atomics", typeof Atomics);

const buffer =
  typeof SharedArrayBuffer !== "undefined"
    ? new SharedArrayBuffer(4)
    : new ArrayBuffer(4);

console.log("buffer tag", Object.prototype.toString.call(buffer));
