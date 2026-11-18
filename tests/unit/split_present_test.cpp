// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test (docs/adr/ADR-0009 C2). No GPL-3.0 sources.
//
// The present path across the process boundary, with both ends in one process:
// the child's proxy presenter on one end of an acl_ipc socket pair, the broker
// on the other serving a fake presenter that stands in for the Wayland one.
// What a real compositor adds (a dma-buf import, real input) needs a GPU and
// host ART and is measured outside CI.

#include <catch2/catch_test_macros.hpp>

#include "acl_config.h"

#if ACL_HAVE_VULKAN && ACL_HAVE_WCS

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ipc/ipc.h"
#include "wm/split_present.h"

using namespace acl;

namespace {

// What the fake presenter saw, and the events the test feeds it. Shared
// between the test thread and the broker thread.
struct World {
  std::mutex mu;
  int wake = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  std::deque<std::function<void()>> pending;  // run inside dispatch()

  // Recorded on open.
  int opens = 0;
  int w = 0, h = 0;
  std::string title, app_id;
  bool want_ime = false;
  wm::InputCallback input;

  // Presenter state.
  int asked_w = 0, asked_h = 0;  // a size the compositor asks for; 0: none
  int taken_w = 0, taken_h = 0;  // the last one the broker took up
  std::vector<std::pair<int, int>> asked;  // every size it has asked for
  bool need_frame = true;
  std::uint32_t free_mask = 0b111;
  bool closed = false;

  // Recorded calls.
  struct Frame {
    std::uint32_t w, h, planes, stride1;
    std::uint64_t modifier;
    int slot;
    char byte;  // first byte read through the received fd
  };
  std::vector<Frame> frames;
  int violations = 0;  // frames that reached submit() and should not have
  int keepalives = 0;
  // The video plane.
  struct Video {
    std::uint32_t w, h, format, planes, stride1, offset1;
    std::uint64_t modifier;
    int x, y, dw, dh;
    char byte;
    char byte1;  // first byte of the chroma's own buffer, 0 if it has none
  };
  std::vector<Video> video;
  std::vector<std::array<int, 4>> video_bounds;
  int video_hides = 0, video_resets = 0, video_pumps = 0;
  int video_violations = 0;  // video frames that should not have got through
  std::vector<bool> ime;
  std::vector<std::string> clipboard;

  // Audio: what the broker's sinks were given.
  struct Stream {
    int rate = 0, channels = 0;
    std::vector<std::int16_t> samples;
    bool closed = false;
  };
  std::vector<Stream> streams;        // in the order they were opened
  std::uint64_t audio_pending = 0;    // what every sink reports as unplayed
  std::int64_t audio_latency_ns = 0;  // and as its device's latency
  bool audio_open_fails = false;
  int audio_violations = 0;  // writes that were not whole frames

  ~World() { ::close(wake); }

  // Queue `fn` to run on the broker thread, as a compositor event would.
  void post(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(mu);
      pending.push_back(std::move(fn));
    }
    const std::uint64_t one = 1;
    (void)!::write(wake, &one, sizeof(one));
  }
};

class FakePresenter final : public wm::IPresenter {
 public:
  explicit FakePresenter(World& world) : world_(world) {}

  int fd() noexcept override { return world_.wake; }
  int repeat_fd() noexcept override { return -1; }
  bool dispatch() noexcept override {
    std::uint64_t n = 0;
    (void)!::read(world_.wake, &n, sizeof(n));
    std::deque<std::function<void()>> run;
    {
      std::lock_guard<std::mutex> lock(world_.mu);
      run.swap(world_.pending);
    }
    for (auto& fn : run)
      fn();
    return !closed();
  }
  void dispatch_repeat() noexcept override {}
  bool ready() const noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    return world_.need_frame && world_.free_mask != 0;
  }
  int next_buffer() const noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    return world_.free_mask != 0 ? __builtin_ctz(world_.free_mask) : -1;
  }
  std::uint32_t free_mask() const noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    return world_.free_mask;
  }
  std::uint64_t frame_time_ns() const noexcept override { return 123456789; }
  bool last_present_feedback(std::uint64_t* present_ns,
                             std::uint32_t* refresh_ns,
                             std::uint32_t* flags) const noexcept override {
    *present_ns = 1000;
    *refresh_ns = 16'666'667;
    *flags = 5;
    return true;
  }
  const std::vector<std::uint64_t>& modifiers() const noexcept override {
    return modifiers_;
  }
  bool submit(const gfx::DmabufFrame& frame, int slot) noexcept override {
    char byte = 0;
    (void)!::pread(frame.fd, &byte, 1, 0);
    // What must hold for anything that gets this far, whatever the child
    // sent: this is where a compositor would be asked to import the buffer.
    struct stat st{};
    const bool file = ::fstat(frame.fd, &st) == 0 && S_ISREG(st.st_mode);
    const std::uint64_t end =
        frame.planes[0].offset +
        static_cast<std::uint64_t>(frame.planes[0].stride) * frame.height;
    std::lock_guard<std::mutex> lock(world_.mu);
    // The size the window opened at, or one the compositor has asked for
    // since. Which of those is checked by the test that resizes.
    const std::pair<int, int> size{static_cast<int>(frame.width),
                                   static_cast<int>(frame.height)};
    const bool sized = size == std::pair<int, int>{world_.w, world_.h} ||
                       std::find(world_.asked.begin(), world_.asked.end(),
                                 size) != world_.asked.end();
    if (!file || !sized ||
        (frame.drm_format != 0x34325241u && frame.drm_format != 0x34325258u) ||
        frame.planes[0].stride < frame.width * 4ull ||
        end > static_cast<std::uint64_t>(st.st_size) || frame.plane_count < 1 ||
        frame.plane_count > 4 || slot < -1 || slot > 63 ||
        (frame.modifier != 0 && frame.modifier != 0x00ffffffffffffffull &&
         std::find(modifiers_.begin(), modifiers_.end(), frame.modifier) ==
             modifiers_.end()))
      ++world_.violations;
    if (slot < 0 || slot > 2 || (world_.free_mask & (1u << slot)) == 0)
      return false;
    world_.free_mask &= ~(1u << slot);
    world_.need_frame = false;
    world_.frames.push_back({frame.width, frame.height, frame.plane_count,
                             frame.planes[1].stride, frame.modifier, slot,
                             byte});
    return true;
  }
  bool keepalive() noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    ++world_.keepalives;
    return true;
  }
  bool closed() const noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    return world_.closed;
  }
  bool take_resize(int* width, int* height) noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    if (world_.asked_w == 0)
      return false;
    *width = world_.taken_w = world_.asked_w;
    *height = world_.taken_h = world_.asked_h;
    world_.asked.emplace_back(world_.asked_w, world_.asked_h);
    world_.asked_w = world_.asked_h = 0;
    return true;
  }
  std::vector<std::uint64_t> modifiers_for(
      std::uint32_t drm_format) const noexcept override {
    // NV12 only, linear and one tiled modifier; P010 is not advertised.
    if (drm_format == 0x3231564eu)
      return {0, 0x0200000000401b03ull};
    return {};
  }
  void present_video(const gfx::DmabufFrame& frame,
                     int x,
                     int y,
                     int w,
                     int h) noexcept override {
    char byte = 0;
    (void)!::pread(frame.fd, &byte, 1, 0);
    struct stat st{};
    const bool file = ::fstat(frame.fd, &st) == 0 && S_ISREG(st.st_mode);
    const auto size = static_cast<std::uint64_t>(st.st_size);
    const std::uint64_t luma_end =
        frame.planes[0].offset +
        static_cast<std::uint64_t>(frame.planes[0].stride) * frame.height;
    const std::uint64_t chroma_end =
        frame.planes[1].offset +
        static_cast<std::uint64_t>(frame.planes[1].stride) *
            ((frame.height + 1) / 2);
    // The chroma's own buffer, where the frame has one.
    char byte1 = 0;
    std::uint64_t chroma_size = size;
    bool file1 = true;
    if (!frame.one_buffer()) {
      struct stat st1{};
      file1 = ::fstat(frame.plane_fd(1), &st1) == 0 && S_ISREG(st1.st_mode);
      chroma_size = static_cast<std::uint64_t>(st1.st_size);
      (void)!::pread(frame.plane_fd(1), &byte1, 1, 0);
    }
    std::lock_guard<std::mutex> lock(world_.mu);
    // What must hold for any video frame that reaches the compositor.
    if (!file || !file1 || frame.drm_format != 0x3231564eu ||
        frame.width == 0 || frame.height == 0 || frame.width > 16384 ||
        frame.height > 16384 || frame.plane_count < 2 ||
        frame.plane_count > 4 ||
        (frame.modifier != 0 && frame.modifier != 0x0200000000401b03ull) ||
        frame.planes[0].stride < frame.width ||
        frame.planes[1].stride < (frame.width + 1) / 2 * 2 || luma_end > size ||
        chroma_end > chroma_size || w <= 0 || h <= 0 || w > 65536 ||
        h > 65536 || x < -65536 || x > 65536 || y < -65536 || y > 65536)
      ++world_.video_violations;
    world_.video.push_back({frame.width, frame.height, frame.drm_format,
                            frame.plane_count, frame.planes[1].stride,
                            frame.planes[1].offset, frame.modifier, x, y, w, h,
                            byte, byte1});
  }
  void hide_video() noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    ++world_.video_hides;
  }
  void set_video_bounds(int x, int y, int w, int h) noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    if (w <= 0 || h <= 0 || w > 65536 || h > 65536 || x < -65536 || x > 65536 ||
        y < -65536 || y > 65536)
      ++world_.video_violations;
    world_.video_bounds.push_back({x, y, w, h});
  }
  void pump_video() noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    ++world_.video_pumps;
  }
  void reset_video_overlay() noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    ++world_.video_resets;
  }
  void set_ime_visible(bool visible) noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    world_.ime.push_back(visible);
  }
  void set_clipboard(const std::string& utf8) noexcept override {
    std::lock_guard<std::mutex> lock(world_.mu);
    world_.clipboard.push_back(utf8);
  }

 private:
  World& world_;
  std::vector<std::uint64_t> modifiers_{0x11, 0x22, 0x0200000000401b03ull};
};

