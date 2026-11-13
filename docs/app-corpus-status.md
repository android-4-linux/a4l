<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original acl status doc; observations from acl runs of redistributable F-Droid
     APKs on this host. No app screenshots are checked in — several corpus apps are GPL-3.0, which
     this tree does not carry (license hygiene); regenerate locally with
     tools/shot-app.sh. No GPL-3.0 sources. -->

# App corpus — what runs today

Unmodified F-Droid APKs on `a4l run --vm=art`, with the real framework floor and acl's
android.graphics natives on Android's host Skia. What follows was observed on screen, on a Wayland
compositor.

**No screenshots are checked in.** Several of these apps are GPL-3.0 (a2dp.Vol, opendrummer), and a
screenshot carries their rendered UI — which this Apache-2.0 tree does not carry (license hygiene; note the CI license gate scans source, not images, so this is a judgment call, not
a gate). Regenerate them locally instead:

```sh
# any compositor on its own WAYLAND_DISPLAY — sway, tinywl, weston headless, ...:
ACL_ART_HOME=<aosp>/out/host/linux-x86 tools/shot-app.sh <app.apk> /tmp/<app>.png wayland-1
```

`shot-app.sh <apk> <out.png> <wayland-display>` runs the app the canonical way (`tools/run-apk.sh`)
on the GPU present path **against that one compositor**, waits for its first presented frame
(`dmabuf present(A)`), and saves the frame **acl itself rendered** — via `ACL_GPU_DUMP`, which
`readPixels` the just-flushed Skia surface (gpu_dmabuf.cpp). The capture does NOT come from the
compositor, so it needs no screencopy/screenshot protocol and works on a bare tinywl; the image is
the app's surface exactly, no desktop chrome, at the size acl presents. The `<wayland-display>` is
explicit so one invocation targets one compositor; it does not read the ambient session. To simply
watch it on your desktop:

```sh
ACL_ART_HOME=<aosp>/out/host/linux-x86 ACL_PRESENT_GPU=1 \
  tools/run-apk.sh <app.apk> --present-frames=0
```

**`ACL_PRESENT_GPU=1` is not optional for a real app.** It selects the zero-copy path — the app's
views draw through Skia (Ganesh Vulkan) into a dma-buf with a **tiled format modifier**, handed
straight to the compositor for scanout, no CPU copy and no detiling:

```
--present(gpu): handing off to PresentDriver (looper owns dispatch)
dmabuf present: compositor advertises 6 modifier(s) for XRGB8888
dmabuf present(A): 1024x768 tiled modifier=0x200000000401b03 1 plane(s), 2 buffers
```

The default `--present` path instead blits from a `gfx::Surface` it gets via `Bitmap.aclPtr()`,
which exists only on acl's Bitmap **shim** — so it cannot show a real app's frame and says so.
(That default should probably change; see the note at the end.)

## What cleared after that corpus run — 2026-07-19 to 2026-08-04

**Read this before the sections below.** The corpus table and the per-app notes that follow were
written on 2026-07-18 and are kept as the record of that day. Four of the walls they name have since
fallen, so a row saying "out of scope" or "a large component" is history, not status. Each item
below cites the commit that closed it; `git show <sha>` carries the measurement.

- **Flutter renders.** APKs built with the Flutter tooling run on an unmodified engine:
  `libflutter.so` loads against acl's own bionic-side `libandroid.so` (`6a49143`), its EGL/GLES
  forward to host Mesa (`f0b8f6d`, `b238e0e`), and Mesa commits acl's toplevel `wl_surface` itself,
  so no `SurfaceView` is involved. The load-bearing fix was `f177773`: a recent engine builds its GL
  environment synchronously inside `Activity.onCreate`, so acl now creates the GL session in the
  native-EGL `get_display` hook and adopts it in `nGlOpen`. Text needed the AOSP faces bound at
  `/fonts` (`c961326`), network images needed a CA store where Dart looks for it (`ae2b49f`),
  `Texture`-widget external textures import an acl dma-buf as an `EGLImage` through `ImageReader`
  (`7bb42e6`, animated frames on screen with Flutter 3.44), and a `video_player` clip decodes via
  VAAPI, plays and loops (`a6e1931` + `f177773`). Details in `docs/ndk-scope.md`.
