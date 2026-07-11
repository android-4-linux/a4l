// SPDX-License-Identifier: Apache-2.0
// Provenance: wayland-cxx-scanner (MIT) — its scanner-generated protocol headers
// and header-only wl/ CRTP framework (proxy_impl/registry/display/xdg_shell/
// presentation), and the structure of its presentation-shm example. Wayland +
// xdg-shell + presentation-time protocols (freedesktop, MIT). No GPL-3.0 sources.
//
// The real Wayland presenter (compiled only when ACL_HAVE_WCS): show acl::gfx
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
#include "fractional_scale_v1_client.hpp"
#include "viewporter_client.hpp"

#include <wl/client_helpers.hpp>
#include <wl/ime/backend.hpp>            // wl::ime::SelectedTextInput (text-input-v3)
#include <wl/ime/text_input_receiver.hpp>
#include <wl/scale_policy.hpp>           // wl::ScalePolicy (buffer-at-physical math)
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

namespace acl::wm {
namespace {

using namespace wayland::client;
using namespace xdg_shell::client;
namespace fs = fractional_scale_v1::client;
namespace vp = viewporter::client;

constexpr const char* kTag = "acl.wm";

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
// fractional-scale + viewporter (HiDPI). Managers/viewport have no events (Attach);
// wp_fractional_scale_v1 carries preferred_scale.
class FracScaleMgrH : public fs::CWpFractionalScaleManagerV1<FracScaleMgrH> {};
class ViewporterH : public vp::CWpViewporter<ViewporterH> {};
class ViewportH : public vp::CWpViewport<ViewportH> {};
class FracScaleH : public fs::CWpFractionalScaleV1<FracScaleH> {
 public:
  std::int32_t* scale_120 = nullptr;
  bool* dirty = nullptr;
  void OnPreferredScale(uint32_t scale) override {
    if (scale_120 != nullptr) *scale_120 = static_cast<std::int32_t>(scale);
    if (dirty != nullptr) *dirty = true;
  }
};
// wl_output: integer scale (the fallback when there's no fractional scale) + the
// display mode (for the Android Display metrics, read separately by query_display).
class OutputH : public CWlOutput<OutputH> {
 public:
  std::int32_t* scale = nullptr;  // presenter: integer scale for set_buffer_scale
  bool* dirty = nullptr;
  int* mode_w = nullptr;          // query_display: current mode dimensions + scale
  int* mode_h = nullptr;
  int* out_scale = nullptr;
  void OnScale(int32_t factor) override {
    if (factor <= 0) return;
    if (scale != nullptr) *scale = factor;
    if (dirty != nullptr) *dirty = true;
    if (out_scale != nullptr) *out_scale = factor;
  }
  void OnMode(uint32_t flags, int32_t w, int32_t h, int32_t) override {
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0u) return;  // 0x1: the active mode
    if (mode_w != nullptr) *mode_w = w;
    if (mode_h != nullptr) *mode_h = h;
  }
};

struct ShmMapping {
  int fd = -1;
  void* data = MAP_FAILED;
  std::size_t size = 0;
  ~ShmMapping() { Reset(); }
  bool Create(std::size_t n) noexcept {
    Reset();
    fd = memfd_create("acl-wm", 0);
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
      else if (iface == fs::wp_fractional_scale_manager_v1_traits::interface_name)
        { fsm_name_ = name; fsm_ver_ = ver; }
      else if (iface == vp::wp_viewporter_traits::interface_name)
        { vpr_name_ = name; vpr_ver_ = ver; }
      else if (iface == wl_output_traits::interface_name)
        { output_name_ = name; output_ver_ = ver; }
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
    // fractional-scale + viewporter managers (HiDPI); event-less -> Attach.
    if (fsm_name_ != 0)
      if (wl_proxy* raw = registry_.Bind<fs::wp_fractional_scale_manager_v1_traits>(fsm_name_, std::min(fsm_ver_, fs::wp_fractional_scale_manager_v1_traits::version)))
        frac_mgr_.Attach(raw);
    if (vpr_name_ != 0)
      if (wl_proxy* raw = registry_.Bind<vp::wp_viewporter_traits>(vpr_name_, std::min(vpr_ver_, vp::wp_viewporter_traits::version)))
        viewporter_.Attach(raw);
    // wl_output: integer-scale fallback (has a scale event -> _SetProxy/BindHandler).
    if (output_name_ != 0 && wl::BindHandler<wl_output_traits>(registry_, output_, output_name_, output_ver_)) {
      output_.Get()->scale = &output_scale_;
      output_.Get()->dirty = &scale_dirty_;
    }
    if (!wl::RoundtripWithTimeout(display_.Get())) return false;  // shm formats + clock_id + seat caps + output

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
    // Per-surface fractional_scale (preferred_scale event) + viewport (dst=logical).
    if (!frac_mgr_.IsNull() &&
        wl::SetupHandler(frac_scale_, wl::construct<fs::wp_fractional_scale_v1_traits,
            fs::wp_fractional_scale_manager_v1_traits::Op::GetFractionalScale>(
                *frac_mgr_.Get(), surface_.Get()->GetProxy()))) {
      frac_scale_.Get()->scale_120 = &frac_scale_120_;
      frac_scale_.Get()->dirty = &scale_dirty_;
    }
    if (!viewporter_.IsNull())
      if (wl_proxy* raw = wl::construct<vp::wp_viewport_traits, vp::wp_viewporter_traits::Op::GetViewport>(
              *viewporter_.Get(), surface_.Get()->GetProxy()))
        viewport_.Attach(raw);

    frame_.need = &need_frame_;
    frame_.count = &frame_callbacks_;

    gfx::Surface canvas(w_, h_);
    // Commit (no buffer) to elicit configure + preferred_scale, then size the buffer
    // physically for the reported scale and map it back to logical via the viewport.
    surface_.Get()->Commit();
    (void)wl::RoundtripWithTimeout(display_.Get());
    apply_scale();
    if (!MakeBuffers()) { log::w(kTag, "present: shm buffer allocation failed"); return false; }

    const int fd = wl_display_get_fd(display_.Get());
    // A bounded run (a test / fixed frame count) gets a safety deadline so it can't
    // hang; an unbounded run (--present-frames=0) presents until the user closes the
    // window, so it has no deadline.
    const bool bounded = max_frames_ != 0;
    const std::uint64_t deadline = now_ns(CLOCK_MONOTONIC) + 15ull * 1000000000ull;
    bool clean = true;
    while (!closed_) {
      if (bounded && now_ns(CLOCK_MONOTONIC) > deadline) { log::w(kTag, "present: loop deadline exceeded"); clean = false; break; }
      // Runtime scale change (compositor moved the window to a different-DPI output):
      // recompute the physical size and re-create the buffers.
      if (scale_dirty_) {
        const std::int32_t opw = pw_, oph = ph_;
        apply_scale();
        if ((pw_ != opw || ph_ != oph) && !rebuild_buffers()) { clean = false; break; }
        need_frame_ = true;  // redraw at the new physical size
      }
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
    if (input_ != nullptr && *input_) (*input_)(TextInput{std::string(utf8), false, 1});
  }
  void OnPreeditString(std::string_view utf8, int32_t /*cursor_begin*/,
                       int32_t /*cursor_end*/) override {
    if (input_ != nullptr && *input_) (*input_)(TextInput{std::string(utf8), true, 1});
  }
  void OnDeleteSurroundingText(uint32_t before, uint32_t after) override {
    if (input_ != nullptr && *input_) (*input_)(DeleteSurroundingText{before, after});
  }
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
    blit_to_buffer(canvas, bufs_[b].pixels);
    request_frame();  // ask for the next vsync tick
    presentation_.Arm(surface_.Get()->GetProxy(), static_cast<std::uint32_t>(frame_index_));  // before commit
    surface_.Get()->Attach(bufs_[b].buffer.Get()->GetProxy(), 0, 0);
    surface_.Get()->Damage(0, 0, w_, h_);
    surface_.Get()->Commit();
    bufs_[b].busy = true;
    ++frame_index_;
    return true;
  }

