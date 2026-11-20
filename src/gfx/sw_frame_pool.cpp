// SPDX-License-Identifier: Apache-2.0
// Provenance: gbm public API (gbm_bo_create, gbm_bo_map, gbm_bo_get_*), Linux
// dma-buf uapi (linux/dma-buf.h, DMA_BUF_IOCTL_SYNC), libdrm drm_fourcc.h.
// No GPL-3.0 sources.

#include "gfx/sw_frame_pool.h"

#if ACL_HAVE_GL

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstring>

#include <drm_fourcc.h>
#include <gbm.h>

#include "gfx/hardware_buffer.h"
#include "log/log.h"

namespace acl::gfx {
namespace {
constexpr const char* kTag = "gfx.swpool";

// 16-bit little-endian samples, as P010 and a software decoder's 10-bit
// planes keep them. A decoder's rows are not promised to be aligned for 16-bit
// loads, and a compiler will not vectorize a loop of unaligned 16-bit loads
// through a byte pointer (GCC 14: "relevant stmt not supported"), which left
// it moving one sample at a time. So a row goes in chunks: copied as bytes
// into a small array of uint16_t, shifted there, where the loop does
// vectorize, and copied out. On a big-endian host the bytes are swapped on
// the way.
constexpr bool kLittle = std::endian::native == std::endian::little;
constexpr std::uint32_t kChunk = 256;  // samples: 512 bytes on the stack

std::uint16_t le16(std::uint16_t v) {
  if constexpr (kLittle)
    return v;
  else
    return static_cast<std::uint16_t>((v >> 8) | (v << 8));
}

// `n` samples from `src` to `dst`, each moved up by `shift` bits.
void shift_row(std::uint8_t* dst,
               const std::uint8_t* src,
               std::uint32_t n,
               unsigned shift) {
  if (shift == 0) {
    std::memcpy(dst, src, 2 * static_cast<std::size_t>(n));
    return;
  }
  std::uint16_t s[kChunk];
  for (std::uint32_t at = 0; at < n; at += kChunk) {
    const std::uint32_t k = std::min(kChunk, n - at);
    std::memcpy(s, src + 2 * static_cast<std::size_t>(at), 2 * k);
    for (std::uint32_t i = 0; i < k; ++i)
      s[i] = le16(static_cast<std::uint16_t>(le16(s[i]) << shift));
    std::memcpy(dst + 2 * static_cast<std::size_t>(at), s, 2 * k);
  }
}

// `n` Cb samples from `u` and `n` Cr samples from `v`, interleaved into
// `dst`, each moved up by `shift` bits.
void interleave_row(std::uint8_t* dst,
                    const std::uint8_t* u,
                    const std::uint8_t* v,
                    std::uint32_t n,
                    unsigned shift) {
  std::uint16_t a[kChunk], b[kChunk], pairs[2 * kChunk];
  for (std::uint32_t at = 0; at < n; at += kChunk) {
    const std::uint32_t k = std::min(kChunk, n - at);
    std::memcpy(a, u + 2 * static_cast<std::size_t>(at), 2 * k);
    std::memcpy(b, v + 2 * static_cast<std::size_t>(at), 2 * k);
    for (std::uint32_t i = 0; i < k; ++i) {
      pairs[2 * i] = le16(static_cast<std::uint16_t>(le16(a[i]) << shift));
      pairs[2 * i + 1] = le16(static_cast<std::uint16_t>(le16(b[i]) << shift));
    }
    std::memcpy(dst + 4 * static_cast<std::size_t>(at), pairs, 4 * k);
  }
}

// `n` Cb bytes from `u` and `n` Cr bytes from `v`, interleaved into `dst`.
// Through a local array as above: written straight to `dst`, which may alias
// `u` and `v` as far as the compiler knows, the loop ran a byte at a time.
void interleave_row8(std::uint8_t* dst,
                     const std::uint8_t* u,
                     const std::uint8_t* v,
                     std::uint32_t n) {
  std::uint8_t pairs[2 * kChunk];
  for (std::uint32_t at = 0; at < n; at += kChunk) {
    const std::uint32_t k = std::min(kChunk, n - at);
    const std::uint8_t* a = u + at;
    const std::uint8_t* b = v + at;
    for (std::uint32_t i = 0; i < k; ++i) {
      pairs[2 * i] = a[i];
      pairs[2 * i + 1] = b[i];
    }
    std::memcpy(dst + 2 * static_cast<std::size_t>(at), pairs, 2 * k);
  }
}

// Brackets CPU access to a mapped dma-buf, which is what makes the exporter
// flush caches for the device that reads the buffer next.
void dmabuf_sync(int fd, std::uint64_t flags) {
  struct dma_buf_sync s{};
  s.flags = flags;
  int r;
  do {
    r = ::ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
  } while (r < 0 && (errno == EINTR || errno == EAGAIN));
}
}  // namespace

struct SwFramePool::Buffer {
  gbm_bo* bo = nullptr;
  int fd = -1;
  void* map = nullptr;  // the whole dma-buf, when the exporter allows mmap
  std::size_t map_size = 0;
  DmabufFrame frame;
  bool held = false;  // guarded by the pool's mu_

