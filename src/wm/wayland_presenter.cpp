// SPDX-License-Identifier: Apache-2.0
// Provenance: wayland-cxx-scanner (MIT) — its scanner-generated protocol headers
// and header-only wl/ CRTP framework (proxy_impl/registry/display/xdg_shell/
// presentation), and the structure of its presentation-shm example. Wayland +
// xdg-shell + presentation-time protocols (freedesktop, MIT). No GPL-3.0 sources.
//
// The real Wayland presenter (compiled only when ACLCXX_HAVE_WCS): show aclcxx::gfx
// frames via wcs CRTP bindings. present_loop() runs a Choreographer-style loop —
// each wl_surface.frame callback (the vsync tick) drives one FrameCallback, which
// draws the next frame; wp_presentation feedback supplies the vsync-aligned frame
// time (like Choreographer.FrameCallback's frameTimeNanos). Double-buffered so the
// compositor is never reading a buffer we're writing. present_surface() is the
// one-frame case, delegating to present_loop(max_frames=1).
//
// wcs lifetime rule: handler objects must have a stable address for their bound
// lifetime (the dispatcher is registered by address). All handlers are direct
// members of the non-movable Presenter, so their addresses never change.

#include "wm/present.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

#include <poll.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

// Generated protocol headers FIRST (traits + CRTP proxy classes), then the wl/
// framework headers that extend them (interface tables, xdg + presentation helpers).
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"
#include "presentation_time_client.hpp"
#include "text_input_v3_client.hpp"  // WL_IME_BACKEND_TEXT_INPUT_V3

#include <wl/client_helpers.hpp>
#include <wl/ime/backend.hpp>            // wl::ime::SelectedTextInput (text-input-v3)
#include <wl/ime/text_input_receiver.hpp>
#include <wl/display.hpp>
#include <wl/keyboard.hpp>
#include <wl/pointer.hpp>
#include <wl/presentation.hpp>
#include <wl/present_feedback.hpp>
#include <wl/proxy_impl.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

#include "log/log.h"

namespace aclcxx::wm {
namespace {

using namespace wayland::client;
using namespace xdg_shell::client;

constexpr const char* kTag = "aclcxx.wm";

std::uint64_t now_ns(clockid_t clk) noexcept {
  timespec ts{};
  clock_gettime(clk, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// Event-less interfaces (wl_compositor, wl_shm_pool) have no generated _Dispatch,
// so they use WlPtr::Attach; interfaces with events use _SetProxy.
class CompositorH : public CWlCompositor<CompositorH> {};
class ShmPoolH : public CWlShmPool<ShmPoolH> {};
class ShmH : public CWlShm<ShmH> {
 public:
  uint32_t formats = 0;
  void OnFormat(uint32_t f) override { if (f < 32u) formats |= (1u << f); }
};
class SurfaceH : public CWlSurface<SurfaceH> {};
// One wl_buffer; its release event clears the busy flag the loop checks.
class BufferH : public CWlBuffer<BufferH> {
 public:
  bool* busy = nullptr;
  void OnRelease() override { if (busy != nullptr) *busy = false; }
};
// wl_surface.frame: the compositor's "ready for the next frame" tick (vsync).
class FrameH : public CWlCallback<FrameH> {
 public:
  bool* need = nullptr;
  std::uint64_t* count = nullptr;
  void OnDone(uint32_t) override {
    if (need != nullptr) *need = true;
    if (count != nullptr) ++*count;
  }
};

struct ShmMapping {
  int fd = -1;
  void* data = MAP_FAILED;
  std::size_t size = 0;
  ~ShmMapping() { Reset(); }
  bool Create(std::size_t n) noexcept {
    Reset();
    fd = memfd_create("aclcxx-wm", 0);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(n)) < 0) { Reset(); return false; }
    data = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { Reset(); return false; }
    size = n;
    return true;
  }
  void Reset() noexcept {
    if (data != MAP_FAILED) { munmap(data, size); data = MAP_FAILED; }
    if (fd >= 0) { close(fd); fd = -1; }
    size = 0;
  }
};

class Presenter;

// wl_seat: on the capabilities event, create the wl_pointer and point its handler
// at the Presenter. Only uses Presenter as a pointer, so a forward declaration is
// enough (kept above Presenter because Presenter owns a WlPtr<SeatH>).
class SeatH : public CWlSeat<SeatH> {
 public:
  Presenter* app_ = nullptr;
  wl::WlPtr<wl::PointerHandler<Presenter>>* pointer_slot_ = nullptr;
  wl::WlPtr<wl::KeyboardHandler<Presenter>>* keyboard_slot_ = nullptr;
  void OnCapabilities(uint32_t caps) override {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0u && pointer_slot_ != nullptr &&
        pointer_slot_->IsNull()) {
      if (wl::SetupHandler(*pointer_slot_,
              wl::construct<wl_pointer_traits, wl_seat_traits::Op::GetPointer>(*this)))
        pointer_slot_->Get()->app_ = app_;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0u && keyboard_slot_ != nullptr &&
        keyboard_slot_->IsNull()) {
      if (wl::SetupHandler(*keyboard_slot_,
              wl::construct<wl_keyboard_traits, wl_seat_traits::Op::GetKeyboard>(*this)))
        keyboard_slot_->Get()->app_ = app_;
    }
  }
};

class Presenter : public wl::ime::TextInputListener {
 public:
  Presenter() = default;
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;
  ~Presenter() override { presentation_.Release(); }

