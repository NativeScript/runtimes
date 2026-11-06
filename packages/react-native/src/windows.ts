import { Platform } from "react-native";
import NativeScriptNativeApi from "./NativeScriptNativeApi";

/**
 * React Native for Windows. NativeScript runs in a Hermes runtime of its own on
 * the UI thread (see windows/NativeScriptNativeApi/UIRuntime.h): React Native's
 * runtime can't host it there, and react-native-worklets has no Windows
 * implementation. Worklets passed to runOnUI run in that runtime, rebuilt from
 * the code the Worklets Babel plugin embeds in each one; their arguments,
 * captured values and results travel as JSON.
 */
export const isWindows = Platform.OS === "windows";

/** The UI runtime's globals (UIRuntime.cpp's prelude). */
const UI_RUNTIME_GLOBALS = ["Windows", "NSWinRT", "XamlWindow"];

// A worklet that uses Windows captures it like any outside name, so making the
// worklet reads `Windows` in React Native's runtime, where it doesn't exist and
// would throw. Declared here as undefined, it's captured as undefined, which
// runOnUI resolves to the UI runtime's own global.
if (isWindows) {
  const scope = globalThis as Record<string, unknown>;
  for (const name of UI_RUNTIME_GLOBALS) if (!(name in scope)) scope[name] = undefined;
}

interface WindowsModule {
  install(metadataPath: string): boolean;
  isInstalled(): boolean;
  runWorklet(code: string, payload: string): Promise<string>;
}

interface WorkletFunction {
  __initData?: { code?: string };
  __closure?: Record<string, unknown>;
}

interface WorkletResult {
  ok: boolean;
  value?: unknown;
  error?: string;
  logs?: [string, string][];
}

const windowsModule = NativeScriptNativeApi as unknown as WindowsModule;

export function initWindows(): boolean {
  return windowsModule.isInstalled() || windowsModule.install("");
}

function workletPayload(callback: WorkletFunction, args: unknown[]): string {
  const values: Record<string, unknown> = {};
  // Captured names with no value on React Native's side (Windows, XamlWindow,
  // NSWinRT) are globals of the UI runtime.
  const globals: string[] = [];
  for (const [name, value] of Object.entries(callback.__closure ?? {})) {
    if (value === undefined) {
      globals.push(name);
    } else if (typeof value === "function") {
      throw new TypeError(
        `NativeScript.runOnUI on Windows passes a worklet's captured values as JSON, so it can't capture the function '${name}'. Define it inside the worklet.`,
      );
    } else {
      values[name] = value;
    }
  }
  return JSON.stringify({ args, closure: values, globals });
}

export async function runOnUIWindows<Args extends unknown[], ReturnValue>(
  callback: (...args: Args) => ReturnValue | Promise<ReturnValue>,
  args: Args,
): Promise<ReturnValue> {
  const code = (callback as WorkletFunction).__initData?.code;
  if (typeof code !== "string") {
    throw new TypeError(
      "NativeScript.runOnUI requires a worklet: start the function with 'worklet' and add react-native-worklets/plugin to your Babel plugins (on Windows it only compiles the worklet; no native Worklets library is needed).",
    );
  }
  initWindows();
  const json = await windowsModule.runWorklet(code, workletPayload(callback as WorkletFunction, args));
  const result = JSON.parse(json) as WorkletResult;
  for (const [level, text] of result.logs ?? []) {
    const log = (console as unknown as Record<string, (...a: unknown[]) => void>)[level] ?? console.log;
    log(text);
  }
  if (!result.ok) throw new Error(result.error ?? "The worklet failed");
  return result.value as ReturnValue;
}
