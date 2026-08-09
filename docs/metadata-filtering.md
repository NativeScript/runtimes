# Filtering Android metadata by what the app actually uses

Android metadata is currently shipped whole: every class of every jar on the
compile classpath, whether or not the app can reach it. For the runtime test-app
that is **2.83 MB** of `.dat` in the APK, all of it parsed into a tree at
startup. An app that touches 5% of the platform pays for 100% of it.

The generator has accepted `whitelist.mdg` / `blacklist.mdg` filters for years.
Nothing has ever produced them automatically, and this document explains why
that is harder than it looks -- then what to do about it.

Everything below is measured on the test-app (520 specs, Hermes/jsi, arm64,
device QV7120NC26), not reasoned about.

## How to reproduce the measurements

Two instruments, both added by this lane:

**Ground truth** -- what the app really resolves at runtime:

```bash
cd platforms/android/test-app
./gradlew :app:assembleDebug -Pengine=HERMES -PbindingLayer=jsi -Pmetadata-usage-trace
# run the app, then:
adb logcat | grep NS_MD_USE      # "C android/app/Activity", "P android/os", …
```

`-Pmetadata-usage-trace` compiles `NS_METADATA_USAGE_TRACE` into
`MetadataNode::GetOrCreateInternal`, the one place a metadata tree node becomes
a `MetadataNode`. It logs each node once. It is a diagnostic build only.

> The flag has to reach the compiler, not just Gradle. Confirm with
> `grep NS_METADATA_USAGE_TRACE runtime/.cxx/Debug/*/arm64-v8a/compile_commands.json`
> and match the APK's `.so` build-id to that config -- a stale `.so` packaged by
> AGP has produced convincing all-green runs of a feature that was compiled out.
> Note also that `-Pengine=V8-13` builds the **napi** tree; only
> `-PbindingLayer=jsi` compiles the tree these instruments live in.

**Static harvest** -- what an analysis of the JS can see:

```bash
node platforms/android/test-app/build-tools/metadata-filter/harvest.js \
     platforms/android/test-app/app/src/main/assets/app --json harvest.json
```

## What the measurements say

Static harvest of the 203 app JS files found **179** class names. The runtime
resolved **192** classes and **48** packages. The overlap is worse than those
totals suggest: **56 classes the app needs were invisible to the parser.**

But the headline result is the one that comes from feeding the *exact ground
truth* back in as a whitelist:

| Metadata | Size | Result |
|---|---|---|
| Unfiltered | 2.83 MB | 520 specs, 0 failures |
| Exact ground truth, 192 classes | **136 KB** | **crashes at launch** |
| …plus `android.os.Bundle` (1 class) | 136 KB | runs; 15 failures |

The size prize is real -- **95%**. And a filter containing every class the app
was *observed to use* still cannot start the app.

## Why perfect knowledge of usage is not enough

`android.os.Bundle` is never named in the JS and never resolved as a metadata
node. It appears only as the parameter type of `Activity.onCreate(Bundle)`.
Strip it, and the runtime cannot rebuild that signature:

```
java.lang.NoSuchMethodError: no non-static method
    "Landroid/app/Activity;.onCreate(Ljava/lang/Object;)V"
```

The parameter degraded to `Object` and no longer matched. **Metadata has
referential integrity**: a retained method drags in every type in its
signature. The 15 failures that remain after adding `Bundle` are all the same
shape -- `CharSequence` missing from a return type, `StackTraceElement` missing
from `Throwable.getStackTrace()`.

This is the failure mode that would make a naive filter ship: it passes the
developer's happy path and breaks on the screen nobody tested.

## The two lookup paths, and why only one is forgiving

Missing metadata is **not** uniformly fatal. `MetadataReader::GetOrCreateTreeNodeByName`
falls back to `CallbackHandlers::GetTypeMetadata` -> `com.tns.Runtime.getTypeMetadata`,
which builds the type description **reflectively at runtime**. So a class
reached *by name* -- marshalling a Java object whose concrete class was
stripped -- recovers by itself, more slowly.

Property access from JS does not. Package objects are built by enumerating the
metadata tree (`CreateTopLevelNamespaces`, `CreatePackageObject`), so a stripped
class is simply absent:

```
// with `java.text:*` blacklisted
TypeError: Cannot read property 'NumberFormat' of undefined
```

Measured: blacklisting `java.text` cost exactly 1 of 520 specs -- a hard,
unrecoverable throw.

**Design consequence.** Concrete types the app never names (`WindowManagerImpl`,
`DirectByteBuffer`, `ArrayList` obtained from a return value) are safe to strip:
the reflective path covers them. Anything the JS *names*, and anything reachable
through a retained signature, is not.

