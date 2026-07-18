<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original acl status doc; observations from acl runs of redistributable F-Droid
     APKs on this host. No app screenshots are checked in — several corpus apps are GPL-3.0, which
     this tree does not carry (CONTRIBUTING.md license hygiene); regenerate locally with
     tools/shot-app.sh. No GPL-3.0 sources. -->

# App corpus — what runs today

Unmodified F-Droid APKs on `acl run --vm=art`, with the real framework floor and acl's
android.graphics natives on Android's host Skia. What follows was observed on screen, on a Wayland
compositor.

**No screenshots are checked in.** Several of these apps are GPL-3.0 (a2dp.Vol, opendrummer), and a
screenshot carries their rendered UI — which this Apache-2.0 tree does not carry (CONTRIBUTING.md,
license hygiene; note the CI license gate scans source, not images, so this is a judgment call, not
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

## Status

| App | Toolkit | Result |
| --- | --- | --- |
| `SVS.pdfinspector` | Jetpack Compose | **renders** — Activity → RESUMED, full UI |
| `agrigolo.opendrummer` | Views / Material | **renders** — full UI; also registers `SoundPool` natives |
| `a2dp.Vol` | Views / ActionBar | **renders** — ActionBar + buttons |
| `barilyuk.batterytemperature` | Views (XML layout) | **renders** — full UI (adaptive icon, CheckBox, RadioButtons) |
| `app.fedilab.castlab` | React Native | RN native libs load via the bionic bridge; stops in Expo (below) |
| `S.N.A.K.E` | custom View game | presents, but **black** — draws 5-7 ops/frame yet nothing shows (below) |
| `app.fedilab.openmaps` | WebView | can't inflate `android.webkit.WebView` — no browser engine (below) |

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

- **`S.N.A.K.E`** — presents a frame that is **all black** (0 non-black pixels). Pinned with
  `ACL_DRAWPATH_DIAG`: it draws its snake as a single stroked `Canvas.drawPath` (custom View,
  superclass `android.view.View` — **not** a SurfaceView, verified by dexdump), and under the diag
  it makes **zero** drawPath calls. So the game's own render never runs; the 5-7 ops acl logs each
  frame are the framework decor, not the game. This is neither an acl drawing bug nor SurfaceView —
  the app's content is simply not produced (the game does not initialize/start without input in a
  headless run). `app.flicky` presents black the same way. Reaching game content would need input
  delivered to the running app, not a rendering fix.

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
    `SurfaceView` and the `gles_jni` EGL/GLES JNI, a large component.
  - `at.manuelbichler.octalsuntime` — NPE (a null String in the Clock activity's setup).
  - `app.smarzaro.foss4g2024.schedule` — `Resources$NotFoundException` for a layout resource id.
  - `app.crossword.yourealwaysbe.forkyzscanner` — now installs its `<provider>`s (FileProvider +
    `androidx.startup.InitializationProvider`, see the ContentProvider note below) and reaches
    `ForkyzScannerActivity.onCreate`, but stops there: a Compose/androidx ViewModel helper walks the
    `getBaseContext()` chain of `LocalContext.current` looking for an `Application`
    (`instanceof Application`, else follow `getBaseContext`) and is handed the bare `AppContext`
    (base = null), so it throws `IllegalStateException: Could not find an Application in the given
    context`. Distinct from ContentProvider: it's about which Context Compose's `LocalContext` is
    seeded with (it should reach the Application/Activity, not the null-base `AppContext`). A
    `getBaseContext()`→Application override is NOT the fix — it would cycle `AppContext ↔ Application`
    and hang Compose's `findActivity`-style walks.

Recurring, actionable gaps this surfaced: **`libstdc++.so`** (two apps — now staged, see above) and
single missing natives like **`Bitmap.nativeHasMipMap`** (fixed) — the latter is the cheap kind, one
function unblocks an app. With `libstdc++.so` staged, the two libGDX apps fold into the larger
**GLSurfaceView / `gles_jni.EGLImpl`** gap (the SurfaceView + GLES path). The black-frame apps share
the S.N.A.K.E question (custom-View content not reaching the screen).

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
`forkyzscanner` also installs its providers now but is blocked one step later on a Compose
`LocalContext`/ViewModel `Application`-lookup (see the "Wider survey" entry above).

## Notes

- The window is 1024x768 because that is the display acl seeds from the compositor's `wl_output`.
  `tools/shot-app.sh` captures from acl's own Skia surface (`ACL_GPU_DUMP`), not the compositor, so
  the image is the app at acl's present size regardless of the compositor and needs no screencopy
  protocol — a bare tinywl works. (An earlier version used `grim`, which needs `wlr-screencopy` and
  so failed on mutter/GNOME; the readPixels dump has neither limitation.)
- `--present` still defaults to the shm path, which by construction cannot serve a real app (it
  needs acl's Bitmap shim). Every real app therefore needs `ACL_PRESENT_GPU=1` today; making the
  GPU path the default is a separate change.
