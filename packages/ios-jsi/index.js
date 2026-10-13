function getJSIRuntimePointer() {
  if (typeof JSIRuntime !== 'undefined' && typeof JSIRuntime.runtimePointer === 'function') {
    return Number(JSIRuntime.runtimePointer());
  }
  return 0;
}

function isJSIRuntimeAvailable() {
  return getJSIRuntimePointer() !== 0;
}

function disposeJSIRuntimeForCurrentIsolate() {
  if (typeof JSIRuntime !== 'undefined' && typeof JSIRuntime.disposeRuntimeForCurrentIsolate === 'function') {
    JSIRuntime.disposeRuntimeForCurrentIsolate();
  }
}

module.exports = {
  getJSIRuntimePointer,
  isJSIRuntimeAvailable,
  disposeJSIRuntimeForCurrentIsolate,
};
