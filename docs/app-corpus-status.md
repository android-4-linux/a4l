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
| `barilyuk.batterytemperature` | Views (XML layout) | fails inflating `android.widget.CheckBox` (below) |
| `app.fedilab.castlab` | React Native | native libs need `libjnigraphics.so` + `libz.so` (below) |

Also tried, outside the F-Droid corpus (not redistributable, run locally only):

| App | Result |
| --- | --- |
| `com.uptodown` (the Uptodown store app) | launcher + natives fine; dies because acl runs no ContentProviders (below) |
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

## Not yet

- **`barilyuk.batterytemperature`** — `InflateException: Binary XML file line #198 ...
  Error inflating class android.widget.CheckBox`. It reaches its own layout inflation, so the
  framework, resources and classloader are all working; a specific widget's construction is not.
  The next step is naming what CheckBox's constructor actually throws (`ACL_JVMTI_DIAG=1`).
- **`app.fedilab.castlab`** (React Native) — its `.so`s load as far as their dependencies allow:
  `libjsi.so`, `libc++_shared.so`, `libglob.so`, `libpcre2-8.so` load, and `libandroid.so` is now
  acl's own (`src/ndk/bionic/acl_libandroid.c`), but `libfbjni`/`libreactnative`/`libhermesvm` also
  need **`libjnigraphics.so`** and **`libz.so`**, which acl does not stage yet. See
  `docs/ndk-scope.md`.

- **`com.uptodown`** — resolves its launcher, extracts its 4 x86_64 natives, then:
  `Application com.uptodown.UptodownApp construct failed`, and onCreate throws
  `IllegalStateException: Default FirebaseApp is not initialized in this process`. Nothing is wrong
  with the app: Firebase initializes itself from a `<provider>` (`FirebaseInitProvider`) that
  Android runs before `Application.onCreate`, and **acl runs no ContentProviders at all**. The same
  mechanism backs `androidx.startup` and WorkManager, so this gates a whole class of real apps —
  arguably the next framework item after the Activity/Window layer.
- **`com.spotify.music`** (apkmirror, x86_64 among its ABIs) — acl says "no launcher activity in the
  manifest and no --main given", and that is correct: `aapt2 dump badging` reports no
  `launchable-activity`, and the manifest contains **zero** `android.intent.action.MAIN` /
  `android.intent.category.LAUNCHER` entries. It is an app-bundle **base** APK whose launcher lives
  in a split that is not in the file, so no runtime can start it as-is. acl still parses it,
  extracts all 6 x86_64 libs (`libfull-jni-spotify.so` + crashlytics), and brings up audio. Worth
  noting `libfull-jni-spotify.so` needs `libandroid.so` (now acl's own) plus `libOpenSLES.so` and
  `libmediandk.so`, which acl does not stage.

## Notes

- The window is 1024x768 because that is the display acl seeds from the compositor's `wl_output`.
  `tools/shot-app.sh` captures from acl's own Skia surface (`ACL_GPU_DUMP`), not the compositor, so
  the image is the app at acl's present size regardless of the compositor and needs no screencopy
  protocol — a bare tinywl works. (An earlier version used `grim`, which needs `wlr-screencopy` and
  so failed on mutter/GNOME; the readPixels dump has neither limitation.)
- `--present` still defaults to the shm path, which by construction cannot serve a real app (it
  needs acl's Bitmap shim). Every real app therefore needs `ACL_PRESENT_GPU=1` today; making the
  GPU path the default is a separate change.
