// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l implementation (docs/adr/ADR-0009,
// docs/split-process-art.md). No GPL-3.0 sources.

#include "wm/split_present.h"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "log/log.h"

namespace acl::wm {
namespace {

constexpr std::string_view kTag = "acl.wm.split";

struct ChildState {
  ipc::Channel channel;
  int width = 0;
  int height = 0;
  int scale = 1;
  bool active = false;
};

ChildState& child_state() noexcept {
  static ChildState s;
  return s;
}

}  // namespace

void split_child_install(int ipc_fd, int display_w, int display_h, int scale) {
  ChildState& s = child_state();
  s.channel = ipc::Channel(ipc_fd);
  s.width = display_w;
  s.height = display_h;
  s.scale = scale > 0 ? scale : 1;
  s.active = s.channel.valid();
}

bool split_child_active() noexcept {
  return child_state().active;
}

bool split_child_display(int& width, int& height, int& scale) noexcept {
  const ChildState& s = child_state();
  if (!s.active || s.width <= 0 || s.height <= 0)
    return false;
  width = s.width;
  height = s.height;
  scale = s.scale;
  return true;
}

#if ACL_HAVE_VULKAN && ACL_HAVE_WCS

namespace {

void close_fds(ipc::Received& r) noexcept {
  if (r.fd0 >= 0)
    ::close(r.fd0);
  if (r.fd1 >= 0)
    ::close(r.fd1);
  r.fd0 = r.fd1 = -1;
}

bool would_block() noexcept {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

std::span<const std::byte> as_bytes(std::string_view s) noexcept {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string as_string(const std::vector<std::byte>& b, std::size_t cap) {
  const std::size_t n = b.size() < cap ? b.size() : cap;
  return {reinterpret_cast<const char*>(b.data()), n};
}

// ── Child: the proxy presenter ──────────────────────────────────────────

class SplitPresenter final : public IPresenter {
 public:
  SplitPresenter(ipc::Channel& channel,
                 InputCallback input,
                 std::vector<std::uint64_t> modifiers)
      : channel_(channel),
        input_(std::move(input)),
        modifiers_(std::move(modifiers)) {}

  int fd() noexcept override { return channel_.native_handle(); }
  int repeat_fd() noexcept override { return -1; }  // the broker repeats keys

  bool dispatch() noexcept override {
    ipc::Received r;
    while (channel_.recv(r, /*nonblock=*/true)) {
      handle(r);
      close_fds(r);
    }
    if (!would_block())
      closed_.store(true, std::memory_order_relaxed);  // broker went away
    return !closed();
  }
  void dispatch_repeat() noexcept override {}

  bool ready() const noexcept override {
    return ready_.load(std::memory_order_relaxed) && !closed();
  }
  int next_buffer() const noexcept override {
    const std::uint32_t mask = free_mask();
    return mask != 0 ? std::countr_zero(mask) : -1;
  }
  std::uint32_t free_mask() const noexcept override {
    return free_mask_.load(std::memory_order_relaxed);
  }
  std::uint64_t frame_time_ns() const noexcept override {
    return frame_time_ns_.load(std::memory_order_relaxed);
  }
  bool last_present_feedback(std::uint64_t* present_ns,
                             std::uint32_t* refresh_ns,
                             std::uint32_t* flags) const noexcept override {
    if (!feedback_ok_.load(std::memory_order_relaxed))
      return false;
    *present_ns = present_ns_.load(std::memory_order_relaxed);
    *refresh_ns = refresh_ns_.load(std::memory_order_relaxed);
    *flags = feedback_flags_.load(std::memory_order_relaxed);
    return true;
  }
  const std::vector<std::uint64_t>& modifiers() const noexcept override {
    return modifiers_;
  }

  // May run on the render thread: one sendmsg, and atomics for the state.
  bool submit(const gfx::DmabufFrame& frame, int slot) noexcept override {
    if (closed() || frame.fd < 0 || frame.plane_count == 0 ||
        frame.plane_count > ipc::kMaxPlanes)
      return false;
    ipc::Message m;
    m.type = ipc::MsgType::PresentFrame;
    ipc::PresentFrame& p = m.present;
    p.buffer_id = static_cast<std::uint32_t>(slot);  // -1 = broker picks
    p.w = frame.width;
    p.h = frame.height;
    p.drm_format = frame.drm_format;
    p.modifier = frame.modifier;
    p.stride = frame.planes[0].stride;
    p.offset = frame.planes[0].offset;
    p.damage_w = static_cast<std::int32_t>(frame.width);
    p.damage_h = static_cast<std::int32_t>(frame.height);
    p.plane_count = frame.plane_count;
    for (std::uint32_t i = 1; i < frame.plane_count; ++i) {
      p.plane_stride[i - 1] = frame.planes[i].stride;
      p.plane_offset[i - 1] = frame.planes[i].offset;
    }
    if (!channel_.send(m, {}, frame.fd)) {
      closed_.store(true, std::memory_order_relaxed);
      return false;
    }
    // The real presenter is not ready again until the compositor's next
    // frame callback; the broker reports that with a PresentState. Until
    // then this slot is busy and the others are as last reported.
    ready_.store(false, std::memory_order_relaxed);
    if (slot >= 0 && slot < 32)
      free_mask_.fetch_and(~(1u << slot), std::memory_order_relaxed);
    else
      free_mask_.store(0, std::memory_order_relaxed);
    submitted_.store(true, std::memory_order_relaxed);
    return true;
  }

  bool keepalive() noexcept override {
    if (closed() || !submitted_.load(std::memory_order_relaxed))
      return false;
    ipc::Message m;
    m.type = ipc::MsgType::Keepalive;
    if (!channel_.send(m))
      return false;
    ready_.store(false, std::memory_order_relaxed);
    return true;
  }

  bool closed() const noexcept override {
    return closed_.load(std::memory_order_relaxed);
  }

  void set_ime_visible(bool visible) noexcept override {
    ipc::Message m;
    m.type = ipc::MsgType::ImeVisible;
    m.u32 = visible ? 1 : 0;
    channel_.send(m);
  }

  void set_clipboard(const std::string& utf8) noexcept override {
    const std::string_view text =
        std::string_view(utf8).substr(0, ipc::kMaxText);
    ipc::Message m;
    m.type = ipc::MsgType::Clipboard;
    m.extra_len = static_cast<std::uint32_t>(text.size());
    channel_.send(m, as_bytes(text));
  }

 private:
  void handle(const ipc::Received& r) noexcept {
    switch (r.msg.type) {
      case ipc::MsgType::PresentState: {
        const ipc::PresentState& st = r.msg.present_state;
        frame_time_ns_.store(st.frame_time_ns, std::memory_order_relaxed);
        if (st.feedback_ok != 0) {
          present_ns_.store(st.present_ns, std::memory_order_relaxed);
          refresh_ns_.store(st.refresh_ns, std::memory_order_relaxed);
          feedback_flags_.store(st.feedback_flags, std::memory_order_relaxed);
          feedback_ok_.store(true, std::memory_order_relaxed);
        }
        free_mask_.store(st.free_mask, std::memory_order_relaxed);
        ready_.store(st.ready != 0, std::memory_order_relaxed);
        if (st.closed != 0)
          closed_.store(true, std::memory_order_relaxed);
        break;
      }
      case ipc::MsgType::Input:
        deliver(r);
        break;
      case ipc::MsgType::Bye:
        closed_.store(true, std::memory_order_relaxed);
        break;
      default:
        break;
    }
  }

  void deliver(const ipc::Received& r) noexcept {
    if (!input_)
      return;
    const ipc::Input& in = r.msg.input;
    switch (in.kind) {
      case 0: {
        if (in.action < 0 || in.action > 2)
          return;
        static constexpr PointerInput::Action kActions[] = {
            PointerInput::Action::Down, PointerInput::Action::Move,
            PointerInput::Action::Up};
        input_(PointerInput{kActions[in.action], in.x, in.y});
        break;
      }
      case 1:
        input_(KeyInput{
            in.action == 0 ? KeyInput::Action::Down : KeyInput::Action::Up,
            static_cast<std::uint32_t>(in.keycode), in.keysym});
        break;
      case 2:
        input_(TextInput{as_string(r.extra, ipc::kMaxText), in.action != 0,
                         in.keycode});
        break;
      case 3:
        input_(DeleteSurroundingText{in.before, in.after});
        break;
      default:
        break;
    }
  }

  ipc::Channel& channel_;
  InputCallback input_;
  std::vector<std::uint64_t> modifiers_;
  std::atomic<bool> ready_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> submitted_{false};
  std::atomic<bool> feedback_ok_{false};
  std::atomic<std::uint32_t> free_mask_{0};
  std::atomic<std::uint64_t> frame_time_ns_{0};
  std::atomic<std::uint64_t> present_ns_{0};
  std::atomic<std::uint32_t> refresh_ns_{0};
  std::atomic<std::uint32_t> feedback_flags_{0};
};

}  // namespace

std::unique_ptr<IPresenter> split_presenter_open(int width,
                                                 int height,
                                                 std::string_view title,
                                                 std::string_view app_id,
                                                 InputCallback input,
                                                 bool want_ime) noexcept {
  ChildState& s = child_state();
  if (!s.active)
    return nullptr;
  return split_presenter_open_on(s.channel, width, height, title, app_id,
                                 std::move(input), want_ime);
}

std::unique_ptr<IPresenter> split_presenter_open_on(ipc::Channel& channel,
                                                    int width,
                                                    int height,
                                                    std::string_view title,
                                                    std::string_view app_id,
                                                    InputCallback input,
                                                    bool want_ime) noexcept {
  if (!channel.valid() || width <= 0 || height <= 0)
    return nullptr;

  std::string names(title.substr(0, 256));
  names.push_back('\0');
  names.append(app_id.substr(0, 256));
  ipc::Message m;
  m.type = ipc::MsgType::SurfaceCreate;
  m.surface.w = static_cast<std::uint32_t>(width);
  m.surface.h = static_cast<std::uint32_t>(height);
  m.surface.want_ime = want_ime ? 1 : 0;
  m.extra_len = static_cast<std::uint32_t>(names.size());
  if (!channel.send(m, as_bytes(names))) {
    log::w(kTag, "split: SurfaceCreate send failed: {}", std::strerror(errno));
    return nullptr;
  }

  // The broker answers once the toplevel is configured. Nothing else is
  // expected before that; anything that does arrive is dropped.
  ipc::Received r;
  for (;;) {
    if (!channel.recv(r)) {
      log::w(kTag, "split: broker closed before the surface was ready");
      return nullptr;
    }
    close_fds(r);
    if (r.msg.type == ipc::MsgType::SurfaceReady)
      break;
  }
  if (r.msg.surface_ready.ok == 0) {
    log::w(kTag, "split: the broker could not create the surface");
    return nullptr;
  }
  std::vector<std::uint64_t> mods(r.extra.size() / sizeof(std::uint64_t));
  if (!mods.empty())
    std::memcpy(mods.data(), r.extra.data(),
                mods.size() * sizeof(std::uint64_t));
  log::i(kTag, "split: surface {}x{} via the broker, {} modifier(s)", width,
         height, mods.size());
  return std::make_unique<SplitPresenter>(channel, std::move(input),
                                          std::move(mods));
}

// ── Broker ──────────────────────────────────────────────────────────────

namespace {

class Broker {
 public:
  Broker(ipc::Channel& channel, const PresenterFactory& open) noexcept
      : channel_(channel), open_(open) {}

  BrokerStats run(int linger_ms) noexcept {
    using Clock = std::chrono::steady_clock;
    // A child that stops reading must not block the broker. A PresentState
    // that does not fit is sent again on the next pass; input is dropped.
    channel_.set_send_nonblocking(true);
    Clock::time_point close_deadline{};
    for (;;) {
      pollfd fds[3];
      nfds_t n = 0;
      fds[n++] = {channel_.native_handle(), POLLIN, 0};
      const int pfd = presenter_ ? presenter_->fd() : -1;
      const int rfd = presenter_ ? presenter_->repeat_fd() : -1;
      nfds_t pi = 0, ri = 0;
      if (pfd >= 0) {
        pi = n;
        fds[n++] = {pfd, POLLIN, 0};
      }
      if (rfd >= 0) {
        ri = n;
        fds[n++] = {rfd, POLLIN, 0};
      }
      int timeout = -1;
      if (stats_.window_closed) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            close_deadline - Clock::now());
        if (left.count() <= 0)
          return stats_;  // the child did not leave; the caller ends it
        timeout = static_cast<int>(left.count());
      }
      if (::poll(fds, n, timeout) < 0 && errno != EINTR)
        return stats_;

      if (pi != 0 && fds[pi].revents != 0)
        presenter_->dispatch();
      if (ri != 0 && (fds[ri].revents & POLLIN) != 0)
        presenter_->dispatch_repeat();
      if (fds[0].revents != 0 && !drain_child())
        return stats_;  // the child disconnected or said Bye

      sync_state();
      if (presenter_ && presenter_->closed() && !stats_.window_closed) {
        stats_.window_closed = true;
        close_deadline = Clock::now() + std::chrono::milliseconds(linger_ms);
      }
    }
  }

 private:
  // False once the child is gone.
  bool drain_child() noexcept {
    ipc::Received r;
    for (;;) {
      if (!channel_.recv(r, /*nonblock=*/true)) {
        // A child that exits with our messages unread resets the socket. The
        // error is reported once, ahead of what the child sent last; read on.
        if (errno == ECONNRESET && !reset_seen_) {
          reset_seen_ = true;
          continue;
        }
        break;
      }
      ++stats_.from_child;
      const bool keep = handle(r);
      close_fds(r);
      if (!keep)
        return false;
    }
    return would_block();
  }

  bool handle(ipc::Received& r) noexcept {
    switch (r.msg.type) {
      case ipc::MsgType::SurfaceCreate:
        create_surface(r);
        break;
      case ipc::MsgType::PresentFrame:
        present(r);
        break;
      case ipc::MsgType::Keepalive:
        if (presenter_) {
          presenter_->keepalive();
          child_ready_ = false;
        }
        break;
      case ipc::MsgType::ImeVisible:
        if (presenter_)
          presenter_->set_ime_visible(r.msg.u32 != 0);
        break;
      case ipc::MsgType::Clipboard:
        if (presenter_)
          presenter_->set_clipboard(as_string(r.extra, ipc::kMaxText));
        break;
      case ipc::MsgType::Hello:
      case ipc::MsgType::Log:
        break;
      case ipc::MsgType::Bye:
        return false;
      default:
        ++stats_.rejected;
        break;
    }
    return true;
  }

  void reply_surface(bool ok) noexcept {
    ipc::Message m;
    m.type = ipc::MsgType::SurfaceReady;
    m.surface_ready.ok = ok ? 1 : 0;
    std::span<const std::byte> extra;
    if (ok) {
      const std::vector<std::uint64_t>& mods = presenter_->modifiers();
      const std::size_t count =
          mods.size() < ipc::kMaxModifiers ? mods.size() : ipc::kMaxModifiers;
      m.surface_ready.modifier_count = static_cast<std::uint32_t>(count);
      extra = {reinterpret_cast<const std::byte*>(mods.data()),
               count * sizeof(std::uint64_t)};
    }
    m.extra_len = static_cast<std::uint32_t>(extra.size());
    if (channel_.send(m, extra))
      ++stats_.to_child;
  }

  void create_surface(const ipc::Received& r) noexcept {
    const ipc::SurfaceInfo& si = r.msg.surface;
    if (si.w == 0 || si.h == 0 || si.w > ipc::kMaxSurfaceDim ||
        si.h > ipc::kMaxSurfaceDim) {
      ++stats_.rejected;
      reply_surface(false);
      return;
    }
    // extra = title, NUL, app id. Either may be empty. Control bytes are
    // dropped: both strings end up in the compositor's window list.
    const std::string names = as_string(r.extra, 600);
    const std::size_t nul = names.find('\0');
    const auto clean = [](std::string_view in, const char* fallback) {
      std::string out;
      for (const char c : in.substr(0, 256)) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f)
          out.push_back(c);
      }
      return out.empty() ? std::string(fallback) : out;
    };
    const std::string title =
        clean(std::string_view(names).substr(0, nul), "a4l");
    const std::string app_id = clean(
        nul == std::string::npos ? std::string_view{}
                                 : std::string_view(names).substr(nul + 1),
        "org.acl.app");

