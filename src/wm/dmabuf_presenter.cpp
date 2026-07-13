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
#include <ctime>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <wayland-client.h>

// Generated CRTP client headers (scanner output) — MUST precede the wl/ framework.
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"
#include "linux_dmabuf_client.hpp"

#include <wl/client_helpers.hpp>
#include <wl/dmabuf_feedback.hpp>  // wl::DmabufFeedback<App>, wl::FeedbackSnapshot
#include <wl/display.hpp>
#include <wl/linux_dmabuf.hpp>     // must follow linux_dmabuf_client.hpp
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

// The presenter is the DmabufFeedback "App": it receives OnDmabufFeedback (v4 tranches)
// and OnDmabufLegacyFormat (v1–v3 fallback) via the wcs binding, and drives xdg-shell.
class Presenter {
 public:
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
  bool run_loop(const DmabufFrameFn& render, const InputCallback& /*input*/,
                bool /*want_ime*/, std::string_view title, std::string_view app_id,
                std::uint64_t max_frames) noexcept {
    max_frames_ = max_frames;
    if (!setup(title, app_id)) return false;
    frame_fn_ = &render;
    return present_loop_dynamic();
  }

  // Connect, bind (wl_compositor / xdg_wm_base / zwp_linux_dmabuf_v1), learn the advertised
  // modifiers, create the surface + xdg_toplevel, and commit-for-configure. Shared by the
  // static and animated present paths.
  bool setup(std::string_view title, std::string_view app_id) noexcept {
    title_ = std::string(title);
    app_id_ = std::string(app_id);
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
      else if (iface == xdg_wm_base_traits::interface_name) { xdg_name_ = name; xdg_ver_ = ver; }
      else if (iface == dmabuf::zwp_linux_dmabuf_v1_traits::interface_name) {
        feedback_.Record(name, ver);
      }
    });
    if (!wl::RoundtripWithTimeout(display_.Get())) {
      log::w(kTag, "dmabuf present: registry roundtrip failed");
      return false;
    }
    if (comp_name_ == 0 || xdg_name_ == 0) {
      log::w(kTag, "dmabuf present: compositor/xdg_wm_base not advertised");
      return false;
    }
    if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
            comp_name_, std::min(comp_ver_, wl_compositor_traits::version)))
      comp_.Attach(raw);
    else return false;
    if (!wl::BindHandler<xdg_wm_base_traits>(registry_, xdg_wm_base_, xdg_name_, xdg_ver_))
      return false;
    if (!feedback_.Bind(registry_, this)) {
      log::w(kTag, "dmabuf present: zwp_linux_dmabuf_v1 bind failed");
      return false;
    }
    // v4+ has per-format modifier feedback; v1–v3 use the legacy format list.
    if (feedback_.BoundVersion() >= 4) (void)feedback_.StartDefault(display_.Get());
    if (!wl::RoundtripWithTimeout(display_.Get())) return false;
    if (feedback_.BoundVersion() < 4) feedback_.CommitLegacy();

    mods_ = feedback_.Current().ModifiersFor(kDrmFormatXrgb8888);
    log::i(kTag, "dmabuf present: compositor advertises {} modifier(s) for XRGB8888",
           mods_.size());

    if (!wl::SetupHandler(surface_, wl::construct<wl_surface_traits,
                          wl_compositor_traits::Op::CreateSurface>(*comp_.Get())))
      return false;
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

    frame_.need = &need_frame_;
    frame_.count = &frame_callbacks_;

    // Commit (no buffer) to elicit the initial configure, then present.
    surface_.Get()->Commit();
    (void)wl::RoundtripWithTimeout(display_.Get());
    return true;
  }

  // wl::DmabufFeedback<Presenter> callbacks.
  void OnDmabufFeedback(const wl::FeedbackSnapshot&) noexcept {}
  void OnDmabufLegacyFormat(std::uint32_t, std::uint64_t) noexcept {}
  // xdg handlers (the wcs XdgSurface/XdgToplevel handlers dispatch to these).
  void OnXdgSurfaceConfigure(std::uint32_t) noexcept { configured_ = true; need_frame_ = true; }
  void OnToplevelConfigure(std::int32_t, std::int32_t) noexcept {}
  void OnToplevelClose() noexcept { closed_ = true; }

 private:
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
        surface_.Get()->Attach(slots_[s].handler.GetProxy(), 0, 0);
        surface_.Get()->Damage(0, 0, w_, h_);
        surface_.Get()->Commit();
        ++frame_index_;
      }
      if (max_frames_ != 0 && frame_index_ >= max_frames_ && frame_callbacks_ >= max_frames_)
        break;
      while (wl_display_flush(display_.Get()) < 0 && errno == EAGAIN) {}
      pollfd pfd{wl_display_get_fd(display_.Get()), POLLIN, 0};
      const int n = poll(&pfd, 1, kTickMs);
      if (n < 0 && errno != EINTR) break;
      if (n > 0 && (pfd.revents & POLLIN)) {
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
    const bool clean = frame_index_ > 0 && (closed_ || frame_callbacks_ >= frame_index_);
    log::i(kTag, "dmabuf present(loop): {} frame(s), {} callback(s); {} ({})", frame_index_,
           frame_callbacks_, closed_ ? "toplevel closed" : "done", clean ? "clean" : "aborted");
    return clean;
  }

  static std::uint64_t frame_time_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
  }

  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::DmabufFeedback<Presenter> feedback_;
  wl::WlPtr<CompositorH> comp_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<SurfaceH> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<Presenter>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<Presenter>> xdg_toplevel_;
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

  std::string title_, app_id_;
  std::vector<std::uint64_t> mods_;           // compositor's advertised XRGB8888 modifiers
  const DmabufFrameFn* frame_fn_ = nullptr;   // per-tick producer (animated loop)
  std::uint32_t comp_name_ = 0, comp_ver_ = 0, xdg_name_ = 0, xdg_ver_ = 0;
  int w_ = 0, h_ = 0;
  bool busy_ = false;
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

}  // namespace acl::wm

#endif  // ACL_HAVE_WCS && ACL_HAVE_VULKAN
