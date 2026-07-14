<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: Android NDK stable API docs (libandroid/ANativeWindow/ALooper), AOSP bionic
     (BSD) + linker docs, libhybris/Anbox (Apache-2.0, per-file license check). No GPL-3.0 sources. -->

# NDK / native libraries — scope and status

`plan.md` ADR-5 stages NDK support to Phase 4. This tracks the increments.

## The chain

An APK ships native code as `lib/<abi>/*.so` (ELF, linked against **bionic** + the NDK stable
APIs). `System.loadLibrary("foo")` → the classloader's native search path → `lib<foo>.so` →
`dlopen` → its `JNI_OnLoad` runs and `RegisterNatives` binds the app's `native` methods. Running
these on a glibc/Wayland host breaks into: (1) find/extract the right ABI, (2) CPU architecture,
(3) the bionic ABI bridge, (4) the NDK trampolines. See the fuller write-up in the project notes.

## DONE — front of the chain: ABI selection + search path

- **`pkg`**: `Apk::host_abi_candidates()` gives this build's ABIs in Android's preference order
  (x86_64 → `{x86_64, x86}`, arm64 → `{arm64-v8a, armeabi-v7a, armeabi}`); `Apk::select_abi(apkAbis)`
  returns the first host-compatible ABI present, else "".
- **`acl run`**: when the APK has native libs, extract `lib/<abi>/*.so` for the selected ABI to a
  per-app dir (`$XDG_CACHE_HOME/acl/<pkg>/lib`) and add `-Djava.library.path=<dir>` so host ART's
  `System.loadLibrary` resolves them. An ABI mismatch (e.g. an arm-only app on an x86_64 host) is
  reported, not extracted.

**Verified end-to-end** with a host-built x86_64 JNI fixture (`libacldemo.so` with a `JNI_OnLoad`
registering `answer()`; a `NativeMain` that `System.loadLibrary("acldemo")` then calls it): acl
selects `x86_64`, extracts the lib, and the app logs `loaded libacldemo; answer()=42` — so
`loadLibrary → dlopen → JNI_OnLoad → RegisterNatives → native call` all work on host ART.

This turns the old opaque `UnsatisfiedLinkError: lib*.so: cannot open shared object file` into
either a successful load (host-compatible `.so`) or a precise ABI-mismatch report.

## NEXT — why real apps still don't load their native code

The corpus apps that hit this ship **arm-only** (Spotify `libstub.so`: arm64/armeabi) or **arm +
x86-32** (Rive `librive.so`) — no x86_64 — so on an x86_64 host they fail on **architecture** even
with the search path wired. Two hard pieces remain (Phase 4 / R-6):

1. **Bionic ABI bridge** — app `.so` are linked against **bionic**, not glibc (different libc /
   `pthread`/TLS layout / ifuncs). Build AOSP `bionic` + `linker64` for the host and load app `.so`
   in a **bionic linker namespace**, bridging only at the NDK-API boundary. The tractable target is
   an **arm64 host** (the automotive boards) where no CPU translation is needed; on x86_64 desktop
   this additionally needs arm→x86_64 translation (QEMU user / houdini-class) or x86_64-native apps.
2. **NDK trampolines** (`src/ndk/`) — resolve the `.so`'s imports to acl: `libandroid`
   (`ANativeWindow` → our dma-buf/Wayland surface, `ALooper` → the reactor, `AAssetManager` → `pkg`,
   `AChoreographer`), `liblog`, `libEGL`/`libGLESv3`/`libvulkan` → host Mesa, `libaaudio`/
   `libOpenSLES` → PipeWire, `libjnigraphics` → Skia. `NativeActivity` apps need this with no View
   tree; JNI-augmented Java apps need it plus what already works.

The front of the chain (this change) is what every later step builds on.

## What a real engine expects — Flutter, read from source (so we don't implement blind)

Read from `flutter/engine/src/flutter/shell/platform/android/`. `libflutter.so` (x86_64 build,
still bionic-linked) is a concrete, representative NDK client. What it needs:

- **Direct-linked libs** (must resolve at `dlopen`): bionic `libc.so`/`libm.so`/`libdl.so` (the
  bridge), `libandroid.so`, `libEGL.so`, `libGLESv2.so`, `liblog.so`, `libjnigraphics.so`.
