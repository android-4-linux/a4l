<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0008: Framework native floor — reuse AOSP host libs, not clean-room reimplement

- **Status:** Accepted (refines ADR-0002's native-underside clause)
- **Date:** 2026-07-12
- **Provenance:** plan.md §2, §3 ADR-2; ADR-0002; Spike A in docs/phase2-scope.md +
  docs/art-host-spike.md; AOSP `frameworks/base` + host-buildable native libs (Apache-2.0).
  No GPL-3.0 sources.

## Context
ADR-0002 vendored the real AOSP Java framework but committed to **replacing the native
underside** (`android_view_*`, `android_os_*`, … JNI registrations) with clean-room acl C++ —
a very large surface. Spike A (post host-ART bring-up) tested whether that reimplementation is
actually necessary once the app's DEX runs on host ART:

- The real framework (`framework-minus-apex`, 26,890 classes) **loads and verifies on host
  ART**: 26,880 verified, **0 VerifyError**, 0 NoClassDefFound (the 10 misses are cross-module
  refs — appsearch/telephony/services — the modularization boundary). So the Java framework
  runs as vendored; no hand-shimming of `android.*` behavior is required.
- `libandroid_runtime` and `libandroidfw` are **`host_supported: true`** (Apache-2.0). The host
  `libandroid_runtime.so` dlopens clean and exports `JNI_OnLoad` + the `register_android_*`
  functions + `AndroidRuntime::registerNativeMethods` — a standard registration seam.
- The one gap: `libandroid_runtime`'s `JNI_OnLoad` requires an `AndroidRuntime` instance (the
  `app_process`/zygote bootstrap), and `app_process` is target-only.

## Decision
Reuse the **real** AOSP framework (loaded natively on host ART) **and** the host-buildable AOSP
native libraries (`libandroid_runtime`, `libandroidfw`, …) as the framework's JNI floor, rather
than clean-room reimplementing the native underside. acl provides a **host `AndroidRuntime`
bootstrap** (adapted from `frameworks/base/cmds/app_process/app_main.cpp` + `AndroidRuntime::
start`/`startReg`) to register the `gRegJNI` table, and substitutes **only at the device
boundary**: Binder/SystemServer → the in-process `svc` hub (ADR-0003); SurfaceFlinger → Wayland
+ Skia (ADR-0004). Clean-room acl C++ is written only where a host lib is hopelessly device-tied
or absent. This **refines** — does not revoke — ADR-0002: the framework is still vendored AOSP;
only its "replace the native underside" clause is changed to "reuse where host-buildable".

## Consequences
Collapses the largest Phase-2 surface (native reimplementation) into (a) a bounded
`AndroidRuntime` host-bootstrap — the opening P2.2 task — and (b) targeted device-boundary
substitution. The shape is layoutlib-like (real framework + host natives + stubbed device
edges), not from-scratch. License posture is unchanged and in-scope: the reused libs are
Apache-2.0/BSD; keep the per-file license gate; no the predecessor, no GPL.

Risks and follow-on:
- **`host_supported` = builds, not fully functions.** Layoutlib stubs plenty; expect a tail of
  device-tied natives (display, SystemProperties, some Binder paths) that need substitution
  against the `svc` hub. Sized incrementally as the bootstrap brings classes online.
- **ABI pinning.** Runtime (ART), framework DEX, and native libs must all be built at one tag
  (**android-13.0.0_r84**); the reused native ABI is tied to that framework build.
- **Fallback.** Where a reused native proves unworkable on host, drop that class to the
  ADR-0002 shim / clean-room path — the two approaches coexist per class.

## Refinement (2026-07-12, from the A(2) bridge spike)
The reuse is a **hybrid**, narrower than first hoped. Host `libandroid_runtime` builds only its
~12 device-independent sources (SystemProperties, Log, Trace, SystemClock, EventLog, ... → the 33
exported `register_android_*`). The framework-heavy JNI — `AndroidRuntime`/`startReg`,
`android_util_AssetManager`, View, graphics, Parcel, Binder — sits in the `Android.bp`
`target: { android: {...} }` block and is **not compiled for host** (device-tied deps:
SurfaceFlinger, Binder, HALs). So a real Activity's native floor (Resources/AssetManager/View)
is not reachable by simply dlopening the stock host lib. Getting it means enabling those `.cpp`
for host one subsystem at a time and **stubbing each device dep against acl's substitutes**
(SurfaceFlinger→Wayland/Skia per ADR-0004, Binder→svc hub per ADR-0003) — the same substitution
work as P2.4/P2.5, entered from the native side; `libandroidfw` (resources) is already
`host_supported`, so resources is the first candidate. This is what AOSP's own host framework
(ravenwood/layoutlib) does. See docs/phase2-scope.md "A(2)".

**Correction (2026-07-12, from R1):** the hybrid boundary is narrower than A(2) first stated.
The host `libandroid_runtime` (via its LayoutlibLoader host srcs block) DOES compile and export
**31 framework registrars** — including resources (`register_android_content_AssetManager`,
ApkAssets, XmlBlock, StringBlock), Looper (`MessageQueue`), the input stack, Parcel, SQLite,
animation. A(2)'s "AssetManager is target-only" was a wrong-name search
(`register_android_content_res_AssetManager` does not exist; the symbol is
`register_android_content_AssetManager`). So acl reuses these directly (R1: ArtVm registers 28).
Only the **render** natives (View, Bitmap/Canvas, Configuration) are genuinely target-only and
still need the P2.4 SurfaceFlinger→Wayland/Skia substitution. Resources beyond the native is now
a **data** problem (framework-res.apk), not a native one.
