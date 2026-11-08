/**
 * The Android conformance fixture for @nativescript/react-native.
 *
 * Driven by scripts/test_react_native_android.sh, which installs this as an RN
 * app's App.tsx, launches it and reads the results off logcat. Every assertion
 * exercises something the guest runtime has to get right: resolving the Android
 * SDK and the app's own dependencies, constructing and calling Java objects,
 * generating real Java subclasses, and marshalling a callback that arrives on
 * another thread.
 */
import React from 'react';
import {SafeAreaView, Text, ScrollView} from 'react-native';
import NativeScriptNativeApi from '@nativescript/react-native/src/NativeScriptNativeApi';
import {NativeClass, JavaProxy} from '@nativescript/react-native/src/NativeClass';

declare const global: any;

const MARK = 'NS_ANDROID_SMOKE';
const lines: string[] = [];

function record(label: string, fn: () => unknown) {
  try {
    const value = fn();
    const text = String(value);
    // A check that catches its own error and returns a description of it is
    // still a failed check. Reporting on "did it throw" alone marked a
    // "FAIL ...LookedUpClassNotFound" result as PASS, which is the one thing a
    // test harness must never do.
    const failed = text.startsWith('FAIL');
    lines.push(`${label} = ${text}`);
    console.log(`${MARK} ${failed ? 'FAIL' : 'PASS'} ${label} = ${text}`);
  } catch (e: any) {
    lines.push(`${label} THREW ${e && e.message}`);
    console.log(`${MARK} FAIL ${label} :: ${e && e.message}`);
  }
}

