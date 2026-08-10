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
> Note that `-Pengine=V8-13` builds the **napi** tree and `-Pengine=HERMES
> -PbindingLayer=jsi` the jsi one. Both carry the tracer; they did not always,
> and a trace taken against the tree you did not build is empty rather than
> wrong, which is easy to misread as "nothing resolved".

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

## Result

Implemented and measured on the same 520-spec suite:

| Metadata | Size | Result |
|---|---|---|
| Unfiltered | 2.83 MB | 520 specs, 0 failures |
| Exact usage, no closure | 136 KB | crashes at launch |
| **Generated seed + closure** | **700 KB** | **520 specs, 0 failures** |

**75% smaller, suite green.** The seed is 449 classes harvested from the app's
own JS; the closure grows it to 2,183 -- and stops there, well short of the
classpath, so the unbounded closure is affordable and no depth limit is needed.

Getting from "runs with 15 failures" to green took four fixes, each a category
the first measurement predicted:

1. **Signature closure** -- supertypes, nested classes, and the types in every
   retained signature. Launch crash -> runs.
2. **Kotlin extension functions** -- declared in a file class the app never
   names and that nothing references, so the closure has to reach it from the
   receiver side. Extension collection now runs *before* the closure, and
   unfiltered, or the classes it needs are already gone. 10 failures -> 2.
3. **Keyword-mangled roots** -- `in.tns.tests.JavascriptKeywordClass` is written
   `$in.…` in JS. The `$` prefix is now treated as proof of a Java package, so
   this no longer depends on the hardcoded root list.
4. **Package spine** -- `if (android.support.design && …)` threw once every
   class under `android.support` was filtered out and the package node went
   with them. Empty package nodes are recreated for packages the seed names --
   but only up to the deepest prefix that exists on the classpath. Inventing
   the rest breaks the same guard the other way: it passes, and the constructor
   behind it is undefined.

### ProGuard/R8 keep rules

The generator writes `metadata-keep-rules.pro` (build dir, not assets) from the
same retained set. Metadata says what JS can name; R8 decides what survives into
the dex; neither can see the other's input, so both are driven from one set.
Classes absent from the rules are absent from the metadata too, so R8 is free to
remove them. Shrinking is still off by default -- the rules make it safe to turn
on, they do not turn it on.

Members are kept wholesale per retained class. Per-member rules would need the
JS side to have resolved every call exactly, which it cannot; the class-level
strip is where the size comes from anyway.

## Turning it on

One flag, both builds. Off by default.

```bash
# runtime test-app
./gradlew :app:assembleDebug -PnsFilterMetadata

# a React Native app (release variants only -- debug has no bundle on disk)
./gradlew :app:assembleRelease -PnsFilterMetadata
```

The seed is derived automatically from the app's own JS: the test-app reads its
asset directory, and the RN plugin reuses the bundle the static binding
generator already produces, so no third Metro run. An app-authored
`whitelist.mdg` is concatenated with the generated one rather than replacing it
-- both are keep-lists, so a hand-written entry can only ever add.

A build without the flag ignores any generated seed still sitting in the working
directory. Consulting the file rather than the flag is how the RN plugin first
went wrong: the seed task's output survives, so every later unfiltered build went
on filtering against it. The flag is also a declared task input, since turning it
off leaves no file change behind for Gradle to notice.

## The two checks

Neither one subsumes the other, and the distinction matters.

**Soundness** -- `ClosureVerifier`, in the generator, no device needed. Proves
the emitted metadata is internally consistent: no signature quietly rewritten.

**Completeness** -- `verify-coverage.js`, needs a traced run. Proves the app did
not resolve anything the filter dropped. The soundness check is blind to this:
a class the JS names that never entered the seed produces no substitution for
it to notice, so nothing in the generator ever hears about it.

Run completeness like this:

```bash
./gradlew :app:assembleDebug -PnsFilterMetadata -Pmetadata-usage-trace
# run the app through as much of itself as possible, capturing logcat, then:
node build-tools/metadata-filter/verify-coverage.js \
     --trace logcat.txt --keep-rules app/build/nativescript/metadata-keep-rules.pro
```

On the 520-spec suite: **179 classes resolved from metadata, all retained; 13
rebuilt by reflection**. That second number is the fallback path working as
designed -- `WindowManagerImpl`, `DirectByteBuffer`, `DecimalFormat` and the
Kotlin companions are absent from the filtered metadata and recovered at
runtime. The trace marks them `R` rather than `F` so the check does not count
them as misses.