- **WebView composites.** The provider is auto-wired (`c8de6a6`) and real Chromium content draws
  through its `AwDrawFn` Vulkan functor onto the present surface — a full OSM map, an EPUB reader —
  once acl drove the functor's `on_sync` on a split RenderThread (`c94d0c0`). Startup needed the
  signal handlers reclaimed from crashpad (`f48adb2`), navigation needed the NDK `ALooper` bridged
  on the Skia present path (`fd81ccf`), z-order needed the present-root walk, now the default
  (`703366a`), and overlay flicker took a snapshot freeze plus a vsync cap (`6010b1a`).
- **A video `SurfaceView` hole-punches.** acl backs it with a real hardware-plane Surface
  (`abad8d0`), presents it on a `wl_subsurface` below the toplevel and clears a transparent hole in
  the UI above it (`3cc990d`, which also had to make the toplevel buffer ARGB8888 — Mutter ignores
  an XRGB toplevel's alpha, so the hole rendered black), scaled to the View's bounds with
  `wp_viewport` (`110762c`). This is the video case, not a general GL or camera `SurfaceView`.
- **`gles_jni` / `GLSurfaceView` is registered.** `com.google.android.gles_jni.EGLImpl` plus
  `android.opengl.GLES20`/`GLES30` natives exist (`4e96f6d`, `4e1661f`, `75fe3a7`), Surface
  injection and GL present make libGDX GL apps run (`eef2785`), they receive input (`ae15c49`), and
  a mixed Java-EGL/native-GL app's Java `gl*` calls route onto the native-GL donor `%fs` so its
  texture setup has a current context (`5f03d36`, `585c746`). `EGL14` is registered too (`62b214b`).
- **Obfuscated Compose UIs resume.** An R8-renamed `ComponentActivity` never reached RESUMED, so
  Compose drew one frame and never recomposed; the `LifecycleRegistry` is now found by the shape of
  `handleLifecycleEvent` and driven by ordinal (`187d1f8`).

What is still open is narrower than those four: an app whose own GL renders to an FBO or gates its
draws still presents nothing (an app-side GL issue, with the context bridging proven correct), a
GLES3-heavy app can link its entire native stack and then trip a null `real`-env deref in acl's JNI
bridge (`6391708`), and the black-frame question below is unchanged.

## Corpus re-run — 2026-07-18

18 F-Droid apps re-fetched (latest F-Droid version) and run headless (`run-apk.sh … --present-frames=0`),
classified by log: did the app reach a `ViewRoot performTraversals` (layout) without failing
`onCreate`. **15 of 18 reach layout; 3 fail.** "Reached layout" means booted + no onCreate crash + the
decor measured/laid out — it is NOT a pixel check (a few reach layout with a secondary non-fatal issue
that can leave content incomplete; those are flagged). Verified-on-screen this session: `forkyz`,
`octalsuntime`.

| App | Result |
| --- | --- |
| `app.crossword.yourealwaysbe.forkyz` | reached layout — **fixed this session** (was ContentProvider crash) |
| `at.manuelbichler.octalsuntime` | **renders** (screenshot) — fixed this session (getComponentName + nDrawArc) |
| `app.fedilab.castlab` (RN) | reached layout — improved (was "stops in Expo"); RN coroutine warnings remain |
| `allen.town.focus.podcast` | reached layout |
| `alt.nainapps.aer` | reached layout |
| `a2dp.Vol` | reached layout |
| `app.areada` | reached layout |
| `app.floatdeck` | reached layout |
| `app.weatheroverview` | reached layout — but TLS cert validation fails (Conscrypt `SSLHandshakeException`), so no network data |
| `app.sterna` | reached layout |
| `app.flicky` | reached layout — but a Koin `InstanceCreationException` fires, so content likely incomplete (was a black frame) |
| `agrigolo.opendrummer` | reached layout |
| `barilyuk.batterytemperature` | reached layout |
| `app.halma` (libGDX GL) | reached layout (decor); GL content via the `gles_jni` path |
| `ardash.doodledudette` (libGDX GL) | reached layout (decor); GL content via the `gles_jni` path |
| `app.smarzaro.foss4g2024.schedule` | resource gap **fixed** (see below) — now inflates its layout; advances to a separate app-side NPE in `MainActivity.onCreate` |
| `app.fedilab.openmaps` | **FAIL** — `InflateException` on `android.webkit.WebView` (no browser engine; out of scope) — **superseded**: WebView composites, see the top of this file |
| `app.crossword.yourealwaysbe.forkyzscanner` | **FAIL** — `NoClassDefFoundError w0.e` (app-side R8/protobuf `dynamicColors_`; fails on stock ART too) |

**`foss4g.schedule` — now fixed.** The layout id `0x7f0c004b` (`main_layout`) exists only as
config-qualified variants (`(port)`/`(land)`/`(xlarge)`/`(sw600dp)`) with **no default**, and acl built
the app `Resources` with `Resources.getSystem().getConfiguration()` — orientation `UNDEFINED`, screen
dims 0 — so nothing matched and `getLayout()` threw. `appResources` now builds a `Configuration` +
`DisplayMetrics` consistent with acl's advertised display (real orientation / screen dp / density /
`smallestScreenWidthDp` / screenLayout size). The `NotFoundException` is gone; foss4g inflates its layout
and advances to a separate app-side NPE in `MainActivity.onCreate`. The remaining two re-run failures are
out-of-scope (WebView) and app-side (protobuf). Caveat: this is a batch/log classification, not per-app screenshots; several
"reached layout" apps were already rendering before this session — the session's contribution is
eliminating the `onCreate` crashes (ContentProviders, Hilt `getApplicationContext`, `getComponentName`,
`nDrawArc`).

