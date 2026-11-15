// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design (docs/adr/ADR-0009 Decision 3, C5); DRM
// fourcc and modifier values per drm_fourcc.h; the dma-buf filesystem magic
// per linux/magic.h; UTF-8 per RFC 3629. No GPL-3.0 sources.

#include "wm/split_validate.h"

#include <algorithm>

#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace acl::wm {

namespace {

// linux/magic.h DMA_BUF_MAGIC, "DMAB". Spelled out so that wm/ builds against
// kernel headers that predate it.
constexpr long kDmabufMagic = 0x444d4142;

// drm_fourcc.h, spelled out so that wm/ need not pull in libdrm.
constexpr std::uint32_t kArgb8888 = 0x34325241u;  // 'AR24'
constexpr std::uint32_t kXrgb8888 = 0x34325258u;  // 'XR24'
constexpr std::uint64_t kModLinear = 0;
constexpr std::uint64_t kModInvalid = 0x00ffffffffffffffull;
constexpr std::uint32_t kBytesPerPixel = 4;   // both formats above
constexpr std::uint32_t kNv12 = 0x3231564eu;  // 'NV12'
constexpr std::uint32_t kP010 = 0x30313050u;  // 'P010'

}  // namespace

BufferFacts inspect_buffer(int fd) noexcept {
  BufferFacts facts;
  if (fd < 0)
    return facts;
  struct statfs fs{};
  if (::fstatfs(fd, &fs) != 0 || static_cast<long>(fs.f_type) != kDmabufMagic)
    return facts;
  facts.is_dmabuf = true;
  // A dma-buf answers SEEK_END with its size and keeps no position worth
  // restoring; nothing reads it through this descriptor.
  const off_t end = ::lseek(fd, 0, SEEK_END);
  if (end > 0)
    facts.size = static_cast<std::uint64_t>(end);
  (void)::lseek(fd, 0, SEEK_SET);
  return facts;
}

const char* verdict_name(FrameVerdict v) noexcept {
  switch (v) {
    case FrameVerdict::Ok:
      return "ok";
    case FrameVerdict::NoSurface:
      return "no surface";
    case FrameVerdict::NoBuffer:
      return "no buffer";
    case FrameVerdict::NotDmabuf:
      return "not a dma-buf";
    case FrameVerdict::BadSize:
      return "size is not the surface's";
    case FrameVerdict::BadFormat:
      return "format not accepted";
    case FrameVerdict::BadModifier:
      return "modifier not advertised";
    case FrameVerdict::BadPlanes:
      return "bad planes";
    case FrameVerdict::BadLayout:
      return "stride or offset outside the buffer";
    case FrameVerdict::BadSlot:
      return "bad buffer slot";
  }
  return "unknown";
}

FrameVerdict check_frame(const ipc::PresentFrame& p,
                         bool fd_present,
                         const BufferFacts& facts,
                         const FrameLimits& limits) noexcept {
  if (!limits.have_surface)
    return FrameVerdict::NoSurface;
  if (!fd_present)
    return FrameVerdict::NoBuffer;
  if (!facts.is_dmabuf || facts.size == 0)
    return FrameVerdict::NotDmabuf;

  // The surface's own size, which check_surface() bounded when it was made.
  if (p.w == 0 || p.h == 0 || p.w > ipc::kMaxSurfaceDim ||
      p.h > ipc::kMaxSurfaceDim || p.w != limits.surface_w ||
      p.h != limits.surface_h)
    return FrameVerdict::BadSize;

  if (p.drm_format != kArgb8888 && p.drm_format != kXrgb8888)
    return FrameVerdict::BadFormat;

  // Linear needs no advertisement; "invalid" is how a producer says it has no
  // explicit modifier, which the compositor reads as its implicit default.
  if (p.modifier != kModLinear && p.modifier != kModInvalid &&
      std::find(limits.modifiers.begin(), limits.modifiers.end(), p.modifier) ==
          limits.modifiers.end())
    return FrameVerdict::BadModifier;

  const std::uint32_t planes = p.plane_count == 0 ? 1 : p.plane_count;
  if (planes > ipc::kMaxPlanes)
    return FrameVerdict::BadPlanes;

  // Plane 0 holds the pixels: every row fits, and the last row ends inside
  // the buffer. 64-bit, so the products cannot wrap: stride is 32 bits and
  // the height is at most kMaxSurfaceDim.
  const std::uint64_t min_stride =
      static_cast<std::uint64_t>(p.w) * kBytesPerPixel;
  if (p.stride < min_stride)
    return FrameVerdict::BadLayout;
  const std::uint64_t end = static_cast<std::uint64_t>(p.offset) +
                            static_cast<std::uint64_t>(p.stride) * p.h;
  if (end > facts.size)
    return FrameVerdict::BadLayout;

  // Auxiliary planes (compression metadata under some modifiers) have a
  // layout only the driver knows. They must at least start inside the buffer
  // and have a stride.
  for (std::uint32_t i = 1; i < planes; ++i) {
    if (p.plane_stride[i - 1] == 0 || p.plane_offset[i - 1] >= facts.size)
      return FrameVerdict::BadPlanes;
  }

  const auto slot = static_cast<std::int32_t>(p.buffer_id);
  if (slot < -1 || slot >= limits.slots)
    return FrameVerdict::BadSlot;
  return FrameVerdict::Ok;
}

