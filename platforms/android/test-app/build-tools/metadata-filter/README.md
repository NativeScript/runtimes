# NativeScript Android metadata filter

This directory contains the build-time metadata filtering tools used by the
Android test application. The filter derives a conservative class seed from
the JavaScript bundle and lets the metadata generator expand that seed into a
safe metadata closure.

## Purpose

The complete Android classpath contains substantially more metadata than an
application normally needs. Filtering reduces the generated metadata while
preserving classes required indirectly through:

- superclass and interface relationships;
- nested and enclosing classes;
- method and field signatures;
- constructors and generated bindings;
- NativeScript runtime-owned requirements;
- unresolved or computed JavaScript access, which widens the retained scope.

The filter is conservative. If an access cannot be resolved safely, it retains
the narrowest provably safe package or class scope. If no safe scope exists,
the complete relevant member set is retained rather than risking a runtime
metadata miss.

## Tools

`harvest.js` scans a JavaScript bundle and emits the names that can be
resolved statically. It records both exact resolved classes and broader roots
for computed or uncertain access.

`seed.js` converts the harvested bundle, Android manifest, resources, and SBG
bindings into a deterministic seed file. The metadata generator then expands
the seed through its superclass, interface, nested-class, and signature-type
closure.

`check-runtime-keeplist.js` checks that classes referenced directly by native
runtime source are represented in the runtime keep list.

## Typical usage

From the Android test-app directory:

```bash
node build-tools/metadata-filter/harvest.js \
  app/src/main/assets/app \
  --json build-tools/metadata-filter/harvest.json

node build-tools/metadata-filter/seed.js \
  app/src/main/assets/app \
  --manifest platforms/android/app/src/main/AndroidManifest.xml \
  --out build-tools/whitelist.mdg

node build-tools/metadata-filter/check-runtime-keeplist.js \
  ../../../../NativeScript/ffi/jni/jsi \
  ../../../../NativeScript/runtime/android/jsi
```

`seed.js` runs the harvester itself, so invoking `harvest.js` separately is
only useful when inspecting or debugging the intermediate JSON. Normal Gradle
builds invoke the seed and closure steps through the metadata-generation task.
The generated JSON, seed, and whitelist are build outputs and should not be
edited manually.

## Build controls

Release builds enable filtering by default when the bundle is available. Use
`-PnsFilterMetadata=false` to disable filtering and generate metadata for the
complete classpath. This is useful for compatibility comparisons and
diagnosing a suspected filtering miss.

Filtering is intentionally disabled when the required bundle analysis input
is unavailable. A partial analysis must not silently produce an incomplete
metadata set.

## Invariants

The filter must remain:

1. deterministic for identical inputs;
2. conservative for computed property access;
3. closed over all referenced types and supertypes;
4. compatible with generated bindings and runtime-owned requirements; and
5. fail-fast when its input or generated closure is inconsistent.

After changing the filter, rebuild the Android test app and exercise the
metadata-backed Java and Kotlin runtime tests. A successful JavaScript bundle
build alone does not prove that the generated metadata is complete.