## Status

| App | Toolkit | Result |
| --- | --- | --- |
| `SVS.pdfinspector` | Jetpack Compose | **renders** — Activity → RESUMED, full UI |
| `agrigolo.opendrummer` | Views / Material | **renders** — full UI; also registers `SoundPool` natives |
| `a2dp.Vol` | Views / ActionBar | **renders** — ActionBar + buttons |
| `barilyuk.batterytemperature` | Views (XML layout) | **renders** — full UI (adaptive icon, CheckBox, RadioButtons) |
| `app.fedilab.castlab` | React Native | RN native libs load via the bionic bridge; stops in Expo (below) |
| `S.N.A.K.E` | custom View game | presents, but **black** — draws 5-7 ops/frame yet nothing shows (below) |
| `app.fedilab.openmaps` | WebView | **renders** its full OSM map through the `AwDrawFn` functor (superseded this row; see the top of this file) |

Also tried, outside the F-Droid corpus (not redistributable, run locally only):

| App | Result |
| --- | --- |
| `com.uptodown` (the Uptodown store app) | launcher + natives fine; historically died in `Application.onCreate` (Firebase from a `<provider>`) — ContentProviders now run (below), not re-verified on this APK |
| `com.spotify.music` (apkmirror base APK) | not launchable by anything: the APK declares no launcher (below) |

### SVS.pdfinspector — Jetpack Compose (MIT)
Draws its real Compose UI: the document icon, the "Inspect any PDF" heading and body text, the
"Open a PDF" button, and the Feature/Bug/Sponsor row. Compose composes on the frame clock, so this
is the Looper-driven present doing its job, not a static blit.

### agrigolo.opendrummer — Views / Material (GPL-3.0-or-later)
Draws its teal app bar (hamburger, "OpenDrummer", ABOUT) and the "SELECT A STYLE FROM THE MENU"
prompt at the foot. This one also reaches `android.media.SoundPool natives registered` /
`SoundPool created (maxStreams=1)`, so it is the natural first test of the audio path once it
plays.

### a2dp.Vol — Views / ActionBar (GPL-3.0-only)
Draws the classic View stack: an ActionBar with the overflow menu, three buttons (STORED LOCATION /
LOAD DEVICES / START SERVICE) and the "Car Dock" entry — measured, laid out and drawn.

### barilyuk.batterytemperature — Views / XML layout (GPL-3.0)
Draws its whole screen: the orange thermometer AdaptiveIconDrawable, the "Battery Temperature: 0.0
°C" readout, the START LOGGING controls, the "Enable temperature alarm" CheckBox and the Black/White
RadioButtons. Getting here needed the animated-vector morph fix (CheckBox/RadioButton inflation)
plus a retargetable software Canvas + BitmapShader (the adaptive icon composites offscreen).

## Not yet

