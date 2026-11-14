// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test (docs/adr/ADR-0009 C5). No GPL-3.0 sources.
//
// The checks the --split broker makes on what the app process sends: is the
// descriptor a dma-buf and how large, does the frame fit it and the surface,
// is a label fit to show. Pure functions, so every refusal is reached here
// without a compositor. The broker applying them is in
// split_present_test.cpp.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "wm/split_validate.h"

using namespace acl;
using wm::FrameVerdict;

namespace {

constexpr std::uint32_t kAr24 = 0x34325241u;
constexpr std::uint32_t kXr24 = 0x34325258u;
constexpr std::uint64_t kTiled = 0x0200000000401b03ull;

// A 640x480 ARGB frame in slot 0, and the limits and facts it passes under.
ipc::PresentFrame good_frame() {
  ipc::PresentFrame p{};
  p.w = 640;
  p.h = 480;
  p.drm_format = kAr24;
  p.modifier = kTiled;
  p.stride = 2560;
  p.plane_count = 1;
  return p;
}
const std::vector<std::uint64_t> kAdvertised = {0x11, kTiled};
wm::FrameLimits limits() {
  wm::FrameLimits l;
  l.have_surface = true;
  l.surface_w = 640;
  l.surface_h = 480;
  l.modifiers = kAdvertised;
  l.slots = 64;
  return l;
}
constexpr wm::BufferFacts kBuffer{true, 2560ull * 480};

FrameVerdict check(const ipc::PresentFrame& p) {
  return wm::check_frame(p, true, kBuffer, limits());
}

// A real dma-buf of `bytes`, from the udmabuf driver, or -1 where this user
// cannot open it.
int real_dmabuf(std::size_t bytes) {
  const int dev = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
  if (dev < 0)
    return -1;
  const int mem = ::memfd_create("udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  int out = -1;
  if (mem >= 0 && ::ftruncate(mem, static_cast<off_t>(bytes)) == 0 &&
      ::fcntl(mem, F_ADD_SEALS, F_SEAL_SHRINK) == 0) {
    udmabuf_create create{};
    create.memfd = static_cast<std::uint32_t>(mem);
    create.flags = UDMABUF_FLAGS_CLOEXEC;
    create.size = bytes;
    out = ::ioctl(dev, UDMABUF_CREATE, &create);
  }
  if (mem >= 0)
    ::close(mem);
  ::close(dev);
  return out;
}

}  // namespace

TEST_CASE("validate: a well-formed frame passes", "[split][validate]") {
  CHECK(check(good_frame()) == FrameVerdict::Ok);

  ipc::PresentFrame p = good_frame();
  p.drm_format = kXr24;
  CHECK(check(p) == FrameVerdict::Ok);
  p = good_frame();
  p.modifier = 0;  // linear needs no advertisement
  CHECK(check(p) == FrameVerdict::Ok);
  p.modifier = 0x00ffffffffffffffull;  // "no explicit modifier"
  CHECK(check(p) == FrameVerdict::Ok);
  p = good_frame();
  p.plane_count = 0;  // an older sender's single plane
  CHECK(check(p) == FrameVerdict::Ok);
  p = good_frame();
  p.buffer_id = static_cast<std::uint32_t>(-1);  // the presenter picks
  CHECK(check(p) == FrameVerdict::Ok);
  p.buffer_id = 63;
  CHECK(check(p) == FrameVerdict::Ok);
}

TEST_CASE("validate: each way a frame is refused", "[split][validate]") {
  const ipc::PresentFrame ok = good_frame();

  SECTION("before any surface, and without a descriptor") {
    wm::FrameLimits none = limits();
    none.have_surface = false;
    CHECK(wm::check_frame(ok, true, kBuffer, none) == FrameVerdict::NoSurface);
    CHECK(wm::check_frame(ok, false, {}, limits()) == FrameVerdict::NoBuffer);
  }
  SECTION("a descriptor that is not a dma-buf, or has no size") {
    CHECK(wm::check_frame(ok, true, {false, 1 << 22}, limits()) ==
          FrameVerdict::NotDmabuf);
    CHECK(wm::check_frame(ok, true, {true, 0}, limits()) ==
          FrameVerdict::NotDmabuf);
  }
  SECTION("a size that is not the surface's") {
    for (const auto& [w, h] :
         std::vector<std::pair<std::uint32_t, std::uint32_t>>{
             {0, 480},
             {640, 0},
             {641, 480},
             {640, 479},
             {1280, 960},
             {16385, 480},
             {0xffffffffu, 0xffffffffu}}) {
      ipc::PresentFrame p = ok;
      p.w = w;
      p.h = h;
      p.stride = 0xffffffffu;  // not what refuses it
      INFO(w << "x" << h);
      CHECK(check(p) == FrameVerdict::BadSize);
    }
  }
  SECTION("a format the presenter does not import") {
    for (const std::uint32_t format :
         {0u, 0x3231564eu /* NV12 */, 0x34324241u /* AB24 */, 0xffffffffu}) {
      ipc::PresentFrame p = ok;
      p.drm_format = format;
      CHECK(check(p) == FrameVerdict::BadFormat);
    }
  }
  SECTION("a modifier the compositor did not advertise") {
    for (const std::uint64_t modifier :
         {0x12ull, 0x0100000000000002ull, ~0ull}) {
      ipc::PresentFrame p = ok;
      p.modifier = modifier;
      CHECK(check(p) == FrameVerdict::BadModifier);
    }
    // With nothing advertised, only linear and "invalid" remain.
    wm::FrameLimits bare = limits();
    bare.modifiers = {};
    CHECK(wm::check_frame(ok, true, kBuffer, bare) ==
          FrameVerdict::BadModifier);
  }
  SECTION("too many planes, or an auxiliary plane outside the buffer") {
    ipc::PresentFrame p = ok;
    p.plane_count = 5;
    CHECK(check(p) == FrameVerdict::BadPlanes);
    p.plane_count = 0xffffffffu;
    CHECK(check(p) == FrameVerdict::BadPlanes);
    p = ok;
    p.plane_count = 2;
    p.plane_stride[0] = 0;
    p.plane_offset[0] = 16;
    CHECK(check(p) == FrameVerdict::BadPlanes);  // no stride
    p.plane_stride[0] = 64;
    p.plane_offset[0] = static_cast<std::uint32_t>(kBuffer.size);
    CHECK(check(p) == FrameVerdict::BadPlanes);  // starts at the end
    p.plane_offset[0] = static_cast<std::uint32_t>(kBuffer.size) - 1;
    CHECK(check(p) == FrameVerdict::Ok);
    p.plane_count = 4;  // planes 2 and 3 are zero
    CHECK(check(p) == FrameVerdict::BadPlanes);
  }
  SECTION("a stride or offset that does not fit the buffer") {
    ipc::PresentFrame p = ok;
    p.stride = 2559;  // a row does not hold 640 pixels
    CHECK(check(p) == FrameVerdict::BadLayout);
    p.stride = 0;
    CHECK(check(p) == FrameVerdict::BadLayout);
    p = ok;
    p.offset = 1;  // the last row ends one byte past the buffer
    CHECK(check(p) == FrameVerdict::BadLayout);
    p = ok;
    p.stride = 2561;
    CHECK(check(p) == FrameVerdict::BadLayout);
    // Values whose 32-bit product wraps to something small.
    p = ok;
    p.stride = 0x80000000u;
    CHECK(check(p) == FrameVerdict::BadLayout);
    p.stride = 0xffffffffu;
    p.offset = 0xffffffffu;
    CHECK(check(p) == FrameVerdict::BadLayout);
    // Exactly full is fine.
    wm::BufferFacts roomy{true, 2560ull * 480 + 4096};
    p = ok;
    p.offset = 4096;
    CHECK(wm::check_frame(p, true, roomy, limits()) == FrameVerdict::Ok);
    p.offset = 4097;
    CHECK(wm::check_frame(p, true, roomy, limits()) == FrameVerdict::BadLayout);
  }
  SECTION("a slot outside the ring") {
    for (const std::uint32_t id :
         {64u, 1000u, 0x7fffffffu, static_cast<std::uint32_t>(-2),
          0x80000000u}) {
      ipc::PresentFrame p = ok;
      p.buffer_id = id;
      CHECK(check(p) == FrameVerdict::BadSlot);
    }
  }
  // Every verdict has a name of its own.
  for (int v = 0; v < wm::kFrameVerdictCount; ++v)
    CHECK(std::string(wm::verdict_name(static_cast<FrameVerdict>(v))) !=
          "unknown");
}

TEST_CASE("validate: the kernel says what a descriptor is",
          "[split][validate]") {
  CHECK_FALSE(wm::inspect_buffer(-1).is_dmabuf);

  // Things an app process can send that are not dma-bufs.
  const int mem = ::memfd_create("not-a-dmabuf", MFD_CLOEXEC);
  REQUIRE(mem >= 0);
  REQUIRE(::ftruncate(mem, 1 << 20) == 0);
  CHECK_FALSE(wm::inspect_buffer(mem).is_dmabuf);
  ::close(mem);

  int pipefd[2];
  REQUIRE(::pipe2(pipefd, O_CLOEXEC) == 0);
  CHECK_FALSE(wm::inspect_buffer(pipefd[0]).is_dmabuf);
  ::close(pipefd[0]);
  ::close(pipefd[1]);

  for (const char* path : {"/dev/null", "/proc/self/maps", "/"}) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    INFO(path);
    CHECK_FALSE(wm::inspect_buffer(fd).is_dmabuf);
    ::close(fd);
  }