## What a parser cannot see

Every one of the 56 misses, classified -- this is the list any design has to answer:

| # | Category | Examples | Visible statically? |
|---|---|---|---|
| A | **Supertypes** of a used class | `Dialog` under `AlertDialog`; `ContextWrapper`; `Throwable`; `AbstractList` | No, but computable from the class graph |
| B | **Signature types** of retained members | `android.os.Bundle`, `CharSequence`, `StackTraceElement` | No, but computable from the class graph |
| C | **Nested classes** | `Button1.InnerButton`, `ViewGroup.MarginLayoutParams` | No, but computable |
| D | **Platform-returned concrete types** | `WindowManagerImpl`, `DirectByteBuffer`, `PathClassLoader` | No — and *not* computable (unbounded downward closure) |
| E | **Thrown types** | `FileNotFoundException`, `ClassCastException`, `LinkageError` | No |
| F | **Runtime's own dependencies** | `com.tns.NativeScriptException`, `java.lang.reflect.Method` | No — belongs to the runtime, not the app |
| G | **Computed access** | `java.lang[name]`, `Class.forName(s)` | Package only |
| H | **Unparseable files** | minified/exotic syntax | Nothing at all |

A, B and C are the large ones and they are all *mechanical*: the generator
already holds the whole class graph in `SecuredClassRepository`. D and E are
covered by the reflective fallback for the marshalling path. F is fixed by a
keep-list that ships with the runtime. G and H are the genuinely undecidable
cases, and are what the easing rules exist for.

### One concrete bug this exposed

`harvest.js` recognises a Java FQN by matching its first segment against a
hardcoded root list (`java`, `android`, `com`, …). The test-app contains
`in.tns.tests.JavascriptKeywordClass`, whose root is the JS keyword `in` and
which the runtime therefore exposes as `$in` (`CreateTopLevelNamespaces`
prefixes keywords with `$`). The hardcoded list misses it entirely.

A hardcoded root list is also the thing most certain to rot: it is a guess about
which top-level packages will exist. **The root set must be derived from the
metadata tree itself**, and the `$`-prefix mangling honoured. That single change
is most of the "still correct in ten years" requirement.

## Proposed design

**Seed** (what the app is known to want)

1. Static harvest of the final JS bundle -- the same bundle the static binding
   generator consumes, so there is one bundling step and one source of truth.
2. `AndroidManifest.xml`: every `android:name` (activities, services,
   receivers, providers, `application`).
3. SBG output: every `.extend` base and `@Interfaces` entry.
4. A keep-list versioned with the runtime, for the classes the runtime itself
   resolves (`com.tns.*`, the boxed primitives, `java.lang.reflect.*`, the
   `Throwable` hierarchy, `java.nio.ByteBuffer`). It lives beside the code that
   depends on it so the two cannot drift.

**Closure** (computed in the generator, which already has the class graph)

5. Transitive supertypes and implemented interfaces.
6. All nested classes of a retained class.
7. All types appearing in the signatures of retained members, then *their*
   supertypes. This is the step whose absence crashes the app at launch, and it
   is the one no JS-side tool can do.

**Easing** (the user-facing safety valve, and the reason this can be default-on)

8. Any construct the harvester cannot resolve widens the filter instead of
   narrowing it: a computed access under `java.lang` keeps `java.lang:*`; a
   package alias keeps that package; **a file that fails to parse disables
   filtering for the whole build**, loudly. Over-inclusion costs bytes;
   under-inclusion costs a crash. `harvest.js` already emits this as its
   `roots` set — in the measurements above it independently flagged
   `android.support.design`, which was one of the 15 failures.
9. App-authored keep rules, in the existing `.mdg` grammar.

**Verification** (so a mistake is caught at build time, not in the field)

10. A build-time check that the filtered tree is referentially closed: every
    type named by a retained signature is itself retained. Cheap, and it is
    exactly the invariant whose violation produced the launch crash above.
11. A diagnostic mode that ships unfiltered metadata but logs what a filtered
    build *would* have dropped, so an app can be audited before switching over.

**Rollout.** Off by default; opt-in per app; on by default only once it has
carried the 520-spec suite and the RN fixture. The generator's own default is
already fail-open -- absent `whitelist.mdg`, `UserPatternsCollection` allows
`*:*` -- so a build that cannot analyse its input ships everything, which is the
correct direction to fail.

## Status

Done: both instruments, the measurements above, the failure taxonomy.
Not started: closure (5-7), easing (8-9), verification (10-11).