- **`S.N.A.K.E`** — presents **all black**, but this is the app's *correct* empty state, not a
  broken render. Dexdump shows it is a full-screen multi-touch **drawing canvas**, not a snake game:
  `MainActivity.onCreate` does `setContentView(new a.c(this))`, a custom `View` whose `onTouchEvent`
  starts a stroke on DOWN, appends points on MOVE, and `invalidate()`s; `onDraw` builds a `Path` per
  stroke and each stroke **fades after 2000 ms**. With no input the stroke list is empty, so `onDraw`
  draws nothing — the ~18 ops per frame are the (dark) decor. **Fixed:** the touch *was* reaching
  `a.c.onTouchEvent` all along — classic View-tree dispatch works fine (confirmed by probing
  `decor.dispatchTouchEvent` vs. a direct dispatch to the content child, both handled) — but creating
  a stroke calls `Color.HSVToColor` (a random stroke color), and `Color.nativeHSVToColor` was never
  registered (the host libandroid_runtime does not carry the graphics JNI). It threw
  `UnsatisfiedLinkError` *inside* `onTouchEvent`, which propagated out of `dispatchTouchEvent` and
  acl's input path swallowed it — so it *looked* like dropped input. Registered `Color.nativeHSVToColor`
  / `nativeRGBToHSV` (Skia `SkHSVToColor`/`SkRGBToHSV`); a synthetic drag (`ACL_TEST_DRAG=
  "x1,y1,x2,y2[,frame]"`) now records and draws a stroke. NB S.N.A.K.E is a *static* app (redraws
  only on input), so an occluded/unfocused window on wayland-0 stops receiving `wl_surface.frame`
  callbacks and the loop idles at frame 0 — raise/focus the window to interact.

**SurfaceView (a real, unimplemented gap — no corpus app exercises it yet).** Games, video players,
camera and GL/Vulkan apps draw into a `SurfaceView`, whose pixels live in a separate graphics buffer
rather than the Activity's View-tree draw. acl does not back this: `SurfaceControl`/`SurfaceSession`
natives are stubbed and `android.view.Surface.nativeLockCanvas` is absent, so such an app's draws go
nowhere and its SurfaceView region shows through as a hole. The design is a `SurfaceView` →
`wl_subsurface` presentation path composited with the View tree (planned in `src/wm/meson.build` /
`plan.md` A02, not built). The corpus now **does** exercise it: the two libGDX games (`app.halma`,
`ardash.doodledudette`, once past the `libstdc++.so` wall) drive a `GLSurfaceView` and die
initializing the legacy `com.google.android.gles_jni.EGLImpl` bindings — so the concrete first
target is a GL-backed SurfaceView (an EGL/GLES context rendering into the subsurface), which is more
than the plain-canvas SurfaceView path.
- **`app.fedilab.openmaps`** — fails inflating `android.webkit.WebView`:
  `InflateException ... Error inflating class android.webkit.WebView`. The app is a WebView shell
  (OpenStreetMap in a web view), and acl has no `WebView` implementation — that is an embedded
  browser engine (Chromium/`android_webview`), a large separate component, not a native shim. Any
  WebView-based app hits this. Out of scope for the foreseeable corpus work; noted so it is not
  mistaken for a small gap.
- **`app.fedilab.castlab`** (React Native / Expo) — `tools/run-apk.sh` enables the bionic bridge, so
  the RN engine's native libs load through it: `libreactnative.so`, `libfbjni.so`, `libappmodules.so`
  (plus `libjsi`/`libhermesvm`/`libc++_shared`/`libz`). It cleared the Expo Updates config read once
  `ApplicationInfo.metaData` was populated from the manifest `<meta-data>` (17 entries), and now
  reaches `MainActivity.onCreate`, where it stops on `getPackageName()` on a **null base Context** —
  the RN/Expo `ReactActivity` path reads the Context before acl attaches the base, unlike the
  View-tree apps. That is Activity/Context-attach framework work. (`libimagepipeline`/`libstatic-webp`
  still want `libjnigraphics.so`, which acl does not provide yet, but those are off the RN startup
  path.)

