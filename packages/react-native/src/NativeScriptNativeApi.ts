import type {TurboModule} from 'react-native';
import type {UnsafeObject} from 'react-native/Libraries/Types/CodegenTypes';
import {TurboModuleRegistry} from 'react-native';

export interface Spec extends TurboModule {
  readonly install: (metadataPath: string) => boolean;
  // Holder handshake (ARCHITECTURE.md §3.5, §7.1): installs the NativeScript
  // ObjC bridge onto the Worklets UI runtime, the same StableApi.h path
  // Reanimated uses. Called once from the RN JS thread at bootstrap (a
  // one-time exception to the "only enter the UI runtime from main" rule,
  // same as Worklets' own bootstrap use of runOnUISync; see §3.3/§9.2).
  // `schedulerHolder` (M1) is the UIScheduler holder handshake alongside the
  // WorkletRuntime one; lets native route off-main async entries through
  // the sanctioned `worklets::scheduleOnUI` instead of a raw dispatch_async.
  readonly installUIRuntime: (
    runtimeHolder: UnsafeObject,
    schedulerHolder: UnsafeObject,
    metadataPath: string,
  ) => boolean;
  readonly isInstalled: () => boolean;
  readonly defaultMetadataPath: () => string;
  readonly getRuntimeBackend: () => string;
  readonly __writeTestMarker: (content: string) => boolean;
  // Test-only companion to __writeTestMarker: symmetric read-back of the
  // same smoke-marker file.
  readonly __readTestMarker: () => string;
  // JOB2 dev-reload test: a marker file SEPARATE from the smoke marker
  // above (native's own install-sequence "stage=..." writes to the smoke
  // marker on every reload would otherwise clobber a phase flag stored
  // there before the reloaded JS ever reads it back; confirmed on-sim as
  // an infinite reload loop). Used to detect, from a freshly-reloaded JS
  // VM, whether a previous phase already wrote it.
  readonly __writeReloadPhaseMarker: (content: string) => boolean;
  readonly __readReloadPhaseMarker: () => string;

  // defineNativeComponent's native registration step (ARCHITECTURE.md §5.2
  // steps 1-2): extracts a worklets Serializable from `spec` synchronously
  // on the JS thread (no UI-runtime entry, so no ordering race with first
  // mount), stores it keyed by `name` alongside `hookMask` (a bitwise-OR of
  // NativeScriptComponentHook from NativeScriptFabricGateway.h), and
  // registers the flavored Fabric class.
  // `shouldBeRecycled`: tri-state as a number (codegen-friendly, no optional
  // booleans); -1 means the spec never set the flag (leave RN's own
  // `shouldBeRecycled: true` default alone), 0/1 are false/true. Wired onto
  // a per-flavor `+shouldBeRecycled` class method (M1 review §2/(c)).
  readonly registerComponent: (
    name: string,
    spec: UnsafeObject,
    hookMask: number,
    shouldBeRecycled: number,
  ) => boolean;
}

export default TurboModuleRegistry.getEnforcing<Spec>('NativeScriptNativeApi');
