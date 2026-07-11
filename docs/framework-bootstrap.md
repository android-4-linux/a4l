<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: plan.md §3 ADR-2, §4 Phase 2, §5, §A01; ADR-0002; AOSP frameworks/base
     package layout (Apache-2.0, public). No GPL-3.0 sources. -->

# Framework bootstrap — scope (Gate G2 dependency)

## The gap

The end-to-end run path executes a self-contained APK whose entry class has a static
`main()` (see `tests/integration/run-apk`). Real Android apps have no `main`: their entry is
`Activity.onCreate(Bundle)`, instantiated and driven by the framework (`ActivityThread`). The
app's DEX is full of references into `android.*` — `extends android.app.Activity`,
`invoke-virtual android.app.Activity.setContentView`, `android.os.Bundle`, `android.util.Log`.
The converter already emits those references by name (superclass + methodref); the JVM resolves
them **lazily from the classpath at load time**. So "run a real app" reduces to: **put a
compatible `android.*` on the classpath and drive the Activity lifecycle instead of `main`.**

That splits into two independent problems.

## Two problems

**(A) The loading mechanism** — acl-owned, no AOSP needed. Boot-classpath plumbing, the
JNI native underside (`android.*` `native` methods → C++), and a lifecycle driver that replaces
the static-`main` entry with instantiate-Activity + `onCreate`. This is ordinary engineering and
is where we start; it de-risks (B) by giving it a target to load into.

**(B) The content** — the vendored AOSP `frameworks/base` subset (ADR-0002) plus the
shim-at-Manager-boundary classes. This is the hard, open-ended part (risk R-2: subsetting pulls
in Binder/SystemServer). It is gated behind (A) and grown package-by-package against a budget.

## (A) Mechanism design

### A1. Boot classpath
`VmConfig.boot_classpath` already exists but the JVM backend ignores it. Wire it: the JVM
backend prepends boot-classpath entries to `-Djava.class.path` (framework resolves ahead of, and
independently of, app classes). The ART target (R-1) will instead build a real boot image from
these entries; the seam is the same. `--framework=<dir>` (and `$ACL_FRAMEWORK`) populate it;
default is the vendored staging dir once it exists.

### A2. Framework staging (source → .class, NOT dex-translate)
The framework is vendored as **Java source** (ADR-0002), so it is compiled with `javac` to a
`framework/classes/` boot dir — no DEX round-trip. (Only the *app* takes the DEX→JVM path; the
framework is already Java.) The staging layout:
```
third_party/aosp/frameworks/  # unmodified vendored AOSP .java (Apache-2.0, per-file provenance)
framework/shim/               # acl source-compatible shims (from SDK docs; never the predecessor)
build/framework/classes/      # javac output -> boot_classpath
```

### A3. Native underside (JNI seam)
`android.*` classes carry `native` methods (`Canvas.nDrawRect`, `Log.println_native`, …). On VM
start (or lazily on first class init) acl `RegisterNatives` C++ implementations that forward to
the existing native core (`svc/`, `gfx/`, `wm/`, `log/`). New module `runtime/framework/` owns
these registrations, one translation unit per `android.*` package. The registry is data-driven
(class → {name, sig, fnptr}) so coverage is measurable and gaps fail loudly ("no native for X").

### A4. Lifecycle driver (replaces static `main`)
A minimal, acl-owned `ActivityThread`-equivalent (a source-compatible shim, from SDK docs):
1. load the launcher class from the app classpath;
2. if it is an `android.app.Activity` subclass, instantiate it, `attach()` a shimmed `Context`,
   call `onCreate(null)` → `onStart()` → `onResume()`; else fall back to `call_static_main`.
`shell/main.cpp` picks the path from the resolved launcher. No `Looper`/message-pump fidelity in
the first cut — a single synchronous lifecycle pass proves the vertical; the real `Looper`→epoll
mapping (plan.md §2 threading) lands with windowing (G2 proper).

## (B) Content strategy — vendor + subset + shim

