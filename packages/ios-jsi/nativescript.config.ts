import { NativeScriptConfig } from '@nativescript/core';

export default {
  ios: {
    SPMPackages: [
      {
        name: 'NativeScriptJSI',
        libs: ['NativeScriptJSI'],
        path: '.',
      },
    ],
  },
} as NativeScriptConfig;
