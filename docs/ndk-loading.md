<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original acl design. Grounds on ADR-5 (plan.md §4 Phase 4), AOSP bionic/linker +
     libnativeloader docs, NDK stable-ABI docs, libhybris (Apache-2.0, per-file check on reference).
     No GPL-3.0 sources. -->

# NDK native-library loading (host path)

The gate between "acl runs Java/Kotlin apps" and "acl runs the real corpus." Apps ship native code as
`lib/<abi>/*.so` compiled against the **Android NDK = bionic ABI**; acl is a **glibc** process running a
**glibc** host ART. When the app does `System.loadLibrary("foo")`, ART must `dlopen` a bionic `.so`
in-process with the glibc runtime — the two-libc problem, now for app code.

## The failure, reproduced on the host (2026-07-16)

Built a real `x86_64-linux-android` NDK `.so` (AOSP prebuilt clang `--target=x86_64-linux-android31`,
linked against the device bionic `libc.so`/`liblog.so`/`libdl.so` + `crtbegin_so.o`/`crtend_so.o`);
`NEEDED: libc.so liblog.so libdl.so` (bionic sonames). A glibc process `dlopen`ing it:

```
dlopen FAILED: /lib64/libc.so: invalid ELF header
```

The glibc loader tries to satisfy the bionic `.so`'s `NEEDED libc.so`, finds glibc's `/lib64/libc.so`
(a GNU ld **linker script**, not an ELF → "invalid ELF header"), and stops. Even if the *real* bionic
`libc.so` were on the path, glibc's `ld.so` cold-loading it SIGTRAPs (proven on the Pi, pi-plan §P-B/M3).
So it fails at **dependency resolution, in-process, before any app code runs**. (Repro build:
`tools/build-ndk-repro.sh`.)

## Why the obvious non-solutions don't work

- **Plain glibc `dlopen`** — the above.
- **`dlmopen` (new glibc link-map namespace)** — still glibc's `ld.so` loading bionic `libc` → SIGTRAP.
  A separate namespace isolates symbols, not the *loader*.
- **A bionic sidecar process for the `.so`** — JNI native calls (ART↔`.so`) are hot and frequent;
  a process boundary per JNI call is a non-starter. The `.so` MUST be in-process with ART.

## The approach (ADR-5): a bionic linker namespace inside the glibc process

Don't let glibc's `ld.so` touch the bionic libs. Embed the **bionic linker** (`ld-android.so` /
`linker64`) and use *its* `dlopen` to load the app `.so` and its bionic deps into a bionic namespace;
glibc and bionic coexist because they never share a loader or a `libc`. This is the class of problem
**libhybris** solves (Apache-2.0 — reference is license-fine per plan.md; not vendored locally yet).

Integration point: **ART's `libnativeloader`** (`art/libnativeloader/native_loader.cpp`
`OpenNativeLibrary` → `android_dlopen_ext` with a classloader namespace). On the glibc host ART this
lowers to glibc `dlopen`. acl intercepts here — route app `System.loadLibrary` through the bionic
namespace loader instead.

The two coexistence hazards (what libhybris patches around): bionic **TLS** model + `__libc_init`
expectations vs glibc's, and the bionic linker's assumption that it *is* the process's PT_INTERP. A
host bionic linker (AOSP `linux_bionic-x86_64` `ld-android.so`, not built here yet) or the hybris
linker handles these.

## The bridge surface — bounded, because the NDK ABI is stable

The app `.so`'s `NEEDED`/imports are the **stable NDK libraries**, not internal platform libs — a small,
versioned, documented set. acl provides bionic-ABI implementations that trampoline into the host /
acl services:

| NDK lib | acl bridge |
|---|---|
| `libc.so`, `libm.so`, `libdl.so` | real bionic (loaded in the namespace) — the only genuinely-bionic deps |
| `liblog.so` | → acl `log/` |
| `libandroid.so` (`ANativeWindow`, `ALooper`, `AAssetManager`, sensors, input) | → acl `wm/` (`ANativeWindow`→BufferQueue), the reactor (`ALooper`→epoll), `pkg/` (assets) |
| `libEGL.so`, `libGLESv2/v3.so`, `libvulkan.so` | the GPU path — see below |
| `libnativewindow.so` (`AHardwareBuffer`) | host `gbm`/minigbm dma-buf alloc (see below) |
| `libaaudio.so`, `libOpenSLES.so` | → `audio/` (PipeWire) |
| `libmediandk.so` | → `media/` |

## GPU-from-NDK: decouple at the buffer, not the API (Vulkan external memory)

Do **not** thunk the whole EGL/GLES/Vulkan API from the bionic namespace to glibc Mesa (huge, fragile).
Instead give NDK GPU code its own **bionic Vulkan ICD** (bionic V3DV/turnip — Android's own Mesa) in the
namespace; hand rendered images to the host glibc side via **`VK_KHR_external_memory_fd` (dma-buf)** +
**`VK_KHR_external_semaphore_fd` (sync_file/drm_syncobj)**. The two drivers share **buffers + fences,
never address space** — zero-copy, correct explicit sync, no API thunk, in-process.

This is Android's own model: `AHardwareBuffer`/gralloc + `ANativeWindow` ARE dma-buf + external-memory
interop. So acl implements the contract, not a new one:
1. **`AHardwareBuffer_allocate` → HOST `gbm`/minigbm** (single allocator ⇒ the buffer is always
   host-Mesa- and display-consumable; the app never allocates, only imports).
2. **`ANativeWindow` = acl BufferQueue over host AHBs** (`ndk/`). `dequeueBuffer`→free AHB + acquire
   fence; `queueBuffer`→return + completion fence.
3. Two producers import the *same* host AHB: the **framework** (glibc HWUI-equiv, host Vulkan/Skia —
   trivial, no bionic) and **NDK GPU code** (bionic ICD, via `VK_ANDROID_external_memory_android_
   hardware_buffer` or the raw dma-buf fd + `VK_KHR_external_memory_fd`).
4. **Consumer = host glibc Mesa / drm-cxx** imports the dma-buf for composition/scanout.
5. Sync via `sync_file`/`drm_syncobj` fds on both handoffs — libc-agnostic.

The only things crossing the bionic↔glibc line: the AHB handle (dma-buf fd + format/stride/modifier
ints) + sync fds. This extends acl's existing "re-own `android.graphics` on host Skia+Vulkan-dmabuf" to
the NDK GPU-app case (`render-architecture-decision`).

## Plan / first steps

1. **Loader spike (make-or-break):** stand up a bionic linker in the glibc process (host
   `linux_bionic-x86_64 ld-android.so` + bionic `libc`, or the hybris linker) and load the repro `.so`
   + call `nativeAnswer()` → 42. Proves the in-process bionic namespace is viable at all.
2. **Intercept `libnativeloader`:** route ART's `OpenNativeLibrary` for app libs to the namespace loader.
3. **Bridge libs, thinnest first:** `liblog` → acl `log`; then `libandroid`/`ANativeWindow`.
4. **GPU:** the `AHardwareBuffer`-on-host-`gbm` + external-memory interop above (extends `gfx/` + `wm/`).
5. **Corpus/CTS:** an SDL/NDK sample APK (Gate G4).

Until (1) lands, acl should keep ADR-5's earlier posture: **detect `lib/<abi>/` and warn/degrade**
rather than crash, so Java-only apps stay unaffected.