// What the broker plays a child's audio through: records what it is given.
class FakeSink final : public audio::Sink {
 public:
  explicit FakeSink(World& world) : world_(world) {}
  bool open(const audio::Format& fmt) override {
    std::lock_guard<std::mutex> lock(world_.mu);
    if (world_.audio_open_fails)
      return false;
    index_ = world_.streams.size();
    world_.streams.push_back({fmt.sample_rate, fmt.channels, {}, false});
    channels_ = fmt.channels;
    return true;
  }
  int write(const std::int16_t* pcm, int n_samples) override {
    std::lock_guard<std::mutex> lock(world_.mu);
    if (n_samples <= 0 || channels_ <= 0 || n_samples % channels_ != 0)
      ++world_.audio_violations;
    auto& s = world_.streams[index_].samples;
    s.insert(s.end(), pcm, pcm + n_samples);
    return n_samples;
  }
  void drain() override {}
  void close() override {
    std::lock_guard<std::mutex> lock(world_.mu);
    world_.streams[index_].closed = true;
  }
  std::uint64_t frames_written() const override { return 0; }
  std::uint64_t frames_pending() const override {
    std::lock_guard<std::mutex> lock(world_.mu);
    return world_.audio_pending;
  }
  std::int64_t latency_ns() const override {
    std::lock_guard<std::mutex> lock(world_.mu);
    return world_.audio_latency_ns;
  }

 private:
  World& world_;
  std::size_t index_ = 0;
  int channels_ = 0;
};

// A broker on its own thread over one end of a socket pair; the test holds
// the other end, as the child does.
// Tests have no dma-buf to send. The broker is told that whatever regular
// file arrives is one, of the file's real size; "not a dma-buf" is tested
// with the kernel's own answer in split_validate_test.cpp.
wm::BufferFacts facts_of_any_file(int fd) {
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    return {};
  return {true, static_cast<std::uint64_t>(st.st_size)};
}

struct Rig {
  World world;
  ipc::Channel child;
  ipc::Channel broker_end;
  std::thread thread;
  wm::BrokerStats stats;
  wm::BrokerOptions options;

  int linger = 200;

  // `start_now` false leaves the broker unstarted, so that a test can queue
  // messages and events for it to find all at once; call start().
  explicit Rig(int linger_ms = 200,
               wm::BrokerOptions opts = {},
               bool start_now = true) {
    options = std::move(opts);
    options.inspect = facts_of_any_file;
    options.audio_sink = [this]() -> std::unique_ptr<audio::Sink> {
      return std::make_unique<FakeSink>(world);
    };
    linger = linger_ms;
    auto pair = ipc::Channel::make_pair();
    child = std::move(pair.first);
    broker_end = std::move(pair.second);
    if (start_now)
      start();
  }
  void start() {
    const int linger_ms = linger;
    thread = std::thread([this, linger_ms] {
      const wm::PresenterFactory open =
          [this](int w, int h, std::string_view title, std::string_view app_id,
                 wm::InputCallback input,
                 bool want_ime) -> std::unique_ptr<wm::IPresenter> {
        std::lock_guard<std::mutex> lock(world.mu);
        ++world.opens;
        world.w = w;
        world.h = h;
        world.title = std::string(title);
        world.app_id = std::string(app_id);
        world.want_ime = want_ime;
        world.input = std::move(input);
        return std::make_unique<FakePresenter>(world);
      };
      stats = wm::serve_split_child(broker_end, open, options, linger_ms);
    });
  }
  // Disconnect the child and collect the broker's counters.
  wm::BrokerStats finish() {
    child.close();
    if (thread.joinable())
      thread.join();
    return stats;
  }
  ~Rig() { finish(); }
};

// Dispatch the proxy until `done` holds or 2 s pass.
bool pump(wm::IPresenter& p, const std::function<bool()>& done) {
  for (int i = 0; i < 200; ++i) {
    if (done())
      return true;
    pollfd fd{p.fd(), POLLIN, 0};
    ::poll(&fd, 1, 10);
    p.dispatch();
  }
  return done();
}