A third check, `checkMetadataRuntimeKeepList`, guards the one hand-written part
of the seed: it greps the runtime C++ for class-name literals and fails if
`RUNTIME_KEEP` no longer covers them. It needs no device and is wired into
`check`.

## The soundness check

`ClosureVerifier` fails the build if the filter changed the meaning of any
signature it emitted. It does not re-derive the closure -- it watches what
generation actually did, so it catches a closure that is wrong as well as one
that is incomplete. Only substitutions of types that exist on the classpath
count; a type that is genuinely absent is widened unfiltered too.

Verified in both directions. Clean on the working filter, and with
`android.os:Bundle` blacklisted to simulate a closure bug:

```
Metadata filter is unsound: 1 signature type(s) were replaced by a supertype
because the filter dropped them. Each is a NoSuchMethodError waiting to happen
at runtime.
  android.os.Bundle -> android.os.BaseBundle (needed by android.animation.LayoutTransition)
```

That is the exact defect that produced the launch crash, caught at build time.

## App size

Release APK, arm64 only, Hermes, `-PnsFilterMetadata` (which also turns on R8
with the generated keep rules). Clean build on each side.

| | Unfiltered | Filtered + R8 | Saved |
|---|---|---|---|
| APK total, as built | 28,899,229 | 26,432,698 | -2,466,531 (-8.5%) |
| **APK minus test fixtures** | **7,902,658** | **5,436,127** | **-2,466,531 (-31.2%)** |
| dex (uncompressed) | 7,060,432 | 1,986,864 | **-5,073,568 (-71.9%)** |
| metadata (compressed) | 939,188 | 238,698 | -700,490 (-74.6%) |

Read the second row, not the first. 21 MB of this APK is four
`assets/app/modules/libCalc-*.so` test fixtures -- the same native test module
for all four ABIs, byte-identical in both builds. They live under `assets/`
rather than `lib/`, so `-PonlyArm64` does not filter them, and no real app
carries anything like them. Against the payload an app actually ships, the
saving is about a third of the APK.

**The dex is where the win is** -- 7.1 MB to 2.0 MB. Metadata filtering saves
0.7 MB on its own; letting R8 strip the same set saves five megabytes more. The
two only work together: R8 cannot see that JS reaches a class, so without the
generated keep rules it strips the app's own dependencies, and with a keep-all
rule it strips nothing.

What is left after the dex is native code that no metadata work can touch:
libhermesvm 1.83 MB, libNativeScript 0.50 MB, libc++_shared 0.45 MB, libfbjni
0.06 MB, libjsi 0.04 MB -- 2.9 MB compressed in total. Once the dex is shrunk,
the engine is the floor.

Verified: **520 specs, 0 failures** on the shrunk release APK, not just on the
filtered metadata.

> Measure on a clean `packageRelease`. Incremental packaging reuses the previous
> APK layout and pads the freed space instead of reclaiming it -- two debug APKs
> whose metadata differed by 787KB came out **byte-identical in total size**,
> which reads exactly like the filter having no effect.

### On a React Native app

The conformance fixture, release, all four ABIs, minification left at the
template's `minifyEnabled false`:

| | Unfiltered | Filtered | Saved |
|---|---|---|---|
| metadata (uncompressed) | 3,229,026 | 656,364 | -2,572,662 (-79.7%) |
| APK total | 62,457,492 | 61,610,564 | -846,928 (-1.4%) |
| dex | 11,637,348 | 11,637,348 | 0 |

Seed 261 classes, 2,203 retained after closure, out of a classpath of 89 jars.

The dex row is the whole point of the caveat the plugin logs: filtering shrinks
the metadata whatever the app does, but the five-megabyte half of the test-app's
win came from R8, and an app with `minifyEnabled false` gets none of it. The
plugin writes the keep rules either way and wires them into R8 only if the app
already minifies -- turning minification on is the app's decision, not a side
effect of asking for smaller metadata.

Verified on device: all 16 conformance checks pass on the filtered release APK.

#### The `global.` prefix

That device run is the only reason this section is not reporting a false
success. Four checks failed on the first filtered APK --
`System.currentTimeMillis`, `Build.VERSION.SDK_INT`, and two through
`java.util.Collections` -- each with `Cannot read property … of undefined`, the
JS-property-access failure mode described above.

In a module, `java` is not a declared binding, so the natural spelling is
`global.java.lang.System`. The harvester walked that chain from its root
identifier and emitted `global.java.lang:System`, which matches no package and
retained nothing. The build was green, the closure was sound, and the app broke
on the fourth line. `staticChain` now unwraps a root of `global`, `globalThis`,
`window` or `self` before reading the package path.

