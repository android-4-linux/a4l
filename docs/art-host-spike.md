<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: ADR-1 / ADR-0001 (host-ART vs OpenJDK fallback), plan.md §3 R-1, §4
     Phase 1 exit gate; AOSP art host-build docs (public, Apache-2.0). No GPL-3.0 sources. -->

# Host-ART spike — scope & go/no-go plan (the ADR-1 decision)

## The question this spike answers

> Can we stand up an **embeddable host ART** (`libart.so` + a boot image) at the api-33
> pin and run a DEX through it from `acl`, **in budget**?

`--vm=art` (`Backend::Art`) is the plan's *target*; the DEX→JVM translator (`--vm=jvm`,
ADR-0001) is the *fallback*. Session evidence tipped the decision: the translator loads
real F-Droid apps at ~85% but has an unbounded verify-correctness tail **and** a hard
ceiling — it can never run native `.so` code and never pass CTS (see the DEX-translator
commit chain and the ComponentActivity uninitialized-object / "Bad local variable type"
findings). ART removes both: it *is* the reference Dalvik runtime.

**The spike does NOT commit to ART.** It buys the go/no-go with the least investment:
boot host ART and print from a trivial DEX. If that works in budget → commit (M3+);
if M1 blows the budget → the OpenJDK fallback stands and we keep the translator for the
automotive host paths (media/CAL never need native code or full fidelity).

## RESULT (2026-07-12) — M1 PASSED, ADR-1 = GO

Host ART **boots and runs a real DEX**, in well under budget (hours, not weeks). Decision:
**GO** — pursue host ART for the full-app path; the DEX→JVM translator stays only for the
automotive hosts.

- **Build tree:** the ART-team **`master-art`** reduced manifest (coherent, ~1 hr sync +
  ~16 min build) — NOT the android-13 full-platform tree, whose *partial* sync hit pervasive
  mainline/APEX entanglement (see scratchpad/partial-tree-adaptations.md; abandoned).
  master-art is main-branch and **throwaway** — see "Version locking" below.
- **Build:** `lunch silvermont-trunk_staging-eng && art/tools/buildbot-build.sh --host`.
  (Must `unset -f grep egrep fgrep sed awk` first — this shell wraps them with a ugrep tool
  that breaks AOSP `lunch` parsing.)
- **Artifacts:** `out/host/linux-x86/{lib64/libart.so, bin/{art,dalvikvm64,dex2oat64,d8}}` +
  boot image `out/host/linux-x86/apex/art_boot_images/javalib/x86_64/boot.art`.
- **Verified:** `art -Ximage:.../javalib/boot.art -cp hello.dex HelloWorld` → prints, exit 0
  (boot-image-backed; `-Ximage` omits the ISA subdir — ART inserts `/x86_64/`).

### Spotify verification experiment (2026-07-12) — the translator-killer, confirmed

