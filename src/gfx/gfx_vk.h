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
