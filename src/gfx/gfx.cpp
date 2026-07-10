// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/gfx.md (pending), AOSP android.graphics Canvas/Bitmap JNI
// surface, Skia API, CTS. No GPL-3.0 sources.
//
// The gfx rasterizer: a self-contained software ARGB_8888 raster backing
// android.graphics.Canvas/Bitmap. Deterministic and headless-verifiable; a pinned
// Skia backend (ADR-4) replaces these ops behind the same Surface when vendored.

#include "gfx/gfx.h"

#include <algorithm>

namespace aclcxx::gfx {

Surface::Surface(int width, int height)
    : width_(std::max(0, width)),
      height_(std::max(0, height)),
      pixels_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0) {}

void Surface::draw_color(std::uint32_t argb) {
  std::fill(pixels_.begin(), pixels_.end(), argb);
}

void Surface::fill_rect(int left, int top, int right, int bottom, std::uint32_t argb) {
  const int l = std::clamp(std::min(left, right), 0, width_);
  const int t = std::clamp(std::min(top, bottom), 0, height_);
  const int r = std::clamp(std::max(left, right), 0, width_);
  const int b = std::clamp(std::max(top, bottom), 0, height_);
  for (int y = t; y < b; ++y) {
    for (int x = l; x < r; ++x) {
      pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
              static_cast<std::size_t>(x)] = argb;
    }
  }
}

std::uint32_t Surface::pixel(int x, int y) const {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
  return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                 static_cast<std::size_t>(x)];
}

const char* module_name() noexcept { return "gfx"; }

}  // namespace aclcxx::gfx