- **Pin:** AOSP `platform/manifest` at an `android-13.0.0_rXX` (api-33) tag, recorded in
  `third_party/aosp/METADATA`. Skia milestone paired per the §A02 pinning rule.
- **Seed + closure:** start from the packages ADR-0002 names (`os`, `util`, `content`, `app`,
  `view`, `graphics`, `text`, `widget`). An extraction tool (`tools/`) computes the transitive
  `.java` dependency closure of a seed set, reports per-package class count against a **hard
  budget** (R-2), and flags every edge into the denylist.
- **Denylist (no-Binder v1, ADR-3):** `com.android.server.*`, `android.os.Binder`/`IBinder`/
  `ServiceManager`, `*.aidl`-generated `IInterface` stubs, `Parcel` over binder. A class that
  reaches the denylist is either (a) cut from the closure (unused transitively) or (b) replaced by
  a **shim** at the Manager boundary.
- **Shim-at-Manager-boundary:** Managers (`ActivityManager`, `WindowManager`, `PackageManager`,
  …) are the seam. Vendor the real *client-side* classes; where a class is hopelessly entangled
  with SystemServer, write a minimal source-compatible shim in `framework/shim/` that calls the
  in-process `svc/` hub directly (no Binder). Shims are provenance-tagged "SDK docs", never the predecessor.
- **libcore friction (drives the ART decision):** real `frameworks/base` depends on Android's
  `libcore` (`java.*` variant) and `dalvik.system.*` (`VMRuntime`, `VMStack`, …), absent on a
  stock JVM. The JVM fallback needs `dalvik.system.*` shims and tolerates OpenJDK `java.*`; where
  that fidelity gap bites, it is direct evidence for the ADR-1 host-ART decision. Track shim count
  as the metric.

## Guardrails (carry from Gate G0)

- **Clean-room:** vendored classes come from AOSP only (per-file SPDX + provenance); shims from
  SDK docs; **never the predecessor**. CI license scan already gates GPLv3.
- **Budget:** per-package vendored class count is logged and capped (R-2). Silent closure growth
  is a review stop.
- **Provenance:** vendored files keep their AOSP header; shims carry `// Provenance: SDK docs …`.

## Phasing

| Step | Deliverable | Exit criterion |
|------|-------------|----------------|
| **G2.a** ✅ | Boot-classpath seam (A1) + `--framework` + native-seam scaffold (A3) | an app resolves a class it does NOT bundle, from `--framework` (`run-apk-framework`) |
| **G2.b** ✅ | `android.util.Log` on the boot cp (docs-derived shim, `framework/shim/`) + its native `write0` bound by acl (`runtime/framework_natives`) forwarding to `acl::log` | app calls `android.util.Log.i(...)` → line via `acl::log` (`run-apk-log`) |
| **G2.c** ✅ | Extraction tool (`tools/fw_extract.py`) + AOSP pin (`android-13.0.0_r84`) + closure/budget/denylist report | tool reports class count + denylist edges (`fw-extract` test); METADATA pinned |
| **G2.d** ✅ | Lifecycle driver (`JvmVm::call_activity_lifecycle`) + `Activity`/`Bundle` shims | `activity.apk`'s `MainActivity` subclass is instantiated and `onCreate`→`onStart`→`onResume` driven — no static `main` (`run-apk-activity`) |
| **G2.e** 🚧 | Automotive media path (no Views): `MediaBrowserService` + `MediaSession` + **audio** | media control plane (`run-apk-media`): browse tree + now-playing metadata + transport `onPlay`/`onPause`. **Audio (`run-apk-audio`)**: `audio.apk` writes PCM via `android.media.AudioTrack` → the acl native underside → `acl::audio` — the **first native integration** (`src/audio/`, real PipeWire playback via `ACL_AUDIO=pipewire`; counting sink for the deterministic test). Next: `PlaybackState`; Context/Service hierarchy for real apps |