try {
  const installed = NativeScriptNativeApi.install('');
  console.log(`${MARK} install -> ${installed}`);
  console.log(`${MARK} backend -> ${NativeScriptNativeApi.getRuntimeBackend()}`);

  record('typeof java', () => typeof global.java);
  record('typeof android', () => typeof global.android);
  record('System.currentTimeMillis', () =>
    global.java.lang.System.currentTimeMillis(),
  );
  record('new java.lang.StringBuilder', () =>
    new global.java.lang.StringBuilder('ns').append('-ok').toString(),
  );
  record('Integer.parseInt', () => global.java.lang.Integer.parseInt('41') + 1);
  record('Build.VERSION.SDK_INT', () => global.android.os.Build.VERSION.SDK_INT);
  // From a third-party library the app installs, not the Android SDK.
  record('androidx.core.view.ViewCompat', () =>
    typeof global.androidx.core.view.ViewCompat,
  );

  // --- subclassing Java from JS ---
  record('ts_helpers.__extends', () => typeof global.__extends);
  record('ts_helpers.__createNativeProxy', () =>
    typeof global.__createNativeProxy,
  );

  record('implement Runnable', () => {
    let ran = false;
    const r = new global.java.lang.Runnable({
      run() {
        ran = true;
      },
    });
    r.run();
    return ran;
  });

  record('extend a Java class', () => {
    const MyList = global.java.util.ArrayList.extend('NsRnMyList', {
      size() {
        return 42;
      },
    });
    const instance = new MyList();
    return instance.size();
  });

  // No explicit class name: the runtime keys the generated class on the
  // extend() call site (file/line/column), which is engine-sensitive.
  record('unnamed extend', () => {
    const Anon = global.java.util.ArrayList.extend({
      size() {
        return 7;
      },
    });
    return new Anon().size();
  });

  // ES class syntax over a native base -- what a React Native developer would
  // reach for first, and the form that on the NativeScript CLI needs
  // @NativeClass() plus a TypeScript transformer.
  record('class syntax extends', () => {
    const MyEsList = NativeClass(
      class MyEsList extends global.java.util.ArrayList {
        size() {
          return 5;
        }
      },
    );
    const instance = new MyEsList();
    // Java's own view of it: Collections.unmodifiableList calls size() from
    // Java, so this only agrees if the generated class really overrides it.
    const viaJava = global.java.util.Collections.unmodifiableList(instance).size();
    return `${instance.size()}/${viaJava}`;
  });

  // @NativeClass with several Java interfaces -- a JS class has one base, so
  // this is the only way to say "implements Runnable and Callable".
  record('@NativeClass multiple interfaces', () => {
    // Applied as a function rather than with @-syntax: decorator syntax needs
    // @babel/plugin-proposal-decorators, and this is exactly what the legacy
    // dialect desugars the decorator to.
    const Both = NativeClass({
      interfaces: [
        global.java.lang.Runnable,
        global.java.util.concurrent.Callable,
      ],
    })(
      class Both extends global.java.lang.Object {
        ran = false;
        run() {
          this.ran = true;
        }
        call() {
          return new global.java.lang.String('called');
        }
      },
    );

    const instance = new Both();

    // Ask the generated Java class what it implements. submit() would be
    // ambiguous here -- ExecutorService overloads it for both Runnable and
    // Callable, and this object is both.
    const declared = instance.getClass().getInterfaces();
    const names: string[] = [];
    for (let i = 0; i < declared.length; i++) {
      names.push(String(declared[i].getName()));
    }
    const wanted = ['java.lang.Runnable', 'java.util.concurrent.Callable'];
    return wanted.every((n) => names.indexOf(n) >= 0)
      ? 'both'
      : `only ${names.join(',')}`;
  });

  // A fully-qualified name makes the runtime *look up* a pre-generated class
  // instead of building one on the device, so this only passes in a release
  // build, where the static binding generator has run. In debug it is expected
  // to report LookedUpClassNotFound, which is why the check is explicit about
  // which build it is looking at rather than simply failing.
  record('@JavaProxy resolves a pre-generated class', () => {
    try {
      const Ticker = JavaProxy('com.example.nsrn.Ticker')(
        class Ticker extends global.java.lang.Object {
          toString() {
            return 'ticked';
          }
        },
      );

      const instance = new Ticker();
      const name = String(instance.getClass().getName());
      if (name !== 'com.example.nsrn.Ticker') {
        return `wrong class ${name}`;
      }

      // Round-trip through Java so this cannot pass on the JS override alone.
      return String(new global.java.lang.StringBuilder().append(instance).toString());
    } catch (e: any) {
      const message = String(e && e.message ? e.message : e);
      if (message.indexOf('LookedUpClassNotFound') >= 0) {
        return __DEV__ ? 'skipped (debug: no build-time generator)' : `FAIL ${message}`;
      }
      return `FAIL ${message}`;
    }
  });

  record('Comparator drives a real Java sort', () => {
    const list = new global.java.util.ArrayList();
    list.add(new global.java.lang.Integer(3));
    list.add(new global.java.lang.Integer(1));
    list.add(new global.java.lang.Integer(2));
    const cmp = new global.java.util.Comparator({
      compare(a: any, b: any) {
        // The runtime may hand back a JS number rather than an Integer proxy,
        // depending on how the boxed value was marshalled.
        const av = a && a.intValue ? a.intValue() : Number(a);
        const bv = b && b.intValue ? b.intValue() : Number(b);
        return av - bv;
      },
    });
    global.java.util.Collections.sort(list, cmp);
    const at = (i: number) => {
      const v = list.get(i);
      return v && v.intValue ? v.intValue() : Number(v);
    };
    return `${at(0)}${at(1)}${at(2)}`;
  });
} catch (e: any) {
  console.log(`${MARK} FATAL ${e && e.message}`);
  lines.push(`FATAL ${e && e.message}`);
}
// Cross-thread callback, checked asynchronously.
//
// Deliberately NOT joined: a Java thread calling into JS is marshalled onto the
// JS thread through the runtime's scheduler, so blocking the JS thread waiting
// for that thread would deadlock -- the callback can only run once the JS thread
// is free again.
let threadFlag = '';
try {
  const r = new global.java.lang.Runnable({
    run() {
      threadFlag = 'called';
    },
  });
  new global.java.lang.Thread(r).start();
} catch (e: any) {
  threadFlag = `THREW ${e && e.message}`;
}

setTimeout(() => {
  const ok = threadFlag === 'called';
  lines.push(`Runnable on a Java thread = ${threadFlag}`);
  console.log(
    `${MARK} ${ok ? 'PASS' : 'FAIL'} Runnable on a Java thread = ${threadFlag}`,
  );
  console.log(`${MARK}_DONE`);
}, 1500);

export default function App() {
  return (
    <SafeAreaView>
      <ScrollView>
        {lines.map((l, i) => (
          <Text key={i}>{l}</Text>
        ))}
      </ScrollView>
    </SafeAreaView>
  );
}