// Wait up to 2 s for the broker thread to have recorded something.
bool settle(World& world, const std::function<bool()>& done) {
  for (int i = 0; i < 200; ++i) {
    {
      std::lock_guard<std::mutex> lock(world.mu);
      if (done())
        return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// A memfd holding `byte`, standing in for a dma-buf, large enough for the
// frame frame_of() describes.
int buffer_fd(char byte) {
  const int fd = ::memfd_create("frame", MFD_CLOEXEC);
  REQUIRE(fd >= 0);
  REQUIRE(::write(fd, &byte, 1) == 1);
  REQUIRE(::ftruncate(fd, 2 * 1024 * 1024) == 0);
  return fd;
}

gfx::DmabufFrame frame_of(int fd) {
  gfx::DmabufFrame f{};
  f.fd = fd;
  f.width = 640;
  f.height = 480;
  f.plane_count = 2;
  f.planes[0].stride = 2560;
  f.planes[1].stride = 1280;
  f.planes[1].offset = 4096;
  f.modifier = 0x0200000000401b03ull;
  f.drm_format = 0x34325241;  // AR24
  return f;
}

}  // namespace

TEST_CASE("split: the proxy opens a surface through the broker", "[split]") {
  Rig rig;
  auto p = wm::split_presenter_open_on(rig.child, 640, 480, "Clock\x01\x7f",
                                       "org.example.clock", {}, true);
  REQUIRE(p != nullptr);
  CHECK(p->modifiers() ==
        std::vector<std::uint64_t>{0x11, 0x22, 0x0200000000401b03ull});
  CHECK(p->repeat_fd() == -1);
  std::lock_guard<std::mutex> lock(rig.world.mu);
  CHECK(rig.world.opens == 1);
  CHECK(rig.world.w == 640);
  CHECK(rig.world.h == 480);
  CHECK(rig.world.title == "Clock");  // control bytes dropped
  CHECK(rig.world.app_id == "org.example.clock");
  CHECK(rig.world.want_ime);
}

TEST_CASE("split: a frame crosses with its fd, and the slot ring is mirrored",
          "[split]") {
  Rig rig;
  auto p =
      wm::split_presenter_open_on(rig.child, 640, 480, "t", "a", {}, false);
  REQUIRE(p != nullptr);

  // The fake starts ready with all three slots free; the proxy learns that.
  REQUIRE(pump(*p, [&] { return p->ready(); }));
  CHECK(p->next_buffer() == 0);
  CHECK(p->free_mask() == 0b111);
  CHECK(p->frame_time_ns() == 123456789);
  std::uint64_t present_ns = 0;
  std::uint32_t refresh_ns = 0, flags = 0;
  REQUIRE(p->last_present_feedback(&present_ns, &refresh_ns, &flags));
  CHECK(present_ns == 1000);
  CHECK(refresh_ns == 16'666'667);
  CHECK(flags == 5);

  const int fd = buffer_fd('Q');
  REQUIRE(p->submit(frame_of(fd), 0));
  ::close(fd);  // the broker's copy keeps the buffer alive
  // Before the broker says anything: not ready, slot 0 busy, slot 1 next.
  CHECK_FALSE(p->ready());
  CHECK(p->next_buffer() == 1);

  // The compositor's frame callback, with slot 0 still on screen. It cannot
  // precede the commit, so wait for the broker to have submitted.
  REQUIRE(settle(rig.world, [&] { return rig.world.frames.size() == 1; }));
  rig.world.post([&] {
    std::lock_guard<std::mutex> lock(rig.world.mu);
    rig.world.need_frame = true;
  });
  REQUIRE(pump(*p, [&] { return p->ready(); }));
  CHECK(p->next_buffer() == 1);
  CHECK(p->free_mask() == 0b110);

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.frames == 1);
  CHECK(stats.rejected == 0);
  REQUIRE(rig.world.frames.size() == 1);
  const World::Frame& f = rig.world.frames[0];
  CHECK(f.w == 640);
  CHECK(f.h == 480);
  CHECK(f.planes == 2);
  CHECK(f.stride1 == 1280);
  CHECK(f.modifier == 0x0200000000401b03ull);
  CHECK(f.slot == 0);
  CHECK(f.byte == 'Q');  // the received fd is the sender's buffer
}

TEST_CASE("split: the first size is the child's before it draws", "[split]") {
  Rig rig;
  {
    // The compositor's first configure names a size other than the one the
    // child opens at (a tile, an embedded shell's view).
    std::lock_guard<std::mutex> lock(rig.world.mu);
    rig.world.asked_w = 800;
    rig.world.asked_h = 600;
  }
  auto p =
      wm::split_presenter_open_on(rig.child, 640, 480, "t", "a", {}, false);
  REQUIRE(p != nullptr);
  // It came ahead of the broker's answer: no dispatch is needed to have it.
  int w = 0, h = 0;
  REQUIRE(p->take_resize(&w, &h));
  CHECK(w == 800);
  CHECK(h == 600);
  CHECK_FALSE(p->take_resize(&w, &h));

  // The first frame is that size, and the broker takes it.
  REQUIRE(pump(*p, [&] { return p->ready(); }));
  const int fd = buffer_fd('a');
  gfx::DmabufFrame f = frame_of(fd);
  f.width = 800;
  f.height = 600;
  f.planes[0].stride = 800 * 4;
  REQUIRE(p->submit(f, 0));
  ::close(fd);
  REQUIRE(settle(rig.world, [&] { return rig.world.frames.size() == 1; }));
  std::lock_guard<std::mutex> lock(rig.world.mu);
  CHECK(rig.world.frames.back().w == 800);
  CHECK(rig.world.frames.back().h == 600);
}

TEST_CASE("split: a resize is the compositor's, one size at a time",
          "[split]") {
  Rig rig;
  auto p =
      wm::split_presenter_open_on(rig.child, 640, 480, "t", "a", {}, false);
  REQUIRE(p != nullptr);
  REQUIRE(pump(*p, [&] { return p->ready(); }));
  int w = 0, h = 0;
  CHECK_FALSE(p->take_resize(&w, &h));  // nothing asked for yet

  const auto ask = [&](int aw, int ah) {
    rig.world.post([&rig, aw, ah] {
      std::lock_guard<std::mutex> lock(rig.world.mu);
      rig.world.asked_w = aw;
      rig.world.asked_h = ah;
    });
  };
  // Send a frame of the given size.
  const auto send = [&](std::uint32_t fw, std::uint32_t fh, char byte) {
    REQUIRE(pump(*p, [&] { return p->ready(); }));
    const int fd = buffer_fd(byte);
    gfx::DmabufFrame f = frame_of(fd);
    f.width = fw;
    f.height = fh;
    f.planes[0].stride = fw * 4;
    REQUIRE(p->submit(f, 0));
    ::close(fd);
  };
  // The compositor's frame callback and release, once the broker has dealt
  // with the frame: they cannot precede its commit.
  const auto release = [&] {
    rig.world.post([&rig] {
      std::lock_guard<std::mutex> lock(rig.world.mu);
      rig.world.need_frame = true;
      rig.world.free_mask = 0b111;
    });
  };
  const auto shown = [&] {
    std::lock_guard<std::mutex> lock(rig.world.mu);
    return rig.world.frames.size();
  };
  const auto took = [&] { return p->take_resize(&w, &h); };

  ask(800, 600);
  REQUIRE(pump(*p, took));
  CHECK(w == 800);
  CHECK(h == 600);
  CHECK_FALSE(p->take_resize(&w, &h));  // told once

  // Until the child follows, its old size is still the window's, and a size
  // nobody asked for is not.
  send(640, 480, 'a');
  REQUIRE(settle(rig.world, [&] { return rig.world.frames.size() == 1; }));
  release();
  send(700, 500, 'b');
  REQUIRE(settle(rig.world, [&] { return rig.world.keepalives == 1; }));
  release();
  CHECK(shown() == 1);

  // The compositor moves on before the child has followed: the child is not
  // told the next size until a frame of the first has arrived.
  ask(720, 540);
  REQUIRE(settle(rig.world, [&] { return rig.world.taken_w == 720; }));
  CHECK_FALSE(pump(*p, took));

  send(800, 600, 'c');
  REQUIRE(settle(rig.world, [&] { return rig.world.frames.size() == 2; }));
  release();
  REQUIRE(pump(*p, took));
  CHECK(w == 720);
  CHECK(h == 540);

  // The size left behind is no longer the window's.
  send(640, 480, 'd');
  REQUIRE(settle(rig.world, [&] { return rig.world.keepalives == 2; }));
  release();
  send(720, 540, 'e');
  REQUIRE(settle(rig.world, [&] { return rig.world.frames.size() == 3; }));
  release();

  // Back to the size the child already has: nothing to tell it.
  ask(720, 540);
  REQUIRE(settle(rig.world, [&] { return rig.world.taken_w == 720; }));
  CHECK_FALSE(pump(*p, took));

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.frames == 3);
  CHECK(stats.resizes == 2);
  CHECK(stats.rejected == 2);
  CHECK(stats.frame_verdicts[static_cast<std::size_t>(
            wm::FrameVerdict::BadSize)] == 2);
  CHECK(rig.world.violations == 0);
  REQUIRE(rig.world.frames.size() == 3);
  CHECK(rig.world.frames[1].w == 800);
  CHECK(rig.world.frames[2].w == 720);
}

TEST_CASE("split: input reaches the proxy's callback unchanged", "[split]") {
  Rig rig;
  std::vector<wm::InputEvent> got;
  auto p = wm::split_presenter_open_on(
      rig.child, 640, 480, "t", "a",
      [&](const wm::InputEvent& ev) { got.push_back(ev); }, false);
  REQUIRE(p != nullptr);

  rig.world.post([&] {
    rig.world.input(
        wm::PointerInput{wm::PointerInput::Action::Down, 12.5f, 34.0f});
    rig.world.input(wm::PointerInput{wm::PointerInput::Action::Up, 13.f, 35.f});
    rig.world.input(wm::KeyInput{wm::KeyInput::Action::Down, 30, 0x61});
    rig.world.input(wm::TextInput{"h\xc3\xa9llo", true, 3});
    rig.world.input(wm::DeleteSurroundingText{2, 1});
  });
  REQUIRE(pump(*p, [&] { return got.size() == 5; }));

  const auto& down = std::get<wm::PointerInput>(got[0]);
  CHECK(down.action == wm::PointerInput::Action::Down);
  CHECK(down.x == 12.5f);
  CHECK(down.y == 34.0f);
  CHECK(std::get<wm::PointerInput>(got[1]).action ==
        wm::PointerInput::Action::Up);
  const auto& key = std::get<wm::KeyInput>(got[2]);
  CHECK(key.action == wm::KeyInput::Action::Down);
  CHECK(key.keycode == 30);
  CHECK(key.keysym == 0x61);
  const auto& text = std::get<wm::TextInput>(got[3]);
  CHECK(text.text == "h\xc3\xa9llo");
  CHECK(text.preedit);
  CHECK(text.cursor == 3);
  const auto& del = std::get<wm::DeleteSurroundingText>(got[4]);
  CHECK(del.before == 2);
  CHECK(del.after == 1);
  CHECK(rig.finish().inputs == 5);
}

TEST_CASE("split: keepalive, IME and clipboard are forwarded", "[split]") {
  Rig rig;
  auto p = wm::split_presenter_open_on(rig.child, 640, 480, "t", "a", {}, true);
  REQUIRE(p != nullptr);
  CHECK_FALSE(p->keepalive());  // nothing submitted yet: nothing to keep alive

  REQUIRE(pump(*p, [&] { return p->ready(); }));
  const int fd = buffer_fd('x');
  REQUIRE(p->submit(frame_of(fd), 0));
  ::close(fd);
  CHECK(p->keepalive());
  p->set_ime_visible(true);
  p->set_ime_visible(false);
  p->set_clipboard("copied");

  rig.finish();
  CHECK(rig.world.keepalives == 1);
  CHECK(rig.world.ime == std::vector<bool>{true, false});
  CHECK(rig.world.clipboard == std::vector<std::string>{"copied"});
}

// A 640x360 NV12 frame in the memfd buffer_fd() makes.
gfx::DmabufFrame video_of(int fd) {
  gfx::DmabufFrame f{};
  f.fd = fd;
  f.width = 640;
  f.height = 360;
  f.plane_count = 2;
  f.planes[0].stride = 640;
  f.planes[1].stride = 640;
  f.planes[1].offset = 640 * 360;
  f.modifier = 0x0200000000401b03ull;
  f.drm_format = 0x3231564e;  // NV12
  return f;
}

