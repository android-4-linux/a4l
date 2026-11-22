// SPDX-License-Identifier: Apache-2.0
// Provenance: gbm public API (gbm_bo_create, gbm_bo_map, gbm_bo_get_*), Linux
// dma-buf uapi (linux/dma-buf.h, DMA_BUF_IOCTL_SYNC), libdrm drm_fourcc.h.
// No GPL-3.0 sources.
//
// NV12 dma-bufs, or P010 for a picture deeper than 8 bits, for a producer
// that has its pixels in system memory: a software video decoder. The producer
// writes a frame into a buffer and gets a gfx::DmabufFrame, which goes down the
// same path as a hardware decoder's surface. One copy per frame; the hardware
// path has none. A 4:2:2 or 4:4:4 picture is made 4:2:0 in the copy: those
// are the layouts every compositor here imports.

#ifndef ACL_GFX_SW_FRAME_POOL_H
#define ACL_GFX_SW_FRAME_POOL_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "acl_config.h"
#include "gfx/dmabuf_frame.h"

namespace acl::gfx {

class SwFramePool : public std::enable_shared_from_this<SwFramePool> {
 public:
  // The most buffers one pool allocates. A frame's buffer is held until its
  // last user lets go (a codec's queued outputs, a consumer's acquired
  // images, the compositor's unreleased wl_buffers), the same set a hardware
  // decoder's surface pool is sized for.
  static constexpr std::size_t kMaxBuffers = 32;

  // Where to write one frame. Rows are `width` samples of luma, and
  // `(width + 1) / 2` interleaved Cb,Cr pairs of chroma. A sample is a byte,
  // or in a deep pool 16 bits, little-endian, the value in the high bits
  // (P010). Strides are in bytes.
  struct Mapped {
    std::uint8_t* y = nullptr;
    std::uint32_t y_stride = 0;
    std::uint8_t* uv = nullptr;
    std::uint32_t uv_stride = 0;
  };

  // A pool of width x height buffers: NV12, or with `deep` P010. No buffer
  // is allocated until write(). nullptr + *error when there is no gbm device.
  static std::shared_ptr<SwFramePool> create(std::uint32_t width,
                                             std::uint32_t height,
                                             std::string* error,
                                             bool deep = false);
  ~SwFramePool();
  SwFramePool(const SwFramePool&) = delete;
  SwFramePool& operator=(const SwFramePool&) = delete;

  std::uint32_t width() const { return width_; }
  std::uint32_t height() const { return height_; }
  bool deep() const { return deep_; }

  // Takes a buffer nobody holds, calls `fill` to write the frame, and
  // describes it in *out. out->fd is the pool's; out->hold keeps the buffer
  // from being written again until every copy of it is dropped. False +
  // *error when no buffer can be allocated or mapped, or all kMaxBuffers are
  // held.
  bool write(const std::function<void(const Mapped&)>& fill,
             DmabufFrame* out,
             std::string* error);

  // write() for an 8-bit 4:2:0 picture of the pool's size held in system
  // memory. `nv12` is luma plus one interleaved chroma plane (planes 0, 1);
  // otherwise three planes, Y, Cb, Cr. Strides are in bytes and must cover
  // the row. False + *error on a null plane or a short stride.
  bool write_420(const std::uint8_t* const planes[3],
                 const int strides[3],
                 bool nv12,
                 DmabufFrame* out,
                 std::string* error);

  // The same for a deep pool and a 4:2:0 picture of 16-bit little-endian
  // samples: `shift` is how far a sample's value is below the top of its 16
  // bits (6 for 10 bits kept in the low ones, as a software decoder leaves
  // them; 0 for P010). Strides are in bytes. False + *error as write_420, and
  // on a pool that is not deep or a shift over 8.
  bool write_420_16(const std::uint8_t* const planes[3],
                    const int strides[3],
                    bool nv12,
                    unsigned shift,
                    DmabufFrame* out,
                    std::string* error);

  // write() for a planar 4:2:2 picture (`full_x` false) or a 4:4:4 one
  // (true), made 4:2:0. A chroma sample is the mean of the two rows it sits
  // between; across a 4:4:4 row it is a [1 2 1] filter on every second
  // sample, where 4:2:0 puts it. Samples are bytes in an 8-bit pool, and in
  // a deep one 16-bit little-endian, moved up by `shift` as in
  // write_420_16(). False + *error as write_420_16(), and on a shift in an
  // 8-bit pool.
  bool write_down_420(const std::uint8_t* const planes[3],
                      const int strides[3],
                      bool full_x,
                      unsigned shift,
                      DmabufFrame* out,
                      std::string* error);

 private:
  SwFramePool() = default;
  struct Buffer;
  Buffer* take(std::string* error);
  std::unique_ptr<Buffer> allocate(std::string* error);
  std::unique_ptr<Buffer> allocate_as(bool native, std::string* error);

  std::shared_ptr<void> gbm_keep_;  // keeps the shared gbm device open
  void* gbm_dev_ = nullptr;         // gbm_device*
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  bool deep_ = false;  // P010: two bytes a sample
  std::mutex mu_;
  std::vector<std::unique_ptr<Buffer>> buffers_;  // guarded by mu_
  bool r8_only_ = false;  // guarded by mu_: gbm has no usable NV12
};

}  // namespace acl::gfx

#endif  // ACL_GFX_SW_FRAME_POOL_H
