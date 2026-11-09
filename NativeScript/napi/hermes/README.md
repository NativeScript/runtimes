Hermes Node-API adapter
=======================

`include/` is the single vendored Static Hermes header surface used by both
Apple and Android builds. It comes from `DjDeveloperr/build-hermes` and must
stay in sync with the Hermes binaries under `Frameworks/` and
`platforms/android/test-app/runtime/src/main/libs/hermes/`.

Android still accepts the historical `SHERMES` engine selector so existing
test/build scripts keep working, but it is now only an alias for `HERMES`.
Both selectors compile the same adapter and link the same Static Hermes
artifact.