  // And one that is, where this user may create it.
  const std::size_t bytes = 2560 * 480;  // a multiple of the page size
  const int buf = real_dmabuf(bytes);
  if (buf < 0) {
    WARN("no /dev/udmabuf for this user: a real dma-buf was not inspected");
    return;
  }
  const wm::BufferFacts facts = wm::inspect_buffer(buf);
  CHECK(facts.is_dmabuf);
  CHECK(facts.size == bytes);
  CHECK(wm::check_frame(good_frame(), true, facts, limits()) ==
        FrameVerdict::Ok);
  ipc::PresentFrame tall = good_frame();
  tall.offset = 4096;  // would read past the real buffer
  CHECK(wm::check_frame(tall, true, facts, limits()) ==
        FrameVerdict::BadLayout);
  ::close(buf);
}

TEST_CASE("validate: surface sizes", "[split][validate]") {
  ipc::SurfaceInfo s{};
  s.w = 1280;
  s.h = 800;
  CHECK(wm::check_surface(s));
  s.w = 16384;
  s.h = 16384;
  CHECK(wm::check_surface(s));
  for (const auto& [w, h] :
       std::vector<std::pair<std::uint32_t, std::uint32_t>>{
           {0, 800},
           {1280, 0},
           {16385, 800},
           {1280, 16385},
           {0xffffffffu, 1},
           {0x80000000u, 0x80000000u}}) {
    s.w = w;
    s.h = h;
    INFO(w << "x" << h);
    CHECK_FALSE(wm::check_surface(s));
  }
}

