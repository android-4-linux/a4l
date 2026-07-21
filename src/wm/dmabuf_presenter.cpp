// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §2, ADR-4/ADR-6, Addendum A02; wcs example skia-vulkan-dmabuf
// (Apache-2.0/MIT) — the zwp_linux_dmabuf_v1 present pattern (DmabufFeedback →
// modifiers → create_params/add/create_immed → wl_buffer) and the wcs wl/ framework
// (wl::DmabufFeedback, registry, xdg_shell). Android Surface/ViewRootImpl docs
// (behavioral). No GPL-3.0 sources.
//
// P2.4/V2b (wm side): present a Ganesh-Vulkan-rendered frame to the compositor as a
// Linux dma-buf, so pixels reach it without a wl_shm copy. A focused presenter kept
// separate from the mature shm wayland_presenter.cpp: bind zwp_linux_dmabuf_v1, learn
// the compositor's advertised modifiers, ask the producer (gfx VkRenderTarget, via the
// DmabufRenderFn) for a dma-buf frame (modifier-tiled direct or linear fallback), build
// a wl_buffer from it, and present it under an xdg_toplevel. Built only when wcs +
// Vulkan are both present (ACL_HAVE_WCS && ACL_HAVE_VULKAN).

#include "wm/present.h"

#if ACL_HAVE_WCS && ACL_HAVE_VULKAN

#include <cerrno>
#include <cstdint>
#include <cstdlib>  // getenv/strtoul (ACL_WL_SHELL shell selection)
#include <ctime>
#include <string_view>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <wayland-client.h>

// Generated CRTP client headers (scanner output) — MUST precede the wl/ framework.
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"
#include "linux_dmabuf_client.hpp"
#include "agl_shell_client.hpp"          // ACL_WL_SHELL=agl (AGL agl_shell)
#include "ivi_application_client.hpp"    // ACL_WL_SHELL=ivi (ivi-shell)
#include "fullscreen_shell_client.hpp"   // ACL_WL_SHELL=simple (zwp_fullscreen_shell_v1)
#include "presentation_time_client.hpp"
#include "text_input_v3_client.hpp"  // WL_IME_BACKEND_TEXT_INPUT_V3 (IME)

#include <wl/client_helpers.hpp>
#include <wl/dmabuf_feedback.hpp>  // wl::DmabufFeedback<App>, wl::FeedbackSnapshot
#include <wl/display.hpp>
#include <wl/ime/backend.hpp>            // wl::ime::SelectedTextInput (text-input-v3)
#include <wl/ime/text_input_receiver.hpp>
#include <wl/keyboard.hpp>              // wl::KeyboardHandler (xkb keymap/repeat)
#include <wl/linux_dmabuf.hpp>     // must follow linux_dmabuf_client.hpp
#include <wl/pointer.hpp>              // wl::PointerHandler
#include <wl/cursor.hpp>               // wl::CursorManager (wl_pointer.set_cursor)
#include <wl/present_feedback.hpp>
#include <wl/presentation.hpp>     // wl::PresentationManager<App>, wl::PresentFeedback
#include <wl/proxy_impl.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>        // must follow xdg_shell_client.hpp

#include "log/log.h"