Windowing/`Canvas`/`Looper`-fidelity (real G2 "renders and is interactive", plan.md §4 Phase 2)
builds on G2.d; the automotive media/template path (G1.5) is preferred first because it needs no
View stack.

## Measured — the Activity closure (G2.c/G2.d, android-13.0.0_r84)

`fw_extract.py` against a real sparse checkout, seeding `android.app.Activity,
android.os.Bundle, android.content.Context`:

- **535** in-tree classes in the transitive import closure (across ~40 packages);
- **148** denylist edges — `Activity` → `IBinder`/`ServiceManager`; `ActivityManager` →
  `Binder`/`IBinder`/`ServiceManager` **and** `com.android.server.{LocalServices,am.*,wm.*}`.

This is the R-2 evidence: you cannot vendor `Activity`'s closure — it drags in SystemServer. So
`Activity`/`Bundle`/`Context` are **shimmed** at the Manager boundary (ADR-0002), source-compatible
from SDK docs (`framework/shim/android/app/Activity.java`, `.../os/Bundle.java`), and the acl
lifecycle driver instantiates the app's `Activity` subclass and calls `onCreate`/`onStart`/
`onResume` directly — no Binder. `setContentView`/`Context` services stay stubbed until the View
stack (Phase 2).

## The svc/ Manager hub (getSystemService — the ADR-3 seam)

`Context.getSystemService(name)` returns a Java **Manager shim** whose native methods call the
in-process `svc/` hub **directly — no Binder, no SystemServer** (ADR-3). The first Manager is
`android.media.AudioManager` → `acl::svc::AudioService` (`src/svc/svc.{h,cpp}`, per-stream volume
state). `MainActivity.onCreate` does `((AudioManager) getSystemService("audio")).setStreamVolume(
STREAM_MUSIC, 7, 0)` and the C++ service logs `set stream 3 volume -> 7` (`run-apk-activity`
asserts it). This is the shim-at-Manager-boundary (ADR-2) working end-to-end: app →
`getSystemService` → Manager shim → JNI native → acl C++ service. `svc::audio` is a general
service (present in the phone build; `svc/car` stays automotive-gated). More Managers
(ActivityManager, WindowManager, MediaSessionManager, ...) join as the app corpus needs them.

## The Context hierarchy (unblocks real entry points)

Real `Service`/`Activity` are `Context`s (`Activity` → `ContextThemeWrapper` → `ContextWrapper` →
`Context`), and real constructors take a `Context` (`MediaSession(Context,…)`, `AudioTrack.Builder`,
…). The shims now model that chain: `framework/shim/android/content/{Context,ContextWrapper}.java`
+ `android/view/ContextThemeWrapper.java`, with `Service extends ContextWrapper` and `Activity
extends ContextThemeWrapper`. So a hosted `Service`/`Activity` *is* a `Context` (the host logs its
`getPackageName()`), and `MediaSession(Context, String)` verifies with the app's `this`. `Context`
is a concrete minimal shim (real is abstract w/ ~100 SystemServer-bound methods — the 535-class
closure); `getSystemService()` returns null until the `svc/` Manager boundary is wired. Real
package injection (attach a `ContextImpl` carrying the manifest package) is the follow-up; today it
defaults, settable via `aclSetPackage`.

## Risks

- **R-2 closure explosion** — mitigate with the budget + denylist + shim boundary; measure early
  (G2.c) before committing to G2.d subset size.
- **libcore/`dalvik.system` gap on the JVM fallback** — expected; quantify via shim count; it is
  the ADR-1 (host ART) trigger, not a surprise.
- **Lifecycle fidelity** — the first driver is synchronous and Looper-less; real message-pump
  semantics are scheduled with windowing, not forced now.

## First increment (this change)

G2.a: wire `boot_classpath` through the JVM backend, add `--framework=<dir>`/`$ACL_FRAMEWORK`,
and prove it with a test where the app's `main` calls a class supplied only via `--framework`
(not bundled in the APK). This makes the seam real and testable before any AOSP is vendored.
