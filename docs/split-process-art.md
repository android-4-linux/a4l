<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original a4l design. Grounds on plan.md §2 (architecture), ADR-0001 (host ART),
     ADR-0003 (no-Binder in-process hub), ADR-0004 (Skia + Vulkan dma-buf present), docs/pi-plan.md
     (arm64 host ART: B1 in-process ruled out, B2 musl port is multi-week). No GPL-3.0 sources. -->

# Split-process host ART for arm64 (B1-split)

## Why (the decision)

a4l runs the app's DEX on a real AOSP **host** ART loaded in-process (ADR-0001), and reimplements the
native underside (JNI) in C++ as the a4l core — a **single process, no Binder** (ADR-0003). On x86 the
host ART is `linux_glibc`, same libc as the rest of a4l, so single-process is trivial.

On **arm64 the only stock AOSP host ART is bionic**, and — proven on a Pi 4 (`docs/pi-plan.md` §P-B/M3)
— a glibc `acl` process **cannot `dlopen` a bionic `libart.so`** (bionic libc can't cold-load under
glibc's loader → SIGTRAP). The two clean fixes were:

- **B2** — build a single-libc (musl) arm64 host ART. The toolchain + full LLVM-runtime port are done
  and proven, but consuming them means porting the *entire* AOSP host module graph to a new
  `linux_musl_arm64` host — a **multi-week** effort (`tools/aosp/b2-musl-arm64/README.md`).
- **B1-split (this doc)** — keep the **stock bionic** arm64 host ART, but run it in **its own
  all-bionic process**, and IPC to the glibc `acl` process that owns the platform (Wayland, Vulkan
  present, PipeWire, input). **The bionic ART already builds (stock `linux_bionic-arm64`) and runs
  natively on the Pi** (P-B/M2) — no toolchain work at all. The cost is a process boundary.

**B1-split is chosen for arm64 bring-up** because it reaches a running `--vm=art` on the Pi without the
multi-week host-graph port, and — the key structural insight below — a4l **already has the exact
boundary** the split needs.

## The boundary already exists: dma-buf

a4l does **not** marshal fine-grained draw calls. The app's View tree renders via `android.graphics`
JNI (`libacl_graphics_jni`, Skia + Vulkan) into a **dma-buf** (`import_pixels_to_dmabuf` in
`jvm_vm.cpp`), and `wm::present_dmabuf_loop` attaches that dma-buf to a `wl_surface` via
`zwp_linux_dmabuf_v1` — no CPU upload, modifier-tiled. Input flows Wayland → `present_deliver_input`
→ JNI into the View.

So a4l's internal frame path is already **[render → dma-buf] → [present]**. Split-process puts a
socket at that seam: the **render** half (ART + framework + Skia) runs in the bionic process, the
**present** half (Wayland) stays in the glibc process, and only the **dma-buf FD + damage** cross the
boundary (via `SCM_RIGHTS`). Both processes are on the same machine and the same GPU (V3DV on the Pi),
so dma-buf sharing is zero-copy. This is the Chromium-GPU-process / Waydroid-style split, and it is a
small, well-defined surface precisely because the boundary is a buffer, not an API.

## Architecture

```
  ┌─────────────────────────────┐         ┌──────────────────────────────────┐
  │  art-host   (BIONIC, arm64) │         │  acl        (glibc, arm64)       │
  │                             │         │                                  │
  │  libart.so + libandroid_    │  Unix   │  wm/ present_dmabuf_loop         │
  │    runtime.so  (bionic)     │ socket  │    (wl_surface + zwp_linux_      │
  │  framework jars + app DEX   │◀──────▶ │     dmabuf_v1 + wp_presentation) │
  │  libacl_graphics_jni (Skia/ │  + SCM_ │  input source (wl_pointer/kbd)   │
  │    Vulkan → dma-buf)        │  RIGHTS │  audio sink (PipeWire)           │
  │  JNI natives = IPC stubs    │  (fds)  │  pkg/ (APK) + manifest resolve   │
  └─────────────────────────────┘         └──────────────────────────────────┘
        renders a frame                          composites + presents,
        forwards fd+damage  ───────────────▶     sends input back  ◀────────
```

- **art-host** (new, **bionic**): a small `linux_bionic-arm64` executable. Boots ART exactly as
  today's `ArtVm::create_java_vm` does (`ANDROID_ROOT`, bootclasspath from `ACL_ART_HOME`,
  `JNI_CreateJavaVM`) — but *this binary is bionic*, so there is no coexistence problem. Runs the
  app; renders the View to a dma-buf; its window/input/audio JNI natives are thin stubs that
  serialize over the socket.