namespace acl::wm {
namespace {

using namespace wayland::client;
using namespace xdg_shell::client;
namespace dmabuf = linux_dmabuf_unstable_v1::client;

constexpr const char* kTag = "acl.wm.dmabuf";

// DRM_FORMAT_XRGB8888 == fourcc('X','R','2','4') — the opaque 32-bit format the gfx
// dma-buf export uses. Hardcoded so wm/ needn't pull in libdrm (the value is stable).
constexpr std::uint32_t kDrmFormatXrgb8888 = 0x34325258u;

// Poll timeout / keep-alive self-tick interval (ms) for the animated loop when the
// compositor withholds frame callbacks (occluded surface). ~30 Hz fallback.
constexpr int kTickMs = 33;

// Event-less globals use WlPtr::Attach; interfaces with events use handler subclasses.
class CompositorH : public CWlCompositor<CompositorH> {};
// wl_shm — not used for buffers on the dma-buf path, only to load the cursor theme
// (wl_cursor_theme_load needs a wl_shm). Formats are ignored.
class ShmH : public CWlShm<ShmH> {
 public:
  void OnFormat(std::uint32_t) override {}
};
class SurfaceH : public CWlSurface<SurfaceH> {};
// The wl_buffer's release event clears the busy flag (compositor done reading the frame).
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
  void OnDone(std::uint32_t) override {
    if (need != nullptr) *need = true;
    if (count != nullptr) ++*count;
  }
};
// zwp_linux_buffer_params_v1 carries created/failed events; create_immed doesn't use
// them, so a bare handler suffices.
class BufferParamsH
    : public dmabuf::CZwpLinuxBufferParamsV1<BufferParamsH> {};

namespace agl = agl_shell::client;
namespace ivi = ivi_application::client;
namespace fsh = fullscreen_shell_unstable_v1::client;

// ── Wayland shell selection (ACL_WL_SHELL) ───────────────────────────────────────────
// The compositor role the surface takes. `Xdg` (default) is the desktop stable shell;
// `Agl` targets AGL's agl_shell (acl acts as the shell client — background/panel, the
// ivi-homescreen automotive pattern); `Ivi` targets ivi-shell's ivi_application (numeric
// surface id); `Simple` uses zwp_fullscreen_shell_v1 (present a surface, no wm).
enum class ShellKind { Xdg, Agl, Ivi, Simple };
// The agl_shell surface role (ACL_AGL_WINDOW_TYPE); Background is the default homescreen role.
enum class AglRole { Background, PanelTop, PanelBottom, PanelLeft, PanelRight, Normal };

ShellKind shell_from_env() noexcept {
  const char* s = std::getenv("ACL_WL_SHELL");
  if (s == nullptr || *s == '\0') return ShellKind::Xdg;  // default: xdg (desktop)
  const std::string_view v(s);
  if (v == "agl") return ShellKind::Agl;
  if (v == "ivi") return ShellKind::Ivi;
  if (v == "simple" || v == "fullscreen") return ShellKind::Simple;
  return ShellKind::Xdg;  // "xdg" or anything unrecognized
}
AglRole agl_role_from_env() noexcept {
  const char* s = std::getenv("ACL_AGL_WINDOW_TYPE");
  if (s == nullptr || *s == '\0') return AglRole::Background;
  const std::string_view v(s);
  if (v == "panel-top") return AglRole::PanelTop;
  if (v == "panel-bottom") return AglRole::PanelBottom;
  if (v == "panel-left") return AglRole::PanelLeft;
  if (v == "panel-right") return AglRole::PanelRight;
  if (v == "normal") return AglRole::Normal;
  return AglRole::Background;
}
std::uint32_t ivi_id_from_env() noexcept {
  const char* s = std::getenv("ACL_IVI_SURFACE_ID");
  if (s == nullptr || *s == '\0') return 0;
  return static_cast<std::uint32_t>(std::strtoul(s, nullptr, 0));
}

// agl_shell: bound_ok/bound_fail report whether the compositor granted us the shell role
// (only one client may hold it); app_state/app_on_output drive app activation (unused —
// acl presents its own surface, it is not the app launcher).
class AglShellH : public agl::CAglShell<AglShellH> {
 public:
  bool bound_ok = false;
  bool bound_done = false;
  void OnBoundOk() override { bound_ok = true; bound_done = true; }
  void OnBoundFail() override { bound_ok = false; bound_done = true; }
  void OnAppState(const char*, std::uint32_t) override {}
  void OnAppOnOutput(const char*, const char*) override {}
};
// ivi_application has no events; a bare proxy is enough to call surface_create.
class IviAppH : public ivi::CIviApplication<IviAppH> {};
// ivi_surface.configure(width,height): the compositor's size for this ivi id. Sets the
// presenter's configured_ flag (pointed at like FrameH::need) so the first frame can go.
class IviSurfaceH : public ivi::CIviSurface<IviSurfaceH> {
 public:
  bool* configured = nullptr;
  bool* need = nullptr;
  std::int32_t w = 0, h = 0;
  void OnConfigure(std::int32_t width, std::int32_t height) override {
    w = width;
    h = height;
    if (configured != nullptr) *configured = true;
    if (need != nullptr) *need = true;
  }
};
// zwp_fullscreen_shell_v1: capability advertises the supported present methods (unused).
class FullscreenShellH : public fsh::CZwpFullscreenShellV1<FullscreenShellH> {
 public:
  void OnCapability(std::uint32_t) override {}
};

class Presenter;

// wl_seat: on the capabilities event, create the wl_pointer / wl_keyboard and point their
// handlers at the Presenter (which delivers the app-facing MotionEvent/KeyEvent). Mirrors
// the shm presenter's SeatH. Forward-declared Presenter is enough (used by pointer only).
class SeatH : public CWlSeat<SeatH> {
 public:
  Presenter* app_ = nullptr;
  wl::WlPtr<wl::PointerHandler<Presenter>>* pointer_slot_ = nullptr;
  wl::WlPtr<wl::KeyboardHandler<Presenter>>* keyboard_slot_ = nullptr;
  void OnCapabilities(std::uint32_t caps) override {
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

// The presenter is the DmabufFeedback "App": it receives OnDmabufFeedback (v4 tranches)
// and OnDmabufLegacyFormat (v1–v3 fallback) via the wcs binding, and drives xdg-shell. It
// is also the pointer/keyboard/IME listener (input parity with the shm presenter).
class Presenter : public IPresenter, public wl::ime::TextInputListener {
 public:
  ~Presenter() override { presentation_.Release(); ti_.Release(); }  // idempotent; static path too

  // Static present: render once, re-attach the same frame each tick.
  bool run(const DmabufRenderFn& render, std::string_view title,
           std::string_view app_id, int frames) noexcept {
    max_frames_ = frames < 0 ? 0 : static_cast<std::uint64_t>(frames);
    if (!setup(title, app_id)) return false;
    // Ask the producer for a frame targeting one of the advertised modifiers.
    gfx::DmabufFrame frame;
    if (!render(mods_, &frame) || frame.fd < 0) {
      log::w(kTag, "dmabuf present: producer did not yield a frame");
      return false;
    }
    log::i(kTag, "dmabuf present: {}x{} {} modifier=0x{:x} {} plane(s)", frame.width,
           frame.height, frame.modifier_tiled ? "tiled" : "linear", frame.modifier,
           frame.plane_count);
    if (!build_buffer(frame)) return false;
    return present_loop();
  }

  // Animated present (S4b): render a fresh GPU frame each vsync tick into a free ping-pong
  // buffer. `input`/`want_ime` are accepted for API parity; wl_seat/text-input binding is a
  // follow-up (the shm path carries input today), so they are unused here for now.
  bool run_loop(const DmabufFrameFn& render, const InputCallback& input, bool want_ime,
                std::string_view title, std::string_view app_id,
                std::uint64_t max_frames) noexcept {
    max_frames_ = max_frames;
    input_ = &input;
    want_ime_ = want_ime;
    if (!setup(title, app_id)) return false;
    frame_fn_ = &render;
    // Enable text input on the surface once it's configured (mirrors the shm path).
    if (ime_bound_) {
      ti_.SetContentType(wl::ime::ContentHint::kNone, wl::ime::ContentPurpose::kNormal);
      ti_.Activate();
    }
    return present_loop_dynamic();
  }

  // ── IPresenter (approach A): the Android Looper drives; we expose primitives ──────────
  // Connect + create the titled toplevel, leaving the presenter ready for its first frame.
  // Owns `input` (the heap presenter outlives the caller, unlike run_loop's borrowed ref).
  bool open(int width, int height, std::string_view title, std::string_view app_id,
            InputCallback input, bool want_ime) noexcept {
    w_ = width;
    h_ = height;
    owned_input_ = std::move(input);
    input_ = &owned_input_;
    want_ime_ = want_ime;
    max_frames_ = 0;
    if (!setup(title, app_id)) return false;
    if (ime_bound_) {
      ti_.SetContentType(wl::ime::ContentHint::kNone, wl::ime::ContentPurpose::kNormal);
      ti_.Activate();
    }
    return true;
  }

  int fd() noexcept override { return wl_display_get_fd(display_.Get()); }
  int repeat_fd() noexcept override {
    return keyboard_.IsNull() ? -1 : keyboard_.Get()->GetRepeatFd();
  }
  void dispatch_repeat() noexcept override {
    if (!keyboard_.IsNull()) keyboard_.Get()->DispatchRepeat();
  }
  // Called when the wl fd is readable: read + dispatch queued events, reap timing feedback,
  // flush replies. Returns false once the toplevel closes.
  bool dispatch() noexcept override {
    if (wl_display_dispatch(display_.Get()) < 0) { closed_ = true; return false; }
    presentation_.Reap();
    while (wl_display_flush(display_.Get()) < 0 && errno == EAGAIN) {}
    return !closed_;
  }
  bool ready() const noexcept override {
    return configured_ && need_frame_ && free_slot() >= 0;
  }
  int next_buffer() const noexcept override { return free_slot(); }
  const std::vector<std::uint64_t>& modifiers() const noexcept override { return mods_; }

  // Present the produced dma-buf into its free ping-pong slot: build the wl_buffer once, arm
  // the next wl_surface.frame tick, attach + damage + commit + flush. Mirrors one iteration of
  // present_loop_dynamic, minus the poll (the Looper owns the wait).
  bool submit(const gfx::DmabufFrame& frame) noexcept override {
    if (frame.fd < 0) return false;
    const int s = free_slot();
    if (s < 0) return false;
    if (!slots_[s].built && !build_slot(s, frame)) return false;
    if (frame_index_ == 0)
      log::i(kTag, "dmabuf present(A): {}x{} {} modifier=0x{:x} {} plane(s), {} buffers",
             frame.width, frame.height, frame.modifier_tiled ? "tiled" : "linear",
             frame.modifier, frame.plane_count, kSlots);
    if (wl_proxy* raw = wl::construct<wl_callback_traits, wl_surface_traits::Op::Frame>(
            *surface_.Get()))
      frame_._SetProxy(raw);
    slots_[s].busy = true;
    need_frame_ = false;
    presentation_.Arm(surface_.Get()->GetProxy(),
                      static_cast<std::uint32_t>(frame_index_));  // before commit
    surface_.Get()->Attach(slots_[s].handler.GetProxy(), 0, 0);
    surface_.Get()->Damage(0, 0, w_, h_);
    surface_.Get()->Commit();
    last_slot_ = s;
    ++frame_index_;
    while (wl_display_flush(display_.Get()) < 0 && errno == EAGAIN) {}
    return true;
  }
  // Keep the loop alive after a dropped frame (see IPresenter::keepalive). Re-arm the
  // wl_surface.frame callback and re-present the LAST submitted buffer (re-attach + damage +
  // commit) so the compositor schedules another frame-done tick — a bare commit with no attach
  // does not make some compositors (weston) repaint, so the buffer must be re-attached.
  // Otherwise a single traversal that fails to submit() strands the vsync loop forever. Only
  // meaningful once a real frame has been shown.
  bool keepalive() noexcept override {
    if (closed_ || !configured_ || last_slot_ < 0 || !slots_[last_slot_].built) return false;
    if (wl_proxy* raw = wl::construct<wl_callback_traits, wl_surface_traits::Op::Frame>(
            *surface_.Get()))
      frame_._SetProxy(raw);
    need_frame_ = false;
    slots_[last_slot_].busy = true;  // compositor reads it again; released on the next real submit
    surface_.Get()->Attach(slots_[last_slot_].handler.GetProxy(), 0, 0);
    surface_.Get()->Damage(0, 0, w_, h_);
    surface_.Get()->Commit();
    while (wl_display_flush(display_.Get()) < 0 && errno == EAGAIN) {}
    return true;
  }
  bool closed() const noexcept override { return closed_; }

  // ── GL mode: acl owns the Looper + vsync clock; Mesa owns the surface's buffers/commit ──
  void* gl_display() noexcept override { return display_.Get(); }
  void* gl_surface() noexcept override {
    return surface_.IsNull() ? nullptr : static_cast<void*>(surface_.Get()->GetProxy());
  }
  // Ask the compositor for presentation feedback on the toplevel's NEXT commit (Mesa's
  // eglSwapBuffers) so OnPresented updates last_present_ns_/refresh_ns_ -> frame_time_ns().
  void arm_present() noexcept override {
    if (!surface_.IsNull())
      presentation_.Arm(surface_.Get()->GetProxy(), static_cast<std::uint32_t>(frame_index_++));
  }
  // Pump events until the first xdg configure arrives (the GL device is built on the configured
  // toplevel surface). Bounded so a broken compositor can't hang the boot.
  bool wait_configured(int timeout_ms) noexcept override {
    for (int i = 0; i < timeout_ms / 10 && !configured_ && !closed_; ++i)
      (void)wl::RoundtripWithTimeout(display_.Get());
    return configured_;
  }

  // Connect, bind (wl_compositor / xdg_wm_base / zwp_linux_dmabuf_v1), learn the advertised
  // modifiers, create the surface + xdg_toplevel, and commit-for-configure. Shared by the
  // static and animated present paths.
  bool setup(std::string_view title, std::string_view app_id) noexcept {
    title_ = std::string(title);
    app_id_ = std::string(app_id);
    shell_kind_ = shell_from_env();
    agl_role_ = agl_role_from_env();
    ivi_id_ = ivi_id_from_env();
    if (shell_kind_ != ShellKind::Xdg)
      log::i(kTag, "dmabuf present: ACL_WL_SHELL selects {} shell", shell_global_name());
    if (!display_.Connect(nullptr)) {
      log::w(kTag, "dmabuf present: no Wayland display (WAYLAND_DISPLAY)");
      return false;
    }
    if (!registry_.Create(display_.Get())) {
      log::w(kTag, "dmabuf present: wl_registry unavailable");
      return false;
    }
    registry_.OnGlobal([this](wl::CRegistry&, std::uint32_t name, std::string_view iface,
                              std::uint32_t ver) {
      if (iface == wl_compositor_traits::interface_name) { comp_name_ = name; comp_ver_ = ver; }
      else if (iface == wl_shm_traits::interface_name) { shm_name_ = name; shm_ver_ = ver; }
      else if (iface == xdg_wm_base_traits::interface_name) { xdg_name_ = name; xdg_ver_ = ver; }
      else if (iface == dmabuf::zwp_linux_dmabuf_v1_traits::interface_name) {
        feedback_.Record(name, ver);
      } else if (iface == presentation_time::client::wp_presentation_traits::interface_name) {
        presentation_.Record(name, ver);
      } else if (iface == wl_seat_traits::interface_name) {
        seat_name_ = name; seat_ver_ = ver;
      } else if (iface ==
                 text_input_unstable_v3::client::zwp_text_input_manager_v3_traits::interface_name) {
        ti_name_ = name; ti_ver_ = ver;
      } else if (iface == agl::agl_shell_traits::interface_name) {
        agl_name_ = name; agl_ver_ = ver;
      } else if (iface == ivi::ivi_application_traits::interface_name) {
        ivi_name_ = name; ivi_ver_ = ver;
      } else if (iface == fsh::zwp_fullscreen_shell_v1_traits::interface_name) {
        fsh_name_ = name; fsh_ver_ = ver;
      } else if (iface == wl_output_traits::interface_name) {
        if (output_name_ == 0) { output_name_ = name; output_ver_ = ver; }
      }
    });
    if (!wl::RoundtripWithTimeout(display_.Get())) {
      log::w(kTag, "dmabuf present: registry roundtrip failed");
      return false;
    }
    if (comp_name_ == 0) {
      log::w(kTag, "dmabuf present: wl_compositor not advertised");
      return false;
    }
    if (!shell_global_present()) {
      log::w(kTag, "dmabuf present: {} not advertised by the compositor",
             shell_global_name());
      return false;
    }
    if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
            comp_name_, std::min(comp_ver_, wl_compositor_traits::version)))
      comp_.Attach(raw);
    else return false;
    // Bind xdg_wm_base whenever present: the Xdg shell needs it, and Agl falls back to it.
    if (xdg_name_ != 0 &&
        !wl::BindHandler<xdg_wm_base_traits>(registry_, xdg_wm_base_, xdg_name_, xdg_ver_))
      return false;
    // Bind the first wl_output (agl set_background / fullscreen present_surface target it).
    if (output_name_ != 0)
      output_ = registry_.Bind<wl_output_traits>(
          output_name_, std::min(output_ver_, wl_output_traits::version));
    // Bind the selected non-xdg shell global.
    if (shell_kind_ == ShellKind::Agl && agl_name_ != 0) {
      if (wl::BindHandler<agl::agl_shell_traits>(registry_, agl_, agl_name_, agl_ver_))
        need_agl_bound_ = true;  // wait for bound_ok/bound_fail before set_background
    } else if (shell_kind_ == ShellKind::Ivi && ivi_name_ != 0) {
      // ivi_application is version 1, event-less: attach a bare proxy for surface_create.
      if (wl_proxy* raw = registry_.Bind<ivi::ivi_application_traits>(
              ivi_name_, std::min(ivi_ver_, ivi::ivi_application_traits::version)))
        ivi_app_.Attach(raw);
    } else if (shell_kind_ == ShellKind::Simple && fsh_name_ != 0) {
      (void)wl::BindHandler<fsh::zwp_fullscreen_shell_v1_traits>(registry_, fsh_, fsh_name_,
                                                                fsh_ver_);
    }
    if (!feedback_.Bind(registry_, this)) {
      log::w(kTag, "dmabuf present: zwp_linux_dmabuf_v1 bind failed");
      return false;
    }
    (void)presentation_.Bind(registry_, this);  // wp_presentation (optional; timing only)
    // wl_seat → pointer/keyboard (created on the capabilities event), and text-input-v3.
    if (seat_name_ != 0 && wl::BindHandler<wl_seat_traits>(registry_, seat_, seat_name_, seat_ver_)) {
      seat_.Get()->app_ = this;
      seat_.Get()->pointer_slot_ = &pointer_;
      seat_.Get()->keyboard_slot_ = &keyboard_;
      // wl_shm (only for the cursor theme) + the cursor manager, so the pointer gets a cursor on
      // enter (this presenter owns no wl_shm otherwise). Best-effort: no shm/theme -> no cursor.
      if (shm_name_ != 0 && wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_))
        cursor_ready_ = cursor_.Init(shm_.Get()->GetProxy(), comp_.Get()->GetProxy());
      if (!cursor_ready_) log::w(kTag, "dmabuf present: no cursor (wl_shm/theme unavailable)");
    }
    if (want_ime_ && ti_name_ != 0 && !seat_.IsNull()) {
      ti_.Record(ti_name_, ti_ver_);
      ime_bound_ = ti_.Bind(registry_, this, seat_.Get()->GetProxy());
      if (!ime_bound_) log::w(kTag, "dmabuf present: text-input bind failed");
    }
    // v4+ has per-format modifier feedback; v1–v3 use the legacy format list.
    if (feedback_.BoundVersion() >= 4) (void)feedback_.StartDefault(display_.Get());
    if (!wl::RoundtripWithTimeout(display_.Get())) return false;
    if (feedback_.BoundVersion() < 4) feedback_.CommitLegacy();

