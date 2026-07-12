// SPDX-License-Identifier: Apache-2.0
// Provenance: AOSP android.graphics Canvas/Bitmap JNI surface, Skia public API
// (include/core/Sk{Surface,Canvas,Paint}.h), ADR-4 (single pinned Skia, hwui
// dropped). No GPL-3.0 sources.
//
// The pinned-Skia raster backend (ADR-4 / A02.2) behind the same gfx::Surface the
// software rasterizer backs. Built only when a pinned Skia prefix is present
// (ACL_HAVE_SKIA); it renders with a real raster SkCanvas into an ARGB_8888 buffer
// and hands the pixels back through Surface::blit — the exact bytes an
// android.graphics.Canvas/Bitmap read path will present via wm/. This is the seam
// android.graphics JNI natives (Canvas/Bitmap) re-target onto host Skia.

#include "gfx/gfx.h"

#include <cstddef>
#include <vector>

#include "include/core/SkAlphaType.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"

namespace acl::gfx {

bool skia_backend_available() noexcept { return true; }

// gfx::Surface stores each pixel as a uint32 0xAARRGGBB. On little-endian that is
// the byte order B,G,R,A — Skia's kBGRA_8888. Unpremultiplied: fill_rect/draw_color
// write opaque colors verbatim, matching the software rasterizer's semantics so the
// two backends are bit-identical for the ops both support.
static SkImageInfo argb8888_info(int w, int h) {
  return SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType);
}

// Render a background clear + one filled rect with a real Skia raster canvas and
// deposit the result into `out`. Colors are SkColor (0xAARRGGBB), matching the
// gfx::Surface uint32 convention directly.
bool skia_render_rect(Surface& out, std::uint32_t bg_argb, int left, int top,
                      int right, int bottom, std::uint32_t rect_argb) {
  const int w = out.width();
  const int h = out.height();
  if (w <= 0 || h <= 0) return false;

  const SkImageInfo info = argb8888_info(w, h);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  if (!surface) return false;

  SkCanvas* canvas = surface->getCanvas();
  canvas->clear(static_cast<SkColor>(bg_argb));

  SkPaint paint;
  paint.setColor(static_cast<SkColor>(rect_argb));
  paint.setAntiAlias(false);  // deterministic, matches the software rasterizer
  canvas->drawRect(SkRect::MakeLTRB(static_cast<SkScalar>(left), static_cast<SkScalar>(top),
                                    static_cast<SkScalar>(right), static_cast<SkScalar>(bottom)),
                   paint);

  std::vector<std::uint32_t> buf(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
  if (!surface->readPixels(info, buf.data(),
                           static_cast<std::size_t>(w) * sizeof(std::uint32_t), 0, 0)) {
    return false;
  }
  out.blit(buf.data());
  return true;
}

}  // namespace acl::gfx