- **`libandroid.so` functions actually called** — direct: `ANativeWindow_fromSurface` /
  `_acquire`/`_release`/`_getWidth`/`_getHeight`; `ALooper_prepare`/`_acquire`/`_addFd`/`_pollOnce`/
  `_wake` (every engine thread runs on an ALooper over real fds); `AAssetManager_fromJava` +
  `AAsset_open/close/getLength/getBuffer`; `__android_log_print`. Resolved lazily via `dlsym` and
  *tolerant of absence*: `AChoreographer_*`, `AHardwareBuffer_*`, `ASurfaceControl/Transaction_*`,
  `ATrace_isEnabled`.
- **Rendering config — pick the easy one:** the engine defaults to Impeller (Vulkan-first on API
  ≥29, needs the AHardwareBuffer/ASurfaceControl set). Forcing **`--enable-impeller=false`** (or
  targeting API < 29) selects **Skia-GLES**, which needs only an **EGL window surface** on the
  `ANativeWindow`: `EGL_OPENGL_ES2_BIT`, `EGL_WINDOW_BIT`, RGBA8888, depth=0/stencil=0, GLES2
  context, `eglSwapBuffers`. That is the minimal render path.
- **Surface flow:** Java `Surface` → `ANativeWindow_fromSurface` → `eglCreateWindowSurface` →
  GLES2 render → `eglSwapBuffers`. So the one load-bearing shim is **`ANativeWindow` backed by an
  acl Wayland/dma-buf surface** (`wm/`+`gfx/` already produce exactly this), with EGL/GLES routed
  to host **Mesa**. **Vsync can be driven entirely from Java** (`FlutterJNI.nativeOnVsync`), so
  `AChoreographer` is optional.
- **Bring-up an embedder drives:** `FlutterLoader.startInitialization` → `loadLibrary` →
  `nativeInit` (ICU is a blob inside `libflutter.so`, located via `--icu-native-lib-path`; AOT via
  `--aot-shared-library-name=libapp.so`; JIT via `kernel_blob.bin`) → `attachToNative` →
  `runBundleAndSnapshotFromLibrary` → `nativeSurfaceCreated(Surface)` → `nativeSetViewportMetrics`
  → frames. The JNI surface is ~40 `FlutterJNI` natives (inventory in the research notes).

**Upshot for acl:** the NDK trampolines map almost entirely onto machinery that already exists —
`ANativeWindow` → `wm/`+`gfx/` dma-buf/Wayland surface, `ALooper` → the reactor, EGL/GLES → host
Mesa, `liblog` → `log/`. The genuinely hard, missing piece is the **bionic libc/libm/libdl bridge**
so the `.so` links at all; the trampolines are then a well-scoped surface, not a guess.

### Vsync — reuse acl's wp_presentation source (per ivi-homescreen)

Reference: ivi-homescreen `shell/vsync/wayland_vsync_provider.cc` + `ivsync_provider.cc` (the
canonical Flutter-on-Wayland embedder, Apache-2.0). Its model, and the mapping to acl:

- **The contract**: Flutter hands out an opaque *baton* (embedder API) / *cookie* (Android
  `FlutterJNI.asyncWaitForVsync(long)`); the embedder returns **exactly one** vsync per baton —
  `FlutterEngineOnVsync(engine, baton, frame_start, frame_target)` / `FlutterJNI.onVsync(start,
  target, cookie)` → `nativeOnVsync` — **on the platform task runner** (Flutter rejects vsync from
  any other thread).
- **The wp_presentation association**: mint a `wp_presentation_feedback` on the surface *before*
  each `wl_surface.commit`/`eglSwapBuffers` (`RequestFeedback`); on the compositor's `presented`
  event, take the scanout timestamp and the `refresh` period and deliver **`frame_start =
  present_ns`, `frame_target = present_ns + refresh_ns`** (`PostOnVsync` → `OnVsync(engine, baton,
  now_ns, now_ns + period_ns)`). Refresh defaults to 60 Hz (16.67 ms) until the first `presented`,
  clamped to a 10 Hz floor.
- **Park vs drain**: if a present is already in flight (feedback pending) → **park** the baton, the
  `presented` event returns it; if the pipeline is idle → **drain inline** on the wall clock so
  Flutter is not stalled. `discarded` still returns the baton (wall clock).
