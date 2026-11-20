// SPDX-License-Identifier: Apache-2.0
// Provenance: Linux dma-buf / zwp_linux_dmabuf_v1 plane layout; libdrm
// drm_fourcc.h (DRM_FORMAT_* / DRM_FORMAT_MOD_*). The present interchange
// shared by every GPU producer (Ganesh Vulkan in gfx_vk.h, host GLES in
// gl_egl.h) and the wm/ presenter. Deliberately free of any Vulkan/GL/EGL
// include so both producers and the presenter share one definition regardless
// of which backends are compiled. No GPL-3.0 sources.

#ifndef ACL_GFX_DMABUF_FRAME_H
#define ACL_GFX_DMABUF_FRAME_H

#include <fcntl.h>
#include <unistd.h>

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
    // This plane's own dma-buf, where the producer keeps planes in separate
    // buffers (a decoder with luma in one and chroma in another). -1, as
    // every producer but such a decoder leaves it: the plane is in `fd`.
    int fd = -1;
  };
  // The dma-buf of plane 0, and of every plane that does not name its own.
  int fd = -1;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t plane_count = 1;
  Plane planes[kMaxPlanes]{};
  std::uint64_t modifier = 0;    // DRM format modifier (0 == LINEAR)
  std::uint32_t drm_format = 0;  // DRM_FORMAT_* fourcc
  // For a YUV picture, how it turns into RGB: the stream's matrix
  // coefficients as H.273 numbers them (1 BT.709, 5 and 6 BT.601, 9 BT.2020;
  // 2 when the stream does not say), and whether its samples use the full
  // range or the narrow one. What a consumer does with "does not say" is
  // video_color()'s to decide, in one place.
  std::uint8_t yuv_matrix = 2;
  bool yuv_full_range = false;
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
  // The dma-buf plane `i` is in.
  int plane_fd(std::uint32_t i) const {
    return i < kMaxPlanes && planes[i].fd >= 0 ? planes[i].fd : fd;
  }
  // True if every plane is in `fd`.
  bool one_buffer() const {
    for (std::uint32_t i = 0; i < plane_count && i < kMaxPlanes; ++i) {
      if (planes[i].fd >= 0 && planes[i].fd != fd)
        return false;
    }
    return true;
  }
};

// Closes the frame's descriptors, for a holder that owns them (dup_frame).
inline void close_frame(DmabufFrame* f) {
  for (DmabufFrame::Plane& p : f->planes) {
    if (p.fd >= 0 && p.fd != f->fd)
      ::close(p.fd);
    p.fd = -1;
  }
  if (f->fd >= 0)
    ::close(f->fd);
  f->fd = -1;
}

// A copy of `src` that owns its descriptors: the same memory, no pixel copy,
// and a lifetime of its own. False, with nothing left open, if a descriptor
// is invalid or cannot be duplicated.
inline bool dup_frame(const DmabufFrame& src, DmabufFrame* out) {
  DmabufFrame f = src;
  f.fd = -1;
  for (DmabufFrame::Plane& p : f.planes)
    p.fd = -1;
  bool ok = src.fd >= 0;
  if (ok) {
    f.fd = ::fcntl(src.fd, F_DUPFD_CLOEXEC, 0);
    ok = f.fd >= 0;
  }
  for (std::uint32_t i = 0;
       ok && i < src.plane_count && i < DmabufFrame::kMaxPlanes; ++i) {
    const int own = src.planes[i].fd;
    if (own < 0 || own == src.fd)
      continue;
    f.planes[i].fd = ::fcntl(own, F_DUPFD_CLOEXEC, 0);
    ok = f.planes[i].fd >= 0;
  }
  if (!ok) {
    close_frame(&f);
    return false;
  }
  *out = std::move(f);
  return true;
}

// The matrix a YUV picture is converted with.
enum class YuvMatrix : std::uint8_t { Bt601, Bt709, Bt2020 };

struct VideoColor {
  YuvMatrix matrix = YuvMatrix::Bt601;
  bool full_range = false;
};

// What `frame` is converted to RGB with. A stream that names one of the three
// matrices gets it. One that does not, or names another, is taken the way
// Android takes it: BT.601 up to 720x576, BT.709 above. A compositor left to
// itself picks either, and two of them picked differently for the same frame.
inline VideoColor video_color(const DmabufFrame& frame) {
  VideoColor c;
  c.full_range = frame.yuv_full_range;
  switch (frame.yuv_matrix) {
    case 1:
      c.matrix = YuvMatrix::Bt709;
      break;
    case 5:
    case 6:
      c.matrix = YuvMatrix::Bt601;
      break;
    case 9:
      c.matrix = YuvMatrix::Bt2020;
      break;
    default:
      c.matrix = frame.width > 720 || frame.height > 576 ? YuvMatrix::Bt709
                                                         : YuvMatrix::Bt601;
  }
  return c;
}

}  // namespace acl::gfx

#endif  // ACL_GFX_DMABUF_FRAME_H