- **`com.uptodown`** — resolves its launcher, extracts its 4 x86_64 natives, then (historically):
  `Application com.uptodown.UptodownApp construct failed`, and onCreate throws
  `IllegalStateException: Default FirebaseApp is not initialized in this process`. Nothing is wrong
  with the app: Firebase initializes itself from a `<provider>` (`FirebaseInitProvider`) that
  Android runs before `Application.onCreate`. **This is now built** — see the ContentProvider note
  below; the same `installContentProviders` path that unblocked `forkyz` runs `FirebaseInitProvider`
  before `Application.onCreate`. (Not re-verified against this specific APK in the session that
  landed it.)
- **`com.spotify.music`** (apkmirror, x86_64 among its ABIs) — acl says "no launcher activity in the
  manifest and no --main given", and that is correct: `aapt2 dump badging` reports no
  `launchable-activity`, and the manifest contains **zero** `android.intent.action.MAIN` /
  `android.intent.category.LAUNCHER` entries. It is an app-bundle **base** APK whose launcher lives
  in a split that is not in the file, so no runtime can start it as-is. acl still parses it,
  extracts all 6 x86_64 libs (`libfull-jni-spotify.so` + crashlytics), and brings up audio. Worth
  noting `libfull-jni-spotify.so` needs `libandroid.so` (now acl's own) plus `libOpenSLES.so` and
  `libmediandk.so`, which acl does not stage.

## Wider survey (newer additions)

A larger batch of F-Droid APKs was iterated (`tools/shot-app.sh` / a batch of `run-apk.sh` runs,
classified by whether the presented frame has non-black content). Roughly half reach a real UI:

- **Render a UI**: `app.areada`, `alt.nainapps.aer`, `app.floatdeck`, `app.weatheroverview`
  (amber app bar + search box), `app.sterna`, `app.crossword.yourealwaysbe.forkyz`,
  `allen.town.focus.podcast` (after the `Bitmap.nativeHasMipMap` native was added),
  `app.fedilab.castlab` (React Native — now presents its splash instead of crashing, but the JS UI
  has not drawn yet).
- **Present a black frame** (custom-View content not showing, as with S.N.A.K.E): `app.flicky`.
- **No frame, with a specific cause**:
  - `app.halma`, `ardash.doodledudette` — both **libGDX** games. The old `libstdc++.so` DT_NEEDED
    wall is **gone**: `tools/stage-bionic-runtime.sh` now stages a real bionic `libstdc++.so`
    (best-effort from an NDK sysroot — the gnustl-era C++ ABI stub that AOSP no longer builds; set
    `ACL_NDK`/`ACL_NDK_LIBSTDCXX` if it is not auto-found), and `libgdx.so` loads through the bionic
    bridge. They now hit the **next** wall, which is the real one: libGDX renders through
    `GLSurfaceView` and initializes the legacy `com.google.android.gles_jni.EGLImpl` (EGL10) Java
    bindings, whose `_nativeClassInit()` native acl does not provide —
    `UnsatisfiedLinkError` in `AndroidGraphics.createGLSurfaceView` → `onCreate` fails. That is the
    **SurfaceView + GLES** path (below), not a single missing native: it needs a GL-backed
    `SurfaceView` and the `gles_jni` EGL/GLES JNI, a large component. **Superseded (2026-07-17):**
    that component was built — `EGLImpl` + `GLES20`/`GLES30` natives, Surface injection, GL present
    and input — and libGDX GL apps run. See the top of this file.
  - `at.manuelbichler.octalsuntime` — **now renders its full octal clock UI** (dial digits 0–7, tick
    marks, the yellow daylight arc, hand, octal time, "Greenwich"). Two fixes: (1) `ClockActivity.
    onCreate` called `getComponentName().getClassName()` and acl never set `Activity.mComponent` →
    now wired in attachActivity (also fixes androidx `NavUtils.getParentActivityName`); (2) the arc
    needed `BaseCanvas.nDrawArc`, added to graphics_jni (`SkCanvas::drawArc`). Verified by screenshot.
  - `app.smarzaro.foss4g2024.schedule` — `Resources$NotFoundException` for a layout resource id.
  - `app.crossword.yourealwaysbe.forkyzscanner` — installs its `<provider>`s (FileProvider +
    `androidx.startup.InitializationProvider`, see the ContentProvider note below) and clears the
    Hilt `EntryPointAccessors` gap in `onCreate` (fixed: `AppContext.getApplicationContext()` now
    falls back to the process Application, so `dagger.hilt.android.internal.Contexts.getApplication`
    finds it — the Activity's own `AppContext` was unwired and returned itself). It now advances to
    an **app-side** blocker, not an acl gap: a protobuf-lite/R8 field-reflection mismatch
    (`RuntimeException: Field dynamicColors_ for w0.b not found. Known fields are [i, j, k, …]`).
    **Confirmed from the shipped DEX** (not inferred): Hilt DI in `Application.onCreate`
    (`r0.j$f`/`g1.b` component providers) triggers a protobuf message's schema init
    (`w0.i.<clinit>`), which calls the bundled protobuf-lite `MessageSchema.field()` helper
    (`W0.K.d0`, the "Samsung workaround" that rethrows with "Known fields are …") →
    `getDeclaredField("dynamicColors_")` on `w0.b`. A `dexdump` of `classes.dex` shows `w0.b`'s
    fields are `i`(boolean)/`j`(String)/`k`(String)/`l`/`m` — no `dynamicColors_` — while
    `dynamicColors_` survives as exactly **one** baked-in string literal (the `buildMessageInfo`
    schema string R8 left un-rewritten). So R8 renamed the field to `i` but not the schema string:
    the classic R8-full-mode + protobuf-lite-without-keep-rules bug. Both the protobuf runtime and
    the message class are in the app's DEX, which acl runs unmodified, so this throws identically on
    stock ART. Nothing for acl to do; the app must rebuild with correct protobuf R8 rules.

Recurring, actionable gaps this surfaced: **`libstdc++.so`** (two apps — now staged, see above) and
single missing natives like **`Bitmap.nativeHasMipMap`** (fixed) — the latter is the cheap kind, one
function unblocks an app. With `libstdc++.so` staged, the two libGDX apps fold into the larger
**GLSurfaceView / `gles_jni.EGLImpl`** gap (the SurfaceView + GLES path) — **since closed**, see the
top of this file. The black-frame apps share the S.N.A.K.E question (custom-View content not
reaching the screen), which is still open.

### ContentProviders (built)

`ActivityThread` runs every declared `<provider>`'s `onCreate` **before** `Application.onCreate`;
acl previously ran none (the `ContentResolver` was a no-op stub), so any app that bootstraps from a
provider — `androidx.startup.InitializationProvider` (WorkManager / Compose resources / Koin),
Firebase's `FirebaseInitProvider` — crashed in `Application.onCreate`. `HostBootstrap.
installContentProviders` now parses `<provider>` straight from the manifest (no PMS/Binder, ADR-3),
instantiates each via the app classloader, and `attachInfo()`s it (which runs `onCreate`), stashing
the per-provider `<meta-data>` so `getProviderInfo(cn, GET_META_DATA)` can re-serve it — the exact
call `androidx.startup` makes to rediscover its `Initializer`s. `exported` / `grantUriPermissions`
are parsed (FileProvider requires `exported=false` + `grantUriPermissions=true`), and installed
providers are registered by authority so `resolver.query/openFile` on a `content://` URI reach the
live provider's in-process `Transport` instead of the stub. Two adjacent gaps fixed alongside:
`AppContext.getUser()` (WorkManager's `PendingIntent.getBroadcast` path NPE'd on the null base).

**Verified:** `app.crossword.yourealwaysbe.forkyz` went from crashing in `Application.onCreate` to
installing all four providers and booting through to a real `ViewRoot` layout (measured 1280x1440).
`forkyzscanner` also installs its providers now; a follow-on fix (`AppContext.
getApplicationContext()` → process Application) cleared its Hilt `EntryPointAccessors` gap in
`onCreate`, and it now stops on an app-side protobuf/R8 issue, not an acl gap (see the "Wider
survey" entry above).

## Notes

- The window is 1024x768 because that is the display acl seeds from the compositor's `wl_output`.
  `tools/shot-app.sh` captures from acl's own Skia surface (`ACL_GPU_DUMP`), not the compositor, so
  the image is the app at acl's present size regardless of the compositor and needs no screencopy
  protocol — a bare tinywl works. (An earlier version used `grim`, which needs `wlr-screencopy` and
  so failed on mutter/GNOME; the readPixels dump has neither limitation.)
- `--present` still defaults to the shm path, which by construction cannot serve a real app (it
  needs acl's Bitmap shim). Every real app therefore needs `ACL_PRESENT_GPU=1` today; making the
  GPU path the default is a separate change.