  ~Buffer() {
    if (map != nullptr)
      ::munmap(map, map_size);
    if (fd >= 0)
      ::close(fd);
    if (bo != nullptr)
      gbm_bo_destroy(bo);
  }
};

std::shared_ptr<SwFramePool> SwFramePool::create(std::uint32_t width,
                                                 std::uint32_t height,
                                                 std::string* error,
                                                 bool deep) {
  if (width == 0 || height == 0 || width > 16384 || height > 16384) {
    if (error)
      *error = "bad frame size";
    return nullptr;
  }
  auto pool = std::shared_ptr<SwFramePool>(new SwFramePool());
  pool->gbm_dev_ = HardwareBuffer::shared_gbm_device(&pool->gbm_keep_, error);
  if (pool->gbm_dev_ == nullptr)
    return nullptr;
  pool->width_ = width;
  pool->height_ = height;
  pool->deep_ = deep;
  return pool;
}

SwFramePool::~SwFramePool() = default;

// One attempt at a buffer: `native` asks the allocator for NV12 (P010 in a
// deep pool), otherwise for linear R8 one and a half frames tall, which is
// the same memory (luma rows, then chroma rows) from an allocator without
// the format; in a deep pool that R8 buffer is twice as wide, a sample being
// two bytes.
std::unique_ptr<SwFramePool::Buffer> SwFramePool::allocate_as(
    bool native,
    std::string* error) {
  auto fail = [&](const char* m) -> std::unique_ptr<Buffer> {
    if (error)
      *error = m;
    return nullptr;
  };
  auto* dev = static_cast<gbm_device*>(gbm_dev_);
  // Chroma is subsampled by two each way, so the buffer is laid out for even
  // dimensions.
  const std::uint32_t w = (width_ + 1u) & ~1u;
  const std::uint32_t h = (height_ + 1u) & ~1u;
  const std::uint32_t row =
      deep_ ? 2 * w : w;  // bytes in a row of either plane
  auto b = std::make_unique<Buffer>();
  DmabufFrame& f = b->frame;
  f.width = width_;
  f.height = height_;
  f.drm_format = deep_ ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
  f.modifier = DRM_FORMAT_MOD_LINEAR;
  f.plane_count = 2;

  if (native) {
    // gbm's formats are the DRM fourccs; not every gbm.h names P010.
    b->bo = gbm_bo_create(dev, w, h, deep_ ? DRM_FORMAT_P010 : GBM_FORMAT_NV12,
                          GBM_BO_USE_LINEAR);
    if (b->bo == nullptr || gbm_bo_get_plane_count(b->bo) != 2 ||
        gbm_bo_get_modifier(b->bo) != DRM_FORMAT_MOD_LINEAR)
      return fail("no linear NV12 or P010 from gbm");
    for (int i = 0; i < 2; ++i) {
      f.planes[i].offset = gbm_bo_get_offset(b->bo, i);
      f.planes[i].stride = gbm_bo_get_stride_for_plane(b->bo, i);
    }
    if (f.planes[0].stride < row || f.planes[1].stride < row ||
        f.planes[1].offset < static_cast<std::uint64_t>(f.planes[0].stride) * h)
      return fail("gbm NV12 or P010 layout not understood");
  } else {
    b->bo =
        gbm_bo_create(dev, row, h + h / 2, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
    if (b->bo == nullptr)
      return fail("gbm_bo_create failed (NV12 or P010, and R8)");
    const std::uint32_t stride = gbm_bo_get_stride(b->bo);
    if (stride < row)
      return fail("gbm R8 stride below the frame width");
    f.planes[0].offset = 0;
    f.planes[0].stride = stride;
    f.planes[1].offset = stride * h;
    f.planes[1].stride = stride;
  }
  f.mem_size = f.planes[1].offset +
               static_cast<std::uint64_t>(f.planes[1].stride) * (h / 2);

  b->fd = gbm_bo_get_fd(b->bo);
  if (b->fd < 0)
    return fail("gbm_bo_get_fd failed");
  f.fd = b->fd;

  // Map the dma-buf itself where the exporter allows it. Some do not; the R8
  // layout is then written through gbm_bo_map, one frame at a time. gbm_bo_map
  // of a two-plane buffer is not relied on.
  const off_t size = ::lseek(b->fd, 0, SEEK_END);
  if (size > 0 && static_cast<std::uint64_t>(size) >= f.mem_size) {
    void* p = ::mmap(nullptr, static_cast<std::size_t>(size),
                     PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (p != MAP_FAILED) {
      b->map = p;
      b->map_size = static_cast<std::size_t>(size);
    }
  }
  if (b->map == nullptr && native)
    return fail("gbm NV12 or P010 buffer cannot be mapped");
  f.host_mapped = b->map != nullptr;
  return b;
}

std::unique_ptr<SwFramePool::Buffer> SwFramePool::allocate(std::string* error) {
  // What worked for the first buffer is used for the rest.
  if (!r8_only_) {
    if (auto b = allocate_as(true, nullptr))
      return b;
    r8_only_ = true;
  }
  return allocate_as(false, error);
}

SwFramePool::Buffer* SwFramePool::take(std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& b : buffers_)
    if (!b->held) {
      b->held = true;
      return b.get();
    }
  if (buffers_.size() >= kMaxBuffers) {
    if (error)
      *error = "every software frame buffer is in use";
    return nullptr;
  }
  auto b = allocate(error);
  if (b == nullptr)
    return nullptr;
  b->frame.buffer_index = static_cast<int>(buffers_.size());
  b->held = true;
  buffers_.push_back(std::move(b));
  if (buffers_.size() == 1) {
    const DmabufFrame& f = buffers_.back()->frame;
    log::i(kTag, "{} {}x{} stride {} chroma at {} ({})",
           deep_ ? "P010" : "NV12", f.width, f.height, f.planes[0].stride,
           f.planes[1].offset, f.host_mapped ? "mmap" : "gbm_bo_map");
  }
  return buffers_.back().get();
}

bool SwFramePool::write(const std::function<void(const Mapped&)>& fill,
                        DmabufFrame* out,
                        std::string* error) {
  if (out == nullptr || !fill)
    return false;
  Buffer* b = take(error);
  if (b == nullptr)
    return false;
  // Gives the buffer back to the pool. The pool is kept alive by the frame,
  // so a frame may outlive the decoder that made it.
  std::shared_ptr<void> hold(static_cast<void*>(b),
                             [self = shared_from_this(), b](void*) {
                               std::lock_guard<std::mutex> lock(self->mu_);
                               b->held = false;
                             });

  const DmabufFrame& f = b->frame;
  Mapped m;
  m.y_stride = f.planes[0].stride;
  m.uv_stride = f.planes[1].stride;
  if (b->map != nullptr) {
    auto* base = static_cast<std::uint8_t*>(b->map);
    m.y = base + f.planes[0].offset;
    m.uv = base + f.planes[1].offset;
    dmabuf_sync(b->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    fill(m);
    dmabuf_sync(b->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
  } else {
    const std::uint32_t w = (width_ + 1u) & ~1u;
    const std::uint32_t h = (height_ + 1u) & ~1u;
    std::uint32_t stride = 0;
    void* cookie = nullptr;
    void* p = gbm_bo_map(b->bo, 0, 0, deep_ ? 2 * w : w, h + h / 2,
                         GBM_BO_TRANSFER_WRITE, &stride, &cookie);
    if (p == nullptr || p == MAP_FAILED || stride != f.planes[0].stride) {
      if (p != nullptr && p != MAP_FAILED)
        gbm_bo_unmap(b->bo, cookie);
      if (error)
        *error = "gbm_bo_map failed";
      return false;
    }
    auto* base = static_cast<std::uint8_t*>(p);
    m.y = base;
    m.uv = base + static_cast<std::size_t>(stride) * h;
    fill(m);
    gbm_bo_unmap(b->bo, cookie);
  }
  *out = f;
  out->hold = std::move(hold);
  return true;
}

bool SwFramePool::write_420(const std::uint8_t* const planes[3],
                            const int strides[3],
                            bool nv12,
                            DmabufFrame* out,
                            std::string* error) {
  const std::uint32_t w = width_;
  const std::uint32_t h = height_;
  const std::uint32_t cw = (w + 1) / 2;  // chroma samples per row
  const std::uint32_t ch = (h + 1) / 2;  // chroma rows
  const bool ok =
      !deep_ && planes[0] != nullptr && planes[1] != nullptr &&
      strides[0] >= static_cast<int>(w) &&
      (nv12 ? strides[1] >= static_cast<int>(2 * cw)
            : (planes[2] != nullptr && strides[1] >= static_cast<int>(cw) &&
               strides[2] >= static_cast<int>(cw)));
  if (!ok) {
    if (error)
      *error = "malformed software frame";
    return false;
  }
  return write(
      [&](const Mapped& m) {
        for (std::uint32_t y = 0; y < h; ++y)
          std::memcpy(m.y + static_cast<std::size_t>(y) * m.y_stride,
                      planes[0] + static_cast<std::ptrdiff_t>(y) * strides[0],
                      w);
        for (std::uint32_t y = 0; y < ch; ++y) {
          std::uint8_t* dst = m.uv + static_cast<std::size_t>(y) * m.uv_stride;
          const std::uint8_t* u =
              planes[1] + static_cast<std::ptrdiff_t>(y) * strides[1];
          if (nv12) {
            std::memcpy(dst, u, 2 * static_cast<std::size_t>(cw));
            continue;
          }
          interleave_row8(
              dst, u, planes[2] + static_cast<std::ptrdiff_t>(y) * strides[2],
              cw);
        }
      },
      out, error);
}

bool SwFramePool::write_420_16(const std::uint8_t* const planes[3],
                               const int strides[3],
                               bool nv12,
                               unsigned shift,
                               DmabufFrame* out,
                               std::string* error) {
  const std::uint32_t w = width_;
  const std::uint32_t h = height_;
  const std::uint32_t cw = (w + 1) / 2;  // chroma samples per row
  const std::uint32_t ch = (h + 1) / 2;  // chroma rows
  const bool ok =
      deep_ && shift <= 8 && planes[0] != nullptr && planes[1] != nullptr &&
      strides[0] >= static_cast<int>(2 * w) &&
      (nv12 ? strides[1] >= static_cast<int>(4 * cw)
            : (planes[2] != nullptr && strides[1] >= static_cast<int>(2 * cw) &&
               strides[2] >= static_cast<int>(2 * cw)));
  if (!ok) {
    if (error)
      *error = "malformed software frame";
    return false;
  }
  return write(
      [&](const Mapped& m) {
        for (std::uint32_t y = 0; y < h; ++y)
          shift_row(m.y + static_cast<std::size_t>(y) * m.y_stride,
                    planes[0] + static_cast<std::ptrdiff_t>(y) * strides[0], w,
                    shift);
        for (std::uint32_t y = 0; y < ch; ++y) {
          std::uint8_t* dst = m.uv + static_cast<std::size_t>(y) * m.uv_stride;
          const std::uint8_t* u =
              planes[1] + static_cast<std::ptrdiff_t>(y) * strides[1];
          if (nv12) {
            shift_row(dst, u, 2 * cw, shift);
            continue;
          }
          interleave_row(
              dst, u, planes[2] + static_cast<std::ptrdiff_t>(y) * strides[2],
              cw, shift);
        }
      },
      out, error);
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL
