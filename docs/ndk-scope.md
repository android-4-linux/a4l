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

### GPU-from-NDK: decouple at the buffer, not the API (Vulkan external memory)

Item 2 lists `libEGL`/`libGLESv3`/`libvulkan` "→ host Mesa" as if the whole GPU API is thunked
across the bionic↔glibc line. For a **NativeActivity / native-GPU** app (a bionic Vulkan or GLES
renderer, not just JNI glue) that naive thunk is the wrong primitive — huge surface, fragile, and it
drags the app's driver state through a foreign libc. The correct decoupling is at the **buffer**:

- Give the NDK GPU code its **own bionic Vulkan ICD** (bionic V3DV / turnip — Android's own Mesa
  build) inside its namespace. It renders with its native driver, no API thunk.
- Hand images to the host glibc side via **`VK_KHR_external_memory_fd` (dma-buf)** +
  **`VK_KHR_external_semaphore_fd` (`sync_file`/`drm_syncobj`)**. The two drivers share **buffers +
  fences, never address space** — zero-copy, explicit sync, in-process (two Vulkan instances).

This is Android's own contract, not a new interop: **`AHardwareBuffer`/gralloc + `ANativeWindow`
ARE dma-buf + external-memory interop**. So acl implements the contract it already owns:
1. **`AHardwareBuffer_allocate` → the HOST allocator** (`gbm`/minigbm on the glibc side) → dma-buf
   fd + stride + offset + modifier in the AHB handle. Single allocator ⇒ every buffer is
   host-Mesa- and display-consumable; the app only ever *imports*, never allocates.
2. **`ANativeWindow` = acl BufferQueue over host AHBs** (`src/ndk/`): `dequeueBuffer` → a free host
   AHB + acquire fence; `queueBuffer` → return + completion fence.
3. **Two producers import the same host AHB:** the framework (glibc ART, HWUI-equiv on host
   Skia/Vulkan — trivial, no bionic) and NDK GPU code (bionic ICD, via
   `VK_ANDROID_external_memory_android_hardware_buffer`, or — since acl owns the AHB — the raw
   dma-buf fd + `VK_KHR_external_memory_fd`).
4. **Consumer = host glibc Mesa / drm-cxx** imports the same dma-buf for composition/scanout.
5. **Sync** via `sync_file`/`drm_syncobj` fds on both handoffs — libc-agnostic.

The only things crossing the bionic↔glibc line are the AHB handle (dma-buf fd + format/stride/
modifier ints) and sync fds — no shared libc, no Vulkan-API thunk, no copy. So `libEGL`/`libGLESv2`
still thunk (small, legacy), but the **Vulkan/Impeller path decouples at AHardwareBuffer**. This
extends acl's "re-own `android.graphics` on host Skia+Vulkan-dmabuf" decision to the NDK GPU-app
case. Open questions: modifier agreement between the two V3DV builds (same silicon ⇒ should match);
V3DV `sync_file`/`drm_syncobj` export; whether the bionic ICD loads in the libhybris namespace
in-process or needs a bionic sidecar — and if a sidecar, *that* (the app's native GPU context,
decoupled by external memory) is the only justified process split, never the framework render path.

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

**Stage 0b — the in-process embedding — PROVEN (2026-07-16).** `tools/ndk-loader-spike/` (run
`build.sh`) does it end to end on the x86_64 host: `SPIKE: nativeAnswer() = 42 (PASS)`, deterministic.
A **glibc** process maps `linker64`, calls `__dl___linker_init` *directly as a function* with a
synthetic `KernelArgumentBlock` (standalone mode: `AT_ENTRY` = the linker's `_start`, a minimal
bionic stub exe as `argv[1]`), then `__loader_dlopen`s a bionic `.so` — resolving its whole dep graph
(bionic `libc`, `liblog`→`libc++`, `libdl`) — `__loader_dlsym`s the symbol, and **calls it** (its
`__android_log_print` runs, no crash) → 42. So bionic app native code loads and runs *inside* a glibc
process; the host NDK premise is de-risked. Key facts that made it work (all verified in the spike):
- **The TLS crux is real but bounded.** `__linker_init` sets `%fs` to bionic's TCB; we `ARCH_GET_FS`
  before and restore glibc's `%fs` after, and bracket every bionic call (dlopen/dlsym/the app fn) with
  the swap. `__libc_init_main_thread_late` migrates `%fs` to a **permanent mmap'd TCB**, so calling
  `__linker_init` and returning is safe (the stack `temp_tcb` isn't the lasting one). Harness is
  `-fno-stack-protector` (the canary lives at `%fs:0x28`; a mid-function swap would false-fail it).
- **No init-only mode**, so we hand the linker a throwaway bionic stub exe and call `__linker_init`
  *directly* (not via `_start`, whose `jmp *%rax` would jump into the stub) to keep control.
- **Linker input errors are fatal** (it `__linker_error`+aborts the process) — real integration must
  pre-validate + absolute-path inputs. Bionic deps resolve via `LD_LIBRARY_PATH` (no host `ld.config`).

The mechanism (map PIE → synthetic KAB → `__linker_init` → `__loader_*`) is arch-neutral in structure;
the arm64 port swaps only the TLS register (`TPIDR_EL0`), auxv, and reloc details (the target
`linker64` carries the relocs). The remaining work is integration, not feasibility.

**Rung 3 — promoted into `src/ndk/` (2026-07-16).** The spike is now a real acl API:
`src/ndk/bionic_runtime.{h,cpp}` — `BionicRuntime` (process-singleton; the linker bootstraps once),
`BionicConfig` (paths from `ACL_BIONIC_LINKER`/`_SYSROOT`/`_STUB`, validated up front because linker
input errors are fatal), `load()`/`symbol()`, and an RAII `RunScope` that brackets bionic calls with
the `%fs` swap (the swap confined to the `-fno-stack-protector` TU). Arch-guarded: non-x86_64 returns
a clean "arm64 TLS port pending" error so cross builds stay green. Verified via `acl-ndk-probe`
(`nativeAnswer()=42 (PASS)`, logging through acl's `log/`).

**Rung 4a — the bidirectional %fs boundary, PROVEN (2026-07-16).** `acl-ndk-probe --boundary` →
`boundary selftest = 1010 (PASS)`. A bionic `.so` under bionic `%fs` writes a bionic thread-local,
calls **back into glibc** (which writes glibc `errno` under glibc `%fs`) through an internal up-call
trampoline that restores glibc `%fs` for the callback and bionic `%fs` on return, and the bionic TLS
is **intact** afterward (the `+1000`). This is the JNI up-call/down-call mechanism in miniature —
`BionicRuntime::run_boundary_selftest` + the `-fno-stack-protector` `upcall_trampoline`. So a loaded
app `.so` can safely call ART's JNI and return. Toolchain note: android clang defaults to
**emulated-TLS** (`__emutls_get_address`, a compiler-rt symbol absent from bionic) — app/fixture
`.so` need `-fno-emulated-tls` so `__thread` uses native ELF TLS (`__tls_get_addr`, bionic-provided).

**Rung 4b — the JNIEnv/JavaVM boundary trampoline, PROVEN (2026-07-16).** `acl-ndk-probe --jni` →
`JNI_OnLoad ver=0x10006 up-calls=4 probe_tls=333 native=4200 (PASS)`. A bionic `.so`'s **real
`JNI_OnLoad`** drives `GetEnv` (VM) → `GetVersion`/`FindClass`/`RegisterNatives` (Env) — 4 up-calls,
each crossing bionic→glibc through a `%fs`-restoring thunk (`probe_tls=333` confirms glibc TLS stayed
intact across them) — and the `RegisterNatives`-bound native is then invoked via a down-call
(returns 4200). Impl: `BionicRuntime::wrap_java_vm(real_vm, real_env)` returns a wrapper `JavaVM`
whose `GetEnv` hands back a wrapper `JNIEnv`; the thunked slots run the real ART fn under a `ToGlibc`
guard. Portable across jni.h variants via `JniEnvTable`/`JniVmTable` (derived from the `functions`
member — OpenJDK names the structs `*_`, Android drops it; same ABI). **The full JNIEnv/JavaVM thunk table is now complete (2026-07-16)** — all ~232 JNIEnv slots +
the JavaVM invoke interface are `%fs`-thunked (`ToGlibc`), generated via type-family macros
(`Call*Method` ×10 types ×3 kinds ×3 forms, `Get/Set*Field` instance+static, primitive `*Array*`
families) plus the fixed-signature functions. Varargs `Call*Method`/`NewObject` forward through
their `...V` variants; `GetJavaVM`/`GetEnv` hand back the wrapper. Runtime-verified including the
riskiest path: `acl-ndk-probe --jni` now does a `CallIntMethod(20,30)` **varargs up-call** from the
registered native → `native=4250` (`4200+50`), proving `va_list`→`...V` across the boundary.

**Down-call trampoline + `load_library` — PROVEN (2026-07-16).** The reverse direction: when ART
invokes a `RegisterNatives`'d bionic native it calls under **glibc** `%fs`, but the native is bionic
code needing bionic `%fs` + the wrapped `JNIEnv`. `w_RegisterNatives` now emits, per native, a tiny
**runtime-generated machine-code trampoline** (`make_downcall`) that substitutes the wrapped env,
`wrfsbase`→bionic, calls the native, `wrfsbase`→glibc, returns — signature-agnostic for
register-argument natives (the JNI norm; stack-arg natives would need arg copying, TODO). Uses
FSGSBASE (verified available: CPU flag + kernel 7.1 + userspace `rdfsbase`). `acl-ndk-probe --jni`
now invokes the native **from glibc `%fs`, exactly as ART would**, through the trampoline →
`native=4250`: the complete realistic round-trip **glibc(ART) → down-call trampoline → bionic native
→ varargs up-call trampoline → glibc → back**. And `BionicRuntime::load_library(path, real_vm,
real_env)` packages the whole thing — dlopen the app `.so`, wrap ART's `JavaVM`, drive `JNI_OnLoad`
across the boundary — the single call the `nativeLoad` hook makes.

**End-to-end against a REAL JVM — PROVEN (2026-07-16).** `acl-ndk-probe --realvm` boots a genuine
host OpenJDK JVM (linked via `jni_dep`), bootstraps the bionic linker **in the same process** (glibc
JVM + bionic runtime coexisting), `load_library`s the bionic `.so` — driving `JNI_OnLoad` against the
**real JVM** (real `FindClass`/`GetStaticMethodID`/`RegisterNatives`) — then calls `Main.answer()`
**from Java**: the JVM's own dispatch invokes the down-call trampoline → bionic native → a
`helper(20,30)` `CallStaticIntMethod` up-call back into the JVM → `4250 (PASS)`. So the entire stack
works against a genuine VM, which is exactly what `jvm_vm.cpp` provides. The Java fixture
(`com.acl.ndktest.Main` with a native `answer()` + a `helper()`) is built by
`tools/ndk-loader-spike/build.sh` (`ACL_NDK_TEST_CLASSES`).

The `jvm_vm.cpp` hook is now de-risked to a thin call. NEXT is pure integration: (1) override
`Runtime.nativeLoad` in `jvm_vm.cpp` (the `register_*_override` pattern) to call
`BionicRuntime::load_library(path, artVM, artEnv)` for app `lib/<abi>/*.so` (glibc supplemental libs
chain to the original); (2) stage a bionic runtime for acl (linker+libs+stub) so `ACL_BIONIC_*`
resolve in a real run; (3) per-thread attach-as-bionic (today uses the main thread's captured glibc
`%fs` — ART worker/app threads each need a bionic TCB); (4) a bionic-JNI APK for the full acl run
path; (5) NDK trampoline `.so` (`liblog`→acl `log`, `libandroid`→`wm`); then arm64.

The mechanism, pinned:
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

### Stage 0b — libhybris core vs a purpose-built bootstrap (the two paths, contrasted)

They are **different philosophies**, not two ways to write the same thing:

- **libhybris = one libc, bionic calls hooked to glibc.** libhybris carries its *own fork* of the
  bionic linker (`hybris/common/<ver>/`) and a large `hooks.c` that intercepts bionic libc calls and
  routes them onto glibc — because in its world (glibc apps using bionic **GL drivers**) FILE*/`pthread_t`/
  `errno`/locale structs cross the boundary constantly, so both sides must share *one* libc's state.
- **Purpose-built = two libcs, isolated, bridged only at JNI.** This is what **Stage 0 already proved**:
  load the *real* AOSP `linker64` + real bionic `libc/libm/libdl` in their own namespace; the app `.so`
  resolves its `NEEDED` against real bionic libc (no per-function hooking). glibc (ART, Skia, Mesa) and
  bionic (app `.so`) never share libc state — the only thing crossing is the **JNI ABI** (`JNIEnv*` +
  `jobject` handles) and the **NDK ABI** (our trampoline `.so`). A JNI boundary is far narrower than a
  GL-driver boundary — no shared `FILE*`/`pthread_t` — so the hook surface collapses to nearly nothing.

| Dimension | libhybris core | Purpose-built `src/ndk/` bootstrap |
|---|---|---|
| Libc model | one (glibc), bionic **hooked** | two, **isolated** (Stage-0 proven) |
| Correctness for arbitrary app JNI | hooks tuned for **GL drivers**; an app's full bionic libc surface may exceed them | app gets **real bionic libc** → behaves as on-device |
| Version coupling | pinned to libhybris' forked linker; must track our android-13 bionic separately | uses the **AOSP tree's own** `linker64`+bionic → auto-matched to the vendored build |
| Direction fit | built for glibc→bionic-**GL**; must be re-patched for glibc-ART→bionic-**app-JNI** | built for exactly our direction |
| Dependency | vendor + patch an external tree (Apache-2.0, per-file check) | fully in-tree, no new dep |
| Time-to-first-`dlopen` | faster (proven loader) | slower (write the KAB/auxv/TLS bootstrap) |
| Ongoing ownership | track upstream + our patches | we own a small, controlled loader |

**The genuinely hard part is the same for both: TLS / thread-pointer coexistence.** Both glibc and
bionic put the thread pointer in the *same* register — `%fs` on x86_64 (`arch_prctl(ARCH_SET_FS)`),
`TPIDR_EL0` on arm64 — and each libc expects it to point at *its own* TCB. ART's threads (main + JNI)
are glibc threads, so when app bionic code on an ART thread calls a bionic libc function it reads TLS
off glibc's TCB → garbage. Resolutions (both approaches must pick one): (a) run app native code on
**dedicated bionic-TLS threads** and marshal JNI upcalls across a known seam (cleanest for the
two-libc model); (b) **swap the thread pointer** on every bionic entry/exit (what libhybris does in
spots — fragile, per-call cost); (c) lay bionic's TLS slots so they don't collide with glibc's usage
on that TP (brittle across libc versions). This is the R-6 crux and it is **arch-specific** — the
register, the set-TLS syscall, and the ELF relocation types (`R_AARCH64_*` vs `R_X86_64_*`, handled by
the target's own `linker64`) all differ per arch.

### The architecture axis — loader vs CPU-ISA (they're orthogonal)

The loader question is separate from *can the app's machine code even run here*:

- **arm64 host (the automotive boards): app `lib/arm64-v8a/*.so` is native → the loader alone is
  sufficient.** No CPU translation. This is the **tractable target** and it's the automotive critical
  path — build the arm64 `linker64`+bionic, bootstrap in-process, done.
- **x86_64 host (desktop): the corpus ships arm-only** (Spotify `libstub`, Rive `librive`) — so even a
  perfect loader maps in an **arm64** `.so` that **cannot execute** on x86_64 without a CPU translator
  (QEMU user-mode / houdini-class) — a separate, larger effort. The loader **is** sufficient on x86_64
  for the **x86_64-native** NDK apps (a Flutter `--target-platform android-x64` build, apps that ship
  `lib/x86_64/`).

**Upshot:** the bootstrap *mechanism* is largely arch-neutral in structure (map PIE linker →
`__linker_init(synthetic KAB)` → set TLS → `__loader_android_dlopen_ext`); only the TLS/auxv/reloc
details are per-arch, and the target's own `linker64` already carries the reloc handling. So the
smart target order is **arm64 first** (loader = complete story, matches the boards), and on x86_64
scope to x86_64-native `.so` until/unless a CPU-translation layer is justified. Recommendation:
**purpose-built** — Stage 0 already validated its exact substrate (real `linker64` + real bionic libc
run here), it auto-matches the vendored bionic, keeps the JNI boundary a clean two-libc seam, and
avoids owning a patched fork of someone else's linker aimed at a different direction; mine libhybris'
`hooks.c` and TP-swap code as a **reference** for the TLS crux rather than vendoring the whole tree.

## Security — signing & containment (R-10)

Native libs are **not individually code-signed** — an ELF `.so` carries no signature, and the bionic
linker verifies nothing at load (so our loader replicating bionic needs no signature logic). Integrity
comes from the **whole-APK signature**: v1 (JAR) digests every entry in `MANIFEST.MF` (covers each
`lib/<abi>/*.so`); v2/v3 sign the archive bytes as a blob. So the `.so` are vetted **exactly as much
as the APK is** — nothing separate to vet at the `.so` level.

- **Integrity is a `pkg/`-layer, one-time concern, not a per-load one.** The right design is a single
  APK v2/v3 signature verification when acl *opens* the APK (mirroring Android install-time), after
  which every extracted `.so` is trusted by construction. acl does **not** do this yet (`pkg/apk.cpp`
  only parses ZIP structure) — a bounded, standard task to add before running untrusted APKs. The NDK
  loader itself stays signature-free.
- **The real runtime worry is containment, not signing.** Signing says *who authored* the code, not
  that it's *safe*. Once an app `.so` runs it is arbitrary native code in acl's **single glibc
  process** — no per-app UID / SELinux / seccomp cage like on-device Android — so it can reach ART,
  Mesa, and the Wayland connection. That's an architectural sandboxing question (seccomp around the
  bionic-namespace threads; process/UID isolation later), orthogonal to both signing and the loader,
  and tracked as its own ADR. For the hash-pinned dev corpus the files are user-vetted, so neither
  blocks bring-up.

## Verify

```sh
# The gap, reproducibly (negative control): build a real bionic-ABI .so and show that a plain
# glibc dlopen CANNOT load it — glibc's ld.so can't satisfy its NEEDED bionic libc.so.
# This is WHY Stage 0b (a bionic linker bootstrapped in-process) is needed, not glibc dlopen.
tools/build-ndk-repro.sh    # -> NEEDED libc.so/liblog.so/libdl.so; "dlopen FAILED: .../libc.so: invalid ELF header"
# Stage 0 (scratchpad /tmp/acl/scratchpad/bionic0/): bionic .so runs on host via linker64
LD_LIBRARY_PATH=broot/lib64 broot/linker64 $PWD/broot/hello_exe   # -> answer()=42
# host-.so fixture (scratchpad): proves the loadLibrary plumbing
acl run ndkdemo.apk --main=com.acl.ndkdemo.NativeMain   # -> "loaded libacldemo; answer()=42"
# a real arm-only app reports the mismatch:
acl run <arm-only>.apk                                  # -> "no host-compatible ABI ... bionic bridge"
```
