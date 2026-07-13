<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: plan.md §2 (wm/+gfx/), ADR-4 (pinned Skia, hwui dropped), Addendum A02.2;
     wcs examples skia-vulkan-dmabuf + skia-egl-canvas (Apache-2.0/MIT, the user's own
     project) as the presentation-path pattern; AOSP android.graphics Canvas/Bitmap JNI
     surface. No GPL-3.0 sources. -->

# Render subsystem host-enablement (P2.4) — scope

The last genuinely target-only native frontier before apps draw (per
`docs/resources-host-scope.md`): the **render** path. The resources subsystem proved the
ADR-0008 hybrid pattern (reuse host-exported AOSP framework natives + acl-owned seams for
device deps); render is the same pattern applied to graphics — but with a real presentation
backend behind it instead of a headless buffer.

## Presentation direction — GPU, patterned after wcs

Decided: the acl present path follows wcs's **`skia-vulkan-dmabuf`** example (preferred) with
**`skia-egl-canvas`** (Ganesh GL) as the alternative — NOT the wl_shm software-raster path.
Rationale:

- The **single pinned Skia** acl links (`~/.local/skia-m150`, ADR-4/A02.2) is built
  `skia_use_vulkan=true skia_use_gl=true skia_use_egl=true is_official_build=true` — the exact
  config those examples require. Ganesh Vulkan is already in the archives acl links
  (`GrVkDirectContext`, `VulkanBackendContext`).
- **Zero-copy present:** Skia's Ganesh Vulkan backend renders into modifier-tiled `VkImage`s
  exported as **dma-buf** via `zwp_linux_dmabuf_v1`, so pixels reach the compositor without
  touching `wl_shm` — and a scanout modifier lets the compositor promote the surface onto a
  hardware plane. This is the production path for an Android app's continuously-animating
  surface (Choreographer-driven), where a CPU shm blit per frame is the wrong cost.
- It's the **same wcs CRTP bindings** acl's `wm/` already uses (`ACL_HAVE_WCS`): the example
  drives `wl::DmabufFeedback`, `wl::LinuxDmabuf`, `wl::Presentation`, `wl::XdgShell` — all
  present in the vendored wcs `include/wl/`. acl reuses the framework, not the example binary.
- The scene draws through a **plain `SkCanvas*`** (`demo::DemoScene::Render`), agnostic to the
  buffer path — exactly the seam acl needs: the `android.graphics` Canvas/Bitmap natives (host
  Skia) draw into that canvas; the backend underneath is acl's concern, not the app's.

The host has it all: Vulkan 1.4 + a real GPU (RADV) + llvmpipe fallback, libdrm, vulkan-hpp,
DRM render node, wcs framework headers. So the whole path builds and is CI-testable here.

## The two seams

1. **gfx/ — the GPU render target (Ganesh Vulkan).** Owns the `VkInstance`/`VkDevice`/queue +
   `skgpu::VulkanMemoryAllocator` + `GrDirectContext` (Vulkan), and a render target that
   exposes an `SkCanvas` and, on the present path, is a dma-buf-exported `VkImage` wrapped as a
   Ganesh `SkSurface`. This is the acl-owned replacement for hwui's RenderThread/`SkiaVulkan`
   pipeline (hwui dropped, ADR-4) — the surface the `android.graphics` natives compose onto.
2. **wm/ — the Wayland present.** dma-buf feedback → modifier selection → per-slot
   `zwp_linux_buffer_params` → `wl_surface.attach`/`damage`/`commit`, with a slot ring and
   (where offered) `wp_linux_drm_syncobj_v1` explicit sync. This extends the existing
   `wm/` wcs presenter (today an shm present-loop probe) with the dma-buf path.

## Milestones

### V1 — headless Ganesh Vulkan render target (gfx/) — NO compositor
InitVulkan patterned after the example (instance 1.1, physical-device pick preferring a real
GPU, graphics queue, `VulkanMemoryAllocators::Make`, `GrDirectContexts::MakeVulkan`), an
offscreen GPU `SkSurface`, render a scene, `flushAndSubmit`, `readPixels` back to a
`gfx::Surface`. This is the example's `--screenshot` verification path (proves the render path
without a compositor grab). **Gate:** a solid-fill + rect rendered on Ganesh Vulkan reads back
matching the software rasterizer (AA off); SKIPs cleanly where no Vulkan device is present.

