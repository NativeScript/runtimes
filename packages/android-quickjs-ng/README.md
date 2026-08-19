# @nativescript/android-quickjs-ng

NativeScript runtime package for Android, built with the QuickJS-ng JavaScript engine.

## Build

From the repo root:

```sh
npm run build-android -- --engine=QUICKJS_NG
```

The staged package lands in `dist/android_quickjs_napi/` and the tarball in
`build/npm-tarballs/`. Add `--binding=jsi` for the jsi runtime tree
(`dist/android_quickjs_jsi/`).