  // Choose the buffer scale and how it maps to the surface:
  //   * a fractional preferred_scale (non-unity) -> physical buffer + wp_viewport
  //     destination = logical (arbitrary ratios),
  //   * else an integer wl_output.scale > 1 -> physical buffer + wl_surface.
  //     set_buffer_scale (HiDPI without a fractional-scale compositor),
  //   * else unity (buffer == logical).
  void apply_scale() {
    scale_dirty_ = false;
    const bool have_frac = !frac_scale_.IsNull() && frac_scale_120_ != 0 &&
                           frac_scale_120_ != wl::ScalePolicy::kUnityScale120 && !viewport_.IsNull();
    if (have_frac) {
      scale_120_ = frac_scale_120_;
    } else if (output_scale_ > 1) {
      scale_120_ = output_scale_ * wl::ScalePolicy::kUnityScale120;
    } else {
      scale_120_ = wl::ScalePolicy::kUnityScale120;
      pw_ = w_;
      ph_ = h_;
      if (!surface_.IsNull()) surface_.Get()->SetBufferScale(1);
      return;
    }
    pw_ = wl::ScalePolicy::ScaledDim(w_, scale_120_);
    ph_ = wl::ScalePolicy::ScaledDim(h_, scale_120_);
    if (pw_ <= 0) pw_ = w_;
    if (ph_ <= 0) ph_ = h_;
    if (have_frac) {
      surface_.Get()->SetBufferScale(1);
      viewport_.Get()->SetDestination(w_, h_);
      log::i(kTag, "fractional scale {}/120: buffer {}x{} -> viewport {}x{}",
             scale_120_, pw_, ph_, w_, h_);
    } else {
      surface_.Get()->SetBufferScale(output_scale_);  // integer HiDPI
      log::i(kTag, "output scale {}: buffer {}x{} (set_buffer_scale)", output_scale_, pw_, ph_);
    }
  }