    presenter_.reset();  // one surface at a time
    child_ready_ = false;
    sent_closed_ = false;
    presenter_ = open_(
        static_cast<int>(si.w), static_cast<int>(si.h), title, app_id,
        [this](const InputEvent& ev) { forward_input(ev); }, si.want_ime != 0);
    if (!presenter_)
      log::w(kTag, "broker: presenter_open {}x{} failed", si.w, si.h);
    else
      log::i(kTag, "broker: surface {}x{} '{}' for the child", si.w, si.h,
             app_id);
    reply_surface(presenter_ != nullptr);
  }

  void present(const ipc::Received& r) noexcept {
    const ipc::PresentFrame& p = r.msg.present;
    const std::uint32_t planes = p.plane_count == 0 ? 1 : p.plane_count;
    // buffer_id is the presenter's ring slot; -1 lets the presenter pick. The
    // presenter range-checks the slot itself, so only reject the absurd.
    const std::int32_t slot = static_cast<std::int32_t>(p.buffer_id);
    if (!presenter_ || r.fd0 < 0 || p.w == 0 || p.h == 0 ||
        p.w > ipc::kMaxSurfaceDim || p.h > ipc::kMaxSurfaceDim ||
        planes > ipc::kMaxPlanes || p.stride == 0 || slot < -1 || slot > 63) {
      ++stats_.rejected;
      return;
    }
    gfx::DmabufFrame fr{};
    fr.fd = r.fd0;  // borrowed: libwayland dups it when the buffer is built
    fr.width = p.w;
    fr.height = p.h;
    fr.plane_count = planes;
    fr.planes[0].stride = p.stride;
    fr.planes[0].offset = p.offset;
    for (std::uint32_t i = 1; i < planes; ++i) {
      fr.planes[i].stride = p.plane_stride[i - 1];
      fr.planes[i].offset = p.plane_offset[i - 1];
    }
    fr.modifier = p.modifier;
    fr.drm_format = p.drm_format;
    fr.modifier_tiled = p.modifier != 0;
    // A refused frame (busy slot, failed import) must not strand the vsync
    // loop: the child already believes it submitted.
    if (presenter_->submit(fr, slot))
      ++stats_.frames;
    else
      presenter_->keepalive();
    child_ready_ = false;
  }