- **a4l** (existing, glibc): opens the APK, resolves the launcher (pkg/), launches art-host, and runs
  the platform side — `present_dmabuf_loop` fed by the received dma-buf, a Wayland input source that
  forwards events, and a PipeWire sink fed by an audio ring. Everything Wayland/Vulkan/PipeWire stays
  glibc and unchanged.

## The IPC protocol (`acl_ipc`, new)

A framed message stream over a `socketpair(AF_UNIX, SOCK_SEQPACKET)`; dma-buf and audio-shm FDs travel
as `SCM_RIGHTS` ancillary data. Messages (small, fixed structs — no general serialization framework):

| Direction | Message | Payload |
|---|---|---|
| art-host → a4l | `SurfaceCreate` | surface id, initial w×h |
| art-host → a4l | `SurfaceResize` | surface id, w×h |
| art-host → a4l | `PresentFrame` | surface id, dma-buf **fd**, w×h, drm_format, modifier, stride, offset, damage rect, acquire-fence fd |
| a4l → art-host | `FrameReleased` | surface id, buffer id (release-fence fd) |
| a4l → art-host | `Input` | type (pointer/key), coords/keycode, action, timestamp |
| art-host → a4l | `AudioOpen` / `AudioWrite` | shm-ring fd; rate/channels; write cursor |
| art-host → a4l | `Vsync?` ← a4l `Vsync` | Choreographer tick from `wp_presentation` feedback |
| both | `Lifecycle` | resumed/paused/stop, config (size, density) |

The buffer lifecycle is **double/triple-buffered with fences**: art-host renders into a free dma-buf,
sends `PresentFrame` with an acquire fence; a4l waits the fence, attaches, and returns `FrameReleased`
(release fence) when the compositor is done — matching how `present_dmabuf_loop` already tracks buffer
age/damage. No frame is copied.

## What's reused vs new

**Reused as-is:** the assembled bionic-arm64 `ACL_ART_HOME` tree (P-B/M2; `tools/assemble-arm64-art.sh`);
`libacl_graphics_jni` (rebuilt `linux_bionic-arm64` — stock toolchain); the whole `wm::present_dmabuf_loop`
+ `zwp_linux_dmabuf_v1` present path (stays in glibc a4l, unchanged); `pkg/` APK/manifest; the framework
boot-classpath split; SoundPool's mixer (its PipeWire sink moves to the a4l side of the audio ring).

**New:** `src/ipc/` — the `acl_ipc` transport (SEQPACKET + SCM_RIGHTS fd passing, the message structs);
`hosts/art_host/` (or `src/runtime/art_host_main.cpp`) — the bionic art-host executable + its
forwarding JNI natives; a `Backend::ArtSplit` (or an `ACL_ART_SPLIT=1` mode of the Art backend) in
`create_vm` that, on arm64, forks/execs art-host and drives it over IPC instead of embedding ART
in-process. `jvm_vm.cpp` splits along the existing dma-buf seam: the render/JNI side compiles into
art-host; `present_dmabuf_loop` + input stay in a4l.

## Bring-up milestones

- **S1 — boot ART in the subprocess. ✅ DONE (2026-07-16).** art-host boots the VM from `ACL_ART_HOME`
  and runs a static-`main` DEX; its `System.out` comes back to a4l over the socket. Proven **on x86**
  (OpenJDK, `acl-art-host` built glibc via meson) and **on the real Pi with the stock bionic host ART**
  (`acl-art-host` built `linux_bionic-arm64`, a Soong `cc_binary`; `tools/aosp/art-host/`):
  `acl run hello.apk --vm=art-split` → the bionic art-host `dlopen`s the bionic `libart.so`, boots ART
  (`JNI_CreateJavaVM` → `Runtime::Start`), runs `Hello.main`, and `a4l-arm64-art: ART booted + main()
  ran on the Pi` streams back over `acl_ipc`. **The two-libc SIGTRAP that killed in-process `dlopen`
  (pi-plan §P-B/M3) is gone** — art-host being bionic, ART loads and runs. Getting there also completed
  the ART tree: the core native libs `Runtime::InitNativeMethods()` loads at runtime
  (`libicu_jni`/`libjavacore`/`libopenjdk`/`libandroidio` + closure, `linux_bionic_arm64`) are not in
  `libart`'s NEEDED set — added to `lib64`; and the i18n/conscrypt boot jars must sit under
  `apex/<name>/javalib/` (both fixes folded into `tools/assemble-arm64-art.sh`).