Worth noting what did *not* catch it. The soundness check cannot: nothing ever
asked the generator for `java.lang.System`. Neither would `verify-coverage.js`
without a run that reaches those lines. A class the JS names but the seed never
hears about is only visible from a device.

### Why the rules say -dontobfuscate

Shrinking is kept; renaming is not. Every lookup this runtime makes is by name:
metadata names classes, JS names them through it. A `-keep` rule preserves the
names of classes it *matches*, but R8 still renames the synthetic classes it
generates itself.

That is not hypothetical. Below minSdk 24, desugaring moves a static interface
method into a synthetic companion (`Foo$-CC`), and the runtime tries the
interface and both companion spellings in turn
(`JEnv::GetInterfaceStaticMethodIDAndJClass`). With renaming on, R8 kept the
companion -- it is in `seeds.txt` -- and emitted it as `B.a`:

```
com.tns.tests.interfaces.staticmethods.StaticProducer$-CC -> B.a:
# {"id":"com.android.tools.r8.synthesized"}
```

Three specs failed with "Could not call static interface method". Adding
`-keep class **$-CC { *; }` did not help, because the defect was renaming rather
than removal. Renaming buys some string space; removing unreachable classes is
where the five megabytes come from, and that still happens.

## Known limits

Two checks and a set of heuristics cover most of what static analysis cannot
see. What remains, stated plainly:

**JS that does not exist at build time cannot be analysed.** CodePush/OTA
updates, `eval` of code fetched at runtime, or class names arriving from a
server. A JS update naming a class the shipped filter dropped will fail, and no
build-time check can see it coming. **Do not enable filtering on an app that
ships JS out of band.** This is not a gap that can be closed -- it is the same
reason R8 cannot shrink a reflective Java app safely.

**`-dontobfuscate` is forced whenever filtering is on.** Correct for a
name-driven bridge, but it removes obfuscation as an option for apps that want
it for other reasons. Shrinking, where the size comes from, is unaffected.

**Coverage checking is only as good as the run.** `verify-coverage.js` proves
nothing the traced run touched is missing. A screen the run never opened is not
covered, so point it at the broadest run available.

**The RN path is release-only**, so an app can pass in debug and fail in
release. That is the safer direction (debug is unfiltered) but it means the
filter's correctness is exercised on the less frequently run build.

## Both binding layers

The instruments and the class-naming scheme they assume live in the runtime, and
the runtime has two independent trees: `NativeScript/ffi/jni/napi` and
`NativeScript/ffi/jni/jsi`. Everything here was built in the jsi tree first, so
for a while the tracer, `synthesizedAtRuntime` and content-keyed bindings simply
did not exist on the napi side -- which is also the **default** binding layer
(`bindingLayer` defaults to `napi` in `runtime/build.gradle`). A napi build with
`-Pmetadata-usage-trace` produced no trace at all, and `-Pcontent-keyed-bindings`
was a no-op there while the static binding generator went on emitting hashed
names, quietly sending every extend down the on-device dex path.

The napi tree now carries all three, and `CollectImplementedInterfaceNames` /
`CollectMethodOverrideNames` are factored out of the JNI array builders on both
sides, so the hashed set and the set handed to the generator cannot drift apart
within a tree. Verified on napi (`-Pengine=V8-13 -Pcontent-keyed-bindings`):

| | jsi | napi |
|---|---|---|
| spec suite | 520 / 0 | 520 / 0 |
| classes generated on device | 27 | 27 |
| of those, content-keyed | 17 | 17 |
| `verify-coverage.js` under filtering | clean | clean |

The one intentional difference: jsi's `ContentKeyedBindingsEnabled` also returns
true under `NS_JSI_HOST_RUNTIME`, because a guest runs inside the embedder's
bundle and can never agree with the generator on source coordinates. napi is
never embedded that way, so there it is the `NS_CONTENT_KEYED_BINDINGS` define
alone.

## Status

Done: the instruments, the taxonomy, the closure, the easing rules, the seed
generator, the keep-rules emitter, the soundness check, Gradle wiring for both
the test-app and the RN plugin, and parity across both binding layers. Green on
the 520-spec suite with filtering on, on a React Native release build, and the
size numbers above.

Not done: R8 shrinking is still off by default -- the keep rules make it safe to
enable, they do not enable it. On React Native that is the whole dex saving, and
the template ships `minifyEnabled false`.
