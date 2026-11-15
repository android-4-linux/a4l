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
#include <condition_variable>
#include <cstring>
#include <mutex>
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
    if (!would_block()) {  // broker went away
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        closed_.store(true, std::memory_order_relaxed);
      }
      state_cv_.notify_all();
    }
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
    // then this slot is busy and the others are as last reported. The frame
    // is numbered so that a state the broker wrote before it saw this frame
    // can be told from one written after.
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      const std::uint32_t seq = ++sent_frames_;
      ready_.store(false, std::memory_order_relaxed);
      if (slot >= 0 && slot < 32) {
        slot_seq_[slot] = seq;
        free_mask_.fetch_and(~(1u << slot), std::memory_order_relaxed);
      } else {
        any_slot_seq_ = seq;
        free_mask_.store(0, std::memory_order_relaxed);
      }
    }
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

  // The state changes on the looper thread, in handle(); a render thread
  // waits here for it.
  bool wait_ready(int timeout_ms) noexcept override {
    std::unique_lock<std::mutex> lock(state_mu_);
    state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                       [this] { return ready() || closed(); });
    return ready();
  }

  // The video plane. Called from the decode thread: one sendmsg, which the
  // kernel keeps whole against the looper thread's. The decoder reuses the
  // descriptor after this returns; the datagram holds its own reference.
  // A broker that falls behind makes this block, which holds the decoder to
  // the rate the screen takes.
  void present_video(const gfx::DmabufFrame& frame,
                     int x,
                     int y,
                     int w,
                     int h) noexcept override {
    if (closed() || frame.fd < 0 || frame.plane_count == 0 ||
        frame.plane_count > ipc::kMaxPlanes)
      return;
    ipc::Message m;
    m.type = ipc::MsgType::VideoFrame;
    ipc::VideoFrame& v = m.video;
    v.w = frame.width;
    v.h = frame.height;
    v.drm_format = frame.drm_format;
    v.plane_count = frame.plane_count;
    v.modifier = frame.modifier;
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
      v.stride[i] = frame.planes[i].stride;
      v.offset[i] = frame.planes[i].offset;
    }
    v.dest = {x, y, w, h};
    channel_.send(m, {}, frame.fd);
  }
  void hide_video() noexcept override {
    ipc::Message m;
    m.type = ipc::MsgType::VideoHide;
    channel_.send(m);
  }
  void set_video_bounds(int x, int y, int w, int h) noexcept override {
    // The app lays out every frame; only a change is worth a datagram.
    const ipc::VideoRect rect{x, y, w, h};
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      if (video_bounds_set_ &&
          std::memcmp(&rect, &video_bounds_, sizeof(rect)) == 0)
        return;
      video_bounds_ = rect;
      video_bounds_set_ = true;
    }
    ipc::Message m;
    m.type = ipc::MsgType::VideoBounds;
    m.video_rect = rect;
    channel_.send(m);
  }
  void reset_video_overlay() noexcept override {
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      video_bounds_set_ = false;
    }
    ipc::Message m;
    m.type = ipc::MsgType::VideoReset;
    channel_.send(m);
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
        {
          // A state the broker wrote before it saw our latest frame: the
          // slots of frames it has not seen are not free, whatever it says,
          // and the compositor is not waiting for a frame.
          std::lock_guard<std::mutex> lock(state_mu_);
          const auto unseen = [&](std::uint32_t seq) {
            return seq != 0 &&
                   static_cast<std::int32_t>(seq - st.frames_seen) > 0;
          };
          std::uint32_t mask = st.free_mask;
          for (int i = 0; i < 32; ++i) {
            if (unseen(slot_seq_[i]))
              mask &= ~(1u << i);
          }
          if (unseen(any_slot_seq_))
            mask = 0;
          free_mask_.store(mask, std::memory_order_relaxed);
          ready_.store(st.ready != 0 && !unseen(sent_frames_),
                       std::memory_order_relaxed);
        }
        if (st.closed != 0) {
          std::lock_guard<std::mutex> lock(state_mu_);
          closed_.store(true, std::memory_order_relaxed);
        }
        state_cv_.notify_all();
        break;
      }
      case ipc::MsgType::Input:
        deliver(r);
        break;
      case ipc::MsgType::Bye: {
        {
          std::lock_guard<std::mutex> lock(state_mu_);
          closed_.store(true, std::memory_order_relaxed);
        }
        state_cv_.notify_all();
        break;
      }
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
  // Frame numbering, under state_mu_: submit() runs on the render thread and
  // PresentState is handled on the looper thread.
  std::mutex state_mu_;
  std::condition_variable state_cv_;  // ready or closed changed
  std::uint32_t sent_frames_ = 0;     // PresentFrame messages sent
  std::uint32_t slot_seq_[32] = {};   // the last frame put in each slot
  std::uint32_t any_slot_seq_ = 0;    // the last frame the broker placed
  ipc::VideoRect video_bounds_{};     // the last VideoBounds sent
  bool video_bounds_set_ = false;
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
  Broker(ipc::Channel& channel,
         const PresenterFactory& open,
         const BrokerOptions& options) noexcept
      : channel_(channel), open_(open), options_(options) {}

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
    // A bounded number per pass: what is left stays queued in the socket and
    // poll() reports it again, after the compositor has had its turn.
    for (int handled = 0; handled < options_.drain_budget; ++handled) {
      if (!channel_.recv(r, /*nonblock=*/true)) {
        // A child that exits with our messages unread resets the socket. The
        // error is reported once, ahead of what the child sent last; read on.
        if (errno == ECONNRESET && !reset_seen_) {
          reset_seen_ = true;
          continue;
        }
        // A datagram shorter than a message is the child's mistake, not the
        // end of the connection.
        if (errno == EPROTO) {
          ++stats_.from_child;
          ++stats_.rejected;
          continue;
        }
        return would_block();
      }
      ++stats_.from_child;
      const bool keep = handle(r);
      close_fds(r);
      if (!keep)
        return false;
    }
    return true;  // budget spent; more may be queued
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
      case ipc::MsgType::VideoFrame:
        present_video(r);
        break;
      case ipc::MsgType::VideoBounds:
        if (!presenter_)
          break;
        if (!check_video_rect(r.msg.video_rect)) {
          ++stats_.rejected;
          break;
        }
        presenter_->set_video_bounds(r.msg.video_rect.x, r.msg.video_rect.y,
                                     r.msg.video_rect.w, r.msg.video_rect.h);
        break;
      case ipc::MsgType::VideoHide:
        if (presenter_) {
          presenter_->hide_video();
          presenter_->pump_video();
        }
        break;
      case ipc::MsgType::VideoReset:
        if (presenter_)
          presenter_->reset_video_overlay();
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
    if (!check_surface(si)) {
      ++stats_.rejected;
      reply_surface(false);
      return;
    }
    // Each surface is a toplevel on the user's screen. An app has a handful
    // over its life; a child that asks for more is refused from then on.
    if (surfaces_created_ >= options_.max_surfaces) {
      if (surfaces_created_ == options_.max_surfaces) {
        log::w(kTag,
               "broker: the child asked for more than {} surfaces; "
               "refusing further ones",
               options_.max_surfaces);
        ++surfaces_created_;
      }
      ++stats_.rejected;
      reply_surface(false);
      return;
    }
    ++surfaces_created_;
    // extra = title, NUL, app id. Either may be empty. Both end up in the
    // compositor's window list, so they are reduced to printable UTF-8. The
    // app id is the broker's own when it has one.
    const std::string names = as_string(r.extra, 600);
    const std::size_t nul = names.find('\0');
    const std::string title =
        clean_label(std::string_view(names).substr(0, nul), 256, "a4l");
    const std::string app_id =
        !options_.app_id.empty()
            ? options_.app_id
            : clean_label(nul == std::string::npos
                              ? std::string_view{}
                              : std::string_view(names).substr(nul + 1),
                          256, "org.acl.app");

    presenter_.reset();  // one surface at a time
    child_ready_ = false;
    sent_closed_ = false;
    limits_ = FrameLimits{};
    video_mods_format_ = 0;
    video_mods_.clear();
    presenter_ = open_(
        static_cast<int>(si.w), static_cast<int>(si.h), title, app_id,
        [this](const InputEvent& ev) { forward_input(ev); }, si.want_ime != 0);
    if (!presenter_) {
      log::w(kTag, "broker: presenter_open {}x{} failed", si.w, si.h);
    } else {
      log::i(kTag, "broker: surface {}x{} '{}' for the child", si.w, si.h,
             app_id);
      limits_.have_surface = true;
      limits_.surface_w = si.w;
      limits_.surface_h = si.h;
    }
    reply_surface(presenter_ != nullptr);
  }

  void present(const ipc::Received& r) noexcept {
    const ipc::PresentFrame& p = r.msg.present;
    ++frames_seen_;  // accepted or not: the child numbers what it sends
    // Everything below was written by the app process. Ask the kernel what
    // the descriptor is and how large, then check the frame against that,
    // the surface, and what the compositor advertised.
    FrameLimits limits = limits_;
    limits.have_surface = limits.have_surface && presenter_ != nullptr;
    if (presenter_)
      limits.modifiers = presenter_->modifiers();
    const BufferFacts facts = r.fd0 >= 0 && limits.have_surface
                                  ? options_.inspect(r.fd0)
                                  : BufferFacts{};
    const FrameVerdict verdict = check_frame(p, r.fd0 >= 0, facts, limits);
    if (verdict != FrameVerdict::Ok) {
      const auto v = static_cast<std::size_t>(verdict);
      if (stats_.frame_verdicts[v]++ == 0)
        log::w(kTag,
               "broker: refused a frame from the app process: {} ({}x{} "
               "format 0x{:x} modifier 0x{:x} stride {} offset {}, buffer of "
               "{} bytes); further ones of this kind are counted, not logged",
               verdict_name(verdict), p.w, p.h, p.drm_format, p.modifier,
               p.stride, p.offset, facts.size);
      ++stats_.rejected;
      // The child believes it submitted. Keep the frame loop alive so that a
      // refused frame does not hang it.
      if (presenter_) {
        presenter_->keepalive();
        child_ready_ = false;
      }
      return;
    }
    const std::uint32_t planes = p.plane_count == 0 ? 1 : p.plane_count;
    const std::int32_t slot = static_cast<std::int32_t>(p.buffer_id);
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
    fr.mem_size = facts.size;
    fr.modifier_tiled = p.modifier != 0;
    // A refused frame (busy slot, failed import) must not strand the vsync
    // loop: the child already believes it submitted.
    if (presenter_->submit(fr, slot))
      ++stats_.frames;
    else
      presenter_->keepalive();
    child_ready_ = false;
  }

  // A decoded frame for the video plane. Checked like a window frame, but
  // against its own format's advertisement: the compositor is asked to
  // import whatever passes, and one it refuses ends the connection.
  void present_video(const ipc::Received& r) noexcept {
    const ipc::VideoFrame& v = r.msg.video;
    VideoLimits limits;
    limits.have_surface = limits_.have_surface && presenter_ != nullptr;
    if (limits.have_surface && video_format_accepted(v.drm_format)) {
      // Asked once per format and surface: the app sends one per frame.
      if (video_mods_format_ != v.drm_format) {
        video_mods_ = presenter_->modifiers_for(v.drm_format);
        video_mods_format_ = v.drm_format;
      }
      limits.modifiers = video_mods_;
    }
    const BufferFacts facts = r.fd0 >= 0 && limits.have_surface
                                  ? options_.inspect(r.fd0)
                                  : BufferFacts{};
    const FrameVerdict verdict =
        check_video_frame(v, r.fd0 >= 0, facts, limits);
    if (verdict != FrameVerdict::Ok) {
      const auto i = static_cast<std::size_t>(verdict);
      if (stats_.frame_verdicts[i]++ == 0)
        log::w(kTag,
               "broker: refused a video frame from the app process: {} "
               "({}x{} format 0x{:x} modifier 0x{:x}, {} plane(s), strides "
               "{}/{}, buffer of {} bytes); further ones of this kind are "
               "counted, not logged",
               verdict_name(verdict), v.w, v.h, v.drm_format, v.modifier,
               v.plane_count, v.stride[0], v.stride[1], facts.size);
      ++stats_.rejected;
      return;
    }
    gfx::DmabufFrame fr{};
    fr.fd = r.fd0;  // borrowed: the presenter takes its own reference
    fr.width = v.w;
    fr.height = v.h;
    fr.plane_count = v.plane_count;
    for (std::uint32_t i = 0; i < v.plane_count; ++i) {
      fr.planes[i].stride = v.stride[i];
      fr.planes[i].offset = v.offset[i];
    }
    fr.modifier = v.modifier;
    fr.drm_format = v.drm_format;
    fr.mem_size = facts.size;
    fr.modifier_tiled = v.modifier != 0;
    presenter_->present_video(fr, v.dest.x, v.dest.y, v.dest.w, v.dest.h);
    // This is the presenter's own thread: show the frame now. A frame the
    // compositor has no room for is dropped there, so a child that sends
    // faster than the screen takes them costs a descriptor at a time.
    presenter_->pump_video();
    ++stats_.video_frames;
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
    st.frames_seen = frames_seen_;
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
  const BrokerOptions& options_;
  FrameLimits limits_;
  int surfaces_created_ = 0;
  std::uint32_t frames_seen_ = 0;  // PresentFrame messages handled
  // What the compositor advertised for the video format last seen.
  std::uint32_t video_mods_format_ = 0;
  std::vector<std::uint64_t> video_mods_;
  std::unique_ptr<IPresenter> presenter_;
  BrokerStats stats_;
  bool child_ready_ = false;  // the child's view of ready()
  bool sent_closed_ = false;
  bool reset_seen_ = false;
};

}  // namespace

BrokerStats serve_split_child(ipc::Channel& channel,
                              const PresenterFactory& open,
                              const BrokerOptions& options,
                              int linger_ms) noexcept {
  return Broker(channel, open, options).run(linger_ms);
}

#endif  // ACL_HAVE_VULKAN && ACL_HAVE_WCS

}  // namespace acl::wm
