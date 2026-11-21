// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test (docs/adr/ADR-0009 C3, video). No GPL-3.0
// sources.
//
// Where a video frame goes on screen. Under --split the rectangle comes
// from the app process and the result goes to wp_viewport, which ends the
// connection on a crop that is empty or outside the buffer. So what is
// tested is less the placement than that no input yields such a crop.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>

#include "wm/video_geometry.h"

using acl::wm::place_video;
using acl::wm::VideoPlacement;

namespace {

// What wp_viewport and wl_subsurface accept.
void require_acceptable(const VideoPlacement& p,
                        int win_w,
                        int win_h,
                        std::uint32_t buf_w,
                        std::uint32_t buf_h) {
  if (!p.visible)
    return;
  REQUIRE(p.w > 0);
  REQUIRE(p.h > 0);
  REQUIRE(p.x >= 0);
  REQUIRE(p.y >= 0);
  REQUIRE(p.x + p.w <= win_w);
  REQUIRE(p.y + p.h <= win_h);
  REQUIRE(p.sx >= 0);
  REQUIRE(p.sy >= 0);
  REQUIRE(p.sw >= 1.0);
  REQUIRE(p.sh >= 1.0);
  REQUIRE(p.sx + p.sw <= static_cast<double>(buf_w));
  REQUIRE(p.sy + p.sh <= static_cast<double>(buf_h));
  // On the protocol's 1/256 grid, so nothing is rounded past the buffer.
  REQUIRE(p.sx * 256.0 == static_cast<double>(static_cast<long>(p.sx * 256)));
  REQUIRE(p.sw * 256.0 == static_cast<double>(static_cast<long>(p.sw * 256)));
}

}  // namespace

TEST_CASE("video geometry: a View inside the window shows the whole frame",
          "[video]") {
  const VideoPlacement p = place_video(120, 164, 640, 360, 1280, 800, 640, 360);
  REQUIRE(p.visible);
  CHECK(p.x == 120);
  CHECK(p.y == 164);
  CHECK(p.w == 640);
  CHECK(p.h == 360);
  CHECK(p.sx == 0);
  CHECK(p.sy == 0);
  CHECK(p.sw == 640);
  CHECK(p.sh == 360);
}

TEST_CASE("video geometry: a View larger than the window is cropped to it",
          "[video]") {
  // A zoom: 2560x1440 laid out at -640,0 in a 1280x800 window, 1920x1080
  // frame. The window sees the middle half across and the top 800/1440.
  const VideoPlacement p =
      place_video(-640, 0, 2560, 1440, 1280, 800, 1920, 1080);
  REQUIRE(p.visible);
  CHECK(p.x == 0);
  CHECK(p.y == 0);
  CHECK(p.w == 1280);
  CHECK(p.h == 800);
  CHECK(p.sx == 480);
  CHECK(p.sw == 960);
  CHECK(p.sy == 0);
  CHECK(p.sh == 600);
}

TEST_CASE("video geometry: a View off the window shows nothing", "[video]") {
  CHECK_FALSE(place_video(1280, 0, 640, 360, 1280, 800, 640, 360).visible);
  CHECK_FALSE(place_video(-640, 0, 640, 360, 1280, 800, 640, 360).visible);
  CHECK_FALSE(place_video(0, 800, 640, 360, 1280, 800, 640, 360).visible);
  CHECK_FALSE(place_video(0, 0, 0, 360, 1280, 800, 640, 360).visible);
  CHECK_FALSE(place_video(0, 0, 640, -1, 1280, 800, 640, 360).visible);
  CHECK_FALSE(place_video(0, 0, 640, 360, 0, 800, 640, 360).visible);
  CHECK_FALSE(place_video(0, 0, 640, 360, 1280, 800, 0, 360).visible);
}

TEST_CASE("video geometry: a crop of less than a buffer pixel shows nothing",
          "[video]") {
  // One window pixel of a 65536-wide View over a 16-pixel frame: 16/65536
  // of a pixel. wp_viewport would be given a zero-width source.
  CHECK_FALSE(place_video(-65535, 0, 65536, 360, 1280, 800, 16, 360).visible);
  CHECK_FALSE(place_video(0, -65535, 640, 65536, 1280, 800, 640, 16).visible);
}

TEST_CASE("video geometry: extreme rectangles do not overflow", "[video]") {
  const int big = 0x7fffffff;
  for (int x : {-big, -1, 0, 1, big}) {
    for (int w : {1, 65536, big}) {
      const VideoPlacement p = place_video(x, x, w, w, 1280, 800, 1920, 1080);
      require_acceptable(p, 1280, 800, 1920, 1080);
    }
  }
}

TEST_CASE("video geometry: no input yields a crop the compositor refuses",
          "[video]") {
  // A fixed seed: the same sequence every run, so a failure repeats.
  std::mt19937 rng(20261114);  // NOLINT(bugprone-random-generator-seed)
  const auto pick = [&](int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
  };
  int visible = 0;
  for (int i = 0; i < 200000; ++i) {
    const int win_w = pick(1, 4096), win_h = pick(1, 4096);
    const auto buf_w = static_cast<std::uint32_t>(pick(1, 16384));
    const auto buf_h = static_cast<std::uint32_t>(pick(1, 16384));
    const int w = pick(1, 65536), h = pick(1, 65536);
    const int x = pick(-65536, 65536), y = pick(-65536, 65536);
    const VideoPlacement p =
        place_video(x, y, w, h, win_w, win_h, buf_w, buf_h);
    require_acceptable(p, win_w, win_h, buf_w, buf_h);
    visible += p.visible;
  }
  CHECK(visible > 1000);  // the sweep reached the case that matters
}