    mods_ = feedback_.Current().ModifiersFor(kDrmFormatXrgb8888);
    log::i(kTag, "dmabuf present: compositor advertises {} modifier(s) for XRGB8888",
           mods_.size());

    // The wl_surface is shared by every shell; its role is established below per ShellKind.
    if (!wl::SetupHandler(surface_, wl::construct<wl_surface_traits,
                          wl_compositor_traits::Op::CreateSurface>(*comp_.Get())))
      return false;
    frame_.need = &need_frame_;
    frame_.count = &frame_callbacks_;

    if (!create_shell_role()) return false;

    // Commit (no buffer) to elicit the initial configure, then present.
    surface_.Get()->Commit();
    (void)wl::RoundtripWithTimeout(display_.Get());
    // Shells without a size-bearing configure handshake (agl background and fullscreen do
    // not send one; ivi may) are ready for the first frame immediately — the buffer is the
    // requested/output size and the compositor lays it out.
    if (!configured_ && shell_kind_ != ShellKind::Xdg) { configured_ = true; need_frame_ = true; }
    return true;
  }

  // ── Shell role creation (ACL_WL_SHELL dispatch) ──────────────────────────────────────
  bool create_shell_role() noexcept {
    switch (shell_kind_) {
      case ShellKind::Xdg:
        return create_xdg_role();
      case ShellKind::Agl:
        if (create_agl_role()) return true;
        log::w(kTag, "dmabuf present: agl_shell unavailable/refused; falling back to xdg");
        shell_kind_ = ShellKind::Xdg;
        return create_xdg_role();
      case ShellKind::Ivi:
        return create_ivi_role();
      case ShellKind::Simple:
        return create_simple_role();
    }
    return false;
  }

