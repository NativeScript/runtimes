# @nativescript/android-hermes

NativeScript runtime package for Android, built with the Hermes JavaScript engine.

## Build

From the repo root:

```sh
npm run build-android -- --engine=HERMES
```

The staged package lands in `dist/android_hermes_napi/` and the tarball in
`build/npm-tarballs/`. Add `--binding=jsi` for the jsi runtime tree
(`dist/android_hermes_jsi/`).
