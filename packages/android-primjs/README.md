# @nativescript/android-primjs

NativeScript runtime package for Android, built with the PrimJS JavaScript engine.

## Build

From the repo root:

```sh
npm run build-android -- --engine=PRIMJS
```

The staged package lands in `dist/android_primjs_napi/` and the tarball in
`build/npm-tarballs/`. Add `--binding=jsi` for the jsi runtime tree
(`dist/android_primjs_jsi/`).