  // xdg-shell (desktop): xdg_surface + xdg_toplevel, titled; size/close via configure.
  bool create_xdg_role() noexcept {
    if (xdg_wm_base_.IsNull()) {
      log::w(kTag, "dmabuf present: xdg_wm_base not bound");
      return false;
    }
    if (!wl::SetupHandler(xdg_surface_, wl::construct<xdg_surface_traits,
                          xdg_wm_base_traits::Op::GetXdgSurface>(
                              *xdg_wm_base_.Get(), surface_.Get()->GetProxy())))
      return false;
    xdg_surface_.Get()->app_ = this;
    if (!wl::SetupHandler(xdg_toplevel_, wl::construct<xdg_toplevel_traits,
                          xdg_surface_traits::Op::GetToplevel>(*xdg_surface_.Get())))
      return false;
    xdg_toplevel_.Get()->app_ = this;
    xdg_toplevel_.Get()->SetTitle(title_.c_str());
    xdg_toplevel_.Get()->SetAppId(app_id_.c_str());
    return true;
  }

  // AGL agl_shell: acl is the shell client. Wait for the bound handshake, then assign the
  // surface the background (default) or a panel edge, and signal ready. `Normal` presents a
  // regular xdg surface and asks the compositor to activate it (app_id-keyed).
  bool create_agl_role() noexcept {
    if (agl_.IsNull()) return false;
    if (need_agl_bound_) {  // pump until bound_ok/bound_fail (agl_shell v2+)
      int ret = 0;
      while (ret >= 0 && !agl_.Get()->bound_done)
        ret = wl_display_dispatch(display_.Get());
      if (!agl_.Get()->bound_ok) {
        log::w(kTag, "dmabuf present: agl_shell bound_fail (extension already in use)");
        return false;
      }
    }
    if (agl_role_ == AglRole::Normal) {
      if (!create_xdg_role()) return false;
      if (output_ != nullptr) agl_.Get()->ActivateApp(app_id_.c_str(), output_);
      return true;
    }
    if (output_ == nullptr) {
      log::w(kTag, "dmabuf present: agl_shell background/panel needs a wl_output");
      return false;
    }
    wl_proxy* surf = surface_.Get()->GetProxy();
    if (agl_role_ == AglRole::Background) {
      agl_.Get()->SetBackground(surf, output_);
    } else {
      const std::uint32_t edge =
          agl_role_ == AglRole::PanelTop    ? static_cast<std::uint32_t>(agl::AglShellEdge::Top)
        : agl_role_ == AglRole::PanelBottom ? static_cast<std::uint32_t>(agl::AglShellEdge::Bottom)
        : agl_role_ == AglRole::PanelLeft   ? static_cast<std::uint32_t>(agl::AglShellEdge::Left)
                                            : static_cast<std::uint32_t>(agl::AglShellEdge::Right);
      agl_.Get()->SetPanel(surf, output_, edge);
    }
    agl_.Get()->Ready();
    return true;
  }

