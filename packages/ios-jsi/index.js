function getJSIRuntimePointer() {
  if (typeof NSJSIRuntime !== 'undefined' && typeof NSJSIRuntime.runtimePointer === 'function') {
    return Number(NSJSIRuntime.runtimePointer());
  }
  return 0;
}

function isJSIRuntimeAvailable() {
  return getJSIRuntimePointer() !== 0;
}

function disposeJSIRuntimeForCurrentIsolate() {
  if (typeof NSJSIRuntime !== 'undefined' && typeof NSJSIRuntime.disposeRuntimeForCurrentIsolate === 'function') {
    NSJSIRuntime.disposeRuntimeForCurrentIsolate();
  }
}

module.exports = {
  getJSIRuntimePointer,
  isJSIRuntimeAvailable,
  disposeJSIRuntimeForCurrentIsolate,
};
