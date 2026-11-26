function getJSIRuntimePointer() {
  if (
    typeof org !== 'undefined' &&
    org.nativescript &&
    org.nativescript.jsi &&
    org.nativescript.jsi.JSIRuntime
  ) {
    return Number(org.nativescript.jsi.JSIRuntime.getRuntimePointer());
  }
  return 0;
}

function isJSIRuntimeAvailable() {
  return getJSIRuntimePointer() !== 0;
}

function disposeJSIRuntimeForCurrentIsolate() {
  if (
    typeof org !== 'undefined' &&
    org.nativescript &&
    org.nativescript.jsi &&
    org.nativescript.jsi.JSIRuntime
  ) {
    org.nativescript.jsi.JSIRuntime.disposeRuntimeForCurrentIsolate();
  }
}

module.exports = {
  getJSIRuntimePointer,
  isJSIRuntimeAvailable,
  disposeJSIRuntimeForCurrentIsolate,
};