  // ivi-shell: create the ivi_surface for the configured numeric id; the compositor's
  // ivi-controller maps the id to a layer/screen. configure(w,h) arrives asynchronously.
  bool create_ivi_role() noexcept {
    if (ivi_app_.IsNull()) return false;
    if (ivi_id_ == 0) {
      log::w(kTag, "dmabuf present: ivi-shell needs ACL_IVI_SURFACE_ID (nonzero)");
      return false;
    }
    if (!wl::SetupHandler(ivi_surface_, wl::construct<ivi::ivi_surface_traits,
                          ivi::ivi_application_traits::Op::SurfaceCreate>(
                              *ivi_app_.Get(), ivi_id_, surface_.Get()->GetProxy())))
      return false;
    ivi_surface_.Get()->configured = &configured_;
    ivi_surface_.Get()->need = &need_frame_;
    log::i(kTag, "dmabuf present: ivi_surface id={}", ivi_id_);
    return true;
  }

  // zwp_fullscreen_shell_v1 ("simple"): present the surface fullscreen, no window
  // management. Default present method lets the compositor choose scaling/centering.
  bool create_simple_role() noexcept {
    if (fsh_.IsNull()) return false;
    fsh_.Get()->PresentSurface(
        surface_.Get()->GetProxy(),
        static_cast<std::uint32_t>(fsh::ZwpFullscreenShellV1PresentMethod::Default), output_);
    return true;
  }

