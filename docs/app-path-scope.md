<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: a4l app-path exploration (host ART, real android.view.View). Android SDK
     behavioral (public + observed @hide). No GPL-3.0 sources. -->

# App path — running a real `android.view.View` on host ART

The graphics subsystem (P2.4) is done: the a4l-owned `android.graphics` natives
(`libacl_graphics_jni`) render color / geometry / **text** / **bitmap** / **path** /
**matrix** / **gradients** on Android's host Skia, verified pixel-by-pixel
(`graphics_ops_check`, `ACL_ART_GRAPHICS_OPS=1`). Those natives work with any Canvas.

The **app path** is the next layer: run a real third-party app's `View.onDraw` through
those natives. Third-party apps subclass `android.view.View`, so we must construct and draw
the *real* framework `View` on host ART. This doc maps that frontier, established empirically
with the `tests/fixtures/art_view/` probe (a real `DrawView extends View`, driven by
`/tmp/.../run_view.sh` on host ART with the real framework jars + `libacl_graphics_jni`).

## The blocker sequence (each cleared in order)

Constructing `new DrawView(context)` on host ART walks into the framework's service + hwui
coupling. Observed, with the fix for each:

1. **`View.<init>` → `ViewConfiguration.get(context)` → `context.getResources()`** — NPE on a
   null Context. *Fix:* a minimal Context — `ContextWrapper(null)` overriding `getResources()`
   → `Resources.getSystem()`, plus `getDisplayId()`=0 (`@hide`, so no `@Override`).
2. **`ViewConfiguration` → `WindowManagerGlobal.getWindowManagerService()` →
   `BinderInternal.getContextObject()`** — `UnsatisfiedLinkError` (a4l has no Binder, ADR-3).
   *Fix (in a4l):* `register_no_binder()` stubs `getContextObject()` → null.
3. **`ServiceManager.getIServiceManager()` returns null → `getService()` NPEs** — a null
   context object leaves `ServiceManager` with no `IServiceManager`. *Fix:* seed a no-op
   `IServiceManager` (fixture, via reflection today).
