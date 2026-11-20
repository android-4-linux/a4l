// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test. No GPL-3.0 sources.
//
// gfx::SwFramePool's 16-bit copy (write_420_16): every sample lands where P010
// puts it, moved up by the shift, from rows that are not 16-bit aligned and
// wider than the copy's chunk. Needs a gbm device and a buffer the CPU can
// map; SKIPs (with a note) where there is none.

#include <catch2/catch_test_macros.hpp>

#include "acl_config.h"

#if ACL_HAVE_GL

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gfx/sw_frame_pool.h"

namespace {

using acl::gfx::DmabufFrame;
using acl::gfx::SwFramePool;

// 2 * 256 + 13: across a chunk of the copy, and odd.
constexpr std::uint32_t kW = 525;
constexpr std::uint32_t kH = 5;
constexpr std::uint32_t kCw = (kW + 1) / 2;
constexpr std::uint32_t kCh = (kH + 1) / 2;

unsigned luma(std::uint32_t x, std::uint32_t y) {
  return (y * 131 + x * 7 + 1) & 0x3ff;
}
unsigned cb(std::uint32_t x, std::uint32_t y) {
  return (y * 37 + x * 3 + 500) & 0x3ff;
}
unsigned cr(std::uint32_t x, std::uint32_t y) {
  return (y * 53 + x * 11 + 900) & 0x3ff;
}

// A plane of 16-bit little-endian samples, one byte into its storage so that
// no row is 16-bit aligned, rows padded past their width.
struct Plane {
  std::vector<std::uint8_t> bytes;
  int stride = 0;
  const std::uint8_t* data() const { return bytes.data() + 1; }
  Plane(std::uint32_t samples, std::uint32_t rows) {
    stride = static_cast<int>(2 * samples + 6);
    bytes.assign(1 + static_cast<std::size_t>(stride) * rows, 0xee);
  }
  void put(std::uint32_t i, std::uint32_t row, unsigned v) {
    std::uint8_t* p =
        bytes.data() + 1 + static_cast<std::size_t>(row) * stride + 2 * i;
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
  }
};

unsigned at(const std::uint8_t* base,
            const DmabufFrame& f,
            int plane,
            std::uint32_t i,
            std::uint32_t row) {
  const std::uint8_t* p =
      base + f.planes[plane].offset +
      static_cast<std::size_t>(row) * f.planes[plane].stride + 2 * i;
  return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8);
}

// Checks `f` against the samples moved up by `shift`.
void check(const DmabufFrame& f, unsigned shift) {
  REQUIRE(f.drm_format == 0x30313050u);  // P010
  REQUIRE(f.plane_count == 2);
  const off_t size = ::lseek(f.fd, 0, SEEK_END);
  REQUIRE(size > 0);
  void* map = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ,
                     MAP_SHARED, f.fd, 0);
  if (map == MAP_FAILED) {
    WARN("the buffer cannot be mapped to read back (skipping)");
    return;
  }
  const auto* base = static_cast<const std::uint8_t*>(map);
  int bad = 0;
  for (std::uint32_t y = 0; y < kH; ++y)
    for (std::uint32_t x = 0; x < kW; ++x)
      bad += at(base, f, 0, x, y) != ((luma(x, y) << shift) & 0xffff);
  for (std::uint32_t y = 0; y < kCh; ++y)
    for (std::uint32_t x = 0; x < kCw; ++x) {
      bad += at(base, f, 1, 2 * x, y) != ((cb(x, y) << shift) & 0xffff);
      bad += at(base, f, 1, 2 * x + 1, y) != ((cr(x, y) << shift) & 0xffff);
    }
  ::munmap(map, static_cast<std::size_t>(size));
  CHECK(bad == 0);
}

}  // namespace

TEST_CASE("SwFramePool: 16-bit samples land where P010 puts them",
          "[gfx][swpool]") {
  std::string err;
  auto pool = SwFramePool::create(kW, kH, &err, /*deep=*/true);
  if (!pool) {
    WARN("no gbm device (skipping): " + err);
    SUCCEED();
    return;
  }
  Plane y(kW, kH), u(kCw, kCh), v(kCw, kCh), uv(2 * kCw, kCh);
  for (std::uint32_t r = 0; r < kH; ++r)
    for (std::uint32_t x = 0; x < kW; ++x)
      y.put(x, r, luma(x, r));
  for (std::uint32_t r = 0; r < kCh; ++r)
    for (std::uint32_t x = 0; x < kCw; ++x) {
      u.put(x, r, cb(x, r));
      v.put(x, r, cr(x, r));
      uv.put(2 * x, r, cb(x, r));
      uv.put(2 * x + 1, r, cr(x, r));
    }

  // Planar, 10 bits at the low end, as a software decoder leaves them.
  {
    const std::uint8_t* planes[3] = {y.data(), u.data(), v.data()};
    const int strides[3] = {y.stride, u.stride, v.stride};
    DmabufFrame f;
    if (!pool->write_420_16(planes, strides, false, 6, &f, &err)) {
      WARN("no P010 buffer here (skipping): " + err);
      SUCCEED();
      return;
    }
    check(f, 6);
  }
  // Interleaved chroma, moved and not.
  for (const unsigned shift : {0u, 6u}) {
    const std::uint8_t* planes[3] = {y.data(), uv.data(), nullptr};
    const int strides[3] = {y.stride, uv.stride, 0};
    DmabufFrame f;
    REQUIRE(pool->write_420_16(planes, strides, true, shift, &f, &err));
    check(f, shift);
  }
  // A shift that would lose bits, and a stride short of the row, are refused.
  {
    const std::uint8_t* planes[3] = {y.data(), uv.data(), nullptr};
    const int strides[3] = {y.stride, uv.stride, 0};
    DmabufFrame f;
    CHECK_FALSE(pool->write_420_16(planes, strides, true, 9, &f, &err));
    const int short_rows[3] = {2 * static_cast<int>(kW) - 2, uv.stride, 0};
    CHECK_FALSE(pool->write_420_16(planes, short_rows, true, 0, &f, &err));
  }
}

#endif  // ACL_HAVE_GL