  // Which shell global must the compositor advertise for the selected ShellKind.
  bool shell_global_present() const noexcept {
    switch (shell_kind_) {
      case ShellKind::Xdg:    return xdg_name_ != 0;
      case ShellKind::Agl:    return agl_name_ != 0 || xdg_name_ != 0;  // agl falls back to xdg
      case ShellKind::Ivi:    return ivi_name_ != 0;
      case ShellKind::Simple: return fsh_name_ != 0;
    }
    return false;
  }
  const char* shell_global_name() const noexcept {
    switch (shell_kind_) {
      case ShellKind::Xdg:    return "xdg_wm_base";
      case ShellKind::Agl:    return "agl_shell";
      case ShellKind::Ivi:    return "ivi_application";
      case ShellKind::Simple: return "zwp_fullscreen_shell_v1";
    }
    return "?";
  }

  // wl::DmabufFeedback<Presenter> callbacks.
  void OnDmabufFeedback(const wl::FeedbackSnapshot&) noexcept {}
  void OnDmabufLegacyFormat(std::uint32_t, std::uint64_t) noexcept {}
  // wp_presentation feedback — the vsync clock that aligns Choreographer's frameTimeNanos.
  void OnPresented(const wl::PresentFeedback& fb) noexcept {
    last_present_ns_ = fb.present_ns;
    if (fb.refresh_ns != 0) refresh_ns_ = fb.refresh_ns;
  }
  void OnDiscarded(std::uint32_t) noexcept {}
  // xdg handlers (the wcs XdgSurface/XdgToplevel handlers dispatch to these).
  void OnXdgSurfaceConfigure(std::uint32_t) noexcept { configured_ = true; need_frame_ = true; }
  void OnToplevelConfigure(std::int32_t, std::int32_t) noexcept {}
  void OnToplevelClose() noexcept { closed_ = true; }

  // wl_pointer → app input (native side of a MotionEvent): left button = touch Down/Up,
  // motion while pressed = Move.
  // Own the cursor: set it on enter (else the compositor leaves it undefined over the window).
  void OnPointerEnter(const wl::PointerEvent& e) {
    px_ = e.x; py_ = e.y;
    pointer_enter_serial_ = e.serial;
    if (cursor_ready_ && pointer_.Get() != nullptr)
      cursor_.Set(pointer_.Get()->GetProxy(), pointer_enter_serial_, "default");
  }
  void OnPointerLeave() { cursor_.Reset(); }
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

  // wl_keyboard → app input (native side of a KeyEvent). Press/release/repeat → Down/Up.
  void OnKey(const wl::KeyEvent& e) {
    if (input_ == nullptr || !*input_) return;
    const bool pressed = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;
    (*input_)(KeyInput{pressed ? KeyInput::Action::Down : KeyInput::Action::Up, e.key,
                       static_cast<std::uint32_t>(e.keysym)});
  }

  // wl::ime::TextInputListener — the compositor's IME (text-input-v3).
  void OnEnter() override {}
  void OnLeave() override {}
  void OnCommitString(std::string_view utf8) override {
    if (input_ != nullptr && *input_) (*input_)(TextInput{std::string(utf8), false, 1});
  }
  void OnPreeditString(std::string_view utf8, std::int32_t, std::int32_t) override {
    if (input_ != nullptr && *input_) (*input_)(TextInput{std::string(utf8), true, 1});
  }
  void OnDeleteSurroundingText(std::uint32_t before, std::uint32_t after) override {
    if (input_ != nullptr && *input_) (*input_)(DeleteSurroundingText{before, after});
  }
  void OnKeysym(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override {}

 private:
  void deliver(PointerInput::Action a) {
    if (input_ != nullptr && *input_)
      (*input_)(PointerInput{a, static_cast<float>(px_), static_cast<float>(py_)});
  }

  bool build_buffer(const gfx::DmabufFrame& frame) noexcept {
    wl_proxy* praw = feedback_.CreateParams();
    if (praw == nullptr) { log::w(kTag, "dmabuf present: create_params failed"); return false; }
    params_.Attach(praw);
    // One dma-buf shared by all planes at distinct offsets; libwayland dups the fd.
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
      params_.Get()->Add(frame.fd, i, frame.planes[i].offset, frame.planes[i].stride,
                         static_cast<std::uint32_t>(frame.modifier >> 32u),
                         static_cast<std::uint32_t>(frame.modifier & 0xffffffffu));
    }
    wl_proxy* braw = wl::construct<wl_buffer_traits,
                                   dmabuf::zwp_linux_buffer_params_v1_traits::Op::CreateImmed>(
        *params_.Get(), static_cast<std::int32_t>(frame.width),
        static_cast<std::int32_t>(frame.height), frame.drm_format, 0u);
    if (braw == nullptr) { log::w(kTag, "dmabuf present: create_immed failed"); return false; }
    buffer_._SetProxy(braw);
    buffer_.busy = &busy_;
    w_ = static_cast<int>(frame.width);
    h_ = static_cast<int>(frame.height);
    return true;
  }

