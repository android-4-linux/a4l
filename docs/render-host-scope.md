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

**V2b — Wayland present (wm/).** Build `wl_buffer`s from the `DmabufFrame` via
`zwp_linux_buffer_params` and present through the wcs `wl::LinuxDmabuf`/`wl::DmabufFeedback`
bindings driven from `wm/`, then the modifier-tiled direct path. **Gate:** the Tier-C
headless-compositor harness (wlheadless, as the existing present-loop test uses) shows an
acl-rendered frame committed via dma-buf.

### V3 — android.graphics natives compose onto the gfx surface
Register the host-exported graphics native floor (`register_android_graphics_classes` /
`_Bitmap` / `_Canvas` / `_Graphics` — the same LayoutlibLoader host block that exported the
resources floor in R1) in ArtVm, and route a real `android.graphics.Canvas`'s draws onto the
gfx Ganesh surface (or, minimally, copy a rendered `Bitmap`'s pixels into it). **Gate:** a real
framework `Bitmap`/`Canvas` op executes on host ART and its pixels reach the gfx surface.

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
export extensions. **V2b (present the DmabufFrame via wcs `wl::LinuxDmabuf` from wm/) is next.**