### V2 — dma-buf export + Wayland present
Split into V2a (gfx-side export, headless-testable) and V2b (wm-side present).

**V2a — dma-buf export (gfx/).** Export a rendered frame as a Linux dma-buf
(`VK_KHR_external_memory_fd` + `VK_EXT_external_memory_dma_buf`): a **LINEAR**
(`DRM_FORMAT_MOD_LINEAR`, guaranteed baseline) exported `VkImage` that the optimal Skia surface
is `vkCmdCopyImage`'d into, with the fd + plane layout (stride/offset/modifier/fourcc) handed
back as a `DmabufFrame`. **Gate:** headless — the exported plane layout is sane and the mmap'd
dma-buf holds exactly the software-rasterizer pixels (no compositor needed).

**V2b — modifier-tiled direct path (gfx/) + Wayland present (wm/).** The **modifier-tiled
direct path is the primary** (per the steer toward the example's zero-copy headline): Skia
renders *straight into* a modifier-tiled, dma-buf-exported `VkImage` (no copy), the modifier
chosen from the compositor's advertised set intersected with the GPU's render+sampleable
modifiers. Then build `wl_buffer`s from the `DmabufFrame` (one `add()` per plane) via
`zwp_linux_buffer_params` → `create_immed`, presented through the wcs `wl::LinuxDmabuf`/
`wl::DmabufFeedback` bindings driven from `wm/`. **Gate:** a libwayland-server test compositor
(extending the existing `close_compositor`) that *advertises* a modifier set (making the direct
path engage) and asserts the received plane params — deterministic, no GPU-import dependency.

### V3 — real android.graphics onto the gfx surface (RE-SCOPED 2026-07-12)
**Decided (user): acl is a host translation layer (host-translation-layer style), not a device container. Render via
Android's own Skia, backed by our Vulkan-dmabuf present.** See memory `render-architecture-decision`.

**Dead end found — DON'T reuse the host libandroid_runtime graphics natives directly.** ArtVm CAN
register the graphics umbrella (`register_android_graphics_classes` + `SkGraphics::Init`, needs a
`graphics_native_classes` CSV + canonical registration order — ColorSpace before Graphics, etc.),
but EXECUTING Bitmap/Canvas crashes: the **target** framework dex carries `@CriticalNative` (real
host ART honors it → critical calling convention, handle-only), while the **host** libandroid_runtime
is the **layoutlib** variant that compiles `@CriticalNative` natives with the normal `(env,clazz,handle)`
signature (`graphics_jni_helpers.h`: `#ifdef __ANDROID__` empty, else `JNIEnv*,jclass` — *"Host targets
(layoutlib) do not differentiate between regular and critical native methods"*). → ABI mismatch →
null-deref in `Bitmap_isImmutable`. (`@FastNative`, which AssetManager/resources use, keeps the normal
signature, so R1/R3 are unaffected.) The real hwui-Vulkan (`libhwui.so`) + device libandroid_runtime
are device-only (bionic) binaries; the host lib has NO Vulkan hwui pipeline. Rejected: clearing
`@CriticalNative` (user concern), porting libhwui to host (heavy + still fights it), and the
device-container model (contradicts ADR-1/host-ART, single-process, no-Binder).

**CHOSEN — the clean split (S1–S4):** Android's Skia renders; acl's Vulkan-dmabuf is the backend.
- **S1 ✅** — build Android's android-13 host `libskia.a` (`m out/soong/.intermediates/external/skia/
  libskia/linux_glibc_x86_64_static/libskia.a` after `lunch aosp_x86_64-eng`; 97MB). android-13 Skia
  is an OLDER API than m150 (`GrDirectContext::MakeVulkan`, `SkSurface::MakeRenderTarget`,
  `GrVkBackendContext`).
- **S2** — acl's gfx present backend is **pure Vulkan (no Skia in meson — avoids Android Skia's
  jpeg/ft2/icu/… dep tree)**: allocate the dma-buf-exported `VkImage`, export fd, present. Expose the
  `VkDevice`/queue + `VkImage` (`GrVkImageInfo`-shaped) handles.
- **S3** — `libacl_graphics_jni`: a Soong module with acl's **own minimal graphics natives**
  (Bitmap/Canvas/Paint calling core Skia directly — the full AOSP `android_graphics_jni` set is
  `libhwui`-coupled, so we write a thin subset rather than extract it), compiled with the **device**
  `@CriticalNative` convention (correct signatures to *match* the annotation — not clearing it, no
  AOSP patch, no libhwui rebuild, no runtime flag — verified there's no ART option to disable
  `@CriticalNative`). acl shares one `VkDevice` with it (via `GrVkBackendContext`); the natives wrap
  acl's dma-buf `VkImage` as the `Canvas`'s `SkSurface` (`GrBackendTexture` /
  `SkSurface::MakeFromBackendTexture`) and render straight in. ONE Skia in the render path (hidden in
  the Soong lib → no ODR). **Gate:** a real `android.graphics.Canvas` draws on host ART into acl's
  dma-buf VkImage.
  **S3 linchpin DE-RISKED (2026-07-12):** a probe Soong module (`vendor/acl/skia_probe/`,
  cc_binary_host) renders a bg+rect into an `SkBitmap` and reads pixels back — **builds + runs on host,
  Android's Skia renders correctly** (`bg=0xff204060 fg=0xffcc8844`). The libskia host **dep closure**
  is now known exactly (`static`: libskia, libwebp-decode/encode, libsfntly, libwuffs_mirror_release_c;
  `shared`: libpng, libjpeg, libz, libexpat, libft2, libicuuc, libicui18n, libharfbuzz_ng, libdng_sdk,
  libpiex, liblog, libbase, libcutils — `static_libs:["libskia"]` does NOT auto-pull them). So the
  Skia-on-host linking is solved; the remaining S3 work is the natives themselves.
  **S3 DONE (2026-07-12):** `libacl_graphics_jni` (Soong `cc_library_shared` host,
  `tools/aosp/graphics_jni/` → `$AOSP/vendor/acl/graphics_jni/`) implements acl's minimal
  `android.graphics` natives (Bitmap/Canvas/BaseCanvas/Paint/ColorSpace$Rgb) over core Skia, the
  `@CriticalNative` methods in the **device convention** (handle-only) to match the annotation host
  ART honors. acl (`ArtVm::load_acl_graphics`, gated `ACL_ART_GRAPHICS_LIB`) `dlopen`s it and calls
  `register_acl_graphics`. **Verified: real `Bitmap.createBitmap(16,10,ARGB_8888)` → `new Canvas` →
  `drawColor` → `new Paint().setColor` → `drawRect` → `getPixel` all execute on host ART, rendered by
  Android's host Skia, pixels correct (`bg=0xff204060 fg=0xffcc8844`)** — no `@CriticalNative`
  clearing, no AOSP patch, no libhwui rebuild, no container. ~20 natives; the register/ABI mechanism
  is proven, so the rest is filling in ops.
- **S3b DONE (2026-07-12) — THE LOOP IS CLOSED, LIVE.** `gfx::VkRenderTarget::import_pixels_to_dmabuf`
  (shared `ensure_linear_export`) writes CPU pixels into the linear dma-buf; `+ acl_read_bitmap` in
  libacl_graphics_jni copies a rendered `Bitmap`'s pixels; `ArtVm::graphics_present_demo` (gated
  `ACL_ART_GRAPHICS_DEMO`) drives a real render via JNI (`createBitmap`→`Canvas`→`drawColor`→`Paint.
  setColor`→`drawRect`), reads the pixels via `acl_read_bitmap`, imports to the dma-buf, and presents
  via `present_dmabuf`. **Verified LIVE against GNOME Shell/Mutter: `app → android.graphics → Android's
  Skia → dma-buf → Wayland`, 3 clean frames.** Unit `[import]` covers the CPU→dma-buf write (163
  assertions); acl suite green (demo opt-in).
### S4 — two tracks (FINDING 2026-07-12)
**Android's host `libskia` is CPU-only** — zero GPU backend (`nm` on the host `libskia.a`: no `GrGL*`,
no `GrVk*`, no `MakeVulkan`/`MakeGL`; Vulkan/`GrVk*`/`libvulkan` are gated to `target: { android }`
in `external/skia/Android.bp`; the host build never needed GPU for layoutlib). Since the app's
`android.graphics.Canvas` must render through *Android's* Skia (type-compatible with the framework
natives), **the S3b CPU-render-then-upload loop is not a shortcut — it's the only path today, and it
is correct + working.** GPU-native therefore forks:
- **S4a — GPU-native (perf).** **Step 1 DONE (2026-07-12):** Android's host `libskia` now builds with
  the Ganesh Vulkan backend (`tools/aosp/patches/external-skia-host-vulkan.patch`: host
  `SkUserConfig.h` `SK_SUPPORT_GPU 1`+`SK_VULKAN`+`SK_USE_VMA`; the ~287-file GPU src set + VMA +
  Vulkan headers added to `libskia`'s `host_linux` block, GL excluded → Vulkan-only). Verified:
  `GrDirectContext::MakeVulkan` defined, 578 `GrVk*` symbols; `libacl_graphics_jni` rebuilds clean.
  Skia calls Vulkan via consumer-provided `GrVkGetProc` (no vk loader linked). **Step 2 foundation
  DONE (2026-07-12):** the `acl_ganesh_probe` (`tools/aosp/ganesh_probe/`, cc_binary_host) creates a
  Vulkan device, `GrDirectContext::MakeVulkan` (Android Skia), an offscreen GPU `SkSurface`
  (`MakeRenderTarget`), renders + reads back — **verified on the real GPU (RADV):
  bg=0xff204060 fg=0xffcc8844.** All vk entrypoints come via `dlopen`(libvulkan)+`GrVkGetProc` (Soong
  can't `-lvulkan` on host; the real lib gets them from acl instead). So Android's Skia does Ganesh
  Vulkan on host+GPU. **Step 2 remaining (wiring):** make a modifier-tiled dma-buf-exportable `VkImage`,
  wrap it as an Android-Skia Ganesh `SkSurface` (`GrBackendTexture`+`GrVkImageInfo`), route the
  `Canvas`'s `nInitRaster` to it, render + export fd + present via `present_dmabuf` — no CPU upload.
  A perf optimization over the working S3b loop, not a correctness fix.
  **STEP 2 SCAFFOLD + BLOCKER FOUND (2026-07-12):** `tools/aosp/graphics_jni/gpu_dmabuf.cpp` (in
  `libacl_graphics_jni`) is the self-contained GPU renderer — Vulkan device via
  `dlopen`(libvulkan)+`GrVkGetProc` (no `-lvulkan`), modifier selection, a modifier-tiled
  dma-buf-exported `VkImage`+fd, and `GrDirectContext::MakeVulkan` — **all verified working** (AMD
  tiled modifier, fd exported). `acl_graphics_gpu_render()` is wired into `graphics_present_demo`
  (gated `ACL_ART_GRAPHICS_GPU`) → `present_dmabuf`. **BLOCKER:** android-13 Ganesh's
  `SkSurface::MakeFromBackendTexture` **rejects a `DRM_FORMAT_MODIFIER` dma-buf image as a render
  target** (tiled AND linear — its `GrVkCaps` lacks the modifier RT support m150 has). So direct GPU
  render into the dma-buf isn't possible on android-13 Skia. **FIX (next):** the gfx_vk copy pattern —
  render into a Skia-owned OPTIMAL `SkSurface` (`MakeRenderTarget`, works, proven by
  `acl_ganesh_probe`) → `vkCmdCopyImage` into the dma-buf `VkImage` (GPU→GPU, no CPU) → export +
  present. ~80 lines; everything else is done. (Alternatively S4b breadth, which needs no GPU path.)
- **S4b — breadth (runs real apps, into Gate G2).** No Skia rebuild: grow graphics ops
  (paths/text/drawBitmap/clip/transform) + Paint setters as real apps hit them, and drive the loop
  from a real `View.draw`/`onDraw` traversal (acl already has the View-traversal driver from Phase 2).
  Then hwui's DisplayList/RenderThread/damage design for device-class behavior.

The reusable probe `tests/fixtures/art_graphics/Main.java` (real `Bitmap`/`Canvas`/`Paint` via
reflection) validates whichever backend once S3 lands.

### V4 — View → frame → present (end-to-end)
A real (or minimal) `View.draw(Canvas)` → the gfx Ganesh surface → dma-buf present, driven by a
Choreographer mapped onto `wp_presentation` frame callbacks (plan.md threading model). **Gate:**
an app's `View` hierarchy draws one frame that reaches the compositor. This is Gate G2 territory
(windowed Java app) — V4 is the bridge from P2.4 into G2.

## Fallbacks & risks
- **No Vulkan device in CI:** V1/V2 SKIP (exit 77), like the ART tests. llvmpipe covers the
  local no-GPU case for render correctness (not scanout).
- **egl-canvas alternative:** if the Vulkan dma-buf path stalls on a compositor/modifier issue,
  `skia-egl-canvas` (Ganesh GL) is the drop-in alternative backend behind the same gfx seam —
  same pinned Skia (`skia_use_gl=true`), same `SkCanvas` interface, EGL/GBM instead of Vulkan.
- **Explicit sync** (`wp_linux_drm_syncobj_v1`) is optional (CPU-fence fallback), gated on the
  host wayland-protocols shipping it — mirror the example's `HAVE_DRM_SYNCOBJ` gate.
- **Skia has no stable API** (A02.2): pin every Ganesh/`skgpu::` header use to the m150 build;
  re-pin only via ADR alongside the `frameworks/base` tag.

---

## V1 RESULT (2026-07-12) — done; real GPU render on host Ganesh Vulkan
`gfx/gfx_vk.{h,cpp}` is the headless Ganesh Vulkan render target, InitVulkan patterned after
the wcs example: Vulkan 1.1 instance, physical-device pick preferring a discrete/integrated GPU
over llvmpipe, graphics queue, `skgpu::VulkanMemoryAllocators::Make`, `GrDirectContexts::Make
Vulkan`, and an offscreen premul-`kBGRA_8888` GPU `SkSurface` (`SkSurfaces::RenderTarget`). It
renders through a real `SkCanvas` (clear + rect), `flushAndSubmit(kYes)`, and `readPixels` back
into a `gfx::Surface`. Built only when the pinned Skia's Vulkan is present + system vulkan-hpp
(`ACL_HAVE_VULKAN`); vulkan-hpp is used with `VULKAN_HPP_NO_EXCEPTIONS` (`.result`/`.value` API,
as the example does), and the bundled VMA + `ThreadSafe` come from the m150 `src/` private
headers. **Verified: an AA-off fill+rect rendered on the real GPU (`AMD … RADV RAPHAEL`) reads
back bit-identical to the software rasterizer (161 assertions).** The Tier A test SKIPs cleanly
(WARN + SUCCEED) where `VkRenderTarget::Create` returns null (no device), so GPU-less CI passes.
libdrm is deliberately NOT a V1 dependency — it enters at V2 with the dma-buf export.

## V2a RESULT (2026-07-12) — done; rendered frame exports as a verified linear dma-buf
`gfx_vk` gained the dma-buf export path (patterned after the example's linear copy-fallback):
device creation now enables `VK_KHR_external_memory_fd` + `VK_EXT_external_memory_dma_buf` when
offered (`dmabuf_export_available()`), and `render_and_export_linear()` renders the scene, flushes
the optimal Skia surface into `TRANSFER_SRC_OPTIMAL` (`skgpu::MutableTextureStates::MakeVulkan`),
`vkCmdCopyImage`s it into a **linear, dma-buf-exported** `VkImage`, and exports the fd via
`vkGetMemoryFdKHR` — handing back a `DmabufFrame` {fd, stride, offset, `MOD_LINEAR`,
`DRM_FORMAT_XRGB8888`, mem_size}. The export memory is allocated host-visible+coherent when
available so the frame is mmap-verifiable. libdrm joins the deps (`drm_fourcc.h` fourcc/modifier
constants). **Verified headless on the real GPU (RADV): the exported plane layout is sane and the
mmap'd dma-buf holds exactly the software-rasterizer pixels (167 assertions).** Caught + fixed a
real bug: `SkSurfaces::RenderTarget` defaults to `kBottomLeft` origin, so a raw `vkCmdCopyImage`
into the dma-buf came out vertically flipped — the render target is now `kTopLeft` (top-down
storage, which is also what Wayland surfaces expect). Test SKIPs cleanly with no device / no
export extensions.

## V2b-gfx RESULT (2026-07-12) — done; modifier-tiled DIRECT (zero-copy) path on real GPU
The gfx side of V2b: the **modifier-tiled direct path** (the example's zero-copy headline, now
the primary). `render_to_modifier_dmabuf(candidate_modifiers, …)` intersects the compositor's
advertised modifiers with the GPU's render+sampleable ones (`getFormatProperties2` +
`DrmFormatModifierPropertiesListEXT`, needing `eColorAttachment | eSampledImage`), allocates a
`eDrmFormatModifierEXT`-tiled, dma-buf-exported `VkImage` (per-plane offset/stride via
`eMemoryPlaneNEXT` — a modifier may span planes), wraps it as a Ganesh `SkSurface`
(`WrapBackendTexture`), and Skia renders **straight in — no copy**, leaving it in
`VK_IMAGE_LAYOUT_GENERAL` for the compositor. Device creation now also enables
`VK_EXT_image_drm_format_modifier` + `VK_KHR_image_format_list` (`modifier_tiled_available()`).
`DmabufFrame` became multi-plane. **Verified on the real GPU (RADV): the direct path chose an
AMD tiled modifier (`0x0200…`, vendor 0x02 — not LINEAR), and the Skia-detiled readback of the
tiled surface matches the software rasterizer exactly (166 assertions).** Pixels of a tiled
image aren't CPU-linear, so verification reads back through Skia (the GPU detiles) rather than
mmap. Test SKIPs cleanly without the modifier extension.

## V2b-wm RESULT (2026-07-12) — done; live modifier-tiled dma-buf present to GNOME Shell
`wm/dmabuf_presenter.cpp` — a focused presenter (kept separate from the mature shm
`wayland_presenter.cpp`): binds `zwp_linux_dmabuf_v1` via the wcs `wl::DmabufFeedback`, learns the
compositor's advertised modifiers (`Current().ModifiersFor(XRGB8888)`), calls the producer's
`DmabufRenderFn` to render a frame targeting one of them, builds a `wl_buffer` (`create_params` →
one `add()` per plane → `create_immed`), and presents it under an xdg_toplevel. `present_dmabuf()`
+ `DmabufRenderFn` are the new `present.h` seam (guarded `ACL_HAVE_WCS && ACL_HAVE_VULKAN`); wm/
needs no Vulkan headers (the `DmabufFrame` is a plain struct; acl_gfx is linked by the consumer).
Generated `linux_dmabuf_client.hpp` (NO `--emit-interface-tables` — the `wl/linux_dmabuf.hpp`
framework emits the interface tables, like xdg-shell). `wm/dmabuf_probe_main.cpp` (`wm_dmabuf_probe`)
drives it with a VkRenderTarget. **Verified LIVE against the running GNOME Shell/Mutter
(`wayland-0`): the compositor advertised 6 XRGB8888 modifiers; the direct path chose an AMD tiled
modifier `0x0200000000056bb03` — 2 planes (AMD DCC metadata, both described via per-plane `add()`);
5 frames, 5 frame-callbacks, clean.** So the whole GPU path is live end-to-end: Ganesh Vulkan render
→ modifier-tiled dma-buf → zwp_linux_dmabuf_v1 → real compositor composites it. Per the steer, the
system compositor is the first target; the probe is run manually (no `meson test` yet — a live
compositor isn't in CI). **NEXT = V2b-ci**: a libwayland-server test compositor (extending
`close_compositor`) that advertises a modifier set + asserts the received plane params, for
deterministic CI coverage. Then V3 (android.graphics natives compose onto the gfx surface).
