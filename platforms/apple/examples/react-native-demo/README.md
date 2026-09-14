# React Native NativeScript Demo

This demo is generated into `build/react-native-demo` so the
repository does not need to commit React Native boilerplate.

Run it from the repository root:

```sh
npm run demo-rn-turbomodule
```

The generated app installs the local `@nativescript/react-native` tarball and
`react-native-worklets`, enables Hermes and the New Architecture, then launches
an iOS simulator app. `NativeScript.init()` installs the Native API into the
Worklets UI runtime, and `runOnUI` executes a small UIKit tweak from a Worklets
callback. The script waits for a simulator marker after the tweak succeeds.