TEST_CASE("split: video frames cross to the broker's video plane", "[split]") {
  Rig rig;
  auto p =
      wm::split_presenter_open_on(rig.child, 1280, 800, "t", "a", {}, false);
  REQUIRE(p != nullptr);
  // The proxy has no plane of its own to wake.
  CHECK(p->video_wake_fd() == -1);

  // The app lays out every frame; the rectangle is sent when it changes.
  p->set_video_bounds(120, 164, 640, 360);
  p->set_video_bounds(120, 164, 640, 360);
  p->set_video_bounds(-640, 0, 2560, 1440);

  // From another thread, as the decoder calls it.
  std::thread decoder([&] {
    for (char c : {'a', 'b', 'c'}) {
      const int fd = buffer_fd(c);
      p->present_video(video_of(fd), 0, 0, 640, 360);
      ::close(fd);  // the decoder reuses its descriptor at once
    }
  });
  decoder.join();
  p->hide_video();
  p->reset_video_overlay();
  // After a reset the same rectangle is news again.
  p->set_video_bounds(-640, 0, 2560, 1440);

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.video_frames == 3);
  CHECK(stats.rejected == 0);
  CHECK(stats.frames == 0);
  World& w = rig.world;
  REQUIRE(w.video.size() == 3);
  CHECK(w.video[0].byte == 'a');
  CHECK(w.video[2].byte == 'c');
  CHECK(w.video[1].w == 640);
  CHECK(w.video[1].h == 360);
  CHECK(w.video[1].format == 0x3231564eu);
  CHECK(w.video[1].planes == 2);
  CHECK(w.video[1].stride1 == 640);
  CHECK(w.video[1].offset1 == 640 * 360);
  CHECK(w.video[1].modifier == 0x0200000000401b03ull);
  CHECK(w.video[1].dw == 640);
  CHECK(w.video[1].dh == 360);
  // Each frame and the hide were shown at once, on the broker's thread.
  CHECK(w.video_pumps == 4);
  CHECK(w.video_hides == 1);
  CHECK(w.video_resets == 1);
  REQUIRE(w.video_bounds.size() == 3);
  CHECK(w.video_bounds[0] == std::array<int, 4>{120, 164, 640, 360});
  CHECK(w.video_bounds[1] == std::array<int, 4>{-640, 0, 2560, 1440});
  CHECK(w.video_bounds[2] == std::array<int, 4>{-640, 0, 2560, 1440});
  CHECK(w.video_violations == 0);
}

TEST_CASE("split: a video frame with a buffer per plane crosses with both",
          "[split]") {
  Rig rig;
  auto p =
      wm::split_presenter_open_on(rig.child, 1280, 800, "t", "a", {}, false);
  REQUIRE(p != nullptr);
  const int luma = buffer_fd('Y');
  const int chroma = buffer_fd('C');
  gfx::DmabufFrame f = video_of(luma);
  f.planes[1].offset = 0;
  f.planes[1].fd = chroma;
  p->present_video(f, 0, 0, 640, 360);
  // The same frame with the plane's descriptor a second name for the first
  // buffer's: one buffer, one descriptor sent.
  gfx::DmabufFrame same = video_of(luma);
  same.planes[1].fd = luma;
  p->present_video(same, 0, 0, 640, 360);
  // Three buffers are more than a datagram carries: not sent at all.
  const int third = buffer_fd('3');
  gfx::DmabufFrame spread = f;
  spread.plane_count = 3;
  spread.planes[2].stride = 64;
  spread.planes[2].fd = third;
  p->present_video(spread, 0, 0, 640, 360);
  ::close(luma);
  ::close(chroma);
  ::close(third);

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.video_frames == 2);
  CHECK(stats.rejected == 0);
  World& w = rig.world;
  REQUIRE(w.video.size() == 2);
  CHECK(w.video[0].byte == 'Y');
  CHECK(w.video[0].byte1 == 'C');
  CHECK(w.video[0].offset1 == 0);
  CHECK(w.video[1].byte == 'Y');
  CHECK(w.video[1].byte1 == 0);  // one buffer: no second was looked at
  CHECK(w.video[1].offset1 == 640 * 360);
  CHECK(w.video_violations == 0);
}

TEST_CASE("split: the broker checks each of a video frame's buffers",
          "[split]") {
  Rig rig;
  const int luma = buffer_fd('Y');
  // A second buffer of one page: room for the chroma of a 64x64 picture.
  const int small = ::memfd_create("chroma", MFD_CLOEXEC);
  REQUIRE(small >= 0);
  REQUIRE(::ftruncate(small, 4096) == 0);
  const int not_a_buffer = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  REQUIRE(not_a_buffer >= 0);
  auto p =
      wm::split_presenter_open_on(rig.child, 1280, 800, "t", "a", {}, false);
  REQUIRE(p != nullptr);
  const auto video = [&](std::uint32_t side,
                         const std::function<void(ipc::VideoFrame&)>& spoil,
                         int fd1) {
    ipc::Message m;
    m.type = ipc::MsgType::VideoFrame;
    ipc::VideoFrame& v = m.video;
    v.w = v.h = side;
    v.drm_format = 0x3231564e;
    v.plane_count = 2;
    v.stride[0] = v.stride[1] = side;
    v.buffer[1] = 1;
    v.dest = {0, 0, 640, 360};
    spoil(v);
    REQUIRE(rig.child.send(m, {}, luma, fd1));
  };
  const auto as_is = [](ipc::VideoFrame&) {};
  video(64, as_is, small);                                // fits: 2048 of 4096
  video(128, as_is, small);                               // 1: chroma is 8192
  video(64, [](auto& v) { v.offset[1] = 2049; }, small);  // 2: a byte past
  video(64, as_is, not_a_buffer);                         // 3
  video(64, as_is, -1);                                // 4: names one not sent
  video(64, [](auto& v) { v.buffer[1] = 0; }, small);  // 5: one not named
  video(64, [](auto& v) { v.buffer[0] = 1; }, small);  // 6
  ::close(luma);
  ::close(small);
  ::close(not_a_buffer);

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.video_frames == 1);
  CHECK(stats.rejected == 6);
  using V = wm::FrameVerdict;
  const auto count = [&](V v) {
    return stats.frame_verdicts[static_cast<std::size_t>(v)];
  };
  CHECK(count(V::BadLayout) == 2);
  CHECK(count(V::NotDmabuf) == 1);
  CHECK(count(V::BadPlanes) == 3);
  CHECK(rig.world.video_violations == 0);
}

TEST_CASE("split: the child is told of a video format the compositor refuses",
          "[split]") {
  Rig rig;
  auto p =
      wm::split_presenter_open_on(rig.child, 1280, 800, "t", "a", {}, false);
  REQUIRE(p != nullptr);
  constexpr std::uint32_t kNv12 = 0x3231564e, kP010 = 0x30313050;
  CHECK_FALSE(p->video_refused(kNv12));
  CHECK_FALSE(p->video_refused(kP010));
  CHECK_FALSE(p->video_refused(0));

  // The fake compositor lists NV12 and no P010.
  const int fd = buffer_fd('p');
  gfx::DmabufFrame deep = video_of(fd);
  deep.drm_format = kP010;
  p->present_video(deep, 0, 0, 640, 360);
  REQUIRE(pump(*p, [&] { return p->video_refused(kP010); }));
  CHECK_FALSE(p->video_refused(kNv12));

  // A format it takes is not reported, and a refusal is said once.
  p->present_video(video_of(fd), 0, 0, 640, 360);
  p->present_video(deep, 0, 0, 640, 360);
  ::close(fd);
  REQUIRE(settle(rig.world, [&] { return rig.world.video.size() == 1; }));
  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.video_frames == 1);
  CHECK(stats.rejected == 2);
  CHECK(p->video_refused(kP010));
  CHECK_FALSE(p->video_refused(kNv12));
}