- **Clock gate**: only usable when `wp_presentation.clock_id == CLOCK_MONOTONIC` (matches
  `FlutterEngineGetCurrentTime`); otherwise leave vsync unset and let Flutter self-schedule.

**acl already has this exact source.** The Looper-driven present (`PresentDriver` +
`acl_deliver_vsync`, clocked by `wp_presentation` `OnPresented → present_ns + refresh`) is the same
mechanism ivi-homescreen's provider implements. On the **Android** engine path, vsync rides Java
`android.view.Choreographer` → `DisplayEventReceiver` — which acl **already drives** from that same
wp_presentation source. So if the NDK trampoline **omits `AChoreographer_*`** (the research shows
the engine then falls back to the Java `nativeOnVsync` path), Flutter's vsync flows through acl's
existing Choreographer/`acl_deliver_vsync` machinery for free — matching ivi-homescreen's
`frame_start = present_ns`, `frame_target = present_ns + refresh` timing. Vsync is **reuse, not new
work**; only the timestamps/thread-affinity above must be honored.

## The bionic bridge — validated foundation + the embedding mechanism

**Stage 0 — PROVEN.** A bionic x86_64 exe, run via the bionic `linker64` with the vendored bionic
runtime, `dlopen`'d a bionic `.so` and called it → `answer()=42` **on this glibc host**. So the
runtime + linker are sound; bionic code runs here. Recipe: NDK clang `--target=x86_64-linux-android24`
for the `.so`/exe; `m linker` → `…/apex/com.android.runtime/bin/linker64`; the real 64-bit bionic libs
are `out/soong/.intermediates/bionic/{libc,libm,libdl}/…/**android_x86_64_shared**/*.so` +
`linker/ld-android/android_x86_64_shared/ld-android.so` (**not** `android_x86_x86_64_shared`, which is
32-bit); run `LD_LIBRARY_PATH=<bionic-libs> ./linker64 <ABSOLUTE-path-exe>` (the missing
`/linkerconfig/ld.config.txt` warning is harmless).

**Stage 0b — the in-process embedding (the hard, dedicated piece).** acl needs the app `.so` loaded
*into* the glibc ART process (so its JNI reaches the JVM), not a child process. The mechanism is now
pinned:
- The bionic `linker64`/`ld-android.so` export a programmatic loader — `__loader_dlopen`,
  `__loader_android_dlopen_ext`, `__loader_dlsym`, `__loader_android_create_namespace`, … (bionic
  `dlfcn.cpp`).
- But glibc **cannot** call them naively: glibc's own linker can't even map `ld-android.so` (a bionic
  `.so` whose `NEEDED` don't resolve against glibc), and `__loader_dlopen` requires the linker's
  `solist`/namespaces initialized plus a `caller_addr`.
- So the bionic linker must be **bootstrapped in-process**: map `linker64` (PIE, self-relocating),
  call `__linker_init(KernelArgumentBlock)` (bionic `linker_main.cpp:698`) with a **synthetic**
  argument block (fake `argc/argv/envp/auxv`, no real main executable), set up TLS + a default
  namespace, then `__loader_android_dlopen_ext` the app `.so` in that namespace with our trampoline
  `.so` on its search path.
- This is exactly what **libhybris** (Apache-2.0) implements (for the reverse direction — glibc apps
  using bionic GL drivers). Options: **(a)** vendor/patch libhybris's linker-loading core, or **(b)** a
  purpose-built bootstrap in `src/ndk/` (a controlled subset — acl loads a known `.so` set, not the
  general case). Both are bionic-version-specific (target the vendored android-13 bionic) and are a
  focused multi-step build, not a small shim. The two-libc coexistence (bionic libc for app `.so` +
  glibc for acl, bridged at the NDK/JNI boundary) is the R-6 risk to manage here.

## Verify

```sh
# Stage 0 (scratchpad /tmp/acl/scratchpad/bionic0/): bionic .so runs on host
LD_LIBRARY_PATH=broot/lib64 broot/linker64 $PWD/broot/hello_exe   # -> answer()=42
# host-.so fixture (scratchpad): proves the loadLibrary plumbing
acl run ndkdemo.apk --main=com.acl.ndkdemo.NativeMain   # -> "loaded libacldemo; answer()=42"
# a real arm-only app reports the mismatch:
acl run <arm-only>.apk                                  # -> "no host-compatible ABI ... bionic bridge"
```
