import { NativeScriptConfig } from '@nativescript/core';

export default {
  ios: {
    SPMPackages: [
      {
        name: 'NativeScriptJSI',
        libs: ['NativeScriptJSI'],
        path: 'node_modules/@nativescript/ios-jsi',
      },
    ],
  },
} as NativeScriptConfig;