  bool Run(int w, int h, std::string_view title, std::string_view app_id,
           const FrameCallback& render, const InputCallback& input, bool want_ime,
           std::uint64_t max_frames) {
    w_ = w;
    h_ = h;
    if (w_ <= 0 || h_ <= 0) { log::w(kTag, "present: empty surface"); return false; }
    render_ = &render;
    input_ = &input;
    want_ime_ = want_ime;
    max_frames_ = max_frames;

    if (!display_.Connect()) { log::w(kTag, "present: no Wayland display"); return false; }
    if (!registry_.Create(display_.Get())) { log::w(kTag, "present: wl_registry unavailable"); return false; }
    registry_.OnGlobal([this](wl::CRegistry&, uint32_t name, std::string_view iface, uint32_t ver) {
      if (iface == wl_compositor_traits::interface_name) { comp_name_ = name; comp_ver_ = ver; }
      else if (iface == wl_shm_traits::interface_name) { shm_name_ = name; shm_ver_ = ver; }
      else if (iface == xdg_wm_base_traits::interface_name) { xdg_name_ = name; xdg_ver_ = ver; }
      else if (iface == presentation_time::client::wp_presentation_traits::interface_name)
        presentation_.Record(name, ver);
      else if (iface == wl_seat_traits::interface_name) { seat_name_ = name; seat_ver_ = ver; }
      else if (iface == text_input_unstable_v3::client::zwp_text_input_manager_v3_traits::interface_name)
        { ti_name_ = name; ti_ver_ = ver; }
    });
    if (!wl::RoundtripWithTimeout(display_.Get())) { log::w(kTag, "present: registry roundtrip failed"); return false; }
    if (comp_name_ == 0 || shm_name_ == 0 || xdg_name_ == 0) {
      log::w(kTag, "present: compositor missing wl_compositor/wl_shm/xdg_wm_base");
      return false;
    }

    if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(comp_name_, std::min(comp_ver_, wl_compositor_traits::version)))
      comp_.Attach(raw);
    else return false;
    if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) return false;
    if (!wl::BindHandler<xdg_wm_base_traits>(registry_, xdg_wm_base_, xdg_name_, xdg_ver_)) return false;
    if (!presentation_.Bind(registry_, this)) return false;  // wp_presentation (optional)
    if (seat_name_ != 0 && wl::BindHandler<wl_seat_traits>(registry_, seat_, seat_name_, seat_ver_)) {
      seat_.Get()->app_ = this;
      seat_.Get()->pointer_slot_ = &pointer_;    // wl_pointer created on the capabilities event
      seat_.Get()->keyboard_slot_ = &keyboard_;  // wl_keyboard created likewise
    }
    // text-input-v3 (IME): bind the manager + per-seat text_input via the wcs facade.
    if (want_ime_ && ti_name_ != 0 && !seat_.IsNull()) {
      ti_.Record(ti_name_, ti_ver_);
      ime_bound_ = ti_.Bind(registry_, this, seat_.Get()->GetProxy());
      if (!ime_bound_) log::w(kTag, "present: text-input bind failed");
    }
    if (!wl::RoundtripWithTimeout(display_.Get())) return false;  // shm formats + clock_id + seat caps