TEST_CASE("validate: a label is made fit to show", "[split][validate]") {
  using wm::clean_label;
  CHECK(clean_label("Clock", 256, "a4l") == "Clock");
  CHECK(clean_label("", 256, "a4l") == "a4l");
  CHECK(clean_label("Clock\x01\x7f\n\t", 256, "a4l") == "Clock");
  CHECK(clean_label("\x1b]0;evil\x07", 256, "a4l") == "]0;evil");
  // Valid multi-byte text is kept.
  CHECK(clean_label("Caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80",
                    256, "a4l") ==
        "Caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80");
  // Invalid UTF-8 is dropped byte by byte: a stray continuation, a truncated
  // sequence, an overlong "/", a surrogate, a value past U+10FFFF.
  CHECK(clean_label("a\x80z", 256, "x") == "az");
  CHECK(clean_label("a\xe6\x97", 256, "x") == "a");
  CHECK(clean_label("a\xc0\xafz", 256, "x") == "az");
  CHECK(clean_label("a\xed\xa0\x80z", 256, "x") == "az");
  CHECK(clean_label("a\xf4\x90\x80\x80z", 256, "x") == "az");
  CHECK(clean_label("\xff\xfe\xfd", 256, "x") == "x");
  // C1 controls, and the characters that reorder or hide text: a
  // right-to-left override would show "gpj.exe" as "exe.jpg".
  CHECK(clean_label("a\xc2\x9bz", 256, "x") == "az");
  CHECK(clean_label("photo\xe2\x80\xae"
                    "gpj.exe",
                    256, "x") == "photogpj.exe");
  CHECK(clean_label("a\xe2\x80\x8b\xe2\x81\xa6z\xef\xbb\xbf", 256, "x") ==
        "az");
  // The cut never lands inside a character.
  CHECK(clean_label("ab\xc3\xa9", 3, "x") == "ab");
  CHECK(clean_label("ab\xc3\xa9", 4, "x") == "ab\xc3\xa9");
  CHECK(clean_label(std::string(1000, 'a'), 256, "x").size() == 256);
  // An embedded NUL is a control character like any other.
  CHECK(clean_label(std::string("a\0b", 3), 256, "x") == "ab");
}
