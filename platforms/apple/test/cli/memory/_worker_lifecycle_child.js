"use strict";

globalThis.onmessage = (event) => {
  const payload = event.data;
  const validCycle = payload.self === payload;
  const validTypedArray =
    payload.typed instanceof Uint32Array && payload.typed[0] === payload.index;

  postMessage({
    index: payload.index,
    validCycle,
    validTypedArray,
  });

  if (payload.closeFromWorker) {
    close();
  }
};