  void forward_input(const InputEvent& ev) noexcept {
    ipc::Message m;
    m.type = ipc::MsgType::Input;
    std::string text;
    if (const auto* p = std::get_if<PointerInput>(&ev)) {
      m.input.kind = 0;
      m.input.action = p->action == PointerInput::Action::Down   ? 0
                       : p->action == PointerInput::Action::Move ? 1
                                                                 : 2;
      m.input.x = p->x;
      m.input.y = p->y;
    } else if (const auto* k = std::get_if<KeyInput>(&ev)) {
      m.input.kind = 1;
      m.input.action = k->action == KeyInput::Action::Down ? 0 : 1;
      m.input.keycode = static_cast<std::int32_t>(k->keycode);
      m.input.keysym = k->keysym;
    } else if (const auto* t = std::get_if<TextInput>(&ev)) {
      m.input.kind = 2;
      m.input.action = t->preedit ? 1 : 0;
      m.input.keycode = t->cursor;
      text = t->text.substr(0, ipc::kMaxText);
    } else if (const auto* d = std::get_if<DeleteSurroundingText>(&ev)) {
      m.input.kind = 3;
      m.input.before = d->before;
      m.input.after = d->after;
    }
    m.extra_len = static_cast<std::uint32_t>(text.size());
    if (channel_.send(m, as_bytes(text))) {
      ++stats_.inputs;
      ++stats_.to_child;
    }
  }

