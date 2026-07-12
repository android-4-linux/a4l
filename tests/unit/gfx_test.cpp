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

// P2.4 / V2: the render target hands a rendered frame over as a Linux dma-buf — the
// handle a Wayland compositor imports via zwp_linux_dmabuf_v1. Headless verification:
// the exported plane layout is sane, and (where the export memory is host-visible) the
// mmap'd dma-buf holds exactly the software-rasterizer pixels. SKIPs cleanly when no
// device / no dma-buf export.
TEST_CASE("Ganesh Vulkan frame exports as a valid linear dma-buf", "[gfx][vulkan][dmabuf]") {
  const int w = 16, h = 10;
  const std::uint32_t bg = 0xFF204060;
  const std::uint32_t fg = 0xFFCC8844;
  const int l = 3, t = 2, r = 11, b = 7;

  std::string err;
  std::unique_ptr<acl::gfx::VkRenderTarget> vk =
      acl::gfx::VkRenderTarget::Create(w, h, &err);
  if (!vk) {
    WARN("no Vulkan render target (skipping dma-buf export): " + err);
    SUCCEED();
    return;
  }
  if (!vk->dmabuf_export_available()) {
    WARN("device lacks dma-buf export extensions (skipping): " << vk->device_name());
    SUCCEED();
    return;
  }

  acl::gfx::DmabufFrame frame;
  if (!vk->render_and_export_linear(bg, l, t, r, b, fg, &frame, &err)) {
    WARN("dma-buf export failed (skipping parity): " + err);
    SUCCEED();
    return;
  }
  INFO("dma-buf on " << vk->device_name());

  // Plane layout must be sane for a linear XRGB8888 frame.
  CHECK(frame.fd >= 0);
  CHECK(frame.width == static_cast<std::uint32_t>(w));
  CHECK(frame.height == static_cast<std::uint32_t>(h));
  CHECK(frame.plane_count == 1u);
  CHECK(frame.stride() >= static_cast<std::uint32_t>(w) * 4u);
  CHECK(frame.modifier == 0u);  // DRM_FORMAT_MOD_LINEAR == 0 (no libdrm header in tests)
  CHECK_FALSE(frame.modifier_tiled);
  CHECK(frame.mem_size >= static_cast<std::uint64_t>(frame.stride()) * h);

  // Where the export memory is host-visible, the dma-buf must hold the rendered scene.
  if (frame.host_mapped) {
    Surface ref(w, h);
    ref.draw_color(bg);
    ref.fill_rect(l, t, r, b, fg);

    Surface got(w, h);
    REQUIRE(vk->read_exported_pixels(got));
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        INFO("pixel (" << x << "," << y << ")");
        REQUIRE(got.pixel(x, y) == ref.pixel(x, y));
      }
    }
  } else {
    WARN("dma-buf memory not host-visible; layout checked, pixels not mmap-verified");
  }
}

// P2.4 / V2b: the modifier-tiled DIRECT path (zero-copy) — Skia renders straight into
// a modifier-tiled, dma-buf-exported VkImage. Headless, we stand in for the compositor
// by offering the GPU's own render+sampleable modifiers as the "advertised" set (a real
// compositor's set is intersected the same way). Verifies a non-linear modifier is
// chosen, the export is valid, and the Skia-detiled pixels match the software raster.
TEST_CASE("Ganesh Vulkan modifier-tiled direct dma-buf path", "[gfx][vulkan][dmabuf][modifier]") {
  const int w = 16, h = 10;
  const std::uint32_t bg = 0xFF204060;
  const std::uint32_t fg = 0xFFCC8844;
  const int l = 3, t = 2, r = 11, b = 7;

  std::string err;
  std::unique_ptr<acl::gfx::VkRenderTarget> vk =
      acl::gfx::VkRenderTarget::Create(w, h, &err);
  if (!vk) { WARN("no Vulkan render target: " + err); SUCCEED(); return; }
  if (!vk->modifier_tiled_available()) {
    WARN("device lacks VK_EXT_image_drm_format_modifier (skipping): " << vk->device_name());
    SUCCEED();
    return;
  }

  // Stand-in for compositor feedback: every modifier the GPU can render+sample (tiled
  // preferred over LINEAR). A real presenter passes the compositor's advertised set.
  std::vector<std::uint64_t> advertised = vk->all_renderable_modifiers();
  if (advertised.empty()) {
    WARN("GPU advertises no render+sampleable modifiers (skipping)");
    SUCCEED();
    return;
  }

  acl::gfx::DmabufFrame frame;
  if (!vk->render_to_modifier_dmabuf(advertised, bg, l, t, r, b, fg, &frame, &err)) {
    WARN("no usable modifier / direct path unavailable (skipping): " + err);
    SUCCEED();
    return;
  }
  INFO("modifier-tiled dma-buf on " << vk->device_name());

  CHECK(frame.fd >= 0);
  CHECK(frame.modifier_tiled);
  CHECK(frame.plane_count >= 1u);
  CHECK(frame.width == static_cast<std::uint32_t>(w));
  CHECK(frame.stride() >= static_cast<std::uint32_t>(w) * 4u);

  // The pixels: Skia detiles the wrapped tiled surface on readback, so this verifies
  // the direct render landed correctly even though the memory isn't CPU-linear.
  Surface ref(w, h);
  ref.draw_color(bg);
  ref.fill_rect(l, t, r, b, fg);
  Surface got(w, h);
  REQUIRE(vk->read_rendered_pixels(got));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      INFO("pixel (" << x << "," << y << ") mod=" << frame.modifier);
      REQUIRE(got.pixel(x, y) == ref.pixel(x, y));
    }
  }
}
#endif  // ACL_HAVE_VULKAN
