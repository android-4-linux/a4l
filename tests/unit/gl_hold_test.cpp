// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test (docs/adr/ADR-0009 C3). No GPL-3.0 sources.
//
// Hold mode of the gbm GL window: what lets a --split child hand its GL
// frames to another process. Each exported buffer stays locked until it is
// named in release_buffer(), and each carries a stable index. Needs a DRM
// render node; skipped where there is none (CI).

#include <catch2/catch_test_macros.hpp>

#include "acl_config.h"

#if ACL_HAVE_GL

#include <sys/stat.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "gfx/gl_egl.h"

using namespace acl;

namespace {

std::uint64_t inode_of(int fd) {
  struct stat st{};
  REQUIRE(::fstat(fd, &st) == 0);
  return static_cast<std::uint64_t>(st.st_ino);
}

}  // namespace

TEST_CASE("gl: a gbm window in hold mode keeps every exported buffer",
          "[gl][split]") {
  std::string err;
  std::unique_ptr<gfx::GlDevice> dev = gfx::GlDevice::CreateGbm(&err);
  if (!dev)
    SKIP("no gbm GL device here: " << err);
  std::unique_ptr<gfx::GlWindow> win =
      dev->make_window(64, 64, nullptr, {}, &err);
  REQUIRE(win);
  REQUIRE(win->make_current(&err));

  // Default mode: one front buffer, no index, nothing held.
  gfx::DmabufFrame f;
  REQUIRE(win->swap(&f, &err));
  CHECK(f.buffer_index == -1);
  CHECK(f.fd >= 0);
  CHECK(win->held_buffers() == 0);

  REQUIRE(win->set_hold_buffers(true));
  // Three swaps with no release: three different buffers, all held, each a
  // different dma-buf.
  std::map<int, std::uint64_t> identity;  // index -> dma-buf inode
  for (int i = 0; i < 3; ++i) {
    REQUIRE(win->swap(&f, &err));
    REQUIRE(f.buffer_index >= 0);
    REQUIRE(f.buffer_index < 4);
    CHECK(f.width == 64);
    CHECK(f.height == 64);
    CHECK(f.planes[0].stride >= 64u * 4);
    CHECK(identity.count(f.buffer_index) == 0);
    identity[f.buffer_index] = inode_of(f.fd);
    CHECK(win->held_buffers() == i + 1);
  }
  std::set<std::uint64_t> distinct;
  for (const auto& [index, inode] : identity)
    distinct.insert(inode);
  CHECK(distinct.size() == 3);

  // Releasing one makes it the driver's to draw into again, and when it
  // comes back it has the index and the dma-buf it had before.
  const int released = identity.begin()->first;
  win->release_buffer(released);
  CHECK(win->held_buffers() == 2);
  win->release_buffer(released);  // twice is harmless
  win->release_buffer(-1);
  win->release_buffer(99);
  CHECK(win->held_buffers() == 2);
  for (int i = 0; i < 6; ++i) {
    REQUIRE(win->swap(&f, &err));
    REQUIRE(f.buffer_index >= 0);
    const std::uint64_t inode = inode_of(f.fd);
    if (identity.count(f.buffer_index) != 0)
      CHECK(identity[f.buffer_index] == inode);
    else
      identity[f.buffer_index] = inode;
    // Keep two with the "consumer", give the rest back, as a presenter with
    // one frame on screen and one queued does.
    for (const auto& [index, unused] : identity) {
      if (index != f.buffer_index && win->held_buffers() > 2)
        win->release_buffer(index);
    }
    CHECK(win->held_buffers() <= 2);
  }
  CHECK(identity.size() <= 4);

  // Holding everything: the driver runs out of buffers, swap says so, and
  // nothing is lost. After a release it works again.
  int ok = 0;
  for (int i = 0; i < 8; ++i)
    ok += win->swap(&f, &err) ? 1 : 0;
  CHECK(ok <= 4);
  CHECK(win->held_buffers() <= 4);
  for (int i = 0; i < 4; ++i)
    win->release_buffer(i);
  CHECK(win->held_buffers() == 0);
  REQUIRE(win->swap(&f, &err));
  CHECK(win->held_buffers() == 1);

  // Leaving hold mode gives everything back.
  REQUIRE(win->set_hold_buffers(false));
  CHECK(win->held_buffers() == 0);
  REQUIRE(win->swap(&f, &err));
  CHECK(f.buffer_index == -1);
}

#else  // !ACL_HAVE_GL

TEST_CASE("gl: hold mode needs the GL build", "[gl][split]") {
  SKIP("built without EGL, GLES or gbm");
}

#endif
