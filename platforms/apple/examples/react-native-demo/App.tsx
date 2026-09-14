import React, {useCallback, useEffect, useMemo, useState} from 'react';
import {
  Platform,
  Pressable,
  SafeAreaView,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import NativeScript from '@nativescript/react-native';

type NativeApiHost = {
  backend?: string;
  metadata?: {
    classes?: number;
    functions?: number;
    constants?: number;
    enums?: number;
  };
};

function installNativeScriptGlobals(): NativeApiHost {
  NativeScript.init();
  const api = (globalThis as any).__nativeScriptNativeApi as
    | NativeApiHost
    | undefined;
  if (!api) {
    throw new Error('NativeScript Native API JSI host object was not installed');
  }
  return api;
}

function getActiveUIKitWindow() {
  'worklet';
  const app = UIApplication.sharedApplication;
  const scenes = app.connectedScenes?.allObjects;
  if (scenes) {
    for (let i = 0; i < scenes.count; i++) {
      const scene = scenes.objectAtIndex(i);
      if (
        scene instanceof UIWindowScene &&
        scene.activationState === UISceneActivationState.ForegroundActive
      ) {
        const windows = scene.windows;
        for (let j = 0; j < windows.count; j++) {
          const window = windows.objectAtIndex(j);
          if (window.isKeyWindow) {
            return window;
          }
        }
        if (windows.count > 0) {
          return windows.objectAtIndex(0);
        }
      }
    }
  }
  return app.keyWindow;
}

async function applyUIKitTweaks() {
  if (Platform.OS !== 'ios') {
    throw new Error('This demo uses UIKit and must run on iOS');
  }

  const api = installNativeScriptGlobals();

  const uiSummary = await NativeScript.runOnUI(() => {
    'worklet';
    const window = getActiveUIKitWindow();
    if (!window) {
      throw new Error('No key UIWindow is available yet');
    }

    const nativeAccent = UIColor.systemPinkColor ?? UIColor.magentaColor;
    const nativeBackdrop = UIColor.colorWithRedGreenBlueAlpha(
      0.04,
      0.08,
      0.12,
      1,
    );

    window.tintColor = nativeAccent;
    window.backgroundColor = nativeBackdrop;
    window.overrideUserInterfaceStyle = UIUserInterfaceStyle.Dark;

    const rootView = window.rootViewController?.view;
    if (rootView) {
      rootView.tintColor = nativeAccent;
      rootView.backgroundColor = nativeBackdrop;
    }

    return {
      nativeCallsRanOnMainThread: NSThread.isMainThread === true,
    };
  });

  return {
    backend: api.backend,
    turboBackend: NativeScript.getRuntimeBackend(),
    classes: api.metadata?.classes ?? 0,
    functions: api.metadata?.functions ?? 0,
    constants: api.metadata?.constants ?? 0,
    enums: api.metadata?.enums ?? 0,
    timeoutConstant: NSURLErrorTimedOut,
    darkStyle: UIUserInterfaceStyle.Dark,
    nativeCallsRanOnMainThread: uiSummary.nativeCallsRanOnMainThread,
  };
}

export default function App(): React.JSX.Element {
  const [status, setStatus] = useState('Ready');
  const [details, setDetails] = useState('');
  const [busy, setBusy] = useState(false);

  const runDemo = useCallback(async () => {
    setBusy(true);
    setStatus('Applying UIKit tweaks');
    try {
      const result = await applyUIKitTweaks();
      setStatus('UIKit updated from JavaScript');
      setDetails(JSON.stringify(result, null, 2));
    } catch (error) {
      setStatus('Demo failed');
      setDetails(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  }, []);

  useEffect(() => {
    runDemo();
  }, [runDemo]);

  const buttonLabel = useMemo(
    () => (busy ? 'Working...' : 'Run UIKit Tweak Again'),
    [busy],
  );

  return (
    <SafeAreaView style={styles.screen}>
      <View style={styles.panel}>
        <Text style={styles.kicker}>NativeScript for React Native</Text>
        <Text style={styles.title}>Hermes JSI UIKit Demo</Text>
        <Text style={styles.status}>{status}</Text>
        <Text selectable style={styles.details}>
          {details}
        </Text>
        <Pressable
          accessibilityRole="button"
          disabled={busy}
          onPress={runDemo}
          style={({pressed}) => [
            styles.button,
            busy && styles.buttonDisabled,
            pressed && !busy && styles.buttonPressed,
          ]}>
          <Text style={styles.buttonText}>{buttonLabel}</Text>
        </Pressable>
      </View>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  screen: {
    flex: 1,
    backgroundColor: 'transparent',
    justifyContent: 'center',
    padding: 24,
  },
  panel: {
    gap: 14,
    borderRadius: 8,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: 'rgba(255,255,255,0.26)',
    backgroundColor: 'rgba(8,14,22,0.82)',
    padding: 20,
  },
  kicker: {
    color: '#9bd4ff',
    fontSize: 13,
    fontWeight: '700',
    letterSpacing: 0,
    textTransform: 'uppercase',
  },
  title: {
    color: '#ffffff',
    fontSize: 28,
    fontWeight: '800',
    letterSpacing: 0,
  },
  status: {
    color: '#f7d276',
    fontSize: 17,
    fontWeight: '700',
    letterSpacing: 0,
  },
  details: {
    minHeight: 112,
    color: '#d8e7f4',
    fontFamily: Platform.select({ios: 'Menlo', default: 'monospace'}),
    fontSize: 13,
    lineHeight: 18,
    letterSpacing: 0,
  },
  button: {
    minHeight: 46,
    alignItems: 'center',
    justifyContent: 'center',
    borderRadius: 8,
    backgroundColor: '#ff4fa3',
    paddingHorizontal: 16,
  },
  buttonPressed: {
    backgroundColor: '#e23f90',
  },
  buttonDisabled: {
    opacity: 0.6,
  },
  buttonText: {
    color: '#ffffff',
    fontSize: 16,
    fontWeight: '800',
    letterSpacing: 0,
  },
});
