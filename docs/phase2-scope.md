<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: plan.md §4 (Phases G2–G4), §2 architecture, ADR-0002/ADR-3/A02;
     AOSP frameworks/base + host-buildable native libs (Apache-2.0); this session's
     host-ART spike (docs/art-host-spike.md). No GPL-3.0 sources. -->

# Phase 2 scope — "real apps execute" on host ART

Phase 1 gave us: host ART runs DEX natively, verifies/AOT-compiles real obfuscated apps
(Spotify 9,817/9,820, 0 VerifyError), and runs a trivial real `Activity` lifecycle against
acl's 41-shim framework (`--vm=art`, `run-apk-activity-art`). Phase 2 is the hard core:
**a real Android app launches, draws its UI to a Wayland window, and takes input.**

## North star & realistic first target

- **First target (P2 "done" for a meaningful demo):** a *standard-View* F-Droid app
  (e.g. 2048, a calculator, a simple notes app) launches, inflates its layout, draws to a
  `wl_surface`, and responds to touch/key. This exercises the whole stack without the
  extreme demands below.
- **Stretch (later):** Spotify-class apps — native `.so` cores, DRM, Jetpack-Compose/Skiko.
  It verifies today but needs the full native + render + services stack plus its own libs.
  NOT the P2 bar; it's the ultimate proof, staged after the corpus works.

## The strategic reframe (this session changes the plan)

`plan.md` assumed a largely **clean-room reimplementation** of the native underside. Two
facts found this session argue for far more **reuse of Apache-2.0 AOSP host artifacts**:

1. Host ART runs the app's DEX natively and loads the **real** `android.*` framework
   (`framework-minus-apex` + `ext`, ~2,729 core classes) — no need to hand-shim it.
2. **`libandroid_runtime` and `libandroidfw` are `host_supported: true`** — the framework's
   JNI floor and the resources/AssetManager native layer *build for host* (AOSP uses them
   for layoutlib / host tests). So the plan's 41 hand-shims become a fallback, not the base.

**Revised model (an ADR-0002 refinement to record):** vendor the **real** framework +
**reuse** AOSP's host-buildable native libs, and *substitute only at the device boundary* —
SurfaceFlinger → Wayland/Skia (ADR-A02), Binder/SystemServer → the in-process `svc` hub
(ADR-3). Shims survive only where a host lib is hopelessly device-tied. This is closer to
how layoutlib/Robolectric-native operate, and it collapses a huge amount of Phase-2 work.
Risk: `host_supported` means *builds*, not *fully functions* — layoutlib stubs plenty. The
spikes below measure how far the real libs actually get before a stub is needed.

## Gap analysis (what a real app needs that we lack)

| Need | Have now | Gap |
|---|---|---|
| Framework classes | 41 shims + android.jar stubs | the real ~2,729-class framework, dex2oat'd |
| Framework JNI floor | ~8 hand-bound natives | `libandroid_runtime`/`libandroidfw` (host) wired to ART |
| App native `.so` | none | `System.loadLibrary` → extract/dlopen/JNI_OnLoad |
| Resources | manifest AXML only (pkg/) | `resources.arsc`/assets via `libandroidfw` AssetManager |
| Render/window | Bitmap-Canvas shim → gfx | View measure/layout/draw → Skia → `wl_surface` (wm/gfx) |
| Services | ActivityManager/Window/Media stubs | the svc-hub Manager surface (no Binder, ADR-3) |
| Input/timing | present-path input events | Choreographer on `wp_presentation`, real InputManager |

## Milestones (each a hard gate; dependency-ordered)

### P2.1 — Real framework on ART · the foundation
Build `framework-minus-apex` + `ext` as DEX from the pinned tree, `dex2oat` them onto the
core boot image → a **framework boot image** (replaces `-Xint` + the shim classpath). Wire
`ArtVm` to load it (extends `ACL_ART_FRAMEWORK`/boot-image).
**Gate:** ART loads the real `android.app.Activity` (AOSP impl) + its transitive deps with
0 VerifyError (the Spotify-verify method, aimed at the framework itself). This surfaces
exactly which classes are SystemServer/Binder-entangled → the substitution boundary.

### P2.2 — Native floor · reuse host libandroid_runtime / libandroidfw
`dlopen`/register the host native libs so real `Context`/`Resources`/`AssetManager` and
`android.os`/`android.util` natives resolve. Substitute the device-tied entry points
(SystemProperties, Binder, display) against the `svc` hub.
**Gate:** a real app reads its own `resources.arsc` + assets and a real `ContextImpl` +
`Resources` construct without a fatal native gap.