  // Build the wl_buffer for ping-pong slot `s` from its (stable) dma-buf frame. Each slot's
  // wl_buffer.release clears its own busy flag.
  bool build_slot(int s, const gfx::DmabufFrame& frame) noexcept {
    wl_proxy* praw = feedback_.CreateParams();
    if (praw == nullptr) { log::w(kTag, "dmabuf present(loop): create_params failed"); return false; }
    // A fresh, per-build params object (event-less for create_immed) — destroyed at scope
    // end. A shared member would trip WlPtr::Attach's non-null assert on the second slot.
    wl::WlPtr<BufferParamsH> params;
    params.Attach(praw);
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
      params.Get()->Add(frame.fd, i, frame.planes[i].offset, frame.planes[i].stride,
                        static_cast<std::uint32_t>(frame.modifier >> 32u),
                        static_cast<std::uint32_t>(frame.modifier & 0xffffffffu));
    }
    wl_proxy* braw = wl::construct<wl_buffer_traits,
                                   dmabuf::zwp_linux_buffer_params_v1_traits::Op::CreateImmed>(
        *params.Get(), static_cast<std::int32_t>(frame.width),
        static_cast<std::int32_t>(frame.height), frame.drm_format, 0u);
    if (braw == nullptr) { log::w(kTag, "dmabuf present(loop): create_immed failed"); return false; }
    slots_[s].handler._SetProxy(braw);
    slots_[s].handler.busy = &slots_[s].busy;
    slots_[s].built = true;
    w_ = static_cast<int>(frame.width);
    h_ = static_cast<int>(frame.height);
    return true;
  }

  int free_slot() const noexcept {
    for (int i = 0; i < kSlots; ++i)
      if (!slots_[i].busy) return i;
    return -1;
  }

  bool present_loop() noexcept {
    while (!closed_) {
      if (configured_ && need_frame_ &&
          (max_frames_ == 0 || frame_index_ < max_frames_)) {
        need_frame_ = false;
        // Request the next frame-callback tick, then attach + commit the frame.
        if (wl_proxy* raw = wl::construct<wl_callback_traits, wl_surface_traits::Op::Frame>(
                *surface_.Get()))
          frame_._SetProxy(raw);
        busy_ = true;
        surface_.Get()->Attach(buffer_.GetProxy(), 0, 0);
        surface_.Get()->Damage(0, 0, w_, h_);
        surface_.Get()->Commit();
        ++frame_index_;
      }
      if (max_frames_ != 0 && frame_index_ >= max_frames_ && frame_callbacks_ >= max_frames_)
        break;
      while (wl_display_flush(display_.Get()) < 0 && errno == EAGAIN) {}
      pollfd pfd{wl_display_get_fd(display_.Get()), POLLIN, 0};
      const int n = poll(&pfd, 1, 200);
      if (n < 0 && errno != EINTR) break;
      if (n > 0 && (pfd.revents & POLLIN)) {
        if (wl_display_dispatch(display_.Get()) < 0) break;
      } else if (n == 0) {
        wl_display_dispatch_pending(display_.Get());
      }
    }
    const bool clean = frame_index_ > 0 && (closed_ || frame_callbacks_ >= frame_index_);
    log::i(kTag, "dmabuf present: {} frame(s), {} callback(s); {} ({})", frame_index_,
           frame_callbacks_, closed_ ? "toplevel closed" : "done",
           clean ? "clean" : "aborted");
    return clean;
  }

  // Animated loop (S4b): each vsync tick (wl_surface.frame — the compositor is ready for the
  // next frame), pick a FREE ping-pong buffer (one the compositor has released), render the
  // next GPU frame into it via frame_fn_, and present it. Double-buffered: the compositor
  // holds one image while we render the other, so the frame-callback/release cycle keeps
  // flowing (a single buffer would stall — the compositor holds it for scanout and won't
  // release or re-tick). end_frame fence-waits for the GPU copy before Commit.
  bool present_loop_dynamic() noexcept {
    while (!closed_) {
      if (configured_ && need_frame_ && free_slot() >= 0 &&
          (max_frames_ == 0 || frame_index_ < max_frames_)) {
        const int s = free_slot();
        need_frame_ = false;
        gfx::DmabufFrame frame;
        if (!(*frame_fn_)(s, mods_, frame_time_ns(), frame_index_, &frame) || frame.fd < 0)
          break;
        if (!slots_[s].built && !build_slot(s, frame)) break;
        if (frame_index_ == 0)
          log::i(kTag, "dmabuf present(loop): {}x{} {} modifier=0x{:x} {} plane(s), {} buffers",
                 frame.width, frame.height, frame.modifier_tiled ? "tiled" : "linear",
                 frame.modifier, frame.plane_count, kSlots);
        if (wl_proxy* raw = wl::construct<wl_callback_traits, wl_surface_traits::Op::Frame>(
                *surface_.Get()))
          frame_._SetProxy(raw);
        slots_[s].busy = true;
        presentation_.Arm(surface_.Get()->GetProxy(),
                          static_cast<std::uint32_t>(frame_index_));  // before commit
        surface_.Get()->Attach(slots_[s].handler.GetProxy(), 0, 0);
        surface_.Get()->Damage(0, 0, w_, h_);
        surface_.Get()->Commit();
        ++frame_index_;
      }
      if (max_frames_ != 0 && frame_index_ >= max_frames_ && frame_callbacks_ >= max_frames_)
        break;
      while (wl_display_flush(display_.Get()) < 0 && errno == EAGAIN) {}
      presentation_.Reap();
      // Poll the wl fd + the wcs KeyboardHandler repeat timerfd (a held key repeats).
      const int rfd = keyboard_.IsNull() ? -1 : keyboard_.Get()->GetRepeatFd();
      pollfd pfds[2] = {{wl_display_get_fd(display_.Get()), POLLIN, 0}, {rfd, POLLIN, 0}};
      const int n = poll(pfds, 2, kTickMs);
      if (n < 0 && errno != EINTR) break;
      if (n > 0 && (pfds[1].revents & POLLIN) != 0) keyboard_.Get()->DispatchRepeat();
      if (n > 0 && (pfds[0].revents & POLLIN)) {
        if (wl_display_dispatch(display_.Get()) < 0) break;
      } else if (n == 0) {
        // No frame callback within kTickMs — the compositor is throttling us (the surface
        // is occluded/unmapped, so wl_surface.frame legitimately stops). Keep the animation
        // progressing with a self-tick; when the surface is visible, POLLIN wins first and
        // we run at the compositor's vsync cadence instead.
        wl_display_dispatch_pending(display_.Get());
        need_frame_ = true;
      }
    }
    presentation_.Release();
    ti_.Release();
    const bool clean = frame_index_ > 0 && (closed_ || frame_callbacks_ >= frame_index_);
    log::i(kTag, "dmabuf present(loop): {} frame(s), {} callback(s); {} ({})", frame_index_,
           frame_callbacks_, closed_ ? "toplevel closed" : "done", clean ? "clean" : "aborted");
    return clean;
  }

  // The vsync-aligned time for the frame about to be drawn (Choreographer.frameTimeNanos): the last
  // wp_presentation time plus one refresh interval. CRITICAL: this is the Choreographer CLOCK, which
  // must advance continuously at the refresh rate even when the app is idle / presents stall — it is
  // present-driven only for grid ALIGNMENT. If it stalls, a media renderer that schedules off vsync
  // (ExoPlayer's VideoFrameReleaseHelper) deadlocks: no new vsync -> no frame rendered -> no present ->
  // no new vsync. So if last_present+refresh has fallen into the past, advance it by whole refresh
  // intervals to the next grid point at or after now.
  std::uint64_t frame_time_ns() const noexcept override {
    const clockid_t clk = presentation_.Bound() ? presentation_.clock() : CLOCK_MONOTONIC;
    timespec ts{};
    clock_gettime(clk, &ts);
    const std::uint64_t now =
        static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
    if (last_present_ns_ == 0 || refresh_ns_ == 0) return now;
    std::uint64_t t = last_present_ns_ + refresh_ns_;
    if (t < now) t += ((now - t) / refresh_ns_ + 1) * refresh_ns_;  // catch up to the vsync grid
    return t;
  }

  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::DmabufFeedback<Presenter> feedback_;
  wl::PresentationManager<Presenter> presentation_;  // wp_presentation (vsync clock)
  wl::WlPtr<SeatH> seat_;
  wl::WlPtr<wl::PointerHandler<Presenter>> pointer_;    // created on wl_seat capabilities
  wl::WlPtr<wl::KeyboardHandler<Presenter>> keyboard_;  // created on wl_seat capabilities
  wl::ime::SelectedTextInput ti_;                       // text-input-v3 (IME)
  wl::WlPtr<CompositorH> comp_;
  wl::WlPtr<ShmH> shm_;                 // bound only for the cursor theme
  wl::CursorManager cursor_;            // wl_pointer.set_cursor on enter
  bool cursor_ready_ = false;
  std::uint32_t pointer_enter_serial_ = 0;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<SurfaceH> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<Presenter>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<Presenter>> xdg_toplevel_;
  // Alternate shells (ACL_WL_SHELL): only the selected one is bound. wl_output targets the
  // agl background/panel and the fullscreen present.
  wl::WlPtr<AglShellH> agl_;
  wl::WlPtr<IviAppH> ivi_app_;
  wl::WlPtr<IviSurfaceH> ivi_surface_;
  wl::WlPtr<FullscreenShellH> fsh_;
  wl_proxy* output_ = nullptr;  // first wl_output (event-less bare proxy)
  wl::WlPtr<BufferParamsH> params_;
  BufferH buffer_;  // direct member: stable address for the dispatcher (static path)
  FrameH frame_;    // direct member: stable address for the dispatcher

  // Ping-pong buffers for the animated loop. kSlots must match the producer's dma-buf
  // image count (gpu_dmabuf kBuffers). Each BufferH has a stable address (array member).
  static constexpr int kSlots = 2;
  struct Slot {
    BufferH handler;
    bool busy = false;
    bool built = false;
  };
  Slot slots_[kSlots];
  int last_slot_ = -1;  // slot of the last submitted frame (re-attached by keepalive)

  std::string title_, app_id_;
  std::vector<std::uint64_t> mods_;           // compositor's advertised XRGB8888 modifiers
  const DmabufFrameFn* frame_fn_ = nullptr;   // per-tick producer (animated loop)
  const InputCallback* input_ = nullptr;      // pointer/key/IME sink (loop borrows; A owns below)
  InputCallback owned_input_;                 // owned input sink for the Looper-driven path (A)
  std::uint32_t comp_name_ = 0, comp_ver_ = 0, xdg_name_ = 0, xdg_ver_ = 0;
  std::uint32_t shm_name_ = 0, shm_ver_ = 0;  // wl_shm (cursor theme only)
  std::uint32_t seat_name_ = 0, seat_ver_ = 0, ti_name_ = 0, ti_ver_ = 0;
  std::uint32_t agl_name_ = 0, agl_ver_ = 0, ivi_name_ = 0, ivi_ver_ = 0;
  std::uint32_t fsh_name_ = 0, fsh_ver_ = 0, output_name_ = 0, output_ver_ = 0;
  ShellKind shell_kind_ = ShellKind::Xdg;   // ACL_WL_SHELL (default xdg)
  AglRole agl_role_ = AglRole::Background;   // ACL_AGL_WINDOW_TYPE
  std::uint32_t ivi_id_ = 0;                 // ACL_IVI_SURFACE_ID
  bool need_agl_bound_ = false;              // await agl_shell bound_ok before set_background
  double px_ = 0.0, py_ = 0.0;
  bool pointer_down_ = false;
  bool want_ime_ = false, ime_bound_ = false;
  int w_ = 0, h_ = 0;
  bool busy_ = false;
  std::uint64_t last_present_ns_ = 0, refresh_ns_ = 0;  // wp_presentation vsync clock
  std::uint64_t max_frames_ = 0, frame_index_ = 0, frame_callbacks_ = 0;
  bool configured_ = false, need_frame_ = false, closed_ = false;
};

}  // namespace

bool present_dmabuf(const DmabufRenderFn& render, std::string_view title,
                    std::string_view app_id, int frames) noexcept {
  Presenter p;
  return p.run(render, title, app_id, frames);
}

bool present_dmabuf_loop(int /*width*/, int /*height*/, std::string_view title,
                         std::string_view app_id, const DmabufFrameFn& render,
                         const InputCallback& input, bool want_ime,
                         std::uint64_t max_frames) noexcept {
  Presenter p;
  return p.run_loop(render, input, want_ime, title, app_id, max_frames);
}

std::unique_ptr<IPresenter> presenter_open(int width, int height, std::string_view title,
                                           std::string_view app_id, InputCallback input,
                                           bool want_ime) noexcept {
  auto p = std::make_unique<Presenter>();
  if (!p->open(width, height, title, app_id, std::move(input), want_ime)) return nullptr;
  return p;
}

}  // namespace acl::wm

#endif  // ACL_HAVE_WCS && ACL_HAVE_VULKAN
