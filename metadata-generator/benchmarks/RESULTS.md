# Metadata generator benchmark — 2026-08-01

Baseline commit: `15e5418cb07c01e3a567b4791325b87809668bd9`
(`origin/refactor`). Workload: the full macOS 26.5 SDK, binary metadata,
Foundation/AppKit TypeScript declarations, umbrella header, and signature
dispatch bindings. Each result below is the median of five warm-cache runs from
`run-macos.sh`.

Host: Apple M5 Pro (18 cores, 64 GiB), macOS 27.0 (26A5378n), Xcode 26.6,
Apple Clang 21.0.0, CMake 4.3.2.

| Measurement | Baseline | Optimized | Change |
| --- | ---: | ---: | ---: |
| Wall time | 6.98 s | 4.94 s | -29.2% |
| User CPU | 5.62 s | 4.35 s | -22.6% |
| System CPU | 0.80 s | 0.55 s | -31.2% |
| Retired instructions | 54.86 B | 52.66 B | -4.0% |
| Maximum RSS | 1,212,743,680 B | 1,181,007,872 B | -2.6% |
| Peak footprint | 1,125,811,544 B | 1,095,992,640 B | -2.6% |

All 207 generated artifacts were byte-for-byte identical in every baseline and
optimized run. The primary changes were parsing declarations with
`CXTranslationUnit_SkipFunctionBodies` and reserving Objective-C class member
storage before constructing members.

## Profile summary

The baseline Time Profiler trace contained 4,735 ms of sampled CPU time:

| Stage | Sampled CPU | Share |
| --- | ---: | ---: |
| Clang parse | 3,093 ms | 65.3% |
| IR construction/post-processing | 1,311 ms | 27.7% |
| Metadata serialization | 164 ms | 3.5% |
| TypeScript emission | 64 ms | 1.4% |
| Umbrella discovery | 36 ms | 0.8% |

The largest generator-level CPU sites were `MetadataFactory::process` (889 ms),
retained-return attribute detection (614 ms), class processing (421 ms), and
`MetadataFactory::postProcess` (417 ms). Token/pretty-print scans accounted for
562 ms. `open` and `stat` accounted for about 360 ms; output writing was not a
dominant cost.

A malloc-stack snapshot at an 808.7 MiB process footprint was dominated by
libclang diagnostics, `SmallVector`, `StringMap`, and source buffers. The largest
generator-owned site was Objective-C class member-vector growth (26.9 MiB),
which motivated the exact-capacity reservation.

## Opt-in filtering example

A small AppKit bundle using `NSView` produced a conservative whitelist retaining
Foundation, Runtime, the referenced symbols, and dependency closure:

| Output | Unfiltered | Filtered | Change |
| --- | ---: | ---: | ---: |
| Binary metadata | 6,426,238 B | 485,325 B | -92.4% |
| Signature bindings | 8,447,422 B | 1,322,558 B | -84.3% |
| TypeScript output | 14,864 KiB | 1,024 KiB | -93.1% |
| Wall time | 6.98 s median | 3.80 s | -45.6% |
| Maximum RSS | 1,212,743,680 B | 982,958,080 B | -18.9% |

This is an illustrative workload, not a size guarantee; the dependency closure
and default Foundation/Runtime retention intentionally prefer false positives
over missing native metadata.

## Production bundle and build integration

`validate-bundles.sh` bundles the three repository macOS NativeScript examples
with pinned esbuild 0.25.8. The original source files, separately minified ESM
bundles, and minified dynamic-import code-split output each produced the same 48
rules, with no diagnostics or fail-open marker. All three whitelist files were
byte-identical with SHA-256
`ecf7218d52356b797bb723d0594ce8b0832308137551630977ff189ff1e8af4d`.

The full TestRunner app corpus exercised 184 JavaScript files, including worker
files, intentional invalid-syntax fixtures, `eval`, and dynamic global access.
The analyzer used 18 workers, reported three diagnostics, and emitted `*:*` as
designed, preserving all metadata.

Across 100 warm-cache process invocations of that 184-file corpus, analysis
averaged 9.9 ms with one Rayon worker and 6.5 ms with 18 workers (-34.3%). The
whitelist output was identical at every worker count.

The real Xcode TestRunner build completed with automatic filtering enabled and
ran its focused Metadata test suite (2 specs, 0 failures). The generated
fail-open metadata was 6,457,719 bytes and signature dispatch output was
8,477,285 bytes. A separate end-to-end build-step run using the statically
analyzable split example bundles generated 533,247 bytes of metadata, 1,403,490
bytes of signature bindings, and 1,148 KiB of TypeScript declarations; every
referenced native symbol was present in the declarations.