The app that defeated the DEX→JVM translator (obfuscated R8, native `.so`, uninitialized-object
+ "Bad local variable type" verify failures). Extracted its 8 MB single `classes.dex`, provided
api-33 `android.jar` (d8'd to dex) as the app classpath for framework resolution, and force-loaded
(load+link = **verify**, no `<clinit>`) every class via a `dalvik.system.DexFile` driver on host
ART (`art -cp driver:android33:spotify Driver`):

```
SPOTIFY VERIFY: total=9820  loaded/verified=9817  VerifyError=0  NoClassDefFound=0  other=3
```

**9,817 / 9,820 classes verified clean — ZERO VerifyErrors** (99.97%). The 3 "other" are
`ClassNotFoundException` (`dc.b`, `org.jetbrains.skiko.{m,n}` — missing class *definitions*,
not verify failures). Direct contrast: the translator *stubbed* every method it couldn't verify
and hard-failed on `ComponentActivity`. **ART is the reference Dalvik verifier and ingests R8's
obfuscated output natively** — decisive evidence for the production investment. (Full *execution*
still needs the real framework (M3) + native libs; this validates load/verify, the translator's
exact failure surface.)

**AOT compile (stronger — full compiler, not just verifier):** `dex2oat --compiler-filter=speed`
on the same 8 MB dex (android.jar as `--class-loader-context`, boot image loaded via RELATIVE
`-Xbootclasspath-locations`) → **exit 0, 31.3 MB of native x86_64, ~6 s CPU on 32 threads, and
0 errors / 0 verification failures / 0 skipped-compile.** ART's resolver + register allocator +
codegen handle the whole obfuscated app. Artifacts in scratchpad/spotify/.

### M2 DONE (2026-07-12) — `Backend::Art` embeds host ART in `acl`

`src/runtime/jvm_vm.cpp` refactored: `JvmVm` → base `JniVm` (holds the VM/JNIEnv + all
JNI logic) with a virtual `create_java_vm()`; `JvmVm` and new `ArtVm` are thin subclasses.
`ArtVm` dlopens `$ACL_ART_HOME/lib64/libart.so` (RUNPATH=$ORIGIN resolves its deps),
`JNI_CreateJavaVM` with `-Xbootclasspath:<core jars>` + `-Xint` (imageless), classpath =
the app APK; sets `ANDROID_ROOT`/`ANDROID_ART_ROOT` + the non-apex `ANDROID_I18N_ROOT`/
`ANDROID_TZDATA_ROOT` data roots (else libicu_jni aborts at startup). `vm.cpp` Backend::Art
→ `make_art_vm` (gated on `ACL_ART_HOME`). Result:
```
acl run m2.apk --vm=art  →  "art started" → invoking static main() → "hello ... 42" → exit 0
```
Real host ART executes the app's DEX natively — no DEX→JVM translation. 33/33 tests green
(JVM backend unchanged). APK must be zipaligned (`d8` + `zipalign -f 4`) — ART mmaps the dex
and rejects unaligned offsets (the hand-built fixture the JVM path tolerated).

**Production locks to android-13.0.0_r84** (full sync) per "Version locking"; master-art
proved the approach only. Remaining for the real thing: the M3 framework boot image (so real
`Activity`s load) + native `.so` loading, both at the pinned tag.

## What transfers vs what's retired (why this is not a restart)

**Transfers unchanged** — the embedding seam is the JNI Invocation API, and we already
boot OpenJDK with `JNI_CreateJavaVM`; ART exports the *same* API:
- `runtime/` VM seam (`VmConfig`, `Backend::{Art,Jvm,Null}` — `Art` is already stubbed),
  class-loader/JNI plumbing.
- The **entire native underside** (`register_framework_natives` — it's `RegisterNatives`,
  runtime-agnostic).
- `pkg/` (APK/manifest/resources), `wm/gfx/audio/svc`, the automotive hosts, the
  vendored-framework work (ADR-0002 — becomes ART's boot image instead of a javac dir).

**Retired from the run path:** the DEX→JVM translator (`dex/dex_to_jvm`) — ART loads DEX
natively, so the whole StackMapTable/type-inference machinery is no longer on the
critical path (keep it for `acl dex` inspection).

## Ground truth (this machine, checked)

- `/mnt/dev/aosp` has only `frameworks` + `prebuilts`. **Missing:** `art`, `libcore`,
  `build/soong`, `build/make`, `prebuilts/clang/host`, `bionic`, `external/*`,
  `system/core` — i.e. everything needed to *build* ART.
- No prebuilt host ART / `dalvikvm` / `dex2oat` / `libart.so` anywhere on disk or PATH.
- Disk OK: `/home` 697 G free (a full AOSP tree is ~250 G source + ~150 G build).
- **So the spike's cost is almost entirely M1: obtaining a working host `libart.so`.**

## Milestones (cheapest-info-first; each is a hard go/no-go)

### M0 — approach decision · ~0.5 day · DONE (this doc)
Inventory complete. Two approaches for M1; try B before A:

- **(B) Prebuilt extraction** — cheapest if it exists. Hunt for a host ART at the pin:
  the `com.android.art` **host** apex / `art-host-tests` artifacts from AOSP CI
  (`ci.android.com`, branch `android13-*`), or a distro/CI tarball. If a
  `libart.so` + host core `boot.art` for `linux_glibc_x86_64` can be obtained, skip the
  full source build entirely. **Uncertain** — host ART is a dev/test artifact, rarely
  published standalone.
- **(A) Build from source via Soong** — certain but heavy (below).

### M1 — a host `libart.so` + host *core* boot image · **the make-or-break** · ~1–2 wk
Path A concretely:
1. Expand the checkout at the api-33 pin (`android-13.0.0_r84`): `repo sync` `art`,
   `libcore`, `build/soong`, `build/make`, `build/bazel`, `prebuilts/{clang,build-tools,
   jdk,go,...}`, `bionic`, `system/{core,libbase,logging,unwinding}`,
   `external/{icu,zlib,fmtlib,...}`, `libnativehelper`, `dalvik`. (~200–300 G; hours.)
2. Build host ART for `linux_glibc_x86_64`:
   `source build/envsetup.sh && lunch aosp_x86_64-eng && m dalvikvm dex2oat libart`
   (or ART's `art/tools/buildbot-build.sh --host`). Output lands in
   `out/host/linux-x86/{lib64/libart.so, bin/dalvikvm64, bin/dex2oat}` plus the host
   **core** image `out/host/linux-x86/framework/x86_64/boot.art` (core-oj/core-libart —
   NOT the framework yet).
3. **Validate OUTSIDE aclcxx** (the true go/no-go, no integration needed):
   ```sh
   dalvikvm64 -Ximage:.../boot.art -cp hello.dex HelloWorld   # must print
   ```
   If host `dalvikvm` can't run a hello DEX, embedding can't either — so this is the
   cheapest possible validation of the *whole* ART approach, and it comes before any
   acl work.

**GO/NO-GO:** a host `libart.so` + core `boot.art` exists and `dalvikvm` runs a hello
DEX. **This gate decides ADR-1.** Risk is concentrated entirely here (Soong host build
finickiness: glibc/python/host-toolchain versions, the api-33 ART mainline **apex**
layout — host libs live under a `com.android.art` apex dir the runtime expects via
`ANDROID_ART_ROOT`).

### M2 — embed in `Backend::Art` · ~2–5 days (small; reuses existing code)
`dlopen(libart.so)` + `JNI_CreateJavaVM` with ART options (`-Ximage:<core boot.art>`,
`-Xbootclasspath:`, `ANDROID_ROOT`/`ANDROID_DATA`/`ANDROID_ART_ROOT` env), classpath =
the APK's **DEX** (no translation). Run the *same* `hello.apk` we already run on OpenJDK
— through real ART. This reuses `jvm_vm.cpp`'s JNI Invocation code almost verbatim; the
work is getting ART's options + apex/env layout right.

**GO/NO-GO:** `acl run hello.apk --vm=art` prints via real ART, no DEX→JVM step.
**The spike ends here.** M0–M2 = the ADR-1 answer.

### M3 — host framework boot image · weeks · POST-DECISION (the "commit to ART" work)
`dex2oat` the vendored `frameworks/base` + `libcore` → a framework `boot.art`, so real
`Activity`s load. This is where ADR-0002's vendored framework meets ART (and where the
android.jar-stubs boot classpath is replaced by a real dex2oat'd image). NOT part of the
spike — only started once M2 says "go".

## Version locking — NON-NEGOTIABLE (two-track)

**Nothing production is ever built from tip-of-tree `master`.** The two tracks are distinct
artifacts with distinct purposes:

- **De-risk track (throwaway):** the `master-art` reduced manifest (main branch) — the ART
  team's coherent standalone-build tree. Used ONLY to answer M1/M2 (does host ART boot + run
  a DEX; does the `Backend::Art` embedding seam work). Version-independent questions, so the
  fastest coherent tree wins. Its `libart.so`/core image are **never shipped and never used
  by `acl` in any real run** — throwaway once the seam is proven.
- **Production track (version-locked):** a **release-tag** build. The lock is
  **`android-13.0.0_r84` (API 33 / Tiramisu)** — matches `plan.md`'s target API and the tag
  already synced at `/mnt/dev/aosp`. The production host ART is built from the **full,
  coherent** platform tree at that tag (a full `repo sync`, not the partial+prune tree — the
  partial tree fought android-13's mainline/APEX entanglement; see
  scratchpad/partial-tree-adaptations.md). The `com.android.art` host apex, boot image, and
  M3 framework image are all built at this single pinned tag so runtime + framework match.
  Re-pin only by a deliberate ADR update (e.g. moving to a newer LTS), never by drift.

Sequencing: master-art proves the approach cheaply → then the full android-13.0.0_r84 build
is the thing `acl` actually links/ships against. M2 embedding code is version-independent and
carries over unchanged.

## Budget & decision criteria

| Milestone | Effort (est.) | Gate |
|---|---|---|
| M0 approach | 0.5 d (done) | — |
| M1 host libart + core image | **1–2 wk** (path A) or days (path B if a prebuilt exists) | `dalvikvm` runs a hello DEX |
| M2 embed `Backend::Art` | 2–5 d | `acl run hello.apk --vm=art` prints |
| **Spike total (M0–M2)** | **~2–3 wk** | **ADR-1 decided** |
| M3 framework image (commit) | weeks | — |

**Decide ART** if M1 yields a working host `libart.so` + core image within ~2 weeks of
effort (or path B finds a prebuilt). **Decide fallback** if M1 stalls in Soong/apex/
version hell beyond that — keep the translator for the automotive media/CAL path
(which works today and needs neither native code nor full fidelity).

## Risks / unknowns to watch
- **Host ART is a test/dev artifact**, not a product — thin docs; the apex layout
  (`com.android.art`, api-29+) complicates library resolution and image location on host.
- **Version lock:** host ART, the host core image, and the eventual framework image (M3)
  must all be the *same* api-33 pin — mismatches fail opaquely.
- **Soong host build** wants a specific host toolchain/glibc/python; a stale host env is
  the most likely time-sink.
- **`dex2oat` relocation / image checksums** on host; `-Xno-dex-file-fallback` vs
  interpret-only (`-Xint`) as a first-run simplification.
- **Distribution weight** (post-commit): shipping `libart.so` + a boot image is heavier
  than `.class` files — a real but later concern.

## Smallest first step
Before syncing 250 G, spend a day on **M1 path B**: try to pull a host ART
(`com.android.art` host apex / `art-host-tests`) from AOSP CI at `android13-*`. A hit
collapses M1 from ~2 weeks to ~days. A miss → commit to path A's source build.
