// SPDX-License-Identifier: Apache-2.0
// Provenance: wcs example skia-vulkan-dmabuf (Apache-2.0/MIT, InitVulkan
// pattern), Skia public API (include/gpu/ganesh/vk, include/gpu/vk), ADR-4
// (single pinned Skia, hwui dropped), plan.md §2. No GPL-3.0 sources.
//
// The Ganesh Vulkan render target (P2.4 milestone V1): the GPU-backed
// replacement for hwui's SkiaVulkan pipeline (hwui dropped). Owns a Vulkan
// instance/device/queue
// + Skia GrDirectContext, and renders through a real SkCanvas into an offscreen
// GPU SkSurface whose pixels read back into a gfx::Surface. V2 turns the render
// target into a dma-buf-exported VkImage presented via zwp_linux_dmabuf_v1
// (wm/). Compiled only when the pinned Skia has Vulkan (ACL_HAVE_VULKAN).

#ifndef ACL_GFX_GFX_VK_H
#define ACL_GFX_GFX_VK_H

#include "acl_config.h"

#if ACL_HAVE_VULKAN

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gfx/dmabuf_frame.h"  // gfx::DmabufFrame (the present interchange, shared)
#include "gfx/gfx.h"

namespace acl::gfx {

// A headless Ganesh Vulkan context + offscreen render target. Construct with
// VkRenderTarget::Create (returns nullptr and sets error() when no usable
// Vulkan device / Ganesh context is available — callers SKIP, they don't fail).
class VkRenderTarget {
 public:
  ~VkRenderTarget();
  VkRenderTarget(const VkRenderTarget&) = delete;
  VkRenderTarget& operator=(const VkRenderTarget&) = delete;

  // Create an offscreen ARGB_8888 GPU render target of the given size. On
  // failure returns nullptr; *error carries the reason (device absent vs real
  // failure).
  static std::unique_ptr<VkRenderTarget> Create(int width,
                                                int height,
                                                std::string* error);

  int width() const { return width_; }
  int height() const { return height_; }

  // Selected physical device name (for logging / test diagnostics).
  const std::string& device_name() const { return device_name_; }

  // Render a background clear + one filled rect on the GPU (Ganesh Vulkan),
  // then flush+submit and read the pixels back into `out` (ARGB_8888). This is
  // the V1 scene stand-in and the example's --screenshot verification path in
  // miniature. Returns false on a GPU submit / readback failure.
  bool render_rect(Surface& out,
                   std::uint32_t bg_argb,
                   int left,
                   int top,
                   int right,
                   int bottom,
                   std::uint32_t rect_argb);

  // True when the device offers the dma-buf export extensions (VK_KHR_external_
  // memory_fd + VK_EXT_external_memory_dma_buf) — i.e. export_linear_dmabuf
  // works.
  bool dmabuf_export_available() const;

  // Render the same scene, then hand the frame over as a Linux dma-buf
  // (P2.4/V2): Skia flushes the optimal surface into TRANSFER_SRC_OPTIMAL, a
  // copy pass blits it into a linear dma-buf-exported VkImage, and the exported
  // fd + plane layout fill *out. Returns false (and sets *error) on failure or
  // when export is unavailable. The fd is owned by this target.
  bool render_and_export_linear(std::uint32_t bg_argb,
                                int left,
                                int top,
                                int right,
                                int bottom,
                                std::uint32_t rect_argb,
                                DmabufFrame* out,
                                std::string* error);

  // Read the exported linear image's pixels back into `out` by mapping its
  // host-visible memory (respecting the row stride). Only valid after a
  // successful render_and_export_linear whose frame reported host_mapped; false
  // otherwise. This is the headless verification that the dma-buf holds the
  // rendered pixels.
  bool read_exported_pixels(Surface& out);

  // S3b (P2.4): import CPU pixels (a real android.graphics Bitmap rendered by
  // Android's Skia → gfx::Surface) into the linear dma-buf, ready to present
  // via zwp_linux_dmabuf_v1. Writes the export image's host-visible memory
  // (respecting stride); fills *out. Closes the render loop today; the
  // GPU-native zero-copy path (Ganesh into the dma-buf) is S4.
  bool import_pixels_to_dmabuf(const Surface& src,
                               DmabufFrame* out,
                               std::string* error);

  // True when the device offers the DRM-format-modifier extensions
  // (VK_EXT_image_drm_format_modifier + VK_KHR_image_format_list) — i.e. the
  // modifier-tiled direct path is possible.
  bool modifier_tiled_available() const;

  // Of `candidate_modifiers` (what the compositor advertised for the frame's
  // format), the GPU-renderable+sampleable ones, in the given preference order
  // — the intersection the direct path picks from. Empty ⇒ fall back to
  // render_and_export_linear.
  std::vector<std::uint64_t> renderable_modifiers(
      const std::vector<std::uint64_t>& candidate_modifiers) const;

  // Every DRM modifier the GPU can render+sample for the frame's format (LINEAR
  // moved last so a tiled one is preferred). A stand-in for a
  // maximally-permissive compositor feedback set; the wm/ presenter passes the
  // real advertised set instead.
  std::vector<std::uint64_t> all_renderable_modifiers() const;

  // Modifier-tiled DIRECT path (P2.4/V2b, the zero-copy present): pick the
  // first of `candidate_modifiers` the GPU can render+sample, allocate a
  // modifier-tiled, dma-buf-exported VkImage, wrap it as a Ganesh SkSurface,
  // and render the scene STRAIGHT into it (no copy). Fills *out (multi-plane)
  // with the exported fd + per- plane layout. Returns false (sets *error) when
  // no candidate qualifies or on GPU failure — the caller then uses the linear
  // fallback. The fd is owned by this target.
  bool render_to_modifier_dmabuf(
      const std::vector<std::uint64_t>& candidate_modifiers,
      std::uint32_t bg_argb,
      int left,
      int top,
      int right,
      int bottom,
      std::uint32_t rect_argb,
      DmabufFrame* out,
      std::string* error);

  // Read the pixels back from the surface last rendered (linear-copy source or
  // the modifier-tiled wrapped surface) via Skia's readPixels — the GPU detiles
  // a tiled surface, so this verifies modifier-tiled pixels headless (no CPU
  // mmap needed).
  bool read_rendered_pixels(Surface& out);

 private:
  VkRenderTarget();

  // Build the linear dma-buf-exported image + fd once (shared by the GPU-copy
  // and CPU-import paths).
  bool ensure_linear_export(std::string* error);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  int width_ = 0;
  int height_ = 0;
  std::string device_name_;
};

}  // namespace acl::gfx

#endif  // ACL_HAVE_VULKAN
#endif  // ACL_GFX_GFX_VK_H
