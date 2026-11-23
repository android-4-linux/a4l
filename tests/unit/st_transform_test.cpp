// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test. No GPL-3.0 sources.
//
// gfx::st_transform, SurfaceTexture.getTransformMatrix for a decoder's frame
// bound whole to an external texture: texture coordinate (0, 0) must land on
// the picture's bottom left and (1, 1) on its top right, a texel in from an
// edge that has buffer beyond it.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "acl_config.h"

#if ACL_HAVE_GL

#include "gfx/dmabuf_frame.h"
#include "gfx/surface_texture_gl.h"

namespace {

using acl::gfx::DmabufFrame;

// (s, t) through the column-major matrix, as an app's shader multiplies it.
void apply(const float m[16], float s, float t, float* out_s, float* out_t) {
  *out_s = m[0] * s + m[4] * t + m[12];
  *out_t = m[1] * s + m[5] * t + m[13];
}

DmabufFrame frame(std::uint32_t w,
                  std::uint32_t h,
                  std::uint32_t cx,
                  std::uint32_t cy) {
  DmabufFrame f{};
  f.width = w;
  f.height = h;
  f.crop_x = cx;
  f.crop_y = cy;
  return f;
}

}  // namespace

TEST_CASE("st_transform: an uncropped frame is the Y flip",
          "[gfx][surface_texture]") {
  float m[16];
  acl::gfx::st_transform(frame(640, 360, 0, 0), m);
  const float flip[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1};
  for (int i = 0; i < 16; ++i)
    CHECK(m[i] == Catch::Approx(flip[i]));
}

TEST_CASE("st_transform: a crop at the top left", "[gfx][surface_texture]") {
  // 640x360 starting at (16, 12): the image is 656x372, the right and bottom
  // edges are the image's own, the left and top have buffer beyond them.
  float m[16];
  acl::gfx::st_transform(frame(640, 360, 16, 12), m);
  float s = 0, t = 0;
  apply(m, 0, 0, &s, &t);  // the picture's bottom left
  CHECK(s * 656 == Catch::Approx(17.0F));
  CHECK(t * 372 == Catch::Approx(372.0F));
  apply(m, 1, 1, &s, &t);  // its top right
  CHECK(s * 656 == Catch::Approx(656.0F));
  CHECK(t * 372 == Catch::Approx(13.0F));
  CHECK(m[10] == Catch::Approx(1.0F));
  CHECK(m[15] == Catch::Approx(1.0F));
}

TEST_CASE("st_transform: a degenerate frame is the identity",
          "[gfx][surface_texture]") {
  float m[16];
  acl::gfx::st_transform(frame(0, 0, 0, 0), m);
  for (int i = 0; i < 16; ++i)
    CHECK(m[i] == Catch::Approx(i % 5 == 0 ? 1.0F : 0.0F));
}

TEST_CASE("st_yuv_frame: every plane, its own fd or the frame's",
          "[gfx][surface_texture]") {
  DmabufFrame f = frame(640, 360, 0, 0);
  f.fd = 7;
  f.plane_count = 2;
  f.planes[0] = {0, 640, -1};
  f.planes[1] = {0, 640, 9};
  f.drm_format = 0x3231564e;  // NV12
  f.yuv_matrix = 1;
  f.yuv_full_range = true;
  const AclStYuvFrame y = acl::gfx::st_yuv_frame(f);
  CHECK(y.plane_count == 2);
  CHECK(y.fd[0] == 7);
  CHECK(y.fd[1] == 9);
  CHECK(y.fd[2] == -1);
  CHECK(y.stride[1] == 640);
  CHECK(y.yuv_matrix == 1);
  CHECK(y.yuv_full_range == 1);
  CHECK(y.transform[5] == Catch::Approx(-1.0F));
}

TEST_CASE("st_yuv_frame: a matrix the stream does not say is settled",
          "[gfx][surface_texture]") {
  DmabufFrame f = frame(640, 360, 0, 0);
  f.yuv_matrix = 2;                                  // not said
  CHECK(acl::gfx::st_yuv_frame(f).yuv_matrix == 6);  // BT.601 up to 720x576
  f = frame(1920, 1080, 0, 0);
  f.yuv_matrix = 2;
  CHECK(acl::gfx::st_yuv_frame(f).yuv_matrix == 1);  // BT.709 above
  f.yuv_matrix = 9;
  CHECK(acl::gfx::st_yuv_frame(f).yuv_matrix == 9);
}

#endif  // ACL_HAVE_GL
