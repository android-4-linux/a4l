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

// ── Video frames ─────────────────────────────────────────────────────────

namespace {

constexpr std::uint32_t kNv12 = 0x3231564eu;
constexpr std::uint32_t kP010 = 0x30313050u;
const std::vector<std::uint64_t> kVideoAdvertised = {0, kTiled};

// A 640x360 NV12 frame: luma, then chroma at 640 * 360.
ipc::VideoFrame good_video() {
  ipc::VideoFrame v{};
  v.w = 640;
  v.h = 360;
  v.drm_format = kNv12;
  v.plane_count = 2;
  v.modifier = 0;
  v.stride[0] = 640;
  v.stride[1] = 640;
  v.offset[1] = 640 * 360;
  v.dest = {120, 164, 640, 360};
  return v;
}
constexpr wm::BufferFacts kVideoBuffer{true, 640ull * 360 * 3 / 2};

FrameVerdict check_video(const ipc::VideoFrame& v) {
  wm::VideoLimits l;
  l.have_surface = true;
  l.modifiers = kVideoAdvertised;
  return wm::check_video_frame(v, true, kVideoBuffer, l);
}

}  // namespace

TEST_CASE("validate: a well-formed video frame passes", "[split][validate]") {
  CHECK(check_video(good_video()) == FrameVerdict::Ok);

  // Not the surface's size: the picture is the decoder's.
  ipc::VideoFrame odd = good_video();
  odd.w = 639;
  odd.h = 359;  // 180 chroma rows of 640 bytes still fit
  CHECK(check_video(odd) == FrameVerdict::Ok);

  ipc::VideoFrame tiled = good_video();
  tiled.modifier = kTiled;
  CHECK(check_video(tiled) == FrameVerdict::Ok);

  // P010: two bytes a sample.
  ipc::VideoFrame deep = good_video();
  deep.drm_format = kP010;
  deep.w = 320;
  deep.h = 180;
  deep.stride[0] = deep.stride[1] = 640;
  deep.offset[1] = 640 * 180;
  CHECK(check_video(deep) == FrameVerdict::Ok);

  CHECK(wm::video_format_accepted(kNv12));
  CHECK(wm::video_format_accepted(kP010));
  CHECK_FALSE(wm::video_format_accepted(kAr24));
  CHECK_FALSE(wm::video_format_accepted(0));
}

TEST_CASE("validate: a converted video frame is one plane of pixels",
          "[split][validate]") {
  // What the child sends a compositor that takes no NV12: XBGR8888, linear.
  constexpr std::uint32_t kXb24 = 0x34324258u;
  ipc::VideoFrame v{};
  v.w = 320;
  v.h = 240;
  v.drm_format = kXb24;
  v.plane_count = 1;
  v.modifier = 0;
  v.stride[0] = 1280;
  v.dest = {0, 0, 640, 480};
  const wm::BufferFacts buffer{true, 1280ull * 240};
  wm::VideoLimits l;
  l.have_surface = true;
  l.modifiers = kVideoAdvertised;
  CHECK(wm::video_format_accepted(kXb24));
  CHECK(wm::check_video_frame(v, true, buffer, l) == FrameVerdict::Ok);

  ipc::VideoFrame narrow = v;
  narrow.stride[0] = 1279;  // a row does not fit
  CHECK(wm::check_video_frame(narrow, true, buffer, l) ==
        FrameVerdict::BadLayout);
  ipc::VideoFrame tall = v;
  tall.h = 241;  // the last row ends past the buffer
  CHECK(wm::check_video_frame(tall, true, buffer, l) ==
        FrameVerdict::BadLayout);
  ipc::VideoFrame late = v;
  late.offset[0] = 4;
  CHECK(wm::check_video_frame(late, true, buffer, l) ==
        FrameVerdict::BadLayout);
  ipc::VideoFrame none = v;
  none.plane_count = 0;
  CHECK(wm::check_video_frame(none, true, buffer, l) ==
        FrameVerdict::BadPlanes);
  ipc::VideoFrame unlisted = v;
  unlisted.modifier = 0x33;
  CHECK(wm::check_video_frame(unlisted, true, buffer, l) ==
        FrameVerdict::BadModifier);
  wm::VideoLimits nothing = l;
  nothing.modifiers = {};
  CHECK(wm::check_video_frame(v, true, buffer, nothing) ==
        FrameVerdict::BadFormat);
}

