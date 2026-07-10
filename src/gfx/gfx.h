// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/gfx.md (pending), AOSP android.graphics Canvas/Bitmap JNI
// surface, Skia API. No GPL-3.0 sources.
#ifndef ACLCXX_GFX_GFX_H
#define ACLCXX_GFX_GFX_H

// The rasterization seam: android.graphics.Canvas draws into a gfx::Surface (an
// ARGB_8888 buffer). This build has a self-contained software rasterizer (always
// available, deterministic, headless-verifiable — the null-sink of gfx); a
// pinned-Skia backend (ADR-4, hwui dropped) replaces the raster ops behind the
// same Surface when vendored. Wayland presentation of a Surface (wcs bindings,
// wl_surface/wp_presentation) is a separate seam (wm/), not built here.

#include <cstdint>
#include <vector>

namespace aclcxx::gfx {

// ARGB_8888 raster target (premultiplied not assumed; opaque fills for now).
class Surface {
 public:
  Surface(int width, int height);

  void draw_color(std::uint32_t argb);  // fill the whole surface
  void fill_rect(int left, int top, int right, int bottom, std::uint32_t argb);
  std::uint32_t pixel(int x, int y) const;  // 0 if out of bounds

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  int width_;
  int height_;
  std::vector<std::uint32_t> pixels_;  // row-major, width*height
};

const char* module_name() noexcept;

}  // namespace aclcxx::gfx

#endif  // ACLCXX_GFX_GFX_H
