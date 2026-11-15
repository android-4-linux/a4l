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
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
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
    // What must hold for anything that gets this far, whatever the child
    // sent: this is where a compositor would be asked to import the buffer.
    struct stat st{};
    const bool file = ::fstat(frame.fd, &st) == 0 && S_ISREG(st.st_mode);
    const std::uint64_t end =
        frame.planes[0].offset +
        static_cast<std::uint64_t>(frame.planes[0].stride) * frame.height;
    std::lock_guard<std::mutex> lock(world_.mu);
    if (!file || static_cast<int>(frame.width) != world_.w ||
        static_cast<int>(frame.height) != world_.h ||
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