TEST_CASE("split: the broker drops video that fails validation", "[split]") {
  Rig rig;
  const int fd = buffer_fd('v');
  const int not_a_buffer = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  REQUIRE(not_a_buffer >= 0);
  const auto video = [&](const std::function<void(ipc::VideoFrame&)>& spoil,
                         int with_fd) {
    ipc::Message m;
    m.type = ipc::MsgType::VideoFrame;
    ipc::VideoFrame& v = m.video;
    v.w = 640;
    v.h = 360;
    v.drm_format = 0x3231564e;
    v.plane_count = 2;
    v.stride[0] = v.stride[1] = 640;
    v.offset[1] = 640 * 360;
    v.dest = {0, 0, 640, 360};
    spoil(v);
    REQUIRE(rig.child.send(m, {}, with_fd));
  };
  const auto bounds = [&](ipc::VideoRect rect) {
    ipc::Message m;
    m.type = ipc::MsgType::VideoBounds;
    m.video_rect = rect;
    REQUIRE(rig.child.send(m));
  };
  const auto as_is = [](ipc::VideoFrame&) {};

  video(as_is, fd);  // 1: no surface yet
  // Before a surface these have nothing to act on; they are not errors.
  bounds({0, 0, 640, 360});
  ipc::Message hide;
  hide.type = ipc::MsgType::VideoHide;
  REQUIRE(rig.child.send(hide));

  auto p =
      wm::split_presenter_open_on(rig.child, 1280, 800, "t", "a", {}, false);
  REQUIRE(p != nullptr);
  using V = wm::FrameVerdict;
  video([](auto& v) { v.w = 0; }, fd);                        // 2
  video([](auto& v) { v.h = ipc::kMaxSurfaceDim + 1; }, fd);  // 3
  video([](auto& v) { v.dest.w = 0; }, fd);                   // 4
  video([](auto& v) { v.dest.x = 0x7fffffff; }, fd);          // 5
  video([](auto& v) { v.drm_format = 0x34325241; }, fd);      // 6: ARGB
  video([](auto& v) { v.drm_format = 0x30313050; }, fd);    // 7: P010, unlisted
  video([](auto& v) { v.modifier = 0x33; }, fd);            // 8
  video([](auto& v) { v.plane_count = 1; }, fd);            // 9
  video([](auto& v) { v.plane_count = 0x80000000u; }, fd);  // 10
  video([](auto& v) { v.stride[0] = 1; }, fd);              // 11
  video([](auto& v) { v.stride[1] = 0xffffffffu; }, fd);    // 12: would wrap
  video([](auto& v) { v.offset[1] = 4u << 20; }, fd);       // 13: past the end
  video(as_is, -1);                                         // 14: no fd
  video(as_is, not_a_buffer);                               // 15
  bounds({0, 0, 0, 0});                                     // 16
  bounds({0, 0, 0x7fffffff, 0x7fffffff});                   // 17
  bounds({-0x7fffffff, 0, 640, 360});                       // 18
  video(as_is, fd);  // and one that is fine
  ::close(fd);
  ::close(not_a_buffer);

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 18);
  CHECK(stats.video_frames == 1);
  const auto count = [&](V v) {
    return stats.frame_verdicts[static_cast<std::size_t>(v)];
  };
  CHECK(count(V::NoSurface) == 1);
  CHECK(count(V::BadSize) == 4);
  CHECK(count(V::BadFormat) == 2);
  CHECK(count(V::BadModifier) == 1);
  CHECK(count(V::BadPlanes) == 2);
  CHECK(count(V::BadLayout) == 3);
  CHECK(count(V::NoBuffer) == 1);
  CHECK(count(V::NotDmabuf) == 1);
  CHECK(rig.world.video.size() == 1);
  CHECK(rig.world.video_bounds.empty());
  CHECK(rig.world.video_violations == 0);
  // A refused video frame does not touch the window's own frame loop.
  CHECK(rig.world.keepalives == 0);
}

TEST_CASE("split: audio crosses to the broker's sink, sample for sample",
          "[split][audio]") {
  Rig rig;
  // No window is needed for sound.
  const int a = wm::split_audio_open_on(rig.child, 44100, 2);
  const int b = wm::split_audio_open_on(rig.child, 48000, 1);
  REQUIRE(a >= 0);
  REQUIRE(b >= 0);
  REQUIRE(a != b);

  // More than one datagram's worth, and a count that is not a whole frame:
  // the odd sample is not sent.
  std::vector<std::int16_t> stereo(40001);
  for (std::size_t i = 0; i < stereo.size(); ++i)
    stereo[i] = static_cast<std::int16_t>(i * 7 - 12345);
  CHECK(wm::split_audio_write_on(rig.child, a, stereo.data(),
                                 static_cast<int>(stereo.size())) == 40000);
  const std::int16_t mono[] = {1, -2, 3, -32768, 32767};
  // From another thread, as an app's audio thread writes.
  std::thread writer(
      [&] { CHECK(wm::split_audio_write_on(rig.child, b, mono, 5) == 5); });
  writer.join();
  // Not open, out of range, nothing to send.
  CHECK(wm::split_audio_write_on(rig.child, 5, mono, 5) == 0);
  CHECK(wm::split_audio_write_on(rig.child, -1, mono, 5) == 0);
  CHECK(wm::split_audio_write_on(rig.child, 99, mono, 5) == 0);
  CHECK(wm::split_audio_write_on(rig.child, b, mono, 0) == 0);
  CHECK(wm::split_audio_write_on(rig.child, b, nullptr, 5) == 0);
  wm::split_audio_close_on(rig.child, a);
  CHECK(wm::split_audio_write_on(rig.child, a, mono, 4) == 0);  // closed
  // The id is free again.
  const int c = wm::split_audio_open_on(rig.child, 22050, 2);
  CHECK(c == a);
  wm::split_audio_close_on(rig.child, c);
  wm::split_audio_close_on(rig.child, b);
  wm::split_audio_close_on(rig.child, b);  // twice is nothing

  // Formats no device takes are refused before anything is sent.
  CHECK(wm::split_audio_open_on(rig.child, 0, 2) == -1);
  CHECK(wm::split_audio_open_on(rig.child, 44100, 0) == -1);
  CHECK(wm::split_audio_open_on(rig.child, 44100, 9) == -1);
  CHECK(wm::split_audio_open_on(rig.child, 1 << 30, 2) == -1);

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 0);
  CHECK(stats.audio_frames == 20000 + 5);
  CHECK(stats.audio_dropped == 0);
  World& w = rig.world;
  REQUIRE(w.streams.size() == 3);
  CHECK(w.streams[0].rate == 44100);
  CHECK(w.streams[0].channels == 2);
  stereo.pop_back();
  CHECK(w.streams[0].samples == stereo);
  CHECK(w.streams[0].closed);
  CHECK(w.streams[1].rate == 48000);
  CHECK(w.streams[1].channels == 1);
  CHECK(w.streams[1].samples ==
        std::vector<std::int16_t>{1, -2, 3, -32768, 32767});
  CHECK(w.streams[1].closed);
  CHECK(w.streams[2].rate == 22050);
  CHECK(w.streams[2].samples.empty());
  CHECK(w.audio_violations == 0);
}

TEST_CASE("split: a child cannot hold more streams than the limit",
          "[split][audio]") {
  Rig rig;
  std::vector<int> ids;
  for (std::uint32_t i = 0; i < ipc::kMaxAudioStreams; ++i) {
    ids.push_back(wm::split_audio_open_on(rig.child, 44100, 2));
    CHECK(ids.back() == static_cast<int>(i));
  }
  CHECK(wm::split_audio_open_on(rig.child, 44100, 2) == -1);
  for (int id : ids)
    wm::split_audio_close_on(rig.child, id);
  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 0);
  CHECK(rig.world.streams.size() == ipc::kMaxAudioStreams);
}