### P2.3 — App native `.so` loading
`System.loadLibrary`/`Runtime.load`: pick the ABI (`pkg/` split/ABI selection), extract
`lib/<abi>/*.so` from the APK, `dlopen`, run `JNI_OnLoad`, honor `RegisterNatives`.
**Gate:** an app with a trivial native lib calls into it on ART (unblocks native-core apps).

### P2.4 — View → Skia → Wayland (the visible payoff)
Re-target `android.graphics` to host Skia (ADR-A02, hwui dropped); drive View
measure/layout/draw into a Skia surface presented on a `wl_subsurface`; Choreographer on
`wp_presentation`; damage/buffer-age.
**Gate:** the standard-View F-Droid app **draws its real UI** to a Wayland window
(screenshot-asserted under the Tier-C compositor matrix).

### P2.5 — Services · the svc hub, no Binder (ADR-3)
Flesh out the Java-visible Managers (Activity/Window/Package/Input/…) as direct C++ calls.
**Gate:** the app navigates (start Activity, query package/window/input) end to end.

### P2.6 — Corpus + CTS
Run the 10–15 hash-pinned F-Droid corpus under the screenshot + input-replay harness; the
50-test CTS smoke set. **Gate:** a pass-rate metric, not "looks right".

## De-risk first (spikes before committing to milestones)

### Spike A RESULT (2026-07-12) — reuse strategy VALIDATED (GO)

Built the real framework (`framework-minus-apex`, 4 dex) + host `libandroid_runtime` from
the pinned tree (~8 min) and probed on host ART:

- **Probe 1 — real framework loads/verifies (DECISIVE):** force-loaded all 26,890 framework
  classes on host ART → **26,880 verified, 0 VerifyError, 0 NoClassDefFound**; the 10 misses
  are `ClassNotFoundException` for cross-module refs (appsearch/telephony-data/vcn/services)
  deliberately outside this module — the modularization boundary, not a defect. The real
  `android.*` framework is **not** a load/verify problem on host ART. Biggest risk eliminated.
- **Probe 2 — native floor reachable:** host `libandroid_runtime.so` dlopens clean (deps
  resolve via RUNPATH `$ORIGIN/../lib64`); exports `JNI_OnLoad` + the `register_android_*`
  functions + `AndroidRuntime::registerNativeMethods`. `libandroidfw` is host_supported too.
- **Bootstrap gap pinned down (the first P2.2 task):** a bare `System.load` of
  libandroid_runtime returns `JNI_ERR` — its `JNI_OnLoad` requires an `AndroidRuntime`
  instance (`getRuntime() != NULL`), i.e. the `app_process`/zygote bootstrap. `app_process`
  is **target-only** (no host variant), so acl provides a **mini host AndroidRuntime**
  (reuse `frameworks/base/cmds/app_process/app_main.cpp` + `AndroidRuntime::start`/`startReg`
  to register the `gRegJNI` table with the JavaVM set). Bounded, well-trodden — not a wall.

**Verdict:** the plan's clean-room reimplementation of the framework + native underside is
NOT needed. Reuse the real framework + host native libs; the remaining work is the
AndroidRuntime host bootstrap + the device-boundary substitution (Binder→svc, SF→Wayland).
P2.1/P2.2 are now de-risked; **P2.2's opening move is the host AndroidRuntime bootstrap.**

- **Spike B — one View to a Wayland pixel (still TODO):** a single real `View.draw` → Skia →
  `wl_surface` round-trip (P2.4 core), independent of the framework, to de-risk the render seam.

## Sequencing & honest sizing

Phase 2 is the bulk of the remaining project (plan.md G2–G4) — **months**, not a sprint. The
first *meaningful* deliverable is **P2.1 → P2.4 minimal** (a real app draws). P2.3/P2.5 make
it interactive; P2.6 makes it measurable. Suggested order: **Spike A** (foundation) →
**Spike B** (render) → P2.1 → P2.2 → P2.4 → P2.3 → P2.5 → P2.6.

## Decisions to record before building
- **ADR update:** the reuse-host-native-libs vs clean-room-reimplement strategy (this doc's
  reframe) — refine ADR-0002; note the license posture (AOSP host libs are Apache-2.0/BSD,
  in-scope; keep the per-file license check).
- **Framework boot image vs classpath:** boot image (dex2oat, fast, matches device) vs plain
  classpath (simpler). P2.1 picks boot image.
- **How much framework:** `framework-minus-apex` + `ext` only, or also `services`/
  `telephony`? Start minimal (no SystemServer), grow to the shim boundary Spike A reveals.
