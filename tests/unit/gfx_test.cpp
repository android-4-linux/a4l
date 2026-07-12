// SPDX-License-Identifier: Apache-2.0
// Provenance: original test; plan.md §5 (Tier A), ADR-4 (pinned Skia backend). No GPL-3.0 sources.
//
// gfx/ rasterizer tests. The software rasterizer is always exercised; when the build
// has the pinned Skia backend (ACL_HAVE_SKIA), the Skia raster path is asserted to be
// bit-identical to the software path for the ops both support — the guarantee that
// lets android.graphics Canvas/Bitmap natives swap onto host Skia without changing
// what wm/ presents.

#include "gfx/gfx.h"
#include "gfx/gfx_vk.h"

#include <cstdint>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

using acl::gfx::Surface;

TEST_CASE("software rasterizer fills color and rect", "[gfx]") {
  Surface s(8, 4);
  s.draw_color(0xFF112233);
  CHECK(s.pixel(0, 0) == 0xFF112233);
  CHECK(s.pixel(7, 3) == 0xFF112233);

  s.fill_rect(2, 1, 5, 3, 0xFFAABBCC);
  CHECK(s.pixel(2, 1) == 0xFFAABBCC);
  CHECK(s.pixel(4, 2) == 0xFFAABBCC);
  CHECK(s.pixel(5, 3) == 0xFF112233);  // exclusive right/bottom
  CHECK(s.pixel(1, 1) == 0xFF112233);  // outside the rect
  CHECK(s.pixel(9, 9) == 0u);          // out of bounds
}

#if ACL_HAVE_SKIA
TEST_CASE("Skia backend is available when compiled in", "[gfx][skia]") {
  CHECK(acl::gfx::skia_backend_available());
}

TEST_CASE("Skia raster output matches the software rasterizer", "[gfx][skia]") {
  const int w = 16, h = 10;
  const std::uint32_t bg = 0xFF204060;
  const std::uint32_t fg = 0xFFCC8844;
  const int l = 3, t = 2, r = 11, b = 7;

  // Software reference.
  Surface ref(w, h);
  ref.draw_color(bg);
  ref.fill_rect(l, t, r, b, fg);

  // Skia-rendered.
  Surface sk(w, h);
  REQUIRE(acl::gfx::skia_render_rect(sk, bg, l, t, r, b, fg));

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      INFO("pixel (" << x << "," << y << ")");
      REQUIRE(sk.pixel(x, y) == ref.pixel(x, y));
    }
  }
}
#endif  // ACL_HAVE_SKIA

#if ACL_HAVE_VULKAN
// P2.4 / V1: the Ganesh Vulkan render target draws through a real SkCanvas on the GPU
// and reads the pixels back. AA-off solid fills must match the software rasterizer
// exactly. SKIPs cleanly (SUCCEED with a note) where no Vulkan device is present — the
// same contract the host-ART tests use, so CI without a GPU still passes.
TEST_CASE("Ganesh Vulkan render target matches the software rasterizer", "[gfx][vulkan]") {
  const int w = 16, h = 10;
  const std::uint32_t bg = 0xFF204060;
  const std::uint32_t fg = 0xFFCC8844;
  const int l = 3, t = 2, r = 11, b = 7;

  std::string err;
  std::unique_ptr<acl::gfx::VkRenderTarget> vk =
      acl::gfx::VkRenderTarget::Create(w, h, &err);
  if (!vk) {
    WARN("no Vulkan render target (skipping GPU parity): " + err);
    SUCCEED();
    return;
  }
  INFO("Vulkan device: " << vk->device_name());

  Surface ref(w, h);
  ref.draw_color(bg);
  ref.fill_rect(l, t, r, b, fg);

  Surface gpu(w, h);
  REQUIRE(vk->render_rect(gpu, bg, l, t, r, b, fg));

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      INFO("pixel (" << x << "," << y << ") dev=" << vk->device_name());
      REQUIRE(gpu.pixel(x, y) == ref.pixel(x, y));
    }
  }
}
#endif  // ACL_HAVE_VULKAN
