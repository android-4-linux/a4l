// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test code. No GPL-3.0 sources.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "gfx/egl_attribs.h"

using acl::gfx::egl_surface_type_swapped;
using acl::gfx::kEglNone;
using acl::gfx::kEglPbufferBit;
using acl::gfx::kEglSurfaceType;
using acl::gfx::kEglWindowBit;

TEST_CASE("a pbuffer config request becomes a window config request",
          "[gfx][egl]") {
  constexpr int kRedSize = 0x3024;
  constexpr int kPixmapBit = 0x0002;
  const int pbuffer[] = {kRedSize, 8, kEglSurfaceType, kEglPbufferBit,
                         kEglNone};
  CHECK(
      egl_surface_type_swapped(pbuffer, kEglPbufferBit, kEglWindowBit) ==
      std::vector<int>{kRedSize, 8, kEglSurfaceType, kEglWindowBit, kEglNone});
  // The other bits of the surface type stay.
  const int both[] = {kEglSurfaceType, kEglPbufferBit | kPixmapBit, kEglNone};
  CHECK(
      egl_surface_type_swapped(both, kEglPbufferBit, kEglWindowBit) ==
      std::vector<int>{kEglSurfaceType, kEglWindowBit | kPixmapBit, kEglNone});
  // And the other way, for a display with pbuffer configs only.
  const int window[] = {kEglSurfaceType, kEglWindowBit, kEglNone};
  CHECK(egl_surface_type_swapped(window, kEglWindowBit, kEglPbufferBit) ==
        std::vector<int>{kEglSurfaceType, kEglPbufferBit, kEglNone});
}

TEST_CASE("a config request that needs no change gives nothing to retry",
          "[gfx][egl]") {
  constexpr int kRedSize = 0x3024;
  CHECK(
      egl_surface_type_swapped(nullptr, kEglPbufferBit, kEglWindowBit).empty());
  const int none[] = {kEglNone};
  CHECK(egl_surface_type_swapped(none, kEglPbufferBit, kEglWindowBit).empty());
  // No surface type asked for, or one that is not the kind to replace.
  const int no_type[] = {kRedSize, 8, kEglNone};
  CHECK(
      egl_surface_type_swapped(no_type, kEglPbufferBit, kEglWindowBit).empty());
  const int window[] = {kEglSurfaceType, kEglWindowBit, kEglNone};
  CHECK(
      egl_surface_type_swapped(window, kEglPbufferBit, kEglWindowBit).empty());
  // Something with no terminator in a config list's length is not one.
  std::vector<int> endless(400, kRedSize);
  endless.push_back(kEglNone);
  CHECK(egl_surface_type_swapped(endless.data(), kEglPbufferBit, kEglWindowBit)
            .empty());
}
