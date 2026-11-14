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

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
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
  int keepalives = 0;
  std::vector<bool> ime;
  std::vector<std::string> clipboard;

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
    std::lock_guard<std::mutex> lock(world_.mu);
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

// A broker on its own thread over one end of a socket pair; the test holds
// the other end, as the child does.
struct Rig {
  World world;
  ipc::Channel child;
  ipc::Channel broker_end;
  std::thread thread;
  wm::BrokerStats stats;

  explicit Rig(int linger_ms = 200) {
    auto pair = ipc::Channel::make_pair();
    child = std::move(pair.first);
    broker_end = std::move(pair.second);
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
      stats = wm::serve_split_child(broker_end, open, linger_ms);
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

// A one-page memfd holding `byte`, standing in for a dma-buf.
int buffer_fd(char byte) {
  const int fd = ::memfd_create("frame", MFD_CLOEXEC);
  REQUIRE(fd >= 0);
  REQUIRE(::write(fd, &byte, 1) == 1);
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

TEST_CASE("split: the broker drops what fails validation", "[split]") {
  Rig rig;
  const int fd = buffer_fd('z');
  const auto frame = [&](std::uint32_t w, std::uint32_t h, std::uint32_t planes,
                         std::uint32_t stride, std::uint32_t slot,
                         int with_fd) {
    ipc::Message m;
    m.type = ipc::MsgType::PresentFrame;
    m.present.w = w;
    m.present.h = h;
    m.present.plane_count = planes;
    m.present.stride = stride;
    m.present.buffer_id = slot;
    REQUIRE(rig.child.send(m, {}, with_fd));
  };

  frame(640, 480, 1, 2560, 0, fd);  // 1: no surface yet

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
  frame(0, 480, 1, 2560, 0, fd);                        // 3: zero width
  frame(640, ipc::kMaxSurfaceDim + 1, 1, 2560, 0, fd);  // 4: too tall
  frame(640, 480, ipc::kMaxPlanes + 1, 2560, 0, fd);    // 5: too many planes
  frame(640, 480, 1, 0, 0, fd);                         // 6: zero stride
  frame(640, 480, 1, 2560, 64, fd);                     // 7: slot out of range
  frame(640, 480, 1, 2560, 0, -1);                      // 8: no fd
  ipc::Message unknown;  // 9: not a child message
  unknown.type = ipc::MsgType::FrameReleased;
  REQUIRE(rig.child.send(unknown));
  ipc::Message garbage;  // 10: no such tag
  garbage.type = static_cast<ipc::MsgType>(0xdeadbeef);
  REQUIRE(rig.child.send(garbage));
  ::close(fd);

  const wm::BrokerStats stats = rig.finish();
  CHECK(stats.rejected == 10);
  CHECK(stats.frames == 0);
  CHECK(rig.world.frames.empty());
  CHECK(rig.world.opens == 1);
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

#else  // !(ACL_HAVE_VULKAN && ACL_HAVE_WCS)

TEST_CASE("split: needs the GPU present path", "[split]") {
  SKIP("built without Vulkan or wcs; the split present path is compiled out");
}

#endif
