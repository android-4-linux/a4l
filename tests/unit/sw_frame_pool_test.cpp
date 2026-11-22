// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test. No GPL-3.0 sources.
//
// gfx::SwFramePool's copies: every sample lands where P010 (write_420_16) or
// NV12 (write_420) puts it, from rows wider than the copy's chunk and, for
// 16 bits, not 16-bit aligned; 4:2:2 and 4:4:4 chroma (write_down_420) is
// filtered to 4:2:0 as specified. Needs a gbm device and a buffer the CPU can
// map; SKIPs (with a note) where there is none.

#include <catch2/catch_test_macros.hpp>

#include "acl_config.h"

#if ACL_HAVE_GL

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
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
  // NOLINTNEXTLINE(clang-analyzer-unix.StdCLibraryFunctions): REQUIRE throws
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

TEST_CASE("SwFramePool: 8-bit planar chroma is interleaved into NV12",
          "[gfx][swpool]") {
  std::string err;
  auto pool = SwFramePool::create(kW, kH, &err);
  if (!pool) {
    WARN("no gbm device (skipping): " + err);
    SUCCEED();
    return;
  }
  // Rows padded past their width, as a decoder's are.
  const int ys = static_cast<int>(kW) + 5, cs = static_cast<int>(kCw) + 3;
  std::vector<std::uint8_t> y(static_cast<std::size_t>(ys) * kH, 0xee),
      u(static_cast<std::size_t>(cs) * kCh, 0xee),
      v(static_cast<std::size_t>(cs) * kCh, 0xee);
  for (std::uint32_t r = 0; r < kH; ++r)
    for (std::uint32_t x = 0; x < kW; ++x)
      y[r * ys + x] = static_cast<std::uint8_t>(luma(x, r));
  for (std::uint32_t r = 0; r < kCh; ++r)
    for (std::uint32_t x = 0; x < kCw; ++x) {
      u[r * cs + x] = static_cast<std::uint8_t>(cb(x, r));
      v[r * cs + x] = static_cast<std::uint8_t>(cr(x, r));
    }
  const std::uint8_t* planes[3] = {y.data(), u.data(), v.data()};
  const int strides[3] = {ys, cs, cs};
  DmabufFrame f;
  if (!pool->write_420(planes, strides, false, &f, &err)) {
    WARN("no NV12 buffer here (skipping): " + err);
    SUCCEED();
    return;
  }
  REQUIRE(f.drm_format == 0x3231564eu);  // NV12
  const off_t size = ::lseek(f.fd, 0, SEEK_END);
  REQUIRE(size > 0);
  // NOLINTNEXTLINE(clang-analyzer-unix.StdCLibraryFunctions): REQUIRE throws
  void* map = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ,
                     MAP_SHARED, f.fd, 0);
  if (map == MAP_FAILED) {
    WARN("the buffer cannot be mapped to read back (skipping)");
    return;
  }
  const auto* base = static_cast<const std::uint8_t*>(map);
  int bad = 0;
  for (std::uint32_t r = 0; r < kH; ++r)
    for (std::uint32_t x = 0; x < kW; ++x)
      bad += base[f.planes[0].offset + r * f.planes[0].stride + x] !=
             static_cast<std::uint8_t>(luma(x, r));
  for (std::uint32_t r = 0; r < kCh; ++r)
    for (std::uint32_t x = 0; x < kCw; ++x) {
      const std::uint8_t* p =
          base + f.planes[1].offset + r * f.planes[1].stride + 2 * x;
      bad += p[0] != static_cast<std::uint8_t>(cb(x, r));
      bad += p[1] != static_cast<std::uint8_t>(cr(x, r));
    }
  ::munmap(map, static_cast<std::size_t>(size));
  CHECK(bad == 0);
}

// The 4:2:0 sample write_down_420() makes at (x, y) from 4:2:2 or 4:4:4
// chroma `c` of a kW x kH picture: the mean of rows 2y and 2y + 1 (the last
// row alone where there is none), and across a 4:4:4 row [1 2 1] at 2x.
unsigned want_down(unsigned (*c)(std::uint32_t, std::uint32_t),
                   bool full_x,
                   unsigned mask,
                   std::uint32_t x,
                   std::uint32_t y) {
  const std::uint32_t r1 = std::min(2 * y + 1, kH - 1);
  const auto col = [&](std::uint32_t i) {
    return (c(i, 2 * y) & mask) + (c(i, r1) & mask);
  };
  if (!full_x)
    return (col(x) + 1) / 2;
  const std::uint32_t m = 2 * x;
  return (col(m > 0 ? m - 1 : 0) + 2 * col(m) + col(std::min(m + 1, kW - 1)) +
          4) /
         8;
}

