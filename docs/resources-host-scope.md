<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: docs/phase2-scope.md A(2), ADR-0008 (hybrid native floor), ADR-0002/0003;
     AOSP frameworks/base android_util_AssetManager.cpp + libandroidfw (host_supported,
     Apache-2.0) + framework-res. No GPL-3.0 sources. -->

# Resources subsystem host-enablement — scope

## R1 RESULT (2026-07-12) — done, and it corrected A(2)
R1 turned out trivial: the host `libandroid_runtime` **already exports** the resources native
(`register_android_content_AssetManager` — A(2) wrongly concluded "target-only" by searching the
non-existent name `register_android_content_res_AssetManager`). The host build compiles these
framework natives via the **LayoutlibLoader host srcs block**; **31 framework registrars are
exported** (AssetManager, ApkAssets, XmlBlock/StringBlock resources, MessageQueue/Looper, the
full input stack, Parcel, SQLite, animation). So R1 = **register them in ArtVm** — no new lib.
ArtVm now registers 28 of them (`ART native floor: 28 framework native group(s)`), which
**clears the AssetManager frontier**: `Resources.<init>` → `AssetManager.getSystem()` executes
the native. The new frontier is exactly **R3 (data)**: `Failed to load /system/framework/
framework-res.apk`. Nuance for ADR-0008: only the **render** natives (View, Bitmap/Canvas,
Configuration) are genuinely target-only — the resources/input/os floor is host-exported.
Milestones R2 (register) folded into R1; **R3 (framework-res data) is next**.

## R3 RESULT (2026-07-12) — done; resources live end-to-end (`d0341cc`)
Built `framework-res` (→ `.../system/framework/framework-res.apk`). `AssetManager` hardcodes
`FRAMEWORK_APK_PATH=/system/framework/framework-res.apk` (no override, host can't write
`/system`), but `createSystemAssetsInZygoteLocked(reinit, path)` is public and sets `sSystem`
— so ArtVm pre-loads the system assets from `ACL_ART_FRAMEWORK_RES` before `getSystem()` runs.
Also seeded the version codename props (`all_codenames`/`known_codenames`/...) — `Build$VERSION.
<clinit>` indexes `all_codenames[0]` and crashed on empty. **Verified: a real `Resources`
resolves `android:string/ok` → "OK" (id 0x104000a) on host ART.** Test asserts it (locates
framework-res.apk, sets `ACL_ART_FRAMEWORK_RES`; SKIPs if not built). **The resources subsystem is live** — native floor (R1) + data (R3).

## R4 RESULT (2026-07-12) — app resources resolve; subsystem COMPLETE
Built a resource APK with `aapt2` (compile + link -I framework-res), loaded
`[framework-res, app.apk]` via `ApkAssets.loadFromPath` into an `AssetManager.Builder`, wrapped
in a `Resources`, and **resolved the app's own `com.acl.restest:string/app_greeting` +
`integer/app_answer=42` on host ART**. No new acl code — it works through the standard framework
`AssetManager.Builder`/`Resources` API a real `Context` uses, over the R1 native floor. So the
resources subsystem is **complete** (framework + app). Proven by scratch/r4probe; a dedicated
regression test (aapt2-built fixture) is an optional follow-on — the R3 test already covers the
native floor + resource resolution, the regression-prone half.

**Remaining native frontier before apps draw:** the **render** subsystem (View/Bitmap/Canvas →
Skia → Wayland, P2.4) — the only genuinely target-only natives left.

---


The first framework-native subsystem to host-enable (A(2) / ADR-0008 hybrid). A real
`Activity` needs `Resources` first — `Resources.getSystem()` was the frontier after the
build.prop fix (`ResourcesImpl.<clinit>` → `AssetManager.nativeGetThemeFreeFunction` →
`UnsatisfiedLinkError`). This is the template for every later subsystem (View, graphics).

## Why resources is the right first subsystem
- **It's the immediate blocker** on the real-Activity path (`systemMain` → `getSystem`).
- **Its heavy dependency already builds for host:** `libandroidfw` (the C++ `AssetManager2`
  / `ResTable` / `resources.arsc` engine) is `host_supported: true`. AOSP's own host tooling
  (aapt2, layoutlib, ravenwood) uses it on host.
