// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/gfx.md (pending), AOSP android.graphics Canvas/Bitmap JNI
// surface, Skia API. No GPL-3.0 sources.
#ifndef ACL_GFX_GFX_H
#define ACL_GFX_GFX_H

// The rasterization seam: android.graphics.Canvas draws into a gfx::Surface (an
// ARGB_8888 buffer). This build has a self-contained software rasterizer
// (always available, deterministic, headless-verifiable — the null-sink of
// gfx); a pinned-Skia backend (ADR-4, hwui dropped) replaces the raster ops
// behind the same Surface when vendored. Wayland presentation of a Surface (wcs
// bindings, wl_surface/wp_presentation) is a separate seam (wm/), not built
// here.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "acl_config.h"  // ACL_HAVE_SKIA

namespace acl::gfx {

// ARGB_8888 raster target (premultiplied not assumed; opaque fills for now).
class Surface {
 public:
  Surface(int width, int height);

  void draw_color(std::uint32_t argb);  // fill the whole surface
  void fill_rect(int left, int top, int right, int bottom, std::uint32_t argb);
  std::uint32_t pixel(int x, int y) const;  // 0 if out of bounds

  int width() const { return width_; }
  int height() const { return height_; }

  // Row-major ARGB_8888 pixel data (width*height), for blitting into a
  // presentation buffer (wm/). Valid while the Surface is alive.
  const std::uint32_t* data() const { return pixels_.data(); }

  // Overwrite all pixels from a tightly-packed width*height ARGB_8888 source
  // (e.g. copy a rendered frame into a presenter's working surface).
  void blit(const std::uint32_t* src) {
    std::copy(src, src + pixels_.size(), pixels_.begin());
  }

 private:
  int width_;
  int height_;
  std::vector<std::uint32_t> pixels_;  // row-major, width*height
};

const char* module_name() noexcept;

#if ACL_HAVE_SKIA
// The pinned-Skia raster backend (ADR-4 / A02.2), compiled only when a pinned
// Skia prefix is present. Same ARGB_8888 Surface, real SkCanvas underneath —
// the seam android.graphics Canvas/Bitmap natives re-target onto host Skia. See
// gfx_skia.cpp.
bool skia_backend_available() noexcept;
bool skia_render_rect(Surface& out,
                      std::uint32_t bg_argb,
                      int left,
                      int top,
                      int right,
                      int bottom,
                      std::uint32_t rect_argb);
#endif

}  // namespace acl::gfx

#endif  // ACL_GFX_GFX_H