TEST_CASE("SwFramePool: 4:2:2 and 4:4:4 chroma is made 4:2:0",
          "[gfx][swpool]") {
  std::string err;
  for (const bool deep : {false, true})
    for (const bool full_x : {false, true}) {
      INFO("deep " << deep << ", 4:4:" << (full_x ? 4 : 2));
      auto pool = SwFramePool::create(kW, kH, &err, deep);
      if (!pool) {
        WARN("no gbm device (skipping): " + err);
        SUCCEED();
        return;
      }
      // 12 bits at the low end, as a software decoder leaves them, or bytes.
      const unsigned mask = deep ? 0xfff : 0xff;
      const unsigned shift = deep ? 4 : 0;
      const std::uint32_t sw = full_x ? kW : kCw;
      Plane y(kW, kH), u(sw, kH), v(sw, kH);
      const auto put = [&](Plane& p, std::uint32_t i, std::uint32_t r,
                           unsigned val) {
        if (deep) {
          p.put(i, r, val & mask);
          return;
        }
        p.bytes[1 + static_cast<std::size_t>(r) * p.stride + i] =
            static_cast<std::uint8_t>(val);
      };
      for (std::uint32_t r = 0; r < kH; ++r) {
        for (std::uint32_t x = 0; x < kW; ++x)
          put(y, x, r, luma(x, r) * 3);
        for (std::uint32_t x = 0; x < sw; ++x) {
          put(u, x, r, cb(x, r) * 3);
          put(v, x, r, cr(x, r) * 3);
        }
      }
      const std::uint8_t* planes[3] = {y.data(), u.data(), v.data()};
      const int strides[3] = {y.stride, u.stride, v.stride};
      DmabufFrame f;
      if (!pool->write_down_420(planes, strides, full_x, shift, &f, &err)) {
        WARN("no buffer here (skipping): " + err);
        SUCCEED();
        return;
      }
      const off_t size = ::lseek(f.fd, 0, SEEK_END);
      REQUIRE(size > 0);
      // NOLINTNEXTLINE(clang-analyzer-unix.StdCLibraryFunctions): REQUIRE
      // throws
      void* map = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ,
                         MAP_SHARED, f.fd, 0);
      if (map == MAP_FAILED) {
        WARN("the buffer cannot be mapped to read back (skipping)");
        return;
      }
      const auto* base = static_cast<const std::uint8_t*>(map);
      const auto got = [&](int plane, std::uint32_t i, std::uint32_t r) {
        return deep ? at(base, f, plane, i, r)
                    : unsigned{base[f.planes[plane].offset +
                                    static_cast<std::size_t>(r) *
                                        f.planes[plane].stride +
                                    i]};
      };
      const auto cb3 = [](std::uint32_t x, std::uint32_t r) {
        return cb(x, r) * 3;
      };
      const auto cr3 = [](std::uint32_t x, std::uint32_t r) {
        return cr(x, r) * 3;
      };
      int bad = 0;
      for (std::uint32_t r = 0; r < kH; ++r)
        for (std::uint32_t x = 0; x < kW; ++x)
          bad += got(0, x, r) != (((luma(x, r) * 3) & mask) << shift);
      for (std::uint32_t r = 0; r < kCh; ++r)
        for (std::uint32_t x = 0; x < kCw; ++x) {
          bad +=
              got(1, 2 * x, r) != (want_down(cb3, full_x, mask, x, r) << shift);
          bad += got(1, 2 * x + 1, r) !=
                 (want_down(cr3, full_x, mask, x, r) << shift);
        }
      ::munmap(map, static_cast<std::size_t>(size));
      CHECK(bad == 0);
      // A chroma stride short of its row is refused.
      const int short_rows[3] = {
          y.stride, (deep ? 2 : 1) * static_cast<int>(sw) - 1, v.stride};
      CHECK_FALSE(
          pool->write_down_420(planes, short_rows, full_x, shift, &f, &err));
    }
}

#endif  // ACL_HAVE_GL
