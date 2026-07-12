// SPDX-License-Identifier: Apache-2.0
// Provenance: wcs example skia-vulkan-dmabuf (Apache-2.0/MIT, InitVulkan pattern),
// Skia public API (include/gpu/ganesh/vk, include/gpu/vk), ADR-4 (single pinned
// Skia, hwui dropped), plan.md §2. No GPL-3.0 sources.
//
// The Ganesh Vulkan render target (P2.4 milestone V1): the GPU-backed replacement
// for hwui's SkiaVulkan pipeline (hwui dropped). Owns a Vulkan instance/device/queue
// + Skia GrDirectContext, and renders through a real SkCanvas into an offscreen GPU
// SkSurface whose pixels read back into a gfx::Surface. V2 turns the render target
// into a dma-buf-exported VkImage presented via zwp_linux_dmabuf_v1 (wm/). Compiled
// only when the pinned Skia has Vulkan (ACL_HAVE_VULKAN).

#ifndef ACL_GFX_GFX_VK_H
#define ACL_GFX_GFX_VK_H

#include "acl_config.h"

#if ACL_HAVE_VULKAN

#include <memory>
#include <string>

#include "gfx/gfx.h"

namespace acl::gfx {

// A rendered frame exported as a Linux dma-buf (P2.4/V2): the handle + plane layout
// a Wayland compositor imports via zwp_linux_dmabuf_v1 (zero-copy present). The fd is
// owned by the VkRenderTarget that produced it (closed on its destruction); the
// presenter (wm/) borrows it while the target is alive. V2 baseline is a single-plane
// DRM_FORMAT_MOD_LINEAR image; the modifier-tiled direct path is a later refinement.
struct DmabufFrame {
  int fd = -1;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;   // row pitch in bytes
  std::uint32_t offset = 0;   // plane 0 byte offset
  std::uint64_t modifier = 0; // DRM format modifier (LINEAR for the V2 baseline)
  std::uint32_t drm_format = 0;  // DRM_FORMAT_* fourcc
  std::uint64_t mem_size = 0;
  bool host_mapped = false;   // export memory is host-visible (mmap/verifiable)
};

// A headless Ganesh Vulkan context + offscreen render target. Construct with
// VkRenderTarget::Create (returns nullptr and sets error() when no usable Vulkan
// device / Ganesh context is available — callers SKIP, they don't fail).
class VkRenderTarget {
 public:
  ~VkRenderTarget();
  VkRenderTarget(const VkRenderTarget&) = delete;
  VkRenderTarget& operator=(const VkRenderTarget&) = delete;

  // Create an offscreen ARGB_8888 GPU render target of the given size. On failure
  // returns nullptr; *error carries the reason (device absent vs real failure).
  static std::unique_ptr<VkRenderTarget> Create(int width, int height,
                                                 std::string* error);

  int width() const { return width_; }
  int height() const { return height_; }

  // Selected physical device name (for logging / test diagnostics).
  const std::string& device_name() const { return device_name_; }

  // Render a background clear + one filled rect on the GPU (Ganesh Vulkan), then
  // flush+submit and read the pixels back into `out` (ARGB_8888). This is the V1
  // scene stand-in and the example's --screenshot verification path in miniature.
  // Returns false on a GPU submit / readback failure.
  bool render_rect(Surface& out, std::uint32_t bg_argb, int left, int top,
                   int right, int bottom, std::uint32_t rect_argb);

  // True when the device offers the dma-buf export extensions (VK_KHR_external_
  // memory_fd + VK_EXT_external_memory_dma_buf) — i.e. export_linear_dmabuf works.
  bool dmabuf_export_available() const;

  // Render the same scene, then hand the frame over as a Linux dma-buf (P2.4/V2):
  // Skia flushes the optimal surface into TRANSFER_SRC_OPTIMAL, a copy pass blits it
  // into a linear dma-buf-exported VkImage, and the exported fd + plane layout fill
  // *out. Returns false (and sets *error) on failure or when export is unavailable.
  // The fd is owned by this target.
  bool render_and_export_linear(std::uint32_t bg_argb, int left, int top, int right,
                                int bottom, std::uint32_t rect_argb, DmabufFrame* out,
                                std::string* error);

  // Read the exported linear image's pixels back into `out` by mapping its
  // host-visible memory (respecting the row stride). Only valid after a successful
  // render_and_export_linear whose frame reported host_mapped; false otherwise. This
  // is the headless verification that the dma-buf holds the rendered pixels.
  bool read_exported_pixels(Surface& out);

 private:
  VkRenderTarget();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  int width_ = 0;
  int height_ = 0;
  std::string device_name_;
};

}  // namespace acl::gfx

#endif  // ACL_HAVE_VULKAN
#endif  // ACL_GFX_GFX_VK_H
