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

// `get`, not `getEnforcing`: a build without the native module (Expo Go, or a
// build made before the package was added) must still be able to import the
// package and decide what to do without it.
export default TurboModuleRegistry.get<Spec>('NativeScriptNativeApi');