4. **`ViewConfiguration` requires a non-null `IWindowManager.hasNavigationBar()`** — it does
   *not* tolerate a null WindowManager. *Fix:* `getService("window")` returns a local-interface
   stub — an **`IBinder` proxy** (an interface, so no `Binder` natives) whose
   `queryLocalInterface(DESCRIPTOR)` returns a no-op `IWindowManager` proxy. `asInterface()`
   then uses the local stub (no Binder transaction). NB: `new Binder()` itself needs
   `Binder.getNativeBBinderHolder()` (a native we don't have) — hence the interface-proxy route.
5. **`View.<init>` → `RenderNode.create()` → `RenderNode.nCreate()`** — `UnsatisfiedLinkError`.
   **This is where the probe currently stops.** RenderNode is hwui's display-list node; the
   `View` constructor always creates one (`mRenderNode`). hwui is device-only / dropped
   (ADR-4). *Fix (next milestone):* an a4l-provided **RenderNode shim**.

## The next milestone: a RenderNode shim

`android.graphics.RenderNode` has **93 native methods** (80 `@CriticalNative` handle-only,
`nCreate`/`nGetNativeFinalizer` + ~11 normal). For a4l's software/Skia draw path the RenderNode
is **not used to draw** (a Bitmap-backed Canvas is not hardware-accelerated; `View.draw(canvas)`
calls `onDraw` directly), so the shim is mostly mechanical:

- `nCreate(name)` → a small `AclRenderNode` struct (tracks geometry: left/top/right/bottom,
  alpha, translation/scale/rotation, pivots) returned as a handle; `nGetNativeFinalizer` frees it.
- geometry setters store, getters return; everything else is a no-op (void), `true` (boolean
  "changed"), or 0.
- **Care:** `RegisterNatives` is all-or-nothing per class, so all 93 signatures must be exact,
  and the 80 `@CriticalNative` must be written handle-only (no `JNIEnv*/jclass`) — like the
  Bitmap/Paint `@CriticalNative` natives already in `graphics_jni.cpp`.

Belongs in `libacl_graphics_jni` (it's `android.graphics`). Once it lands, the probe should
reach `View.draw(canvas)` → `onDraw` → the real `android.graphics` natives → pixels.

## Beyond RenderNode (expected further walls)

`View.draw(canvas)` on a software canvas mostly calls `onDraw`, but background/scrollbar paths
may touch more. And a *real* app (not the probe) needs, in rough order: an `Activity`/`Window`
environment (`ActivityThread`/`ContextImpl`/`PhoneWindow`/`DecorView`), resource inflation
(`LayoutInflater` + `resources.arsc`), and the `getSystemService` Managers (served by `svc/`,
ADR-2/3). Those are the broader framework-bootstrap effort (ADR-1/ADR-2). The RenderNode shim is
the smallest next step that unblocks a real `View.onDraw`.

## Architectural note

a4l's *own* view traversal (the `present_content_loop` path) can keep using a minimal shim View
that avoids this coupling. The service/hwui walls here are specifically about hosting
*unmodified third-party* Views — the real target, and genuinely a framework-bootstrap effort,
not a graphics one.

## Update: View + ViewGroup work GPU-native; the runtime provides the environment

Cleared to a **working GPU app path**: a real `android.view.View` (and a `ViewGroup` hierarchy)
constructs, measures, lays out, and draws on host ART, presented GPU-native (Ganesh Vulkan →
tiled dma-buf → Wayland). What made it work:
- **RenderNode shim** (93 natives) + **no-Binder stubs** (`getContextObject`, `getCallingUid`,
  `getNativeBBinderHolder`/`getNativeFinalizer`) — see commits.
- **`org.acl.internal.HostBootstrap`** — an a4l runtime bootstrap DEX injected onto the
  classpath (`ACL_ART_HOST_BOOTSTRAP`): `init()` (main Looper + no-Binder ServiceManager with
  local interface-proxy stubs) and a minimal `Context` (`getResources`=system, `getTheme`=empty
  system theme, `getMainLooper`, `getApplicationInfo`, `getSystemService(layout_inflater)`).
- **`Canvas.nQuickReject`** for `ViewGroup.drawChild` culling.
- The app provides **only a View subclass**; a4l constructs it with the runtime Context and
  drives `present_content_loop_gpu` (`ArtVm::graphics_view_demo`, `ACL_ART_VIEW_HOST`).

## Next frontier: the Activity / Window layer (mapped, not yet cleared)

Real apps enter via an `Activity` whose `onCreate` calls `setContentView` — not a
directly-constructible View. A real `Activity` is `attach()`'d by `ActivityThread` before
`onCreate`; we can't stand up `ActivityThread`, so `HostBootstrap.attachActivity` seeds the
minimum by reflection (`mActivityInfo`, a `PhoneWindow`). The probe (`ACL_ART_VIEW_HOST` = an
Activity → `ArtVm::graphics_view_demo` attach+onCreate, harness `run_hb.sh act`) walked these
walls in order:
1. `Activity.onCreate` reads `mActivityInfo.parentActivityName` → set a default `ActivityInfo`.
2. `PhoneWindow`'s ctor → `LayoutInflater.from(context)` → `getSystemService(layout_inflater)` →
   return a `com.android.internal.policy.PhoneLayoutInflater`.
3. `PhoneWindow` → `context.getContentResolver()` → **null (current wall)**; needs a real-ish
   `ContentResolver` (reads system settings: font scale, etc.).

Behind #3 lie more interlocking pieces (a `ContextImpl`/`ContentResolver`, `setContentView` →
`DecorView` install, theme/resource lookups, `getSystemService` Managers → `svc/`). This is the
Activity/Window bootstrap (ADR-1/ADR-2) — a dedicated multi-step effort, best approached by
standing up a minimal `ContextImpl` (real `ContentResolver` + Managers) rather than field-poking
a bare `Activity`. Until then, the GPU app path is exercised by constructing the app's content
View directly (which works). Fixtures: `MainActivity` (probe), `Panel`/`Tile` (ViewGroup),
`DrawView` (single View).