TEST_CASE("validate: each way a video frame is refused", "[split][validate]") {
  using V = FrameVerdict;
  const auto with = [](auto spoil) {
    ipc::VideoFrame v = good_video();
    spoil(v);
    return check_video(v);
  };
  wm::VideoLimits ok;
  ok.have_surface = true;
  ok.modifiers = kVideoAdvertised;

  SECTION("no surface, no descriptor, not a dma-buf") {
    wm::VideoLimits none = ok;
    none.have_surface = false;
    CHECK(wm::check_video_frame(good_video(), true, kVideoBuffer, none) ==
          V::NoSurface);
    CHECK(wm::check_video_frame(good_video(), false, kVideoBuffer, ok) ==
          V::NoBuffer);
    CHECK(wm::check_video_frame(good_video(), true, {false, 1 << 20}, ok) ==
          V::NotDmabuf);
    CHECK(wm::check_video_frame(good_video(), true, {true, 0}, ok) ==
          V::NotDmabuf);
  }
  SECTION("size") {
    CHECK(with([](auto& v) { v.w = 0; }) == V::BadSize);
    CHECK(with([](auto& v) { v.h = 0; }) == V::BadSize);
    CHECK(with([](auto& v) { v.w = ipc::kMaxSurfaceDim + 1; }) == V::BadSize);
    CHECK(with([](auto& v) { v.dest.w = 0; }) == V::BadSize);
    CHECK(with([](auto& v) { v.dest.h = -1; }) == V::BadSize);
    CHECK(with([](auto& v) { v.dest.x = ipc::kMaxVideoCoord + 1; }) ==
          V::BadSize);
    CHECK(with([](auto& v) { v.dest.y = -ipc::kMaxVideoCoord - 1; }) ==
          V::BadSize);
    CHECK(with([](auto& v) { v.dest.w = 0x7fffffff; }) == V::BadSize);
  }
  SECTION("format and modifier") {
    CHECK(with([](auto& v) { v.drm_format = kAr24; }) == V::BadFormat);
    CHECK(with([](auto& v) { v.drm_format = 0; }) == V::BadFormat);
    CHECK(with([](auto& v) { v.modifier = 0x33; }) == V::BadModifier);
    // "No explicit modifier" is refused unless the compositor listed it.
    CHECK(with([](auto& v) { v.modifier = 0x00ffffffffffffffull; }) ==
          V::BadModifier);
    // A compositor that advertised nothing for the format takes none of it,
    // linear included.
    wm::VideoLimits nothing = ok;
    nothing.modifiers = {};
    CHECK(wm::check_video_frame(good_video(), true, kVideoBuffer, nothing) ==
          V::BadFormat);
  }
  SECTION("planes") {
    CHECK(with([](auto& v) { v.plane_count = 0; }) == V::BadPlanes);
    CHECK(with([](auto& v) { v.plane_count = 1; }) == V::BadPlanes);
    CHECK(with([](auto& v) { v.plane_count = ipc::kMaxPlanes + 1; }) ==
          V::BadPlanes);
    CHECK(with([](auto& v) { v.plane_count = 0xffffffffu; }) == V::BadPlanes);
    // A third plane must have a stride and start inside the buffer.
    CHECK(with([](auto& v) { v.plane_count = 3; }) == V::BadPlanes);
    CHECK(with([](auto& v) {
            v.plane_count = 3;
            v.stride[2] = 64;
            v.offset[2] = 1u << 30;
          }) == V::BadPlanes);
    CHECK(with([](auto& v) {
            v.plane_count = 3;
            v.stride[2] = 64;
            v.offset[2] = 4096;
          }) == V::Ok);
  }
  SECTION("layout") {
    CHECK(with([](auto& v) { v.stride[0] = 639; }) == V::BadLayout);
    CHECK(with([](auto& v) { v.stride[1] = 0; }) == V::BadLayout);
    CHECK(with([](auto& v) { v.stride[1] = 639; }) == V::BadLayout);
    // The luma plane's last row would end one byte past the buffer.
    CHECK(with([](auto& v) { v.offset[0] = 640 * 180 + 1; }) == V::BadLayout);
    // Planes that overlap are the sender's loss, not a reach outside it.
    CHECK(with([](auto& v) { v.offset[0] = 1; }) == V::Ok);
    CHECK(with([](auto& v) { v.offset[1] += 1; }) == V::BadLayout);
    CHECK(with([](auto& v) { v.offset[1] = 0xffffffffu; }) == V::BadLayout);
    // Would wrap in 32 bits.
    CHECK(with([](auto& v) { v.stride[0] = 0x80000000u; }) == V::BadLayout);
    CHECK(with([](auto& v) { v.stride[1] = 0xffffffffu; }) == V::BadLayout);
    // A taller picture than the buffer holds.
    CHECK(with([](auto& v) { v.h = 361; }) == V::BadLayout);
    // P010 rows are twice as wide.
    CHECK(with([](auto& v) { v.drm_format = kP010; }) == V::BadLayout);
  }
}

