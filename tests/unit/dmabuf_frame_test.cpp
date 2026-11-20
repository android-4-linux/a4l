// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test. No GPL-3.0 sources.
//
// gfx::DmabufFrame with a buffer per plane: which descriptor a plane is in,
// and the owned copy a holder makes of a frame (dup_frame, close_frame).

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gfx/dmabuf_frame.h"

using acl::gfx::DmabufFrame;

namespace {

int buffer() {
  const int fd = ::memfd_create("dmabuf-frame-test", MFD_CLOEXEC);
  REQUIRE(fd >= 0);
  return fd;
}

bool open_fd(int fd) {
  return ::fcntl(fd, F_GETFD) != -1;
}

}  // namespace

TEST_CASE("DmabufFrame: planes share the frame's buffer unless they name one",
          "[gfx][dmabuf]") {
  DmabufFrame f;
  f.fd = 7;
  f.plane_count = 2;
  CHECK(f.one_buffer());
  CHECK(f.plane_fd(0) == 7);
  CHECK(f.plane_fd(1) == 7);
  f.planes[1].fd = 7;  // names the same one
  CHECK(f.one_buffer());
  f.planes[1].fd = 9;
  CHECK_FALSE(f.one_buffer());
  CHECK(f.plane_fd(1) == 9);
  CHECK(f.plane_fd(DmabufFrame::kMaxPlanes) == 7);  // out of range: the frame's
}

TEST_CASE("DmabufFrame: an owned copy has every buffer, and closes them",
          "[gfx][dmabuf]") {
  const int luma = buffer(), chroma = buffer();
  DmabufFrame src;
  src.fd = luma;
  src.plane_count = 2;
  src.planes[1].fd = chroma;
  src.planes[1].offset = 5;
  src.width = 64;

  DmabufFrame own;
  REQUIRE(acl::gfx::dup_frame(src, &own));
  CHECK(own.width == 64);
  CHECK(own.planes[1].offset == 5);
  CHECK(own.fd != luma);
  CHECK(own.planes[1].fd != chroma);
  CHECK(own.planes[1].fd != own.fd);
  const int a = own.fd, b = own.planes[1].fd;
  CHECK(open_fd(a));
  CHECK(open_fd(b));
  acl::gfx::close_frame(&own);
  CHECK_FALSE(open_fd(a));
  CHECK_FALSE(open_fd(b));
  CHECK(own.fd == -1);
  CHECK(own.planes[1].fd == -1);
  // The source's are untouched.
  CHECK(open_fd(luma));
  CHECK(open_fd(chroma));

  // A plane that names the frame's own buffer costs no second descriptor.
  src.planes[1].fd = luma;
  REQUIRE(acl::gfx::dup_frame(src, &own));
  CHECK(own.planes[1].fd == -1);
  CHECK(own.one_buffer());
  acl::gfx::close_frame(&own);

  ::close(luma);
  ::close(chroma);
}

TEST_CASE("DmabufFrame: a copy that cannot be made leaves nothing open",
          "[gfx][dmabuf]") {
  const int luma = buffer();
  DmabufFrame src;
  src.fd = luma;
  src.plane_count = 2;
  src.planes[1].fd = 1 << 20;  // not a descriptor
  int before = 0, after = 0;
  for (int fd = 0; fd < 256; ++fd)
    before += open_fd(fd) ? 1 : 0;
  DmabufFrame own;
  own.width = 3;
  CHECK_FALSE(acl::gfx::dup_frame(src, &own));
  CHECK(own.width == 3);  // untouched
  for (int fd = 0; fd < 256; ++fd)
    after += open_fd(fd) ? 1 : 0;
  CHECK(after == before);

  DmabufFrame none;
  CHECK_FALSE(acl::gfx::dup_frame(none, &own));
  ::close(luma);
}

TEST_CASE("video_color: the stream's matrix, or one by the picture's size",
          "[gfx][dmabuf]") {
  using acl::gfx::YuvMatrix;
  acl::gfx::DmabufFrame f{};
  f.width = 640;
  f.height = 360;
  // Not said (2 is what a frame starts with): BT.601 up to 720x576.
  CHECK(f.yuv_matrix == 2);
  CHECK(acl::gfx::video_color(f).matrix == YuvMatrix::Bt601);
  CHECK_FALSE(acl::gfx::video_color(f).full_range);
  f.width = 720;
  f.height = 576;
  CHECK(acl::gfx::video_color(f).matrix == YuvMatrix::Bt601);
  // Above that, BT.709: wider, or taller.
  f.width = 1280;
  f.height = 544;
  CHECK(acl::gfx::video_color(f).matrix == YuvMatrix::Bt709);
  f.width = 704;
  f.height = 578;
  CHECK(acl::gfx::video_color(f).matrix == YuvMatrix::Bt709);

  // Said: the size no longer matters.
  f.width = 1920;
  f.height = 1080;
  for (const std::uint8_t bt601 : {5, 6}) {
    f.yuv_matrix = bt601;
    CHECK(acl::gfx::video_color(f).matrix == YuvMatrix::Bt601);
  }
  f.yuv_matrix = 9;
  CHECK(acl::gfx::video_color(f).matrix == YuvMatrix::Bt2020);
  f.width = 320;
  f.height = 240;
  f.yuv_matrix = 1;
  CHECK(acl::gfx::video_color(f).matrix == YuvMatrix::Bt709);

  // A matrix there is no conversion for here (identity, YCgCo, ...) is taken
  // as not said, and the range is the stream's either way.
  for (const std::uint8_t other : {0, 3, 4, 7, 8, 10, 255}) {
    f.yuv_matrix = other;
    CHECK(acl::gfx::video_color(f).matrix == YuvMatrix::Bt601);
  }
  f.yuv_full_range = true;
  CHECK(acl::gfx::video_color(f).full_range);
}
