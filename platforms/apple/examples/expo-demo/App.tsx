import React, {useEffect, useMemo, useState} from 'react';
import {Pressable, ScrollView, Text, View} from 'react-native';
import NativeScript, {defineUIKitView} from '@nativescript/react-native';

NativeScript.init();

type BadgeProps = {
  title: string;
  tone: 'blue' | 'green';
};

const NativeBadge = defineUIKitView<BadgeProps, UIView>({
  displayName: 'NativeBadge',
  create() {
    const view = UIView.alloc().initWithFrame(CGRectZero);
    view.clipsToBounds = true;

    const label = UILabel.alloc().initWithFrame(CGRectZero);
    label.tag = 1;
    label.textAlignment = NSTextAlignment.Center;
    label.textColor = UIColor.whiteColor;
    label.autoresizingMask =
      UIViewAutoresizing.FlexibleWidth | UIViewAutoresizing.FlexibleHeight;
    view.addSubview(label);

    return view;
  },
  mounted(view) {
    view.layer.cornerRadius = 14;
  },
  update(view, props) {
    view.backgroundColor =
      props.tone === 'green' ? UIColor.systemGreenColor : UIColor.systemBlueColor;
    const label = view.viewWithTag(1) as UILabel;
    label.text = props.title;
  },
});

async function readNativeSummary() {
  const api = (globalThis as any).__nativeScriptNativeApi;

  const uiSummary = await NativeScript.runOnUI(() => {
    'worklet';
    UIApplication.sharedApplication.keyWindow.tintColor =
      UIColor.systemPinkColor;
    return {
      ranOnMainThread: NSThread.isMainThread === true,
    };
  });

  return {
    backend: api?.backend,
    classes: api?.metadata?.classes ?? 0,
    constants: api?.metadata?.constants ?? 0,
    enums: api?.metadata?.enums ?? 0,
    ranOnMainThread: uiSummary.ranOnMainThread,
    timeoutConstant: NSURLErrorTimedOut,
    darkStyle: UIUserInterfaceStyle.Dark,
  };
}
export default function App() {
  const [tone, setTone] = useState<'blue' | 'green'>('blue');
  const [summary, setSummary] = useState('Loading NativeScript...');

  useEffect(() => {
    readNativeSummary()
      .then((value) => setSummary(JSON.stringify(value, null, 2)))
      .catch((error) => {
        setSummary(error instanceof Error ? error.message : String(error));
      });
  }, []);

  const title = useMemo(
    () => (tone === 'blue' ? 'UIKit view from Expo' : 'Updated from React state'),
    [tone],
  );

  return (
    <ScrollView
      contentInsetAdjustmentBehavior="automatic"
      contentContainerStyle={{gap: 18, padding: 24}}>
      <View style={{gap: 8}}>
        <Text style={{fontSize: 28, fontWeight: '800'}}>
          NativeScript Expo
        </Text>
        <Text selectable style={{fontSize: 16, color: '#394150'}}>
          Define UIKit views directly in JavaScript and mount them in an Expo
          development build.
        </Text>
      </View>

      <NativeBadge title={title} tone={tone} style={{height: 56}} />

      <Pressable
        onPress={() => setTone((value) => (value === 'blue' ? 'green' : 'blue'))}
        style={{
          alignItems: 'center',
          borderRadius: 8,
          backgroundColor: '#111827',
          minHeight: 48,
          justifyContent: 'center',
          paddingHorizontal: 16,
        }}>
        <Text style={{color: 'white', fontSize: 16, fontWeight: '700'}}>
          Toggle Native Badge
        </Text>
      </Pressable>

      <Text
        selectable
        style={{
          borderRadius: 8,
          backgroundColor: '#f3f4f6',
          color: '#111827',
          fontFamily: 'Menlo',
          fontSize: 13,
          lineHeight: 18,
          padding: 14,
        }}>
        {summary}
      </Text>
    </ScrollView>
  );
}