TEST_CASE("split: the broker tells the child its device's latency",
          "[split][audio]") {
  Rig rig;
  const int id = wm::split_audio_open_on(rig.child, 48000, 2);
  REQUIRE(id >= 0);
  CHECK(wm::split_audio_latency_us(id) == -1);  // not yet told
  const std::vector<std::int16_t> pcm(96, 3);
  std::size_t written = 0;
  // Write, wait for the sink to have it, and take whatever the broker sent
  // back: the next AudioLatency, or nothing.
  const auto write_and_hear = [&]() -> std::optional<ipc::AudioLatency> {
    CHECK(wm::split_audio_write_on(rig.child, id, pcm.data(), 96) == 96);
    written += 96;
    REQUIRE(settle(rig.world, [&] {
      return !rig.world.streams.empty() &&
             rig.world.streams[0].samples.size() == written;
    }));
    // The report follows the write in the same handler; give it a moment.
    for (int i = 0; i < 20; ++i) {
      ipc::Received r;
      if (rig.child.recv(r, true)) {
        REQUIRE(r.msg.type == ipc::MsgType::AudioLatency);
        return r.msg.audio_latency;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
  };
  const auto set = [&](std::int64_t ns) {
    std::lock_guard<std::mutex> lock(rig.world.mu);
    rig.world.audio_latency_ns = ns;
  };

  set(-1);  // the device has not run
  CHECK_FALSE(write_and_hear().has_value());
  set(21'000'000);
  auto l = write_and_hear();
  REQUIRE(l.has_value());
  CHECK(l->stream == static_cast<std::uint32_t>(id));
  CHECK(l->latency_us == 21'000);
  CHECK_FALSE(write_and_hear().has_value());  // steady: said once
  set(22'000'000);                            // within the step
  CHECK_FALSE(write_and_hear().has_value());
  set(221'000'000);  // another device
  l = write_and_hear();
  REQUIRE(l.has_value());
  CHECK(l->latency_us == 221'000);
  set(INT64_MAX);  // a sink gone wrong is bounded before it is sent
  l = write_and_hear();
  REQUIRE(l.has_value());
  CHECK(l->latency_us == ipc::kMaxAudioLatencyUs);

  // The child's side: what arrives is what the stream's sink reads.
  wm::split_audio_note_latency(
      ipc::AudioLatency{static_cast<std::uint32_t>(id), 221'000});
  CHECK(wm::split_audio_latency_us(id) == 221'000);
  wm::split_audio_note_latency(
      ipc::AudioLatency{static_cast<std::uint32_t>(id), 0xffffffffu});
  CHECK(wm::split_audio_latency_us(id) == ipc::kMaxAudioLatencyUs);
  wm::split_audio_note_latency(
      ipc::AudioLatency{ipc::kMaxAudioStreams, 5});  // no such stream
  CHECK(wm::split_audio_latency_us(ipc::kMaxAudioStreams) == 0);
  CHECK(wm::split_audio_latency_us(-1) == 0);
  // A stream opened on the same id does not inherit the figure.
  wm::split_audio_close_on(rig.child, id);
  const int again = wm::split_audio_open_on(rig.child, 44100, 1);
  REQUIRE(again == id);
  CHECK(wm::split_audio_latency_us(again) == -1);
  wm::split_audio_close_on(rig.child, again);

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 0);
}

TEST_CASE("split: the broker drops audio that fails validation",
          "[split][audio]") {
  wm::BrokerOptions opts;
  opts.max_audio_opens = 4;
  Rig rig(200, opts);
  const auto open = [&](std::uint32_t stream, std::uint32_t rate,
                        std::uint32_t channels) {
    ipc::Message m;
    m.type = ipc::MsgType::AudioOpen;
    m.audio_open.stream = stream;
    m.audio_open.rate = rate;
    m.audio_open.channels = channels;
    REQUIRE(rig.child.send(m));
  };
  const auto write = [&](std::uint32_t stream, std::size_t bytes) {
    const std::vector<std::byte> pcm(bytes, std::byte{0x11});
    ipc::Message m;
    m.type = ipc::MsgType::AudioWrite;
    m.audio_write.stream = stream;
    m.extra_len = static_cast<std::uint32_t>(bytes);
    REQUIRE(rig.child.send(m, pcm));
  };
  const auto close = [&](std::uint32_t stream) {
    ipc::Message m;
    m.type = ipc::MsgType::AudioClose;
    m.u32 = stream;
    REQUIRE(rig.child.send(m));
  };

  write(0, 16);                           // 1: no such stream
  open(ipc::kMaxAudioStreams, 44100, 2);  // 2
  open(0, 0, 2);                          // 3
  open(0, 1u << 31, 2);                   // 4
  open(0, 44100, 0);                      // 5
  open(0, 44100, 0xffff);                 // 6
  open(1, 44100, 2);                      // accepted (1 of 4)
  write(1, 0);                            // 7: nothing
  write(1, 3);                            // 8: not whole samples
  write(1, 6);                            // 9: a frame and a half
  write(0xffffffffu, 16);                 // 10: no such id
  write(1, 16);                           // fine: 4 frames
  close(ipc::kMaxAudioStreams + 7);       // 11
  close(1);
  write(1, 16);       // 12: closed
  close(5);           // never opened: nothing to do
  open(1, 48000, 6);  // accepted (2 of 4)
  open(1, 8000, 1);   // accepted (3 of 4): replaces the last
  write(1, 10);       // fine for one channel: 5 frames
  open(2, 44100, 2);  // accepted (4 of 4)
  open(3, 44100, 2);  // 13: past the limit on opens
  open(4, 44100, 2);  // 14
  write(3, 16);       // 15: was never opened

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 15);
  CHECK(stats.audio_frames == 4 + 5);
  World& w = rig.world;
  REQUIRE(w.streams.size() == 4);
  CHECK(w.streams[0].samples.size() == 8);
  CHECK(w.streams[0].closed);
  CHECK(w.streams[1].channels == 6);
  CHECK(w.streams[1].closed);  // replaced by the reopen
  CHECK(w.streams[2].rate == 8000);
  CHECK(w.streams[2].samples.size() == 5);
  CHECK(w.audio_violations == 0);
}

TEST_CASE("split: audio ahead of the device is dropped, not held",
          "[split][audio]") {
  wm::BrokerOptions opts;
  opts.max_audio_seconds = 1;
  Rig rig(200, opts);
  const int id = wm::split_audio_open_on(rig.child, 8000, 1);
  REQUIRE(id >= 0);
  const std::vector<std::int16_t> pcm(1000, 7);
  CHECK(wm::split_audio_write_on(rig.child, id, pcm.data(), 1000) == 1000);
  REQUIRE(settle(rig.world, [&] {
    return !rig.world.streams.empty() &&
           rig.world.streams[0].samples.size() == 1000;
  }));
  {
    // The sink now holds all but 999 frames of its second.
    std::lock_guard<std::mutex> lock(rig.world.mu);
    rig.world.audio_pending = 8000 - 999;
  }
  CHECK(wm::split_audio_write_on(rig.child, id, pcm.data(), 1000) == 1000);
  CHECK(wm::split_audio_write_on(rig.child, id, pcm.data(), 999) == 999);
  wm::split_audio_close_on(rig.child, id);
  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 0);  // a child that is ahead is not malformed
  CHECK(stats.audio_dropped == 1000);
  CHECK(stats.audio_frames == 1000 + 999);
  CHECK(rig.world.streams[0].samples.size() == 1999);
}

TEST_CASE("split: a closed stream plays out before its sink is closed",
          "[split][audio]") {
  Rig rig;
  World& w = rig.world;
  const int id = wm::split_audio_open_on(rig.child, 8000, 1);
  REQUIRE(id >= 0);
  const std::vector<std::int16_t> pcm(800, 3);
  CHECK(wm::split_audio_write_on(rig.child, id, pcm.data(), 800) == 800);
  REQUIRE(settle(w, [&] {
    return !w.streams.empty() && w.streams[0].samples.size() == 800;
  }));
  {
    // The device has not taken them yet.
    std::lock_guard<std::mutex> lock(w.mu);
    w.audio_pending = 800;
  }
  // The app is done with the track the moment it has written it.
  wm::split_audio_close_on(rig.child, id);
  // The id is free for the child at once, and a new stream on it is a new
  // sink: the old one is still sounding.
  const int again = wm::split_audio_open_on(rig.child, 8000, 1);
  CHECK(again == id);
  REQUIRE(settle(w, [&] { return w.streams.size() == 2; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  {
    std::lock_guard<std::mutex> lock(w.mu);
    CHECK_FALSE(w.streams[0].closed);
    CHECK_FALSE(w.streams[1].closed);
    w.audio_pending = 0;  // played out
  }
  // Closed soon after it empties, with the broker otherwise idle.
  CHECK(settle(w, [&] { return w.streams[0].closed; }));
  {
    std::lock_guard<std::mutex> lock(w.mu);
    CHECK_FALSE(w.streams[1].closed);  // still open
  }
  wm::split_audio_close_on(rig.child, again);
  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 0);
  // Whatever was open or playing out when the child left is closed then.
  CHECK(w.streams[1].closed);
}

TEST_CASE("split: audio with no device is taken and not played",
          "[split][audio]") {
  Rig rig(200, {}, /*start_now=*/false);
  rig.world.audio_open_fails = true;
  rig.start();
  const int id = wm::split_audio_open_on(rig.child, 44100, 2);
  REQUIRE(id >= 0);
  const std::int16_t pcm[8] = {};
  CHECK(wm::split_audio_write_on(rig.child, id, pcm, 8) == 8);
  wm::split_audio_close_on(rig.child, id);
  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 0);
  CHECK(stats.audio_frames == 4);
  CHECK(rig.world.streams.empty());
}

TEST_CASE("split: the broker drops what fails validation", "[split]") {
  Rig rig;
  const int fd = buffer_fd('z');
  const int not_a_buffer = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  REQUIRE(not_a_buffer >= 0);
  // A frame the broker accepts, and one field of it changed at a time.
  const auto frame = [&](const std::function<void(ipc::PresentFrame&)>& spoil,
                         int with_fd) {
    ipc::Message m;
    m.type = ipc::MsgType::PresentFrame;
    m.present.w = 640;
    m.present.h = 480;
    m.present.drm_format = 0x34325241;  // AR24
    m.present.modifier = 0x0200000000401b03ull;
    m.present.plane_count = 1;
    m.present.stride = 2560;
    spoil(m.present);
    REQUIRE(rig.child.send(m, {}, with_fd));
  };
  const auto as_is = [](ipc::PresentFrame&) {};

  frame(as_is, fd);  // 1: no surface yet

  ipc::Message bad_surface;  // 2: a surface larger than any display
  bad_surface.type = ipc::MsgType::SurfaceCreate;
  bad_surface.surface.w = ipc::kMaxSurfaceDim + 1;
  bad_surface.surface.h = 480;
  REQUIRE(rig.child.send(bad_surface));
  ipc::Received r;
  REQUIRE(rig.child.recv(r));
  REQUIRE(r.msg.type == ipc::MsgType::SurfaceReady);
  CHECK(r.msg.surface_ready.ok == 0);

  auto p =
      wm::split_presenter_open_on(rig.child, 640, 480, "t", "a", {}, false);
  REQUIRE(p != nullptr);
  using V = wm::FrameVerdict;
  frame([](auto& f) { f.w = 0; }, fd);                        // 3
  frame([](auto& f) { f.h = ipc::kMaxSurfaceDim + 1; }, fd);  // 4
  frame([](auto& f) { f.w = 1280, f.h = 960; }, fd);  // 5: not the surface
  frame([](auto& f) { f.plane_count = ipc::kMaxPlanes + 1; }, fd);  // 6
  frame([](auto& f) { f.stride = 0; }, fd);                         // 7
  frame([](auto& f) { f.offset = 4u << 20; }, fd);        // 8: past the end
  frame([](auto& f) { f.stride = 0x80000000u; }, fd);     // 9: would wrap
  frame([](auto& f) { f.buffer_id = 64; }, fd);           // 10
  frame([](auto& f) { f.drm_format = 0x3231564e; }, fd);  // 11: NV12
  frame([](auto& f) { f.modifier = 0x33; }, fd);          // 12: not advertised
  frame(as_is, -1);                                       // 13: no fd
  frame(as_is, not_a_buffer);                             // 14: not a buffer
  ipc::Message unknown;  // 15: not a child message
  unknown.type = ipc::MsgType::FrameReleased;
  REQUIRE(rig.child.send(unknown));
  ipc::Message garbage;  // 16: no such tag
  garbage.type = static_cast<ipc::MsgType>(0xdeadbeef);
  REQUIRE(rig.child.send(garbage));
  ::close(fd);
  ::close(not_a_buffer);

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 16);
  CHECK(stats.frames == 0);
  const auto count = [&](V v) {
    return stats.frame_verdicts[static_cast<std::size_t>(v)];
  };
  CHECK(count(V::NoSurface) == 1);
  CHECK(count(V::BadSize) == 3);
  CHECK(count(V::BadPlanes) == 1);
  CHECK(count(V::BadLayout) == 3);
  CHECK(count(V::BadSlot) == 1);
  CHECK(count(V::BadFormat) == 1);
  CHECK(count(V::BadModifier) == 1);
  CHECK(count(V::NoBuffer) == 1);
  CHECK(count(V::NotDmabuf) == 1);
  CHECK(rig.world.frames.empty());
  CHECK(rig.world.violations == 0);
  CHECK(rig.world.opens == 1);
  // Each refused frame kept the frame loop alive instead of hanging it.
  CHECK(rig.world.keepalives == 12);  // frames 3 to 14
}

TEST_CASE("split: the window's app id is the broker's, not the child's",
          "[split]") {
  wm::BrokerOptions opts;
  opts.app_id = "org.verified.package";
  Rig rig(200, opts);
  auto p = wm::split_presenter_open_on(rig.child, 640, 480,
                                       "Sign in \xe2\x80\xae"
                                       "moc.knab",
                                       "org.mozilla.firefox", {}, false);
  REQUIRE(p != nullptr);
  std::lock_guard<std::mutex> lock(rig.world.mu);
  CHECK(rig.world.app_id == "org.verified.package");
  // The title is the child's, without the override that would reverse it.
  CHECK(rig.world.title == "Sign in moc.knab");
}

TEST_CASE("split: a child cannot open surfaces without end", "[split]") {
  wm::BrokerOptions opts;
  opts.max_surfaces = 3;
  Rig rig(200, opts);
  std::vector<std::uint32_t> answers;
  for (int i = 0; i < 6; ++i) {
    ipc::Message m;
    m.type = ipc::MsgType::SurfaceCreate;
    m.surface.w = 64;
    m.surface.h = 64;
    REQUIRE(rig.child.send(m));
    ipc::Received r;
    do {
      REQUIRE(rig.child.recv(r));
    } while (r.msg.type != ipc::MsgType::SurfaceReady);
    answers.push_back(r.msg.surface_ready.ok);
  }
  CHECK(answers == std::vector<std::uint32_t>{1, 1, 1, 0, 0, 0});
  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 3);
  CHECK(rig.world.opens == 3);
}

namespace {

int open_fd_count() {
  int n = 0;
  for (const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) {
    (void)e;
    ++n;
  }
  return n;
}

// One datagram of `bytes`, with `fds` attached, straight onto the socket: what
// a child that does not use ipc::Channel can send.
bool send_raw(int sock,
              const std::vector<std::byte>& bytes,
              const std::vector<int>& fds) {
  iovec iov{const_cast<std::byte*>(bytes.data()), bytes.size()};
  msghdr mh{};
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * 8)] = {};
  if (!fds.empty()) {
    mh.msg_control = cbuf;
    mh.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
    cmsghdr* cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
    std::memcpy(CMSG_DATA(cm), fds.data(), sizeof(int) * fds.size());
  }
  return ::sendmsg(sock, &mh, MSG_NOSIGNAL) >= 0;
}

}  // namespace

TEST_CASE("split: descriptors sent with malformed datagrams are not kept",
          "[split]") {
  const int fd = buffer_fd('k');
  const int before = open_fd_count();
  {
    Rig rig;
    auto p =
        wm::split_presenter_open_on(rig.child, 640, 480, "t", "a", {}, false);
    REQUIRE(p != nullptr);
    const int sock = rig.child.native_handle();
    for (int i = 0; i < 300; ++i) {
      // Too short to be a message, with descriptors attached.
      REQUIRE(send_raw(sock, std::vector<std::byte>(1 + i % 40), {fd, fd}));
      // A whole message with more descriptors than any message carries.
      ipc::Message m;
      m.type = ipc::MsgType::Keepalive;
      std::vector<std::byte> whole(sizeof(m));
      std::memcpy(whole.data(), &m, sizeof(m));
      REQUIRE(send_raw(sock, whole, {fd, fd, fd, fd, fd, fd}));
      // A refused frame with a buffer and a fence.
      ipc::Message f;
      f.type = ipc::MsgType::PresentFrame;
      REQUIRE(rig.child.send(f, {}, fd, fd));
    }
    p.reset();
    const wm::BrokerStats stats = rig.finish();
    CHECK(stats.from_child == 901);  // the surface, then 300 of each
    CHECK(stats.rejected == 600);    // the short ones and the frames
    CHECK(stats.frames == 0);
  }
  CHECK(open_fd_count() == before);
  ::close(fd);
}

TEST_CASE("split: a flooding child does not starve the window's events",
          "[split]") {
  wm::BrokerOptions opts;
  opts.drain_budget = 4;
  Rig rig(200, opts, /*start_now=*/false);
  // Queue a surface and 120 messages before the broker runs at all.
  ipc::Message create;
  create.type = ipc::MsgType::SurfaceCreate;
  create.surface.w = 64;
  create.surface.h = 64;
  REQUIRE(rig.child.send(create));
  for (int i = 0; i < 120; ++i) {
    ipc::Message m;
    m.type = ipc::MsgType::Keepalive;
    REQUIRE(rig.child.send(m));
  }
  // A compositor event that re-arms itself and notes how many of the child's
  // messages had been handled each time it ran.
  std::vector<int> seen;
  std::function<void()> tick = [&] {
    int k = 0;
    {
      std::lock_guard<std::mutex> lock(rig.world.mu);
      k = rig.world.keepalives;
      seen.push_back(k);
    }
    if (k < 120)
      rig.world.post(tick);
  };
  rig.world.post(tick);  // already pending when the broker first looks
  rig.start();
  REQUIRE(settle(rig.world, [&] { return rig.world.keepalives == 120; }));
  rig.finish();
  // Between two runs of the event the broker handled at most one budget of
  // messages; without the budget it would have handled all 120 in one go.
  std::lock_guard<std::mutex> lock(rig.world.mu);
  REQUIRE(seen.size() >= 2);
  int widest = 0;
  for (std::size_t i = 1; i < seen.size(); ++i)
    widest = std::max(widest, seen[i] - seen[i - 1]);
  CHECK(widest <= 4);
}

TEST_CASE(
    "split: random and mutated datagrams never reach the presenter "
    "malformed",
    "[split][fuzz]") {
  // Deterministic: a fixed seed, so a failure reproduces. Each round is a
  // fresh broker fed a few hundred datagrams built by damaging valid
  // messages, with descriptors of several kinds attached. The fake presenter
  // counts any frame that reaches it and should not have; the descriptor
  // count shows whether anything was kept.
  std::mt19937 rng(0xa41c5);
  const auto pick = [&](std::uint32_t n) {
    return std::uniform_int_distribution<std::uint32_t>(0, n - 1)(rng);
  };
  const int big = buffer_fd('b');  // 2 MiB: fits a 640x480 frame
  const int small = ::memfd_create("small", MFD_CLOEXEC);
  REQUIRE(small >= 0);
  REQUIRE(::ftruncate(small, 4096) == 0);
  const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  const int event_fd = ::eventfd(0, EFD_CLOEXEC);
  REQUIRE((null_fd >= 0 && event_fd >= 0));
  const std::vector<int> kinds = {big, small, null_fd, event_fd};

  const int before = open_fd_count();
  std::uint64_t sent = 0, accepted = 0, rejected = 0;
  int violations = 0;
  for (int round = 0; round < 60; ++round) {
    Rig rig(50);
    const int sock = rig.child.native_handle();
    const auto raw = [&](const ipc::Message& m, std::size_t extra_bytes) {
      std::vector<std::byte> bytes(sizeof(m) + extra_bytes);
      std::memcpy(bytes.data(), &m, sizeof(m));
      for (std::size_t i = sizeof(m); i < bytes.size(); ++i)
        bytes[i] = static_cast<std::byte>(pick(256));
      return bytes;
    };
    for (int i = 0; i < 400; ++i) {
      ipc::Message m;
      // Start from something valid, so that mutations land near the paths
      // that do work rather than all in the first check.
      switch (pick(6)) {
        case 0:
          m.type = ipc::MsgType::SurfaceCreate;
          m.surface.w = 640;
          m.surface.h = 480;
          break;
        case 1:
        case 2:
        case 3:
          m.type = ipc::MsgType::PresentFrame;
          m.present.w = 640;
          m.present.h = 480;
          m.present.drm_format = 0x34325241;
          m.present.modifier = pick(2) ? 0x0200000000401b03ull : 0;
          m.present.stride = 2560;
          m.present.plane_count = 1;
          m.present.buffer_id = pick(3);
          break;
        case 4:
          m.type = pick(2) ? ipc::MsgType::Keepalive : ipc::MsgType::Clipboard;
          break;
        default:
          m.type = static_cast<ipc::MsgType>(pick(24));
          break;
      }
      if (m.type == ipc::MsgType::Bye)
        m.type = ipc::MsgType::Hello;  // keep the round going
      // Damage 0 to 3 bytes of the payload, sometimes a whole field.
      auto* body = reinterpret_cast<unsigned char*>(&m);
      for (std::uint32_t k = pick(4); k > 0; --k) {
        const std::size_t at = 8 + pick(sizeof(m) - 8);  // past type, length
        if (pick(4) == 0 && at + 4 <= sizeof(m)) {
          const std::uint32_t v =
              pick(2) ? 0xffffffffu : (pick(2) ? 0x80000000u : rng());
          std::memcpy(body + at, &v, 4);
        } else {
          body[at] = static_cast<unsigned char>(pick(256));
        }
      }
      const std::size_t extra = pick(5) == 0 ? pick(900) : 0;
      m.extra_len = pick(8) == 0 ? rng() : static_cast<std::uint32_t>(extra);
      std::vector<std::byte> bytes = raw(m, extra);
      if (pick(20) == 0)
        bytes.resize(1 + pick(static_cast<std::uint32_t>(bytes.size()) -
                              1));  // short, never empty: that is a disconnect
      std::vector<int> fds;
      for (std::uint32_t k = pick(4); k > 0; --k)
        fds.push_back(kinds[pick(static_cast<std::uint32_t>(kinds.size()))]);
      if (m.type == ipc::MsgType::PresentFrame && pick(4) != 0)
        fds.insert(fds.begin(), big);
      REQUIRE(send_raw(sock, bytes, fds));
      ++sent;
      // Let the fake's buffers come back now and then, so that later frames
      // are not all refused as busy.
      if (i % 16 == 0)
        rig.world.post([&] {
          std::lock_guard<std::mutex> lock(rig.world.mu);
          rig.world.free_mask = 0b111;
          rig.world.need_frame = true;
        });
    }
    const wm::BrokerStats stats = rig.finish();
    CHECK(stats.from_child == 400);
    accepted += stats.frames;
    rejected += stats.rejected;
    std::lock_guard<std::mutex> lock(rig.world.mu);
    violations += rig.world.violations;
  }
  CHECK(violations == 0);
  CHECK(open_fd_count() == before);
  // The corpus is not all noise: some frames are good enough to be accepted
  // and most are not.
  CHECK(accepted > 100);
  CHECK(rejected > 1000);
  INFO(sent << " datagrams, " << accepted << " frames accepted, " << rejected
            << " messages rejected");
  CHECK(sent == 24000);
  for (const int fd : kinds)
    ::close(fd);
}

TEST_CASE("split: closing the window reaches the child, then the broker leaves",
          "[split]") {
  Rig rig(/*linger_ms=*/100);
  auto p =
      wm::split_presenter_open_on(rig.child, 640, 480, "t", "a", {}, false);
  REQUIRE(p != nullptr);
  rig.world.post([&] {
    std::lock_guard<std::mutex> lock(rig.world.mu);
    rig.world.closed = true;
  });
  REQUIRE(pump(*p, [&] { return p->closed(); }));
  CHECK_FALSE(p->ready());
  const int fd = buffer_fd('c');
  CHECK_FALSE(p->submit(frame_of(fd), 0));
  ::close(fd);

  // The child stays connected; the broker gives up after its linger.
  rig.thread.join();
  CHECK(rig.stats.window_closed);
}

TEST_CASE("split: a broker that goes away closes the proxy", "[split]") {
  auto pair = ipc::Channel::make_pair();
  ipc::Channel child = std::move(pair.first);
  std::thread broker([end = std::move(pair.second)]() mutable {
    ipc::Received r;
    REQUIRE(end.recv(r));  // SurfaceCreate
    ipc::Message m;
    m.type = ipc::MsgType::SurfaceReady;
    m.surface_ready.ok = 1;
    REQUIRE(end.send(m));
    end.close();
  });
  auto p = wm::split_presenter_open_on(child, 64, 64, "t", "a", {}, false);
  broker.join();
  REQUIRE(p != nullptr);
  CHECK(p->modifiers().empty());
  CHECK_FALSE(p->dispatch());
  CHECK(p->closed());
}

TEST_CASE(
    "split: a state written before a frame arrived does not free "
    "its slot",
    "[split]") {
  // The broker here is scripted, so that it can say things a real one says
  // only when the timing is unlucky: a PresentState that left the broker
  // just before the child's frame reached it.
  auto pair = ipc::Channel::make_pair();
  ipc::Channel child = std::move(pair.first);
  ipc::Channel end = std::move(pair.second);
  const auto state = [&](std::uint32_t ready, std::uint32_t mask,
                         std::uint32_t frames_seen) {
    ipc::Message m;
    m.type = ipc::MsgType::PresentState;
    m.present_state.ready = ready;
    m.present_state.free_mask = mask;
    m.present_state.frames_seen = frames_seen;
    REQUIRE(end.send(m));
  };
  std::thread opener([&] {
    ipc::Received r;
    REQUIRE(end.recv(r));  // SurfaceCreate
    ipc::Message m;
    m.type = ipc::MsgType::SurfaceReady;
    m.surface_ready.ok = 1;
    REQUIRE(end.send(m));
  });
  auto p = wm::split_presenter_open_on(child, 640, 480, "t", "a", {}, false);
  opener.join();
  REQUIRE(p != nullptr);

  state(1, 0b111, 0);  // nothing sent yet: all free, ready
  REQUIRE(pump(*p, [&] { return p->ready(); }));
  CHECK(p->free_mask() == 0b111u);

  const int fd = buffer_fd('s');
  REQUIRE(p->submit(frame_of(fd), 1));  // frame 1, slot 1
  CHECK(p->free_mask() == 0b101u);
  CHECK_FALSE(p->ready());

  // Stale: written when the broker had seen no frame. Slot 1 is not free
  // and the compositor is not waiting, whatever it says.
  state(1, 0b111, 0);
  p->dispatch();
  CHECK(p->free_mask() == 0b101u);
  CHECK_FALSE(p->ready());
  CHECK_FALSE(p->wait_ready(20));  // and a render thread keeps waiting

  // Current: the broker has seen frame 1 and the slot is in use.
  state(0, 0b101, 1);
  p->dispatch();
  CHECK(p->free_mask() == 0b101u);
  // The compositor released it and wants a frame.
  state(1, 0b111, 1);
  p->dispatch();
  CHECK(p->free_mask() == 0b111u);
  CHECK(p->ready());

  // A second frame, in a slot of the broker's choosing: nothing is free
  // until the broker has seen it.
  REQUIRE(p->submit(frame_of(fd), -1));  // frame 2
  state(1, 0b111, 1);                    // stale again
  p->dispatch();
  CHECK(p->free_mask() == 0u);
  CHECK_FALSE(p->ready());
  state(1, 0b110, 2);
  p->dispatch();
  CHECK(p->free_mask() == 0b110u);
  CHECK(p->ready());

  // wait_ready returns when a state arrives on another thread's dispatch.
  REQUIRE(p->submit(frame_of(fd), 2));  // frame 3
  std::thread looper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    state(1, 0b011, 3);
    pump(*p, [&] { return p->ready(); });
  });
  CHECK(p->wait_ready(2000));
  looper.join();
  ::close(fd);
}

#else  // !(ACL_HAVE_VULKAN && ACL_HAVE_WCS)

TEST_CASE("split: needs the GPU present path", "[split]") {
  SKIP("built without Vulkan or wcs; the split present path is compiled out");
}

#endif
