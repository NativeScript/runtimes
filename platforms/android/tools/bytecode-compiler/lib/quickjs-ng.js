'use strict';

// QuickJS-NG adapter, built on the shared shim (native/qjs-compile.c). The
// runtime reads this container in napi/quickjs js_execute_script (the
// __QUICKJS_NG__ build).
//
// This is the only QuickJS lane left: the bellard tree was dropped and the
// runtime's QUICKJS is an alias for QUICKJS_NG (see test-app/runtime/
// build.gradle), so both names must resolve here. Were QUICKJS left
// unregistered, the driver would silently skip compilation and ship plain JS.
const MAGIC = Buffer.from('NSBCNGS\0', 'latin1'); // 8 bytes, must match the shim + runtime

module.exports = {
  key: 'quickjs-ng',
  engineKeys: ['QUICKJS_NG', 'QUICKJS'],
  ready: true,
  magic: MAGIC,
  supportsSourceMaps: false,
  defaultOptimize: null,

  binName(hostKey) {
    return hostKey.startsWith('win32') ? 'nsbc-quickjs-ng.exe' : 'nsbc-quickjs-ng';
  },

  isBytecode(head) {
    return head.length >= MAGIC.length && head.subarray(0, MAGIC.length).equals(MAGIC);
  },

  buildArgs({ input, output }) {
    return [input, output];
  },

  sourceMapOutput(output) {
    return output + '.map';
  },
};
