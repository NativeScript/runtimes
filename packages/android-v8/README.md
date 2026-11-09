# @nativescript/android-v8

NativeScript runtime package for Android, built with the V8 JavaScript engine.

## Build

From the repo root:

```sh
npm run build-android -- --engine=V8-13
```

The staged package lands in `dist/android_v8_13_napi/` and the tarball in
`build/npm-tarballs/`. Add `--binding=jsi` for the jsi runtime tree
(`dist/android_v8_13_jsi/`).
