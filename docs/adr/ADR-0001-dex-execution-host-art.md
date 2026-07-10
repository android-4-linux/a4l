<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0001: DEX execution — host ART, not OpenJDK+DEX translation

- **Status:** Accepted — decision/fallback gate at Phase 1 exit (Gate G1)
- **Date:** 2026-07-09
- **Provenance:** plan.md §3 ADR-1, R-1. No GPL-3.0 sources.

## Context
Apps ship DEX. Fidelity of `invoke-custom`, hidden-API semantics, `dalvik.system.*`, and JNI
behavior matters. Two options: embed AOSP's ART (host build) or translate DEX back to JVM
bytecode and run OpenJDK.

## Decision
Embed **host ART** (AOSP, Apache-2.0). It preserves DEX/Dalvik semantics exactly. OpenJDK +
d8-reversal loses fidelity and drags in `dalvik.*` shims.

## Consequences
ART host build outside the Android build system is the single hardest infrastructure item
(R-1: timebox 4 weeks, pin an api-33 tag with known host-test targets). **Fallback gate at
Gate G1:** if host ART cannot be stood up in budget, fall back to OpenJDK+Classpath-Exception
plus a `dalvik.system` compatibility layer, accepting the fidelity loss.

## Gate-G1 confrontation — 2026-07-10 (measured; decision reaffirmed)

The JVM fallback is now built and deep: a DEX→JVM translator (`dex/`) running unmodified DEX
apps end-to-end on OpenJDK, a boot-classpath + JNI-native-underside framework seam, the
Activity/Service lifecycle, a Context hierarchy, a `svc/` Manager hub, and an automotive media
stack with real PipeWire audio — **all against SDK-docs shims** (`framework/shim/`). The open
question this ADR gates: can that fallback host the **real** vendored `frameworks/base`, or is
host ART required? Measured against the pinned tree (`third_party/aosp/METADATA` →
`android-13.0.0_r84`, checked out sparsely) with `tools/fw_extract.py`:

- Seed `{android.app, android.os, android.util, android.content}` → **914-class** in-tree closure.
- **1098 edges into JVM-absent packages**, touching **374 / 914 (~41%)** of the closure:
  `com.android.internal` 968, `libcore` 80, `dalvik.system` 43, `sun.misc`+`org.apache.harmony` 7.
  Reproduce: `tools/fw_extract.py --src <repo>/frameworks/base/core/java:<repo>/frameworks/base/graphics/java
  --seed android.app,android.os,android.util,android.content
  --denylist dalvik.system,com.android.internal,libcore,sun.misc,org.apache.harmony`.
- Even **`android.util.Log`** — the simplest useful framework class — does not compile standalone
  (`android.annotation.*`, `com.android.internal.os.RuntimeInit`, `dalvik.annotation.*`, …); only
  trivial leaf POJOs (e.g. `android.util.MutableInt`) do.

Why the fallback cannot host real `frameworks/base`, qualitatively:
- `dalvik.system.*` (`VMRuntime`, `VMStack`, `BaseDexClassLoader`, …) and `libcore.*` are
  **VM-intrinsic** — backed by ART internals, not ordinary Java. Shimming them on OpenJDK is
  exactly the fidelity loss this ADR rejects (and `libcore`'s `java.*` variant collides with
  OpenJDK's own `java.*`, which the bootstrap loader reserves).
- `com.android.internal.*` (487 files under `core/java/android` reference it) is Apache-2.0 and
  vendorable, but it triples the subset surface and itself re-enters the intrinsics.

**Decision reaffirmed: host ART is the target.** The DEX→JVM fallback is validated for exactly
what it is — the **development/CI execution vehicle** and the **runtime seam** — not the
production framework host. It runs translated app DEX and SDK-docs shims faithfully; it will not
carry real `frameworks/base`. The seam is ready for ART to drop in: `runtime::Backend::Art`
(reports Unavailable today), `VmConfig.boot_classpath`, and the DEX/OAT `cache_dir` already model
what a host-ART build consumes.

**Trigger + what standing up host ART requires** (the R-1 item, unblocked once budgeted): a
`repo`-synced platform tree at the pinned tag; a Soong host build of `art` (host) + a host boot
image (`boot.art`/`boot.oat`) from the vendored `frameworks/base` subset + `libcore`; then embed
via `JNI_CreateJavaVM` against `libart.so` with `-Ximage:<boot image>` behind `Backend::Art`
(mirroring the JVM backend). Until then, the fallback stays the working vehicle and every
`framework/shim/` class is a deliberate, provenance-tagged stand-in for a class that host ART
would instead load from real AOSP.