TEST_CASE("validate: video rectangles", "[split][validate]") {
  CHECK(wm::check_video_rect({0, 0, 1, 1}));
  CHECK(wm::check_video_rect({-640, 0, 2560, 1440}));
  CHECK(wm::check_video_rect({ipc::kMaxVideoCoord, -ipc::kMaxVideoCoord,
                              ipc::kMaxVideoCoord, ipc::kMaxVideoCoord}));
  CHECK_FALSE(wm::check_video_rect({0, 0, 0, 1}));
  CHECK_FALSE(wm::check_video_rect({0, 0, 1, 0}));
  CHECK_FALSE(wm::check_video_rect({0, 0, -5, 5}));
  CHECK_FALSE(wm::check_video_rect({0, 0, ipc::kMaxVideoCoord + 1, 1}));
  CHECK_FALSE(wm::check_video_rect({ipc::kMaxVideoCoord + 1, 0, 1, 1}));
  CHECK_FALSE(wm::check_video_rect({0, -0x7fffffff - 1, 1, 1}));
  CHECK_FALSE(wm::check_video_rect({0x7fffffff, 0x7fffffff, 0x7fffffff, 1}));
}

// ── Audio ────────────────────────────────────────────────────────────────

TEST_CASE("validate: audio streams", "[split][validate]") {
  const auto open = [](std::uint32_t stream, std::uint32_t rate,
                       std::uint32_t channels) {
    ipc::AudioOpen o{};
    o.stream = stream;
    o.rate = rate;
    o.channels = channels;
    return wm::check_audio_open(o);
  };
  CHECK(open(0, 44100, 2));
  CHECK(open(ipc::kMaxAudioStreams - 1, 48000, 1));
  CHECK(open(3, ipc::kMinAudioRate, ipc::kMaxAudioChannels));
  CHECK(open(3, ipc::kMaxAudioRate, 6));
  CHECK_FALSE(open(ipc::kMaxAudioStreams, 44100, 2));
  CHECK_FALSE(open(0xffffffffu, 44100, 2));
  CHECK_FALSE(open(0, 0, 2));
  CHECK_FALSE(open(0, ipc::kMinAudioRate - 1, 2));
  CHECK_FALSE(open(0, ipc::kMaxAudioRate + 1, 2));
  CHECK_FALSE(open(0, 0xffffffffu, 2));
  CHECK_FALSE(open(0, 44100, 0));
  CHECK_FALSE(open(0, 44100, ipc::kMaxAudioChannels + 1));
  CHECK_FALSE(open(0, 44100, 0x80000000u));
}

TEST_CASE("validate: audio writes are whole frames", "[split][validate]") {
  CHECK(wm::check_audio_write(2, 1));
  CHECK(wm::check_audio_write(4, 2));
  CHECK(wm::check_audio_write(16384, 2));
  CHECK(wm::check_audio_write(12, 6));
  CHECK_FALSE(wm::check_audio_write(0, 2));  // nothing
  CHECK_FALSE(wm::check_audio_write(1, 1));  // half a sample
  CHECK_FALSE(wm::check_audio_write(2, 2));  // half a frame
  CHECK_FALSE(wm::check_audio_write(6, 2));  // a frame and a half
  CHECK_FALSE(wm::check_audio_write(10, 6));
  CHECK_FALSE(wm::check_audio_write(4, 0));  // no channels
  CHECK_FALSE(wm::check_audio_write(64, ipc::kMaxAudioChannels + 1));
  CHECK_FALSE(wm::check_audio_write(ipc::kMaxText + 2, 1));  // over a datagram
}