  // Copy the logical-size render into the physical buffer (memcpy at unity scale,
  // else a nearest-neighbor upscale).
  void blit_to_buffer(const gfx::Surface& canvas, std::uint32_t* dst) {
    const std::uint32_t* src = canvas.data();
    if (pw_ == w_ && ph_ == h_) {
      std::memcpy(dst, src, static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 4u);
      return;
    }
    for (int y = 0; y < ph_; ++y) {
      const int sy = y * h_ / ph_;
      const std::uint32_t* srow = src + static_cast<std::size_t>(sy) * w_;
      std::uint32_t* drow = dst + static_cast<std::size_t>(y) * pw_;
      for (int x = 0; x < pw_; ++x) drow[x] = srow[x * w_ / pw_];
    }
  }

  // Tear down and re-create the shm buffers at the current physical size (after a
  // runtime scale change). Old proxies/mmap are released; a busy buffer becoming
  // inert is fine — the compositor treats a destroyed wl_buffer as released.
  bool rebuild_buffers() {
    for (Buf& b : bufs_) { b.buffer.Reset(); b.busy = false; b.pixels = nullptr; }
    pool_.Reset();
    mem_.Reset();
    return MakeBuffers();
  }

  bool MakeBuffers() {
    const std::size_t stride = static_cast<std::size_t>(pw_) * 4u;
    const std::size_t per = stride * static_cast<std::size_t>(ph_);
    total_bytes_ = per;
    if (!mem_.Create(per * kBuffers)) return false;
    wl_proxy* raw_pool = wl::construct<wl_shm_pool_traits, wl_shm_traits::Op::CreatePool>(
        *shm_.Get(), mem_.fd, static_cast<int32_t>(per * kBuffers));
    if (raw_pool == nullptr) return false;
    pool_.Attach(raw_pool);
    for (int i = 0; i < kBuffers; ++i) {
      const auto off = static_cast<int32_t>(static_cast<std::size_t>(i) * per);
      wl_proxy* raw = wl::construct<wl_buffer_traits, wl_shm_pool_traits::Op::CreateBuffer>(
          *pool_.Get(), off, pw_, ph_, static_cast<int32_t>(stride), WL_SHM_FORMAT_ARGB8888);
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
  wl::WlPtr<FracScaleMgrH> frac_mgr_;
  wl::WlPtr<FracScaleH> frac_scale_;   // per-surface; preferred_scale event
  wl::WlPtr<ViewporterH> viewporter_;
  wl::WlPtr<ViewportH> viewport_;
  wl::WlPtr<OutputH> output_;
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
  uint32_t fsm_name_ = 0, fsm_ver_ = 0, vpr_name_ = 0, vpr_ver_ = 0, output_name_ = 0, output_ver_ = 0;
  std::int32_t scale_120_ = 120, frac_scale_120_ = 0, output_scale_ = 1, pw_ = 0, ph_ = 0;
  bool scale_dirty_ = false;
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

bool query_display(int& width, int& height, int& scale) noexcept {
  wl::DisplayHandle d;
  if (!d.Connect()) return false;
  wl::CRegistry reg;
  if (!reg.Create(d.Get())) return false;
  uint32_t out_name = 0, out_ver = 0;
  reg.OnGlobal([&](wl::CRegistry&, uint32_t name, std::string_view iface, uint32_t ver) {
    if (iface == wl_output_traits::interface_name) { out_name = name; out_ver = ver; }
  });
  if (!wl::RoundtripWithTimeout(d.Get()) || out_name == 0) return false;
  wl::WlPtr<OutputH> out;
  if (!wl::BindHandler<wl_output_traits>(reg, out, out_name,
                                         std::min(out_ver, wl_output_traits::version)))
    return false;
  int mw = 0, mh = 0, sc = 1;
  out.Get()->mode_w = &mw;
  out.Get()->mode_h = &mh;
  out.Get()->out_scale = &sc;
  // geometry/mode/scale/done arrive together after bind.
  if (!wl::RoundtripWithTimeout(d.Get()) || mw <= 0 || mh <= 0) return false;
  width = mw;
  height = mh;
  scale = sc;
  return true;
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

}  // namespace acl::wm
