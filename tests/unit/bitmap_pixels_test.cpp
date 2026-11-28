// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test code; the pixel layouts are those of Android's
// public android.graphics.Color and <android/bitmap.h> documentation. No
// GPL-3.0 sources.
//
// What a4l's libjnigraphics.so hands a caller of AndroidBitmap_lockPixels, and
// takes back: RGBA_8888 bytes, from and to the Color ints Bitmap.getPixels and
// setPixels deal in. The buffer used to be the Color ints themselves, with red
// and blue in each other's place.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>

#include "ndk/bionic/acl_bitmap_pixels.h"

namespace {
std::array<std::uint8_t, 4> rgba_of(std::uint32_t color, bool premul) {
  std::array<std::uint8_t, 4> px{};
  std::memcpy(px.data(), &color, 4);
  acl_colors_to_rgba(px.data(), 1, premul ? 1 : 0);
  return px;
}

std::uint32_t color_of(std::array<std::uint8_t, 4> px, bool premul) {
  acl_rgba_to_colors(px.data(), 1, premul ? 1 : 0);
  std::uint32_t color = 0;
  std::memcpy(&color, px.data(), 4);
  return color;
}
}  // namespace

TEST_CASE("An opaque Color int becomes the bytes R, G, B, A", "[ndk][bitmap]") {
  const auto px = rgba_of(0xFF2060C0u, true);
  CHECK(px[0] == 0x20);
  CHECK(px[1] == 0x60);
  CHECK(px[2] == 0xC0);
  CHECK(px[3] == 0xFF);
  CHECK(color_of({0x20, 0x60, 0xC0, 0xFF}, true) == 0xFF2060C0u);
}

TEST_CASE("A translucent color is multiplied by its alpha on the way out",
          "[ndk][bitmap]") {
  // Half-transparent red: 255 * 128 / 255.
  const auto px = rgba_of(0x80FF0000u, true);
  CHECK(px[0] == 0x80);
  CHECK(px[1] == 0x00);
  CHECK(px[2] == 0x00);
  CHECK(px[3] == 0x80);
  // And divided by it on the way back.
  CHECK(color_of({0x80, 0x00, 0x00, 0x80}, true) == 0x80FF0000u);
  CHECK(color_of({0x00, 0x40, 0x00, 0x80}, true) == 0x80008000u);
}

TEST_CASE("A Bitmap that is not premultiplied keeps its colors",
          "[ndk][bitmap]") {
  const auto px = rgba_of(0x80FF4020u, false);
  CHECK(px[0] == 0xFF);
  CHECK(px[1] == 0x40);
  CHECK(px[2] == 0x20);
  CHECK(px[3] == 0x80);
  CHECK(color_of({0xFF, 0x40, 0x20, 0x80}, false) == 0x80FF4020u);
}

TEST_CASE("No alpha is black, and a color past its alpha is held at full",
          "[ndk][bitmap]") {
  CHECK(color_of({0x12, 0x34, 0x56, 0x00}, true) == 0x00000000u);
  CHECK(color_of({0xFF, 0x10, 0x00, 0x10}, true) == 0x10FFFF00u);
}

TEST_CASE("Every opaque color comes back as it went out", "[ndk][bitmap]") {
  std::array<std::uint32_t, 5> colors = {0xFF000000u, 0xFFFFFFFFu, 0xFF2060C0u,
                                         0xFFC06020u, 0xFF010203u};
  std::array<std::uint8_t, 20> buf{};
  std::memcpy(buf.data(), colors.data(), buf.size());
  acl_colors_to_rgba(buf.data(), colors.size(), 1);
  acl_rgba_to_colors(buf.data(), colors.size(), 1);
  std::array<std::uint32_t, 5> back{};
  std::memcpy(back.data(), buf.data(), buf.size());
  CHECK(back == colors);
}
