// SPDX-License-Identifier: Apache-2.0
// Provenance: Linux dma-buf / zwp_linux_dmabuf_v1 plane layout; libdrm
// drm_fourcc.h (DRM_FORMAT_* / DRM_FORMAT_MOD_*). The present interchange
// shared by every GPU producer (Ganesh Vulkan in gfx_vk.h, host GLES in
// gl_egl.h) and the wm/ presenter. Deliberately free of any Vulkan/GL/EGL
// include so both producers and the presenter share one definition regardless
// of which backends are compiled. No GPL-3.0 sources.

#ifndef ACL_GFX_DMABUF_FRAME_H
#define ACL_GFX_DMABUF_FRAME_H

#include <cstdint>
#include <memory>

namespace acl::gfx {

// A rendered frame exported as a Linux dma-buf (P2.4/V2): the handle + plane
// layout a Wayland compositor imports via zwp_linux_dmabuf_v1 (zero-copy
// present). The fd is owned by the producer that made it (a VkRenderTarget or a
// GlWindow); the presenter (wm/) borrows it while the producer keeps it alive.
// Producers: the linear copy-fallback (single plane, DRM_FORMAT_MOD_LINEAR),
// the modifier-tiled Vulkan direct path, and the gbm/GLES window (gbm_bo front
// buffer).
struct DmabufFrame {
  static constexpr int kMaxPlanes = 4;
  struct Plane {
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;  // row pitch in bytes
  };
  int fd = -1;  // all planes share this fd at distinct offsets
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t plane_count = 1;
  Plane planes[kMaxPlanes]{};
  std::uint64_t modifier = 0;    // DRM format modifier (0 == LINEAR)
  std::uint32_t drm_format = 0;  // DRM_FORMAT_* fourcc
  std::uint64_t mem_size = 0;
  bool host_mapped = false;  // export memory is host-visible (mmap/verifiable)
  bool modifier_tiled =
      false;  // true = direct/tiled path, false = linear copy-fallback
  // Which of the producer's buffers this is, when the producer keeps a small
  // fixed set and reuses them (a gbm window in hold mode): a stable index,
  // the same every time this buffer comes round. -1 if the producer does not
  // say.
  int buffer_index = -1;
  // What keeps the memory behind `fd` from being reused while this frame is
  // in use, when the fd alone does not: a hardware decoder's surface goes
  // back to the decoder's pool when its frame is dropped, and the next
  // picture is decoded into it, whoever still holds a dup of the fd. Copied
  // with the frame, so that whoever keeps the frame (a queued output buffer,
  // an imported AHardwareBuffer, a wl_buffer the compositor has not
  // released) keeps the surface. Empty for a producer that owns its buffers.
  std::shared_ptr<void> hold;

  std::uint32_t stride() const {
    return planes[0].stride;
  }  // plane-0 convenience
  std::uint32_t offset() const { return planes[0].offset; }
};

}  // namespace acl::gfx

#endif  // ACL_GFX_DMABUF_FRAME_H