bool video_format_accepted(std::uint32_t drm_format) noexcept {
  return drm_format == kNv12 || drm_format == kP010;
}

FrameVerdict check_video_frame(const ipc::VideoFrame& v,
                               bool fd_present,
                               const BufferFacts& facts,
                               const VideoLimits& limits) noexcept {
  if (!limits.have_surface)
    return FrameVerdict::NoSurface;
  if (!fd_present)
    return FrameVerdict::NoBuffer;
  if (!facts.is_dmabuf || facts.size == 0)
    return FrameVerdict::NotDmabuf;

  if (v.w == 0 || v.h == 0 || v.w > ipc::kMaxSurfaceDim ||
      v.h > ipc::kMaxSurfaceDim || !check_video_rect(v.dest))
    return FrameVerdict::BadSize;

  // No advertisement for the format at all: the compositor does not take it.
  if (!video_format_accepted(v.drm_format) || limits.modifiers.empty())
    return FrameVerdict::BadFormat;
  if (std::find(limits.modifiers.begin(), limits.modifiers.end(), v.modifier) ==
      limits.modifiers.end())
    return FrameVerdict::BadModifier;

  // Both formats are two planes: luma, then interleaved chroma at half
  // resolution each way. More are a driver's auxiliary planes.
  if (v.plane_count < 2 || v.plane_count > ipc::kMaxPlanes)
    return FrameVerdict::BadPlanes;

  // Every luma row and every chroma row fits its stride, and the last row
  // of each ends inside the buffer. 64-bit: stride is 32 bits and the
  // height at most kMaxSurfaceDim, so the products cannot wrap.
  const std::uint64_t sample = v.drm_format == kP010 ? 2 : 1;
  const std::uint64_t luma_row = static_cast<std::uint64_t>(v.w) * sample;
  const std::uint64_t chroma_row =
      (static_cast<std::uint64_t>(v.w) + 1) / 2 * 2 * sample;
  const std::uint64_t chroma_rows = (static_cast<std::uint64_t>(v.h) + 1) / 2;
  if (v.stride[0] < luma_row || v.stride[1] < chroma_row)
    return FrameVerdict::BadLayout;
  if (v.offset[0] + static_cast<std::uint64_t>(v.stride[0]) * v.h >
          facts.size ||
      v.offset[1] + static_cast<std::uint64_t>(v.stride[1]) * chroma_rows >
          facts.size)
    return FrameVerdict::BadLayout;
  for (std::uint32_t i = 2; i < v.plane_count; ++i) {
    if (v.stride[i] == 0 || v.offset[i] >= facts.size)
      return FrameVerdict::BadPlanes;
  }
  return FrameVerdict::Ok;
}

bool check_video_rect(const ipc::VideoRect& r) noexcept {
  return r.w > 0 && r.h > 0 && r.w <= ipc::kMaxVideoCoord &&
         r.h <= ipc::kMaxVideoCoord && r.x >= -ipc::kMaxVideoCoord &&
         r.x <= ipc::kMaxVideoCoord && r.y >= -ipc::kMaxVideoCoord &&
         r.y <= ipc::kMaxVideoCoord;
}

bool check_surface(const ipc::SurfaceInfo& info) noexcept {
  return info.w != 0 && info.h != 0 && info.w <= ipc::kMaxSurfaceDim &&
         info.h <= ipc::kMaxSurfaceDim;
}

std::string clean_label(std::string_view in,
                        std::size_t max_bytes,
                        std::string_view fallback) {
  std::string out;
  std::size_t i = 0;
  while (i < in.size()) {
    const auto b0 = static_cast<unsigned char>(in[i]);
    // Length and minimum code point of the sequence b0 starts, per RFC 3629.
    std::size_t len = 0;
    std::uint32_t cp = 0, min = 0;
    if (b0 < 0x80) {
      len = 1, cp = b0;
    } else if ((b0 & 0xe0) == 0xc0) {
      len = 2, cp = b0 & 0x1fu, min = 0x80;
    } else if ((b0 & 0xf0) == 0xe0) {
      len = 3, cp = b0 & 0x0fu, min = 0x800;
    } else if ((b0 & 0xf8) == 0xf0) {
      len = 4, cp = b0 & 0x07u, min = 0x10000;
    }
    bool ok = len != 0 && i + len <= in.size();
    for (std::size_t k = 1; ok && k < len; ++k) {
      const auto b = static_cast<unsigned char>(in[i + k]);
      ok = (b & 0xc0) == 0x80;
      cp = (cp << 6) | (b & 0x3fu);
    }
    // Overlong forms, surrogates and values past U+10FFFF are not UTF-8.
    ok = ok && cp >= min && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
    if (!ok) {
      ++i;  // drop the byte and resynchronize
      continue;
    }
    // C0 and C1 controls, DEL, and the characters that reorder or hide text
    // around them in a window list: bidi controls and zero-width marks.
    const bool control = cp < 0x20 || (cp >= 0x7f && cp < 0xa0);
    const bool hidden = (cp >= 0x200b && cp <= 0x200f) ||
                        (cp >= 0x202a && cp <= 0x202e) ||
                        (cp >= 0x2066 && cp <= 0x2069) || cp == 0xfeff;
    if (!control && !hidden) {
      if (out.size() + len > max_bytes)
        break;
      out.append(in.substr(i, len));
    }
    i += len;
  }
  return out.empty() ? std::string(fallback) : out;
}

}  // namespace acl::wm