    if (!wl::SetupHandler(surface_, wl::construct<wl_surface_traits, wl_compositor_traits::Op::CreateSurface>(*comp_.Get()))) return false;
    if (!wl::SetupHandler(xdg_surface_, wl::construct<xdg_surface_traits, xdg_wm_base_traits::Op::GetXdgSurface>(*xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) return false;
    xdg_surface_.Get()->app_ = this;
    if (!wl::SetupHandler(xdg_toplevel_, wl::construct<xdg_toplevel_traits, xdg_surface_traits::Op::GetToplevel>(*xdg_surface_.Get()))) return false;
    xdg_toplevel_.Get()->app_ = this;
    title_.assign(title);
    app_id_.assign(app_id);
    xdg_toplevel_.Get()->SetTitle(title_.c_str());
    xdg_toplevel_.Get()->SetAppId(app_id_.c_str());

    if (ime_bound_) {  // enable text input on this surface's seat
      ti_.SetContentType(wl::ime::ContentHint::kNone, wl::ime::ContentPurpose::kNormal);
      ti_.Activate();
    }

    if (!MakeBuffers()) { log::w(kTag, "present: shm buffer allocation failed"); return false; }
    frame_.need = &need_frame_;
    frame_.count = &frame_callbacks_;

    gfx::Surface canvas(w_, h_);
    surface_.Get()->Commit();  // trigger the first configure

    const int fd = wl_display_get_fd(display_.Get());
    // A bounded run (a test / fixed frame count) gets a safety deadline so it can't
    // hang; an unbounded run (--present-frames=0) presents until the user closes the
    // window, so it has no deadline.
    const bool bounded = max_frames_ != 0;
    const std::uint64_t deadline = now_ns(CLOCK_MONOTONIC) + 15ull * 1000000000ull;
    bool clean = true;
    while (!closed_) {
      if (bounded && now_ns(CLOCK_MONOTONIC) > deadline) { log::w(kTag, "present: loop deadline exceeded"); clean = false; break; }
      // Render on each vsync tick, up to the frame budget.
      if (configured_ && need_frame_ && (max_frames_ == 0 || frame_index_ < max_frames_)) {
        need_frame_ = false;
        if (!render_frame(canvas)) break;  // FrameCallback asked to stop
      }
      // All requested frames committed AND acknowledged by the compositor.
      if (max_frames_ != 0 && frame_index_ >= max_frames_ && frame_callbacks_ >= max_frames_) break;

      while (wl_display_flush(display_.Get()) < 0 && errno == EAGAIN) {}
      presentation_.Reap();
      // Poll the wl fd and the keyboard repeat timerfd (wcs KeyboardHandler): when
      // the latter is readable, a held key repeats. fd<0 slots are ignored by poll.
      const int rfd = keyboard_.IsNull() ? -1 : keyboard_.Get()->GetRepeatFd();
      pollfd pfds[2] = {{fd, POLLIN, 0}, {rfd, POLLIN, 0}};
      const int n = poll(pfds, 2, 200);
      if (n < 0) { if (errno == EINTR) continue; clean = false; break; }
      if ((pfds[1].revents & POLLIN) != 0) keyboard_.Get()->DispatchRepeat();
      if ((pfds[0].revents & POLLIN) != 0 && wl_display_dispatch(display_.Get()) < 0) {
        log::w(kTag, "present: dispatch error"); clean = false; break;
      }
    }
    ti_.Release();
    presentation_.Release();
    log::i(kTag,
           "present loop stopped: {} frame(s) committed, {} shown, {} presentation-feedback, {} ({})",
           frame_index_, frame_callbacks_, presented_count_,
           closed_ ? "toplevel closed" : "done", clean ? "clean" : "aborted");
    // A user-initiated close is a clean, successful exit (not an abort).
    return clean && frame_index_ > 0;
  }

  // wp_presentation feedback — the vsync clock that aligns the frame time.
  void OnPresented(const wl::PresentFeedback& fb) {
    last_present_ns_ = fb.present_ns;
    if (fb.refresh_ns != 0) refresh_ns_ = fb.refresh_ns;
    ++presented_count_;
  }
  void OnDiscarded(std::uint32_t) {}

  // xdg callbacks (invoked by wl::XdgSurfaceHandler / XdgToplevelHandler)
  void OnXdgSurfaceConfigure(uint32_t) { configured_ = true; need_frame_ = true; }
  void OnToplevelConfigure(int32_t, int32_t) {}
  void OnToplevelClose() { closed_ = true; }

  // wl_pointer -> app input (the native side of a MotionEvent). A left-button
  // press/release maps to a touch Down/Up; motion while pressed maps to Move.
  void OnPointerEnter(const wl::PointerEvent& e) { px_ = e.x; py_ = e.y; }
  void OnPointerLeave() {}
  void OnPointerMotion(const wl::PointerEvent& e) {
    px_ = e.x; py_ = e.y;
    if (pointer_down_) deliver(PointerInput::Action::Move);
  }
  void OnPointerButton(const wl::PointerButtonEvent& e) {
    px_ = e.x; py_ = e.y;
    const bool pressed = e.state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (pressed == pointer_down_) return;  // only edges
    pointer_down_ = pressed;
    deliver(pressed ? PointerInput::Action::Down : PointerInput::Action::Up);
  }

  // wl_keyboard -> app input (the native side of a KeyEvent). Press/release and
  // repeat (timerfd- or server-driven, ev.repeat) all deliver as Down/Up.
  void OnKey(const wl::KeyEvent& e) {
    if (input_ == nullptr || !*input_) return;
    const bool pressed = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;
    (*input_)(KeyInput{pressed ? KeyInput::Action::Down : KeyInput::Action::Up,
                       e.key, static_cast<std::uint32_t>(e.keysym)});
  }

  // wl::ime::TextInputListener — events from the compositor's IME (text-input-v3).
  void OnEnter() override {}
  void OnLeave() override {}
  void OnCommitString(std::string_view utf8) override {
    if (input_ != nullptr && *input_) (*input_)(TextInput{std::string(utf8)});
  }
  void OnPreeditString(std::string_view /*utf8*/, int32_t /*cursor_begin*/,
                       int32_t /*cursor_end*/) override {}
  void OnDeleteSurroundingText(uint32_t /*before*/, uint32_t /*after*/) override {}
  void OnKeysym(uint32_t /*time*/, uint32_t /*sym*/, uint32_t /*state*/,
                uint32_t /*mods*/) override {}

 private:
  // The vsync-aligned time for the frame about to be drawn: the last presentation
  // time plus one refresh interval (Choreographer.frameTimeNanos), or the raw
  // presentation clock before any feedback has arrived.
  std::uint64_t frame_time() const {
    if (last_present_ns_ != 0 && refresh_ns_ != 0) return last_present_ns_ + refresh_ns_;
    return now_ns(presentation_.Bound() ? presentation_.clock() : CLOCK_MONOTONIC);
  }

  void request_frame() {
    if (wl_proxy* raw = wl::construct<wl_callback_traits, wl_surface_traits::Op::Frame>(*surface_.Get()))
      frame_._SetProxy(raw);
  }

  void deliver(PointerInput::Action a) {
    if (input_ != nullptr && *input_)
      (*input_)(PointerInput{a, static_cast<float>(px_), static_cast<float>(py_)});
  }

  bool render_frame(gfx::Surface& canvas) {
    int b = -1;
    for (int i = 0; i < 2; ++i) if (!bufs_[i].busy) { b = i; break; }
    if (b < 0) { request_frame(); return true; }  // both in flight — retry next tick
    if (!(*render_)(canvas, frame_time(), frame_index_)) return false;
    std::memcpy(bufs_[b].pixels, canvas.data(), total_bytes_);
    request_frame();  // ask for the next vsync tick
    presentation_.Arm(surface_.Get()->GetProxy(), static_cast<std::uint32_t>(frame_index_));  // before commit
    surface_.Get()->Attach(bufs_[b].buffer.Get()->GetProxy(), 0, 0);
    surface_.Get()->Damage(0, 0, w_, h_);
    surface_.Get()->Commit();
    bufs_[b].busy = true;
    ++frame_index_;
    return true;
  }

  bool MakeBuffers() {
    const std::size_t stride = static_cast<std::size_t>(w_) * 4u;
    const std::size_t per = stride * static_cast<std::size_t>(h_);
    total_bytes_ = per;
    if (!mem_.Create(per * kBuffers)) return false;
    wl_proxy* raw_pool = wl::construct<wl_shm_pool_traits, wl_shm_traits::Op::CreatePool>(
        *shm_.Get(), mem_.fd, static_cast<int32_t>(per * kBuffers));
    if (raw_pool == nullptr) return false;
    pool_.Attach(raw_pool);
    for (int i = 0; i < kBuffers; ++i) {
      const auto off = static_cast<int32_t>(static_cast<std::size_t>(i) * per);
      wl_proxy* raw = wl::construct<wl_buffer_traits, wl_shm_pool_traits::Op::CreateBuffer>(
          *pool_.Get(), off, w_, h_, static_cast<int32_t>(stride), WL_SHM_FORMAT_ARGB8888);
      if (raw == nullptr) return false;
      bufs_[i].buffer.Get()->_SetProxy(raw);
      bufs_[i].buffer.Get()->busy = &bufs_[i].busy;
      bufs_[i].pixels = reinterpret_cast<std::uint32_t*>(
          static_cast<char*>(mem_.data) + static_cast<std::size_t>(i) * per);
      bufs_[i].busy = false;
    }
    pool_.Reset();  // only needed during buffer creation
    return true;
  }

  static constexpr int kBuffers = 2;
  struct Buf {
    wl::WlPtr<BufferH> buffer;
    std::uint32_t* pixels = nullptr;
    bool busy = false;
  };

  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<CompositorH> comp_;
  wl::WlPtr<ShmH> shm_;
  wl::WlPtr<ShmPoolH> pool_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<SurfaceH> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<Presenter>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<Presenter>> xdg_toplevel_;
  wl::PresentationManager<Presenter> presentation_;
  wl::ime::SelectedTextInput ti_;  // text-input-v3 (IME)
  wl::WlPtr<SeatH> seat_;
  wl::WlPtr<wl::PointerHandler<Presenter>> pointer_;   // created on wl_seat capabilities
  wl::WlPtr<wl::KeyboardHandler<Presenter>> keyboard_;  // created on wl_seat capabilities
  Buf bufs_[kBuffers];
  FrameH frame_;  // direct member: stable address for the dispatcher
  ShmMapping mem_;
  std::string title_, app_id_;
  const FrameCallback* render_ = nullptr;
  const InputCallback* input_ = nullptr;
  double px_ = 0.0, py_ = 0.0;
  bool pointer_down_ = false;

  uint32_t comp_name_ = 0, comp_ver_ = 0, shm_name_ = 0, shm_ver_ = 0, xdg_name_ = 0, xdg_ver_ = 0;
  uint32_t seat_name_ = 0, seat_ver_ = 0, ti_name_ = 0, ti_ver_ = 0;
  bool want_ime_ = false, ime_bound_ = false;
  int w_ = 0, h_ = 0;
  std::size_t total_bytes_ = 0;
  std::uint64_t max_frames_ = 0, frame_index_ = 0, frame_callbacks_ = 0, presented_count_ = 0;
  std::uint64_t last_present_ns_ = 0, refresh_ns_ = 0;
  bool configured_ = false, need_frame_ = false, closed_ = false;
};

}  // namespace

bool wayland_available() noexcept {
  wl::DisplayHandle d;
  return d.Connect();  // disconnects on destruction
}

bool present_loop(int width, int height, std::string_view title,
                  std::string_view app_id, const FrameCallback& render,
                  const InputCallback& input, bool want_ime,
                  std::uint64_t max_frames) noexcept {
  Presenter presenter;
  return presenter.Run(width, height, title, app_id, render, input, want_ime, max_frames);
}

bool present_surface(const gfx::Surface& surface, std::string_view title,
                     std::string_view app_id, int timeout_ms) noexcept {
  (void)timeout_ms;
  // One frame: blit the already-rendered surface into the loop's working canvas.
  const FrameCallback one = [&surface](gfx::Surface& canvas, std::uint64_t, std::uint64_t) {
    canvas.blit(surface.data());
    return true;
  };
  return present_loop(surface.width(), surface.height(), title, app_id, one,
                      InputCallback{}, false, 1);
}

}  // namespace aclcxx::wm