- **Its JNI coupling to the device is narrow.** `android_util_AssetManager.cpp` is in the
  `target: { android: {} }` block, but its only device-tied surface is:
  - `android_util_Binder.h` → used ONLY by `ReturnParcelFileDescriptor` for two methods
    (`nativeOpenAssetFd`, `nativeOpenNonAssetFd`). The ~30 resource-resolution natives
    (`nativeGetResourceValue`, theme, attribute resolution, ...) touch none of it.
  - `AndroidRuntime.h` → only `AndroidRuntime::registerNativeMethods`, which IS
    host-exported already.
  - `android_content_res_ApkAssets` → already host-buildable (its `register_*` is exported).

## Architecture — an acl-owned host JNI lib, NOT a libandroid_runtime patch
The A(2) bridge spike showed patching `libandroid_runtime`'s `Android.bp` per-file is the
wrong seam (it forces the whole device-only block + broke rtti/typeinfo). Instead, acl's ART
build tooling adds its **own** Soong module — `libacl_framework_jni` — that compiles the
*reusable AOSP JNI sources* for host, links the host-buildable engine (`libandroidfw`), and
provides thin **acl stubs for the few device deps**. acl dlopens it and registers those
natives (extending the P2.2 curated set). This is additive (no AOSP module edited), and it is
the **generalizable pattern**: each later subsystem adds its `.cpp` + its device-dep stubs to
this lib. Substitutions reuse acl's existing seams: Binder → svc hub (ADR-0003), later
SurfaceFlinger → Wayland/Skia (ADR-0004).

## Two sides: code and data
Resources needs both the **native code** (the AssetManager JNI) and the **resource data**
(the framework's `resources.arsc`, i.e. the `android:` package that `android.R.*` resolves
against).

## Milestones

### R1 — host-build the AssetManager JNI (code)
Add `libacl_framework_jni` (host `cc_library_shared`) compiling
`android_util_AssetManager.cpp` (+ `android_content_res_ApkAssets.cpp` if not already
reachable) for host, linking `libandroidfw` + `libnativehelper`. Provide acl stubs for the
narrow device surface: a host `ReturnParcelFileDescriptor` (return the fd via a plain host
`ParcelFileDescriptor`/`FileDescriptor`, or leave the two `*OpenAssetFd` methods unregistered
initially — resource *resolution* doesn't use them).
**Gate:** `nm` shows `register_android_content_res_AssetManager` exported from the host lib.

### R2 — register it in acl + Resources constructs
ArtVm dlopens `libacl_framework_jni` and calls `register_android_content_res_AssetManager`
(and ApkAssets) alongside the P2.2 floor. 
**Gate:** on host ART with the real framework, `Resources.getSystem()` constructs (no
`UnsatisfiedLinkError`) — i.e. the exact A(2) frontier is cleared.

### R3 — framework resource data
Build `framework-res` (the `android` package `resources.arsc` + assets) and stage it; point
the AssetManager at it (the boot resources path). acl's `pkg/` already parses `resources.arsc`
for APKs — reuse that understanding for staging/verification.
**Gate:** a real object resolves a framework resource on host ART — e.g. `Resources.getSystem()
.getString(android.R.string.ok)` returns the real string.

### R4 — an app's own resources
An app `AssetManager` opens the APK's `resources.arsc` + assets (ApkAssets), so
`context.getResources().getString(R.string.…)` / a `TypedArray` resolves the app's resources.
**Gate:** a real (non-shim) app reads one of its own resources on host ART.

## Device-dep stubs this subsystem needs
- **ParcelFileDescriptor** for `nativeOpenAssetFd`/`nativeOpenNonAssetFd` (2 methods) — a host
  fd wrapper, or defer (unregister) until an app needs `openFd`.
- Everything else is pure `libandroidfw` + JNI — no Binder/SurfaceFlinger/HAL.

## Risks & sizing
- **Small, well-bounded** relative to later subsystems — `libandroidfw` host-builds and the
  coupling is 2 methods. Days, not weeks, for R1–R3.
- **Watch:** `android_util_AssetManager.cpp` may pull a couple of transitive headers
  (`android_util_Binder.h`, resource-overlay/idmap paths) that assume device; stub or `#if`
  them in the acl lib. The idmap/overlay code may need trimming for host.
- **Data pinning:** `framework-res` must be the android-13.0.0_r84 build (matches the framework
  DEX + runtime). Re-pin only via ADR.
- This validates the hybrid pattern (ADR-0008): if R1–R3 land cleanly, the same recipe scales
  to View/graphics (bigger device surface: SurfaceFlinger→Wayland/Skia, the P2.4 work).