- **S2 — one frame across the boundary. (in progress)** art-host renders a solid-color (then a real
  View) dma-buf and sends `PresentFrame` (fd via `SCM_RIGHTS`); a4l imports it and puts it on screen.
  First pixels on the Pi from a real app under host ART. **Present seam wired** (`wm/present_sink.h`):
  `SplitVm` routes each IPC `PresentFrame` to a `PresentSink` as an `ExternalFrame`
  (fd + w/h/fourcc/modifier/stride/damage). Two display backends slot behind it (Null today, for
  plumbing bring-up):
  - **drm-cxx KMS scanout** — the **appliance / automotive** path (ADR-0004; plan.md drm-cxx allocator
    lineage). `drm::fmt::ScanoutBuffer::import_dmabuf(disp_fd, {w,h,fourcc,modifier,planes[{fd,stride,
    offset}]})` wraps the art-host dma-buf as a KMS framebuffer (→ `fb_id`), and an atomic commit
    (`drmModeAtomicCommit`, à la the MIT `examples/common/kms_present.hpp` `pick_target`/`commit_fb`)
    puts it **straight on the display — no compositor**. This is the natural fit for an in-car /
    embedded panel where a4l is the sole client, and it is exactly what drm-cxx (MIT, `app/drm-cxx`) is
    built for. **Validated on the Pi (2026-07-16):** drm-cxx built in the trixie arm64 container
    (`meson setup -Dtests=false -Dvulkan=false`; self-contained on fmt — uses `<print>` / vendored
    header-only fmt), and `software_present /dev/dri/card0 --no-seat --pattern` scanned out **60 frames
    at 1280x1440@60 XRGB8888 via atomic flip** to the Pi's HDMI (DRM master; the desktop Weston must be
    stopped — it holds card0/vc4). So the whole drm-cxx→KMS→display path works on the Pi; the a4l sink
    reuses `import_dmabuf` + commit for the IPC-arrived dma-buf. Vendored as an optional meson subproject
    (`drm_cxx_dep`; deps libdrm/gbm/libinput/libudev/xkbcommon/libseat/libdisplay-info); the sink
    compiles out to Null when absent so a4l always links.
  - **Wayland** (`zwp_linux_dmabuf_v1`) — the **desktop** path, reusing `wm/dmabuf_presenter`'s import
    to attach the external fd as a `wl_buffer` (works alongside a running compositor, e.g. the Pi's
    labwc). The render side may allocate via drm-cxx's GBM producer or Vulkan export (`gfx_vk`).
  Backend chosen by `ACL_PRESENT_SINK` (`null`|`wayland`|`drm-scanout`).
- **S3 — input roundtrip.** Wayland pointer/key in a4l → `Input` → injected into the View in art-host;
  a tappable UI responds.
- **S4 — audio.** SoundPool/AudioTrack PCM → shm ring → PipeWire sink in a4l.
- **S5 — a real APK.** A launcher-activity app (the framework boot classpath, ADR-0002) rendering and
  interactive on the Pi via `acl run app.apk --vm=art`.

## Boundaries / non-goals

- x86 stays **single-process** (glibc host ART, no IPC) — the split is arm64-only, selected at runtime
  by libc/arch, so desktop x86 pays nothing. **Amended by ADR-0009 (2026-11-13):** that holds for
  vetted APKs; the split is also the containment boundary, and becomes a supported x86-64
  configuration for untrusted ones.
- No Binder and no general RPC/IDL: the IPC is a fixed, small, buffer-centric message set, not a
  serialization framework. It is an a4l-internal transport, not an Android-visible interface.
- This is a **bring-up** architecture for arm64. If/when B2 (single-libc arm64 host ART) is completed,
  arm64 can collapse back to the single-process model; the `Backend::ArtSplit` seam is the only thing
  that would retire. The two are not mutually exclusive — B1-split unblocks the Pi now; B2 remains the
  eventual clean end-state. **Amended by ADR-0009 (2026-11-13):** B2 removes the ABI reason for the
  seam, not the containment one. The seam does not retire.