  // Tell the child when `ready` or `closed` differs from what it believes.
  // After a submit it believes "not ready", so a steady loop sends one
  // PresentState per frame, when the compositor's frame callback lands.
  void sync_state() noexcept {
    if (!presenter_)
      return;
    const bool closed = presenter_->closed();
    const bool ready = !closed && presenter_->ready();
    if (ready == child_ready_ && closed == sent_closed_)
      return;
    ipc::Message m;
    m.type = ipc::MsgType::PresentState;
    ipc::PresentState& st = m.present_state;
    st.ready = ready ? 1 : 0;
    st.free_mask = presenter_->free_mask();
    st.closed = closed ? 1 : 0;
    st.frame_time_ns = presenter_->frame_time_ns();
    st.feedback_ok = presenter_->last_present_feedback(
                         &st.present_ns, &st.refresh_ns, &st.feedback_flags)
                         ? 1
                         : 0;
    if (channel_.send(m)) {
      ++stats_.to_child;
      child_ready_ = ready;
      sent_closed_ = closed;
    }
  }

  ipc::Channel& channel_;
  const PresenterFactory& open_;
  std::unique_ptr<IPresenter> presenter_;
  BrokerStats stats_;
  bool child_ready_ = false;  // the child's view of ready()
  bool sent_closed_ = false;
  bool reset_seen_ = false;
};

}  // namespace

BrokerStats serve_split_child(ipc::Channel& channel,
                              const PresenterFactory& open,
                              int linger_ms) noexcept {
  return Broker(channel, open).run(linger_ms);
}

#endif  // ACL_HAVE_VULKAN && ACL_HAVE_WCS

}  // namespace acl::wm
