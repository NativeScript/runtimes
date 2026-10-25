import type {TurboModule} from 'react-native';
import type {UnsafeObject} from 'react-native/Libraries/Types/CodegenTypes';
import {TurboModuleRegistry} from 'react-native';

export interface Spec extends TurboModule {
  readonly install: (metadataPath: string) => boolean;
  readonly installWorkletRuntime: (
    runtimeHolder: UnsafeObject,
    metadataPath: string,
  ) => boolean;
  readonly isInstalled: () => boolean;
  readonly defaultMetadataPath: () => string;
  readonly getRuntimeBackend: () => string;
  readonly __writeTestMarker: (content: string) => boolean;
}

function loadNativeApi(): Spec {
  const nativeApi = TurboModuleRegistry.get<Spec>('NativeScriptNativeApi');
  if (nativeApi == null) {
    throw new Error(
      "@nativescript/react-native: the native module 'NativeScriptNativeApi' is not in this app binary. " +
        'The package contains native code, so it cannot run in Expo Go: use a development build ' +
        '(npx expo run:ios / npx expo run:android, or EAS Build). ' +
        'If the package was added after the app was built, rebuild the app.',
    );
  }
  return nativeApi;
}

export default loadNativeApi();
