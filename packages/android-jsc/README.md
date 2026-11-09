# @nativescript/android-jsc

NativeScript runtime package for Android, built with the JavaScriptCore engine.

## Build

From the repo root:

```sh
npm run build-android -- --engine=JSC
```

The staged package lands in `dist/android_jsc_napi/` and the tarball in
`build/npm-tarballs/`. Add `--binding=jsi` for the jsi runtime tree
(`dist/android_jsc_jsi/`).
